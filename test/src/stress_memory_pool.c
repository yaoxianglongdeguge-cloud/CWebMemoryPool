/*
 * stress_memory_pool.c —— CWebMemoryPool 压力 / 性质测试
 *
 * 与单元用例不同：这里用“影子模型”逐操作校验不变式，并把操作量拉到百万级。
 *   local 模式（默认）：T 个线程各自持有独立池
 *                        —— 与 c_web 里的用法一致（profes->txt_pool / w->pool / 连接私有暂存池）
 *   shared 模式：T 个线程共享一个池 + 外部 pthread_mutex（池本身无锁，必须外部同步）
 *   shared-unlocked 模式：故意不加锁，仅用于证明“共享必须加锁”（会产生失败/竞争报告）
 *
 * 每次操作校验的不变式：
 *   I1 返回地址在池内，且偏移等于影子模型记录的 bump 期望值
 *   I2 同一池内所有在用块互不重叠
 *   I3 每池“已用 + 空闲 == perSize”账目自洽
 *   I4 每池 hadAllocNum == 模型统计的在用块数
 *   I5 freeEntry[i] == (freeSize[i] >= 1 + 4B 护栏)，即“满”的判定一致
 *   I6 某池在用块全部归还后必须复位（游标回起点 / freeSize 回满 / freeEntry=true）
 *   I7 notfull=0 时所有池的 freeSize 都 < 请求 + 4（不允许“还有空间却让上层 503”）
 *   I8 结束时全部归还 → 所有池复位
 *
 * 用法: ./stress_memory_pool [local|shared|shared-unlocked] [线程数] [每线程操作数] [seed]
 */
#define _POSIX_C_SOURCE 200809L
#include "MemoryPool.h"
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#define GUARD 4
#ifndef PER
#define PER 4096      /* 小池子字节数（可用 -DPER=65536 做“池子大小 vs 503 率”实验） */
#endif
#ifndef ENTRIES
#define ENTRIES 16    /* 池个数（池子个数是控制降级率的主要杠杆） */
#endif
#ifndef MAXLIVE
#define MAXLIVE 64    /* 每池同时跟踪的在用块上限（测试规模内足够） */
#endif
#define MAGIC 0x5A5A0000u

typedef struct { int off; int size; } Blk;

typedef struct {
    Memory_Pool* pool;
    Blk live[ENTRIES][MAXLIVE];
    int liveCnt[ENTRIES];
    int nextOff[ENTRIES];          /* 影子模型：该池下一次分配的期望偏移 */
    long long ops, allocs, frees, fails;
    long long liveBytes;           /* 影子模型统计的在用数据字节 */
    long errors;
    unsigned seed;
    int tid;
    long opsTarget;
} Worker;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void local_destroy(Memory_Pool* p)
{
    if (p == NULL) return;
    free(p->beginPtr);
    free(p->manageEntry);
    free(p->freeEntry);
    free(p);
}

static inline unsigned rnd32(unsigned* s)
{
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (*s = x);
}

#define FAIL(w, ...)                                                              \
    do {                                                                          \
        (w)->errors++;                                                            \
        if ((w)->errors <= 5) {                                                   \
            printf("  [FAIL][t%d] ", (w)->tid);                                   \
            printf(__VA_ARGS__);                                                  \
            printf("\n");                                                         \
        }                                                                         \
    } while (0)

/* 请求大小分布：60% 小块(16..96) / 30% 中块(97..512) / 10% 大块(513..2012)
 * 编 -DSMALL_ONLY 时只申请 16~96B（用于对比“请求大小分布”对降级率的影响） */
#ifndef SMALL_ONLY
static int pick_size(unsigned* s)
{
    unsigned r = rnd32(s) % 100;
    if (r < 60) return 16 + (int)(rnd32(s) % 81);
    if (r < 90) return 97 + (int)(rnd32(s) % 416);
    return 513 + (int)(rnd32(s) % 1500);
}
#endif

/* I3/I4/I5：每次操作后核对池内部账目与“满”标记 */
static void check_entry(Worker* w, int e, const char* when)
{
    Memory_Entry* ent = &w->pool->manageEntry[e];
    long long used = (long long)(ent->allocPtr - ent->beginPtr);

    if (used + ent->freeSize != ent->totalSize)
        FAIL(w, "%s 第 %d 号池账目不平: 已用=%lld + 空闲=%d != %d",
             when, e, used, ent->freeSize, ent->totalSize);
    if (ent->hadAllocNum != w->liveCnt[e])
        FAIL(w, "%s 第 %d 号池计数不符: hadAllocNum=%d 模型在用=%d",
             when, e, ent->hadAllocNum, w->liveCnt[e]);
    if (w->pool->freeEntry[e] != (ent->freeSize >= 1 + GUARD))
        FAIL(w, "%s 第 %d 号池“满”标记不一致: freeEntry=%d freeSize=%d",
             when, e, w->pool->freeEntry[e], ent->freeSize);
}

static void do_alloc(Worker* w)
{
#ifdef SMALL_ONLY
    int sz = 16 + (int)(rnd32(&w->seed) % 81); /* 只申请 16~96B：验证“请求大小分布”是不是降级率主因 */
#else
    int sz = pick_size(&w->seed);
#endif
    void* ptr = NULL;
    int notfull = 0;

    MemoryPoolAlloc(w->pool, sz, &ptr, &notfull);
    w->ops++;
    w->allocs++;

    if (notfull != 1) {
        w->fails++;
        /* I7：既然没有任何池子能装下，每个池的剩余都必须 < 请求 + 护栏 */
        for (int i = 0; i < ENTRIES; i++) {
            if (w->pool->manageEntry[i].freeSize >= sz + GUARD)
                FAIL(w, "notfull=0，但第 %d 号池还剩 %dB（请求 %d+%d）——会出现“有空间却 503”",
                     i, w->pool->manageEntry[i].freeSize, sz, GUARD);
        }
        return;
    }
    if (ptr == NULL) { FAIL(w, "notfull=1 但没返回地址（请求 %d）", sz); return; }

    int e = (int)(((char*)ptr - w->pool->beginPtr) / PER);
    if (e < 0 || e >= ENTRIES) { FAIL(w, "返回地址越界: %p", ptr); return; }

    Memory_Entry* ent = &w->pool->manageEntry[e];
    int off = (int)((char*)ptr - ent->beginPtr);
    if (off != w->nextOff[e])                                    /* I1 */
        FAIL(w, "第 %d 号池偏移不符 bump 顺序: 期望 %d 实际 %d（请求 %d）", e, w->nextOff[e], off, sz);

    if (w->liveCnt[e] >= MAXLIVE) { FAIL(w, "第 %d 号池在用块超过模型容量", e); return; }
    for (int k = 0; k < w->liveCnt[e]; k++) {                    /* I2 */
        Blk* b = &w->live[e][k];
        if (off < b->off + b->size && b->off < off + sz)
            FAIL(w, "第 %d 号池新块 [%d,%d) 与在用块 [%d,%d) 重叠",
                 e, off, off + sz, b->off, b->off + b->size);
    }

    w->live[e][w->liveCnt[e]].off = off;
    w->live[e][w->liveCnt[e]].size = sz;
    w->liveCnt[e]++;
    w->liveBytes += sz;
    w->nextOff[e] = off + sz + GUARD;
    unsigned tag = MAGIC | (unsigned)e;      /* 用 memcpy 写：池只保证字节对齐，直接按 unsigned* 写是 UB */
    memcpy(ptr, &tag, sizeof(tag));          /* （见 README：需要对齐访问时请自行按对齐边界申请） */

    check_entry(w, e, "分配后");
}

static void do_free(Worker* w)
{
    int total = 0;
    for (int i = 0; i < ENTRIES; i++) total += w->liveCnt[i];
    if (total == 0) return;

    int pick = (int)(rnd32(&w->seed) % (unsigned)total);
    int e = -1;
    int k = -1;
    for (int i = 0; i < ENTRIES; i++) {
        if (pick < w->liveCnt[i]) { e = i; k = pick; break; }
        pick -= w->liveCnt[i];
    }
    if (e < 0) return;
    if (w->liveCnt[e] <= 0) { FAIL(w, "第 %d 号池模型计数异常 %d", e, w->liveCnt[e]); return; }

    Blk* b = &w->live[e][k];
    if (b->off < 0 || b->off > PER - 4) {   /* 影子模型被破坏时的保护（只在反例模式可能出现） */
        FAIL(w, "影子模型偏移异常 %d（第 %d 号池），跳过本次释放", b->off, e);
        return;
    }
    void* ptr = w->pool->manageEntry[e].beginPtr + b->off;
    unsigned got = 0;
    memcpy(&got, ptr, sizeof(got));
    if (got != (MAGIC | (unsigned)e))
        FAIL(w, "第 %d 号池偏移 %d 的块被改写（标记 0x%08X != 0x%08X），可能两块共用同一内存",
             e, b->off, got, MAGIC | (unsigned)e);

    MemoryPoolFree(w->pool, ptr);
    w->ops++;
    w->frees++;
    w->liveBytes -= b->size;

    w->live[e][k] = w->live[e][w->liveCnt[e] - 1];   /* swap-remove */
    w->liveCnt[e]--;

    if (w->liveCnt[e] == 0) {                        /* I6：归零必须复位 */
        Memory_Entry* ent = &w->pool->manageEntry[e];
        if (ent->allocPtr != ent->beginPtr || ent->freeSize != ent->totalSize ||
            w->pool->freeEntry[e] != true)
            FAIL(w, "第 %d 号池在用块归零后未复位: 游标偏移=%lld freeSize=%d freeEntry=%d",
                 e, (long long)(ent->allocPtr - ent->beginPtr), ent->freeSize, w->pool->freeEntry[e]);
        w->nextOff[e] = 0;
    }
    check_entry(w, e, "释放后");
}

/* 压力控制：把池子维持在“活跃带”，既不长期空转也不长期饱和
 * （A 阶段的耗尽/恢复循环已经专门覆盖饱和形态） */
static int alloc_prob(long long liveBytes)
{
    long long cap = (long long)PER * ENTRIES;
    if (liveBytes > cap / 2) return 5;          /* 超过半容 → 基本只释放 */
    if (liveBytes > cap * 35 / 100) return 30;
    return 60;
}

static void churn(Worker* w, long ops)
{
    for (long i = 0; i < ops; i++) {
        int live = 0;
        for (int e = 0; e < ENTRIES; e++) live += w->liveCnt[e];
        if (live == 0 || rnd32(&w->seed) % 100 < (unsigned)alloc_prob(w->liveBytes)) do_alloc(w);
        else do_free(w);
    }
}

/* I8：全部归还后所有池复位 */
static void drain(Worker* w)
{
    int live;
    do {
        live = 0;
        for (int e = 0; e < ENTRIES; e++) live += w->liveCnt[e];
        if (live) do_free(w);
    } while (live);

    for (int e = 0; e < ENTRIES; e++) {
        Memory_Entry* ent = &w->pool->manageEntry[e];
        if (ent->allocPtr != ent->beginPtr || ent->freeSize != ent->totalSize ||
            ent->hadAllocNum != 0 || w->pool->freeEntry[e] != true)
            FAIL(w, "结束时第 %d 号池未复位: 游标偏移=%lld freeSize=%d 计数=%d freeEntry=%d",
                 e, (long long)(ent->allocPtr - ent->beginPtr), ent->freeSize,
                 ent->hadAllocNum, w->pool->freeEntry[e]);
    }
}

/* ---- local 模式：每线程独立池（与 c_web 的用法一致） ---- */
static void* worker_local(void* arg)
{
    Worker* w = (Worker*)arg;
    MemoryPoolInit(&w->pool, PER, ENTRIES);
    churn(w, w->opsTarget);
    drain(w);
    local_destroy(w->pool);
    return NULL;
}

/* ---- shared 模式：共享池 + 外部锁（0 = 不加锁，仅用于反例演示） ---- */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static Worker g_shared;
static int g_useLock = 1;

static void* worker_shared(void* arg)
{
    long ops = *(long*)arg;
    for (long i = 0; i < ops; i++) {
        if (g_useLock) pthread_mutex_lock(&g_lock);
        int live = 0;
        for (int e = 0; e < ENTRIES; e++) live += g_shared.liveCnt[e];
        if (live == 0 || rnd32(&g_shared.seed) % 100 < (unsigned)alloc_prob(g_shared.liveBytes)) do_alloc(&g_shared);
        else do_free(&g_shared);
        if (g_useLock) pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

/* ---------- A. 耗尽/恢复循环：验证“每轮容量一致、归零必复位” ---------- */
static long exhaust_cycles(long cycles, long* out_blocks)
{
    Memory_Pool* p = NULL;
    MemoryPoolInit(&p, 2048, 4);
    void* ptrs[4096];
    long bad = 0, expect = -1;

    double t0 = now_sec();
    for (long c = 0; c < cycles; c++) {
        int n = 0, nf = 0;
        for (;;) {
            void* q = NULL;
            MemoryPoolAlloc(p, 100, &q, &nf);
            if (nf != 1) break;
            if (n < 4096) ptrs[n++] = q;
        }
        if (expect < 0) expect = n;
        if (n != expect) {
            bad++;
            if (bad <= 5) printf("  [FAIL] 第 %ld 轮装下 %d 块，期望 %ld（容量漂移）\n", c, n, expect);
        }
        for (int e = 0; e < 4; e++) {                     /* I7 的极端形态：满池 */
            if (p->manageEntry[e].freeSize >= 104) {
                bad++;
                if (bad <= 5) printf("  [FAIL] notfull=0 但第 %d 号池还剩 %dB\n", e, p->manageEntry[e].freeSize);
            }
        }
        for (int i = 0; i < n; i++) MemoryPoolFree(p, ptrs[i]);
        for (int e = 0; e < 4; e++) {                     /* I6/I8 */
            Memory_Entry* ent = &p->manageEntry[e];
            if (ent->allocPtr != ent->beginPtr || ent->freeSize != ent->totalSize ||
                ent->hadAllocNum != 0 || p->freeEntry[e] != true) {
                bad++;
                if (bad <= 5)
                    printf("  [FAIL] 第 %ld 轮第 %d 号池未复位: 偏移=%lld freeSize=%d 计数=%d freeEntry=%d\n",
                           c, e, (long long)(ent->allocPtr - ent->beginPtr), ent->freeSize,
                           ent->hadAllocNum, p->freeEntry[e]);
            }
        }
    }
    printf("  每轮装下 %ld 块（%d 池 × 19 块 × (100+4)B），%ld 轮后容量无漂移，耗时 %.3f s\n",
           expect, 4, cycles, now_sec() - t0);
    *out_blocks = expect;
    local_destroy(p);
    return bad;
}

int main(int argc, char** argv)
{
    const char* mode = (argc > 1) ? argv[1] : "local";
    int threads = (argc > 2) ? atoi(argv[2]) : 4;
    long ops = (argc > 3) ? atol(argv[3]) : 1000000L;
    unsigned seed = (argc > 4) ? (unsigned)strtoul(argv[4], NULL, 10) : 20261016u;
    if (threads < 1) threads = 1;
    if (ops < 1) ops = 1;

    printf("== stress_memory_pool : 压力/性质测试（影子模型逐操作校验 8 条不变式）==\n");
    printf("模式 %s；线程 %d；每线程操作 %ld；seed %u；池配置 perSize=%dB × %d 池\n",
           mode, threads, ops, seed, PER, ENTRIES);

    long blocks = 0;
    printf("\n-- A. 耗尽/恢复循环（perSize=2048 × 4 池，每次 100B，2000 轮）--\n");
    long bad = exhaust_cycles(2000, &blocks);

    printf("\n-- B. 随机压力（%s）--\n", mode);
    double t0 = now_sec();
    long long totalOps = 0, totalFails = 0, totalErrors = 0, totalAllocs = 0;

    if (strcmp(mode, "local") == 0) {
        Worker* ws = calloc((size_t)threads, sizeof(Worker));
        pthread_t* th = calloc((size_t)threads, sizeof(pthread_t));
        if (!ws || !th) { printf("内存不足\n"); return 1; }
        for (int i = 0; i < threads; i++) {
            ws[i].tid = i;
            ws[i].seed = seed + 0x9E3779B9u * (unsigned)(i + 1);
            ws[i].opsTarget = ops;
            pthread_create(&th[i], NULL, worker_local, &ws[i]);
        }
        for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
        for (int i = 0; i < threads; i++) {
            printf("  线程 %d: 操作 %lld，分配 %lld，释放 %lld，池满降级 %lld，不变式错误 %ld\n",
                   i, ws[i].ops, ws[i].allocs, ws[i].frees, ws[i].fails, ws[i].errors);
            totalOps += ws[i].ops;
            totalFails += ws[i].fails;
            totalErrors += ws[i].errors;
            totalAllocs += ws[i].allocs;
        }
        free(ws);
        free(th);
    } else {
        g_useLock = (strcmp(mode, "shared-unlocked") != 0);
        MemoryPoolInit(&g_shared.pool, PER, ENTRIES);
        g_shared.seed = seed;
        g_shared.tid = -1;
        long per = ops;
        pthread_t* th = calloc((size_t)threads, sizeof(pthread_t));
        if (!th) { printf("内存不足\n"); return 1; }
        for (int i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker_shared, &per);
        for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
        printf("  共享池（%s）: 操作 %lld，分配 %lld，释放 %lld，池满降级 %lld，不变式错误 %ld\n",
               g_useLock ? "外部 pthread_mutex" : "故意不加锁（反例）",
               g_shared.ops, g_shared.allocs, g_shared.frees, g_shared.fails, g_shared.errors);
        drain(&g_shared);
        totalOps = g_shared.ops;
        totalFails = g_shared.fails;
        totalErrors = g_shared.errors;
        totalAllocs = g_shared.allocs;
        local_destroy(g_shared.pool);
        free(th);
    }

    double dt = now_sec() - t0;
    printf("  汇总: 操作 %lld，耗时 %.3f s，%.2f Mops/s，池满降级 %lld 次（占分配尝试 %.1f%%，这是设计中的 503 路径），不变式错误 %lld\n",
           totalOps, dt, (double)totalOps / dt / 1e6, totalFails,
           totalAllocs ? 100.0 * (double)totalFails / (double)totalAllocs : 0.0, totalErrors);

    if (bad || totalErrors) {
        printf("\n==== 压力测试失败（A 阶段异常 %ld 处，B 阶段不变式错误 %lld 处）====\n", bad, totalErrors);
        return 1;
    }
    printf("\n==== 压力测试通过：%lld 次操作 + %ld 轮耗尽/恢复，8 条不变式全部成立 ====\n",
           totalOps, 2000L);
    return 0;
}
