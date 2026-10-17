/*
 * bench_memory_pool.c —— CWebMemoryPool 性能基准
 *
 * 对照实现：glibc malloc/free（同机、同优化级别、同循环形态）
 * 编译运行： make -C test bench            （-O2）
 *           ./build/bench_memory_pool 2    （规模倍数，默认 1）
 *
 * 只测“分配/释放本身”，不掺网络与解析负载。所有结果都是同一进程内实测，可直接横向比。
 */
#define _POSIX_C_SOURCE 200809L
#include "MemoryPool.h"
#include <string.h>
#include <time.h>
#include <malloc.h>

#define GUARD 4      /* 与 MemoryPool.c 的 tailSize 一致 */
#define PER_BIG (1024 * 1024)
#define REPEATS 9    /* 每项重复次数，取最快一次（过滤 VM/调频噪声） */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 池没有 destroy 接口，本地释放内部结构，避免把池自身的内存算进结果 */
static void local_destroy(Memory_Pool* p)
{
    if (p == NULL) return;
    free(p->beginPtr);
    free(p->manageEntry);
    free(p->freeEntry);
    free(p);
}

static volatile unsigned long long g_sink; /* 兼容保留 */

/* 公平地“使用”指针：零指令的编译器屏障，避免 GCC 把 malloc/free 对整体消除，
 * 同时不像写 volatile 那样给对照侧引入额外开销 */
static inline void use_ptr(void* p)
{
    __asm__ __volatile__("" : : "r"(p) : "memory");
}

/*==========================================================================
 * 1. 单块 申请 + 释放 延迟（不触碰内存）
 *========================================================================*/
static void bench_single(const int* sizes, int nSizes, long iters)
{
    printf("\n== 1. 单块 申请+释放 延迟（不触碰内存，每项 %ld 次 × %d 次重复取最快）==\n",
           iters, REPEATS);
    printf("  %-6s | %-8s %12s %10s\n", "大小", "实现", "ns/op", "Mops/s");
    printf("  -------+--------------------------------\n");

    for (int s = 0; s < nSizes; s++) {
        int sz = sizes[s];
        double bestPool = 1e18, bestMalloc = 1e18;
        long bad = 0;
        Memory_Pool* pool = NULL;
        void* ptr = NULL;
        int notfull = 0;

        MemoryPoolInit(&pool, PER_BIG, 1);
        for (int rep = 0; rep < REPEATS; rep++) {
            double t0 = now_sec();
            for (long i = 0; i < iters; i++) {
                MemoryPoolAlloc(pool, sz, &ptr, &notfull);
                if (notfull != 1) bad++;
                MemoryPoolFree(pool, ptr);
            }
            double ns = (now_sec() - t0) / (double)iters * 1e9;
            if (ns < bestPool) bestPool = ns;
        }
        printf("  %5dB | %-8s %12.2f %10.2f%s\n", sz, "内存池", bestPool,
               1e3 / bestPool, bad ? "  <== 有分配失败!" : "");
        local_destroy(pool);

        for (int rep = 0; rep < REPEATS; rep++) {
            double t0 = now_sec();
            for (long i = 0; i < iters; i++) {
                void* m = malloc((size_t)sz);
                use_ptr(m);
                free(m);
            }
            double ns = (now_sec() - t0) / (double)iters * 1e9;
            if (ns < bestMalloc) bestMalloc = ns;
        }
        printf("  %5dB | %-8s %12.2f %10.2f   内存池/ malloc = %.2fx\n", sz, "malloc",
               bestMalloc, 1e3 / bestMalloc, bestMalloc / bestPool);
    }
}

/* 一组块（一次申请 K 块再全释放）—— 对应“一个请求拆成多个块”的申请路径 */
static void bench_group(int chunkSize, int chunks, int groups)
{
    printf("\n== 2. 一组块：一次申请 %d 个 %dB 块再全部释放（%d 组 × %d 次重复取最快）==\n",
           chunks, chunkSize, groups, REPEATS);
    long bad = 0;
    void* ptrs[256];
    int notfull = 0;

    Memory_Pool* pool = NULL;
    MemoryPoolInit(&pool, PER_BIG, 1);
    double bestPool = 1e18;
    for (int rep = 0; rep < REPEATS; rep++) {
        double t0 = now_sec();
        for (int g = 0; g < groups; g++) {
            for (int i = 0; i < chunks; i++) {
                ptrs[i] = NULL;
                MemoryPoolAlloc(pool, chunkSize, &ptrs[i], &notfull);
                if (notfull != 1) bad++;
            }
            for (int i = 0; i < chunks; i++) MemoryPoolFree(pool, ptrs[i]);
        }
        double ns = (now_sec() - t0) / (double)((long)groups * chunks) * 1e9;
        if (ns < bestPool) bestPool = ns;
    }
    printf("  内存池 : %7.2f ns/块  %7.2f M块/s  （%s）\n", bestPool, 1e3 / bestPool,
           bad ? "有失败" : "无失败");
    local_destroy(pool);

    double bestMalloc = 1e18;
    for (int rep = 0; rep < REPEATS; rep++) {
        double t0 = now_sec();
        for (int g = 0; g < groups; g++) {
            for (int i = 0; i < chunks; i++) { ptrs[i] = malloc((size_t)chunkSize); use_ptr(ptrs[i]); }
            for (int i = 0; i < chunks; i++) free(ptrs[i]);
        }
        double ns = (now_sec() - t0) / (double)((long)groups * chunks) * 1e9;
        if (ns < bestMalloc) bestMalloc = ns;
    }
    printf("  malloc : %7.2f ns/块  %7.2f M块/s\n", bestMalloc, 1e3 / bestMalloc);
}

/*==========================================================================
 * 3. 真实请求模式：每请求 chunks 个块，顺序申请 → 整请求释放
 *========================================================================*/
static void bench_request(int chunks, int chunkSize, long requests)
{
    printf("\n== 3. 真实请求模式：每请求 %d 个 %dB 块（顺序申请 → 整请求释放，%ld 个请求 × %d 次重复取最快）==\n",
           chunks, chunkSize, requests, REPEATS);
    long bad = 0, adj = 0, total = 0;
    void** ptrs = malloc(sizeof(void*) * (size_t)chunks);
    int notfull = 0;

    Memory_Pool* pool = NULL;
    MemoryPoolInit(&pool, PER_BIG, 4);
    double bestPool = 1e18;
    for (int rep = 0; rep < REPEATS; rep++) {
        double t0 = now_sec();
        for (long r = 0; r < requests; r++) {
            for (int i = 0; i < chunks; i++) {
                ptrs[i] = NULL;
                MemoryPoolAlloc(pool, chunkSize, &ptrs[i], &notfull);
                if (notfull != 1) bad++;
            }
            for (int i = 0; i < chunks; i++) {          /* 触碰内存，模拟真实写入 */
                if (ptrs[i] != NULL) {
                    ((char*)ptrs[i])[0] = (char)r;
                    ((char*)ptrs[i])[chunkSize - 1] = (char)r;
                }
            }
            for (int i = 1; i < chunks; i++) {          /* 相邻块统计（缓存友好度） */
                total++;
                if ((char*)ptrs[i] == (char*)ptrs[i - 1] + chunkSize + GUARD) adj++;
            }
            for (int i = 0; i < chunks; i++) MemoryPoolFree(pool, ptrs[i]);
        }
        double us = (now_sec() - t0) / (double)requests * 1e6;
        if (us < bestPool) bestPool = us;
    }
    printf("  内存池 : %7.2f us/请求  %7.2f 万请求/s  相邻块 %.1f%%  %s\n",
           bestPool, 1e6 / bestPool / 1e4,
           100.0 * (double)adj / (double)total, bad ? "<== 有分配失败" : "");
    local_destroy(pool);

    long adjm = 0, totalm = 0;
    double bestMalloc = 1e18;
    for (int rep = 0; rep < REPEATS; rep++) {
        double t0 = now_sec();
        for (long r = 0; r < requests; r++) {
            for (int i = 0; i < chunks; i++) { ptrs[i] = malloc((size_t)chunkSize); use_ptr(ptrs[i]); }
            for (int i = 0; i < chunks; i++) {
                ((char*)ptrs[i])[0] = (char)r;
                ((char*)ptrs[i])[chunkSize - 1] = (char)r;
            }
            for (int i = 1; i < chunks; i++) {
                totalm++;
                if ((char*)ptrs[i] == (char*)ptrs[i - 1] + (long)malloc_usable_size(ptrs[i - 1])) adjm++;
            }
            for (int i = 0; i < chunks; i++) free(ptrs[i]);
        }
        double us = (now_sec() - t0) / (double)requests * 1e6;
        if (us < bestMalloc) bestMalloc = us;
    }
    printf("  malloc : %7.2f us/请求  %7.2f 万请求/s  相邻块 %.1f%%\n",
           bestMalloc, 1e6 / bestMalloc / 1e4, 100.0 * (double)adjm / (double)totalm);
    free(ptrs);
}

/*==========================================================================
 * 4. “残量池”扫描代价：池子还有空间但装不下当前请求时，每次申请都会被扫到
 *========================================================================*/
static void bench_scan(void)
{
    const int PS = 2048;   /* 小池子大小 */
    const int N = 256;     /* 池个数 */
    const int REQ = 512;   /* 请求大小：3*516=1548 后剩 500 < 516，所以残量池装不下 */
    const long ITERS = 2000000L;

    printf("\n== 4. 残量池扫描代价（perSize=%dB，请求 %dB，%ld 次申请+释放 × %d 次重复取最快）==\n",
           PS, REQ, ITERS, REPEATS);
    int nf = 0;

    /* A) 只有 1 个池：每次申请只检查 1 个池 */
    Memory_Pool* a = NULL;
    MemoryPoolInit(&a, PS, 1);
    void* pa = NULL;
    double nsA = 1e18;
    for (int rep = 0; rep < REPEATS; rep++) {
        double t0 = now_sec();
        for (long i = 0; i < ITERS; i++) {
            MemoryPoolAlloc(a, REQ, &pa, &nf);
            MemoryPoolFree(a, pa);
        }
        double ns = (now_sec() - t0) / (double)ITERS * 1e9;
        if (ns < nsA) nsA = ns;
    }
    printf("  A) 1 个池   （每次扫 1 个池）  : %7.2f ns/op\n", nsA);
    local_destroy(a);

    /* B) N 个池：前 N-1 个都剩 500B（装不下 512B），每次申请必须扫完所有池 */
    Memory_Pool* b = NULL;
    MemoryPoolInit(&b, PS, N);
    void* filler[3 * (N - 1)];
    for (int i = 0; i < 3 * (N - 1); i++) {
        filler[i] = NULL;
        MemoryPoolAlloc(b, REQ, &filler[i], &nf);
    }
    printf("  前置状态：第 0 号池剩 %dB，第 %d 号池剩 %dB（都放不下 %d+%dB 的请求）\n",
           b->manageEntry[0].freeSize, N - 1, b->manageEntry[N - 1].freeSize, REQ, GUARD);
    void* pb = NULL;
    long fails = 0;
    double nsB = 1e18;
    for (int rep = 0; rep < REPEATS; rep++) {
        double t0 = now_sec();
        for (long i = 0; i < ITERS; i++) {
            MemoryPoolAlloc(b, REQ, &pb, &nf);
            if (nf != 1) fails++;
            MemoryPoolFree(b, pb);
        }
        double ns = (now_sec() - t0) / (double)ITERS * 1e9;
        if (ns < nsB) nsB = ns;
    }
    printf("  B) %d 个池（每次扫 %d 个池）: %7.2f ns/op  （失败 %ld 次）\n", N, N, nsB, fails);
    printf("  → 每多扫一个池约 %.2f ns；B 是 A 的 %.1f 倍\n",
           (nsB - nsA) / (double)(N - 1), nsB / nsA);
    for (int i = 0; i < 3 * (N - 1); i++) MemoryPoolFree(b, filler[i]);
    local_destroy(b);
}

/*==========================================================================
 * 5. 内存占用：同样 N 块 sz 字节数据
 *========================================================================*/
static void bench_footprint(int n, int sz)
{
    printf("\n== 5. 内存占用（%d 块 %dB 数据）==\n", n, sz);
    void** ptrs = malloc(sizeof(void*) * (size_t)n);
    int notfull = 0;
    long bad = 0;
    int perSize = (int)((long)n * (sz + GUARD) + 64);   /* 只申请刚好够用的整块 */

    Memory_Pool* pool = NULL;
    MemoryPoolInit(&pool, perSize, 1);
    for (int i = 0; i < n; i++) {
        ptrs[i] = NULL;
        MemoryPoolAlloc(pool, sz, &ptrs[i], &notfull);
        if (notfull != 1) bad++;
    }
    long poolData = (long)n * (sz + GUARD);
    printf("  内存池 : 数据区 %ld B = %.1f B/块（%dB 数据 + %dB 护栏）；为 %d 块预留 %d B\n",
           poolData, (double)poolData / n, sz, GUARD, n, perSize);
    if (bad) printf("           <== 有 %ld 次分配失败\n", bad);
    local_destroy(pool);

    long usable = 0;
    for (int i = 0; i < n; i++) {
        ptrs[i] = malloc((size_t)sz);
        usable += (long)malloc_usable_size(ptrs[i]);
    }
    printf("  malloc : 数据区 %ld B = %.1f B/块（含头部 + 16B 对齐开销）\n",
           usable, (double)usable / n);
    for (int i = 0; i < n; i++) free(ptrs[i]);
    free(ptrs);
}

int main(int argc, char** argv)
{
    long mul = (argc > 1) ? atol(argv[1]) : 1;
    if (mul < 1) mul = 1;

    printf("== bench_memory_pool : CWebMemoryPool 性能基准（-O2，同进程对照 glibc malloc/free）==\n");
    printf("规模倍数 %ld；池与被测代码分属不同编译单元，调用开销不外inline，两侧公平\n", mul);

    static const int sizes[] = {32, 64, 256, 1024, 4096};
    bench_single(sizes, 5, 2000000L * mul);
    bench_group(64, 8, (int)(1000000L * mul));
    bench_request(40, 64, 200000L * mul);
    bench_scan();
    static const int footSizes[] = {16, 64, 256, 1024};
    for (int i = 0; i < 4; i++) bench_footprint(10000, footSizes[i]);

    printf("\n读表提示：内存池的分配/释放是 O(1) bump+计数，成本与“池内残量”相关（第 4 项）；\n"
           "          malloc 的成本与 tcache/bins 状态相关，块越小相对劣势越明显（第 5 项）。\n");
    return 0;
}
