/*
 * bench_other_pool.c —— 老池子（otherPool = cWeb 现在列装的那套）与本仓库新池子的同进程性能对比
 *
 * 老池子的实现要点（otherPool/MemoryPool/MemoryPool.c）：
 *   - 按 1KB/2KB/4KB/.../2^(n-1)KB 分级，每级一块连续内存切成若干固定大小的"条"；
 *   - 每级一个空闲栈（LIFO）**每级一把 pthread_mutex**，分配 = 弹栈、释放 = 压栈；
 *   - 每级只有 init_strip_num 条，用完就 notfull=0；超过最大级 → malloc 兜底（notfull 仍为 1）。
 * 新池子的实现要点（本仓库 MemoryPool.c）：
 *   - 连续大块切成 entryNum 个 perSize 的子池，池内 bump 追加；
 *   - 每个子池维护在用计数，计数归零才整体回收；无锁（每线程/每连接一个池）。
 *
 * 两个池子的同名符号用编译期 -D 重命名区分（见 test/Makefile 的 bench-other），
 * 老池子源码未做任何修改。
 *
 * 编译运行： make -C test bench-other
 */
#define _POSIX_C_SOURCE 200809L
#include "MemoryPool.h"          /* 新池 */
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <pthread.h>

/* ---- 老池子的 void* 接口（test/src/old_pool_shim.c，编译两遍：有锁 / 去锁） ---- */
int  OldPoolCreate(void** pp,int max_num,int init_max,int strip_num_future,int init_strip_num);
int  OldPoolAllocX(void* p,int sizeKB,void** ptr,int* notfull);
int  OldPoolFreeX(void* p,void* ptr,int sizeBytes);
void OldPoolInfo(void* p,int i,int* stripSizeKB,int* stripNum,int* hadedNum);
int  OldPoolClasses(void* p);
void* OldPoolBase(void* p,int i);
void OldPoolDestroy(void* p);

/* 去锁变体（编译时用 -include noop_mutex.h 把 lock/unlock 变成空操作；仅测量用） */
int  OldNoLockPoolCreate(void** pp,int max_num,int init_max,int strip_num_future,int init_strip_num);
int  OldNoLockPoolAllocX(void* p,int sizeKB,void** ptr,int* notfull);
int  OldNoLockPoolFreeX(void* p,void* ptr,int sizeBytes);
void OldNoLockPoolInfo(void* p,int i,int* stripSizeKB,int* stripNum,int* hadedNum);
int  OldNoLockPoolClasses(void* p);
void* OldNoLockPoolBase(void* p,int i);
void OldNoLockPoolDestroy(void* p);

#define REPEATS 9
#define GUARD 4

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void local_destroy(Memory_Pool* p)
{
    if(p == NULL) return;
    free(p->beginPtr);
    free(p->manageEntry);
    free(p->freeEntry);
    free(p);
}

static inline void use_ptr(void* p) { __asm__ __volatile__("" : : "r"(p) : "memory"); }

/*------------------------------------------------------------------
 * 统一封装：一个 PoolRef 可以指向 老池(有锁) / 老池(去锁) / 新池
 * 老池的 alloc 参数是 KB（向上取整到某级），free 参数是字节；新池都是字节。
 *----------------------------------------------------------------*/
typedef struct { const char* name; void* h; int kind; } PoolRef;   /* kind: 0=老池(锁) 1=老池(去锁) 2=新池 */

static inline int pr_alloc(PoolRef* r,int bytes,void** ptr,int* nf)
{
    *ptr = NULL;
    if(r->kind == 0) return OldPoolAllocX(r->h,(bytes + 1023) / 1024,ptr,nf);
    if(r->kind == 1) return OldNoLockPoolAllocX(r->h,(bytes + 1023) / 1024,ptr,nf);
    return MemoryPoolAlloc((Memory_Pool*)r->h,bytes,ptr,nf);
}

static inline void pr_free(PoolRef* r,void* ptr,int bytes)
{
    if(r->kind == 0){ OldPoolFreeX(r->h,ptr,bytes); return; }
    if(r->kind == 1){ OldNoLockPoolFreeX(r->h,ptr,bytes); return; }
    MemoryPoolFree((Memory_Pool*)r->h,ptr);
}

static void pr_destroy(PoolRef* r)
{
    if(r->kind == 0) OldPoolDestroy(r->h);
    else if(r->kind == 1) OldNoLockPoolDestroy(r->h);
    else local_destroy((Memory_Pool*)r->h);
}

/* 老池：1KB~64KB 七级，每级 strips 条（cWeb 的 txt_pool 是 120 条）
 * 注意：老池的 strip_num_future 决定空闲栈容量(strip_num_future+50)，
 * 必须 >= strips，否则它自己的栈会被写爆（ASAN 实测），所以这里一起传 strips */
static void mk_old_locked(PoolRef* r,int strips)
{ r->name = "老池(有锁)"; r->kind = 0; r->h = NULL; OldPoolCreate(&r->h,7,7,strips,strips); }

static void mk_old_nolock(PoolRef* r,int strips)
{ r->name = "老池(去锁)"; r->kind = 1; r->h = NULL; OldNoLockPoolCreate(&r->h,7,7,strips,strips); }

/* 新池：perSize 字节 × entries 个池 */
static void mk_new(PoolRef* r,int perSizeBytes,int entries)
{ r->name = "新池"; r->kind = 2; r->h = NULL; MemoryPoolInit((Memory_Pool**)&r->h,perSizeBytes,entries); }

/*==========================================================================
 * A. 单块 申请+释放：按“同一逻辑块大小”对比（老池按 KB 分级，1KB 是它最小条）
 *========================================================================*/
static void case_single(void)
{
    const long ITERS = 2000000L;
    struct Row { const char* name; int bytes; } rows[] = {
        {"64B (老池占 1KB 条)", 64},
        {"1KB",                 1024},
        {"4KB",                 4096},
        {"16KB",                16384},
        {"64KB",                65536},
    };

    printf("\n== A. 单块 申请+释放 延迟（不触碰内存，每项 %ld 次 × %d 次重复取最快）==\n", ITERS, REPEATS);
    printf("  %-18s | %-12s %9s | %-12s %9s | %-12s %9s | 新池/老池(锁)\n",
           "逻辑块大小","老池(有锁)","ns/op","老池(去锁)","ns/op","新池","ns/op");

    for(int r = 0;r < 5;r++){
        PoolRef refs[3];
        double best[3] = {1e18,1e18,1e18};
        long fail[3] = {0,0,0};

        mk_old_locked(&refs[0],2);
        mk_old_nolock(&refs[1],2);
        mk_new(&refs[2],1024 * 1024,1);

        for(int k = 0;k < 3;k++){
            void* ptr = NULL;
            int nf = 0;
            for(int rep = 0;rep < REPEATS;rep++){
                double t0 = now_sec();
                for(long i = 0;i < ITERS;i++){
                    pr_alloc(&refs[k],rows[r].bytes,&ptr,&nf);
                    if(nf != 1) fail[k]++;
                    pr_free(&refs[k],ptr,rows[r].bytes);
                }
                double ns = (now_sec() - t0) / (double)ITERS * 1e9;
                if(ns < best[k]) best[k] = ns;
            }
        }

        printf("  %-18s | %-12s %9.2f | %-12s %9.2f | %-12s %9.2f | %.2fx%s\n",
               rows[r].name,refs[0].name,best[0],refs[1].name,best[1],refs[2].name,best[2],
               best[0] / best[2],
               (fail[0] || fail[1] || fail[2]) ? "  <== 有分配失败!" : "");
        for(int k = 0;k < 3;k++) pr_destroy(&refs[k]);
    }
}

/*==========================================================================
 * B. 一个请求拆 40 个 64B 块（一起申请 → 一起释放）
 *    老池用 cWeb 的 txt_pool 真实配置 (7,7,200,120)；新池 perSize=1MB × 4
 *========================================================================*/
static void case_request(void)
{
    const int chunks = 40;
    const int chunk = 64;
    const long requests = 200000L;
    void* p[256];
    int nf = 0;

    printf("\n== B. 一个请求拆 %d 个 %dB 块（一起申请 → 一起释放，%ld 个请求 × %d 次重复取最快）==\n",
           chunks,chunk,requests,REPEATS);

    /* ---- 老池 ---- */
    void* op = NULL;
    OldPoolCreate(&op,7,7,200,120);
    double bestOld = 1e18;
    long adjOld = 0,totOld = 0,failOld = 0;
    for(int rep = 0;rep < REPEATS;rep++){
        double t0 = now_sec();
        for(long r = 0;r < requests;r++){
            for(int i = 0;i < chunks;i++){
                p[i] = NULL;
                OldPoolAllocX(op,1,&p[i],&nf);
                if(nf != 1) failOld++;
            }
            for(int i = 1;i < chunks;i++){
                long d = (char*)p[i] - (char*)p[i - 1];
                totOld++;
                if(d == 1024 || d == -1024) adjOld++;   /* LIFO 弹栈：条也是连续的（方向相反） */
            }
            for(int i = 0;i < chunks;i++) OldPoolFreeX(op,p[i],1024);
        }
        double ns = (now_sec() - t0) / ((double)requests * chunks) * 1e9;
        if(ns < bestOld) bestOld = ns;
    }
    printf("  老池(分级+栈+锁): %6.2f ns/块  %8.1f 万块/s  连续块 %5.1f%%  每请求占 %ld B（%d×1KB 条）%s\n",
           bestOld,1e5 / bestOld,100.0 * (double)adjOld / (double)totOld,
           (long)chunks * 1024,chunks,failOld ? "  <== 有分配失败!" : "");
    OldPoolDestroy(op);

    /* ---- 新池 ---- */
    Memory_Pool* np = NULL;
    MemoryPoolInit(&np,1024 * 1024,4);
    double bestNew = 1e18;
    long adjNew = 0,totNew = 0,failNew = 0;
    for(int rep = 0;rep < REPEATS;rep++){
        double t0 = now_sec();
        for(long r = 0;r < requests;r++){
            for(int i = 0;i < chunks;i++){
                p[i] = NULL;
                MemoryPoolAlloc(np,chunk,&p[i],&nf);
                if(nf != 1) failNew++;
            }
            for(int i = 1;i < chunks;i++){
                long d = (char*)p[i] - (char*)p[i - 1];
                totNew++;
                if(d == chunk + GUARD) adjNew++;       /* bump 追加：整段连续 */
            }
            for(int i = 0;i < chunks;i++) MemoryPoolFree(np,p[i]);
        }
        double ns = (now_sec() - t0) / ((double)requests * chunks) * 1e9;
        if(ns < bestNew) bestNew = ns;
    }
    printf("  新池(bump+计数)  : %6.2f ns/块  %8.1f 万块/s  连续块 %5.1f%%  每请求占 %ld B（%d×%dB）%s\n",
           bestNew,1e5 / bestNew,100.0 * (double)adjNew / (double)totNew,
           (long)chunks * (chunk + GUARD),chunks,chunk + GUARD,failNew ? "  <== 有分配失败!" : "");
    printf("  → 块吞吐 %.2f 倍；每请求内存 %ld B → %ld B（省 %.1f 倍）\n",
           bestOld / bestNew,(long)chunks * 1024,(long)chunks * (chunk + GUARD),
           (double)(chunks * 1024) / (double)(chunks * (chunk + GUARD)));
    local_destroy(np);
}

/*==========================================================================
 * C. cWeb 的真实请求形态：63KB 响应头 + 70KB 打包区（>64KB）+ 4 个 1KB 解析缓冲
 *    老池：64KB 是最大级，70KB 那笔会走 malloc 兜底（线上行为就是这样）
 *    新池：perSize=1MB × 8，全在池内
 *========================================================================*/
static void case_cweb_shape(void)
{
    const long requests = 200000L;
    const int headerKB = 63,packKB = 70,smalls = 4;
    void* p[8];
    int nf = 0;

    printf("\n== C. cWeb 真实请求形态：每请求 [%dKB 头 + %dKB 打包 + %d×1KB 解析]（%ld 个请求）==\n",
           headerKB,packKB,smalls,requests);

    void* op = NULL;
    OldPoolCreate(&op,7,7,200,120);              /* txt_pool 的线上配置 */
    double bestOld = 1e18;
    long fallback = 0;
    for(int rep = 0;rep < REPEATS;rep++){
        double t0 = now_sec();
        for(long r = 0;r < requests;r++){
            OldPoolAllocX(op,headerKB,&p[0],&nf);          /* → 64KB 类 */
            OldPoolAllocX(op,packKB,&p[1],&nf);            /* >64KB → malloc 兜底 */
            if(nf == 1 && p[1]) fallback++;
            for(int i = 0;i < smalls;i++) OldPoolAllocX(op,1,&p[2 + i],&nf);
            OldPoolFreeX(op,p[0],headerKB * 1024);
            OldPoolFreeX(op,p[1],packKB * 1024);           /* 走 free() 兜底 */
            for(int i = 0;i < smalls;i++) OldPoolFreeX(op,p[2 + i],1024);
        }
        double us = (now_sec() - t0) / (double)requests * 1e6;
        if(us < bestOld) bestOld = us;
    }
    printf("  老池(分级+栈+锁): %6.3f us/请求  %8.2f 万请求/s（其中 %ld 次走 malloc 兜底）\n",
           bestOld,1e6 / bestOld / 1e4,fallback);
    OldPoolDestroy(op);

    Memory_Pool* np = NULL;
    MemoryPoolInit(&np,1024 * 1024,8);
    double bestNew = 1e18;
    long poolFail = 0;
    for(int rep = 0;rep < REPEATS;rep++){
        double t0 = now_sec();
        for(long r = 0;r < requests;r++){
            MemoryPoolAlloc(np,headerKB * 1024,&p[0],&nf);
            if(nf != 1) poolFail++;
            MemoryPoolAlloc(np,packKB * 1024,&p[1],&nf);
            if(nf != 1) poolFail++;
            for(int i = 0;i < smalls;i++){
                MemoryPoolAlloc(np,1024,&p[2 + i],&nf);
                if(nf != 1) poolFail++;
            }
            for(int i = 0;i < 2 + smalls;i++) MemoryPoolFree(np,p[i]);
        }
        double us = (now_sec() - t0) / (double)requests * 1e6;
        if(us < bestNew) bestNew = us;
    }
    printf("  新池(bump+计数)  : %6.3f us/请求  %8.2f 万请求/s（池内放不下 %ld 次）\n",
           bestNew,1e6 / bestNew / 1e4,poolFail);
    printf("  → 新池是 %.2f 倍；注意老池那笔 70KB 请求根本没进池（malloc/free）\n",bestOld / bestNew);
    local_destroy(np);
}


/*==========================================================================
 * D. 内存占用：同样 1000 个 64B 逻辑块
 *========================================================================*/
static void case_footprint(void)
{
    const int n = 1000;
    void* p[1000];
    int nf = 0;

    printf("\n== D. 内存占用：%d 个 64B 逻辑块 ==\n",n);

    void* op = NULL;
    OldPoolCreate(&op,3,1,20000,n);        /* 只有 1KB 级，n 条 */
    long ok = 0;
    for(int i = 0;i < n;i++){ p[i] = NULL; OldPoolAllocX(op,1,&p[i],&nf); if(nf == 1 && p[i]) ok++; }
    int ss = 0,sn = 0,hd = 0;
    OldPoolInfo(op,0,&ss,&sn,&hd);
    printf("  老池: 1KB 级预分配 %d 条 = %ld B（%.0f B/块；64B 逻辑块只用到 1KB 条的 %.1f%%），成功 %ld/%d\n",
           sn,(long)sn * 1024,(double)sn * 1024 / n,100.0 * 64 / 1024,ok,n);
    for(int i = 0;i < n;i++) OldPoolFreeX(op,p[i],1024);
    OldPoolDestroy(op);

    int perSize = n * (64 + GUARD) + 4;    /* 把整块调到刚好够，只比“同样数据占多少” */
    Memory_Pool* np = NULL;
    MemoryPoolInit(&np,perSize,1);
    long okNew = 0;
    for(int i = 0;i < n;i++){ p[i] = NULL; MemoryPoolAlloc(np,64,&p[i],&nf); if(nf == 1 && p[i]) okNew++; }
    printf("  新池: 数据区 %ld B（%dB/块 = 64B + %dB 护栏），整块按 perSize=%d 预留，成功 %ld/%d\n",
           (long)n * (64 + GUARD),64 + GUARD,GUARD,perSize,okNew,n);
    for(int i = 0;i < n;i++) MemoryPoolFree(np,p[i]);
    local_destroy(np);

    printf("  → 同样 1000 块 64B 数据：老池 %.0f KB（按 1KB 条算）vs 新池 %.1f KB（68B/块）= 老池 %.1f 倍\n",
           (double)n * 1024 / 1024,(double)n * (64 + GUARD) / 1024,
           (double)(n * 1024) / (double)(n * (64 + GUARD)));
    printf("    反过来看 1KB 级块：老池 1024 B/块 vs 新池 1028 B/块（新池多 4B 护栏，基本持平）\n");
}

/*==========================================================================
 * E. 多线程：老池共享一个（内部 mutex） vs 新池每线程一个（无锁）
 *========================================================================*/
typedef struct { void* pool; long ops; double ns; } ThArg;

static void* th_old(void* a)
{
    ThArg* t = (ThArg*)a;
    void* ptr = NULL;
    int nf = 0;
    double t0 = now_sec();
    for(long i = 0;i < t->ops;i++){
        ptr = NULL;
        OldPoolAllocX(t->pool,1,&ptr,&nf);
        OldPoolFreeX(t->pool,ptr,1024);
    }
    t->ns = now_sec() - t0;
    return NULL;
}

static void* th_new(void* a)
{
    ThArg* t = (ThArg*)a;
    void* ptr = NULL;
    int nf = 0;
    Memory_Pool* np = NULL;
    MemoryPoolInit(&np,1024 * 1024,1);      /* 池的创建不计入计时 */
    double t0 = now_sec();
    for(long i = 0;i < t->ops;i++){
        ptr = NULL;
        MemoryPoolAlloc(np,1024,&ptr,&nf);
        MemoryPoolFree(np,ptr);
    }
    t->ns = now_sec() - t0;
    local_destroy(np);
    return NULL;
}

static void case_threads(void)
{
    const int T = 4;
    const long OPS = 1000000L;
    pthread_t th[4];
    ThArg args[4];
    double t0,el;

    printf("\n== E. 多线程（%d 线程 × %ld 次 1KB 申请+释放）==\n",T,OPS);

    void* op = NULL;
    OldPoolCreate(&op,7,7,200,120);
    t0 = now_sec();
    for(int i = 0;i < T;i++){
        args[i].pool = op;
        args[i].ops = OPS;
        pthread_create(&th[i],NULL,th_old,&args[i]);
    }
    for(int i = 0;i < T;i++) pthread_join(th[i],NULL);
    el = now_sec() - t0;
    printf("  老池(共享一个 + 每级内部 mutex): %6.2f Mops/s（总），每线程平均 %6.2f ns/op\n",
           (double)(T * OPS) / el / 1e6,(args[0].ns + args[1].ns + args[2].ns + args[3].ns) / T / (double)OPS * 1e9);
    OldPoolDestroy(op);

    t0 = now_sec();
    for(int i = 0;i < T;i++){
        args[i].pool = NULL;
        args[i].ops = OPS;
        pthread_create(&th[i],NULL,th_new,&args[i]);
    }
    for(int i = 0;i < T;i++) pthread_join(th[i],NULL);
    el = now_sec() - t0;
    printf("  新池(每线程一个, 无锁)        : %6.2f Mops/s（总），每线程平均 %6.2f ns/op\n",
           (double)(T * OPS) / el / 1e6,(args[0].ns + args[1].ns + args[2].ns + args[3].ns) / T / (double)OPS * 1e9);
    printf("  → 这是 2 核 VM，4 线程本身就有排队；重点看老池的 mutex 抢占开销与两者的量级差\n");
}

/*==========================================================================
 * F. 参考测量：老池每级那把 mutex 的成本（用来解释老池的固定开销）
 *========================================================================*/
static void case_mutex_cost(void)
{
    const long ITERS = 20000000L;
    pthread_mutex_t m;
    pthread_mutex_init(&m,NULL);
    double best = 1e18;
    for(int rep = 0;rep < REPEATS;rep++){
        double t0 = now_sec();
        for(long i = 0;i < ITERS;i++){
            pthread_mutex_lock(&m);
            pthread_mutex_unlock(&m);
        }
        double ns = (now_sec() - t0) / (double)ITERS * 1e9;
        if(ns < best) best = ns;
    }
    printf("\n== F. 参考：一次 pthread_mutex lock+unlock（无竞争，%ld 次 × %d 次重复取最快）==\n",
           ITERS,REPEATS);
    printf("  %.2f ns —— 老池每次 alloc 和每次 free 各做一次，所以这对成本是它的固定开销之一\n",best);
    pthread_mutex_destroy(&m);
}

/*==========================================================================
 * G. 高压·同预算（约 15MB）下的容量上限：一直申请直到 notfull=0
 *========================================================================*/
static void case_capacity(void)
{
    printf("\n== G. 高压·同预算（约 15MB）下的容量上限：一直申请直到 notfull=0 ==\n");
    printf("  %-24s | %-6s %14s   %s\n","池/配置","请求","并发块数上限","备注");

    struct { const char* label; int bytes; } rows[] = {{"1KB",1024},{"64KB",65536}};
    void** ptrs = malloc(sizeof(void*) * 400000);

    for(int r = 0;r < 2;r++){
        int bytes = rows[r].bytes;
        PoolRef a;
        long n = 0;
        void* ptr = NULL;
        int nf = 0;

        mk_old_locked(&a,120);                       /* cWeb txt_pool：7 级 × 120 条 ≈ 15.24MB */
        while(n < 400000){
            pr_alloc(&a,bytes,&ptr,&nf);
            if(nf != 1) break;
            ptrs[n++] = ptr;
        }
        printf("  %-24s | %-6s %14ld   预分配 15.24MB；1KB 级只占其中 120KB\n",
               "老池 (7,7,200,120)",rows[r].label,n);
        for(long i = 0;i < n;i++) pr_free(&a,ptrs[i],bytes);
        pr_destroy(&a);

        if(bytes == 1024){                           /* cWeb worker http_pool：每级只有 10 条 */
            PoolRef a10;
            a10.name = "老池"; a10.kind = 0; a10.h = NULL;
            OldPoolCreate(&a10.h,6,6,10,10);
            n = 0;
            while(n < 400000){
                pr_alloc(&a10,bytes,&ptr,&nf);
                if(nf != 1) break;
                ptrs[n++] = ptr;
            }
            printf("  %-24s | %-6s %14ld   预分配 0.63MB，每级 10 条 ← 线上 http_pool\n",
                   "老池 (6,6,10,10)",rows[r].label,n);
            for(long i = 0;i < n;i++) pr_free(&a10,ptrs[i],bytes);
            OldPoolDestroy(a10.h);
        }

        PoolRef b;
        mk_new(&b,15 * 1024 * 1024,1);               /* 同样 ~15MB 预算 */
        b.name = "新池";
        n = 0;
        while(n < 400000){
            pr_alloc(&b,bytes,&ptr,&nf);
            if(nf != 1) break;
            ptrs[n++] = ptr;
        }
        printf("  %-24s | %-6s %14ld   预分配 15MB（单池上限 = perSize-4）\n",
               "新池 (15MB × 1 池)",rows[r].label,n);
        for(long i = 0;i < n;i++) pr_free(&b,ptrs[i],bytes);
        pr_destroy(&b);
    }
    free(ptrs);
}

/*==========================================================================
 * H. 高压·随机交错释放（长跑）：峰值并发块数 / 降级率 / 吞吐
 *========================================================================*/
static unsigned rnd_next(unsigned* s)
{
    unsigned x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return (*s = x);
}

static void case_random_pressure(void)
{
    const long OPS = 5000000L;
    const long MAXLIVE = 400000L;
    printf("\n== H. 高压·随机交错释放（同预算 ~15MB，请求 64B~1KB 随机，各 %ld 次操作）==\n",OPS);
    printf("  规则：60%% 概率申请、40%% 释放（随机挑一个在用块），最后全部释放\n");
    printf("  %-14s | %12s %12s %9s %14s\n","池","峰值并发块","降级次数","降级率","吞吐(万ops/s)");

    PoolRef refs[3];
    mk_old_locked(&refs[0],120);
    mk_old_nolock(&refs[1],120);
    mk_new(&refs[2],15 * 1024 * 1024,1);
    refs[2].name = "新池";

    void** ptrs = malloc(sizeof(void*) * MAXLIVE);
    int* szs = malloc(sizeof(int) * MAXLIVE);

    for(int k = 0;k < 3;k++){
        long live = 0,peak = 0,fails = 0;
        unsigned seed = 20261016u;
        double t0 = now_sec();
        for(long i = 0;i < OPS;i++){
            unsigned r = rnd_next(&seed);
            if(live == 0 || (r % 100) < 60){
                int bytes = 64 + (int)(rnd_next(&seed) % 961);      /* 64..1024B */
                void* ptr = NULL;
                int nf = 0;
                pr_alloc(&refs[k],bytes,&ptr,&nf);
                if(nf != 1) fails++;
                else { ptrs[live] = ptr; szs[live] = bytes; live++; if(live > peak) peak = live; }
            } else {
                long j = (long)(rnd_next(&seed) % (unsigned)live);
                pr_free(&refs[k],ptrs[j],szs[j]);
                ptrs[j] = ptrs[live - 1];
                szs[j]  = szs[live - 1];
                live--;
            }
        }
        double el = now_sec() - t0;
        long left = live;
        for(long i = 0;i < live;i++) pr_free(&refs[k],ptrs[i],szs[i]);
        printf("  %-14s | %12ld %12ld %8.1f%% %14.1f%s\n",
               refs[k].name,peak,fails,100.0 * (double)fails / ((double)OPS * 0.6),
               (double)OPS / el / 1e4,left ? "  (结束后仍有在用块)" : "");
    }
    for(int k = 0;k < 3;k++) pr_destroy(&refs[k]);
    free(ptrs);
    free(szs);
}

/*==========================================================================
 * I. 多线程：老池共享(有锁) / 老池共享(去锁，演示竞争后果) / 新池每线程一个
 *========================================================================*/
#define NL_STRIPS 120
static unsigned char g_inuse[NL_STRIPS];
static unsigned long g_dup = 0;

static void* th_old_shared(void* a)
{
    void* ptr = NULL;
    int nf = 0;
    for(long i = 0;i < 1000000L;i++){
        ptr = NULL;
        OldPoolAllocX(a,1,&ptr,&nf);
        OldPoolFreeX(a,ptr,1024);
    }
    return NULL;
}

static void* th_old_nolock_shared(void* a)
{
    char* base = (char*)OldNoLockPoolBase(a,0);       /* 1KB 级基址 */
    void* ptr = NULL;
    int nf = 0;
    for(long i = 0;i < 1000000L;i++){
        ptr = NULL;
        OldNoLockPoolAllocX(a,1,&ptr,&nf);
        if(nf == 1 && ptr){
            long s = ((char*)ptr - base) / 1024;      /* 第几条 */
            if(s >= 0 && s < NL_STRIPS){
                if(g_inuse[s] != 0) g_dup++;          /* 同一条同时被发出两份 = 数据竞争 */
                g_inuse[s]++;
            }
            OldNoLockPoolFreeX(a,ptr,1024);
            if(s >= 0 && s < NL_STRIPS) g_inuse[s]--;
        }
    }
    return NULL;
}

static void* th_new_own(void* a)
{
    (void)a;
    void* ptr = NULL;
    int nf = 0;
    Memory_Pool* np = NULL;
    MemoryPoolInit(&np,1024 * 1024,1);
    for(long i = 0;i < 1000000L;i++){
        ptr = NULL;
        MemoryPoolAlloc(np,1024,&ptr,&nf);
        MemoryPoolFree(np,ptr);
    }
    local_destroy(np);
    return NULL;
}

static void case_threads3(void)
{
    const int T = 4;
    const long OPS = 1000000L;
    pthread_t th[4];
    double t0,el;

    printf("\n== I. 多线程（%d 线程 × %ld 次 1KB 申请+释放，2 核 VM）==\n",T,OPS);

    void* op = NULL;
    OldPoolCreate(&op,7,7,200,120);
    t0 = now_sec();
    for(int i = 0;i < T;i++) pthread_create(&th[i],NULL,th_old_shared,op);
    for(int i = 0;i < T;i++) pthread_join(th[i],NULL);
    el = now_sec() - t0;
    printf("  老池(共享 + mutex) : %7.2f Mops/s 总，%6.2f ns/op 每线程\n",
           (double)(T * OPS) / el / 1e6,el / (double)OPS * 1e9);
    OldPoolDestroy(op);

    op = NULL;
    OldNoLockPoolCreate(&op,7,7,200,120);
    memset(g_inuse,0,sizeof(g_inuse));
    g_dup = 0;
    t0 = now_sec();
    for(int i = 0;i < T;i++) pthread_create(&th[i],NULL,th_old_nolock_shared,op);
    for(int i = 0;i < T;i++) pthread_join(th[i],NULL);
    el = now_sec() - t0;
    int ss = 0,sn = 0,hd = 0;
    OldNoLockPoolInfo(op,0,&ss,&sn,&hd);
    printf("  老池(共享 + 去锁)  : %7.2f Mops/s 总，%6.2f ns/op 每线程；重复发放 %lu 次，结束计数(应为0)=%d\n",
           (double)(T * OPS) / el / 1e6,el / (double)OPS * 1e9,g_dup,hd);
    OldNoLockPoolDestroy(op);

    t0 = now_sec();
    for(int i = 0;i < T;i++) pthread_create(&th[i],NULL,th_new_own,NULL);
    for(int i = 0;i < T;i++) pthread_join(th[i],NULL);
    el = now_sec() - t0;
    printf("  新池(每线程一个)   : %7.2f Mops/s 总，%6.2f ns/op 每线程\n",
           (double)(T * OPS) / el / 1e6,el / (double)OPS * 1e9);
}

/*==========================================================================
 * J. 容量利用率：给同样 ~15MB 预算，看各池能同时服务多少“数据”
 *    （老池的初始化属性在这里被授权调参，用来找它的上限）
 *========================================================================*/
static long old_prealloc_bytes(void* p)
{
    long total = 0;
    int cls = OldPoolClasses(p);
    for(int i = 0;i < cls;i++){
        int ss = 0,sn = 0,hd = 0;
        OldPoolInfo(p,i,&ss,&sn,&hd);
        if(ss > 0 && sn > 0) total += (long)ss * 1024 * sn;
    }
    return total;
}

/* 判断老池返回的指针是否落在池内某级的条区间里（不在 = 走了 malloc 兜底） */
static int old_owns(void* p,void* ptr,int bytes)
{
    int cls = OldPoolClasses(p);
    for(int i = 0;i < cls;i++){
        int ss = 0,sn = 0,hd = 0;
        OldPoolInfo(p,i,&ss,&sn,&hd);
        void* base = OldPoolBase(p,i);
        if(ss <= 0 || sn <= 0 || base == NULL) continue;
        char* b = (char*)base;
        char* e = b + (long)ss * 1024 * sn;
        if((char*)ptr >= b && (char*)ptr + bytes <= e) return 1;
    }
    return 0;
}

static void util_one(const char* label,PoolRef* r,const int* sizes,int n)
{
    #define UTIL_MAXREQ 20000
    static void* keep[UTIL_MAXREQ][8];
    static int   keepB[UTIL_MAXREQ][8];
    long pre = 0,reqs = 0,poolBytes = 0,mallocs = 0;
    long pooledBlocks = 0;

    if(r->kind == 2){
        Memory_Pool* mp = (Memory_Pool*)r->h;
        pre = (long)mp->perSize * mp->entryNum + 4;
    } else {
        pre = old_prealloc_bytes(r->h);
    }

    for(;;){
        if(reqs >= UTIL_MAXREQ) break;
        void* tmp[8];
        int ok = 1;
        for(int i = 0;i < n;i++){
            tmp[i] = NULL;
            int nf = 0;
            pr_alloc(r,sizes[i],&tmp[i],&nf);
            if(nf != 1){ ok = 0; break; }
        }
        if(!ok) break;                       /* 撞上限（失败的这次不回滚，误差几 KB 可忽略） */
        for(int i = 0;i < n;i++){
            keep[reqs][i] = tmp[i];
            keepB[reqs][i] = sizes[i];
            if(r->kind == 2) poolBytes += sizes[i] + GUARD;
            else if(old_owns(r->h,tmp[i],sizes[i])){ poolBytes += sizes[i]; pooledBlocks++; }
            else mallocs++;
        }
        reqs++;
    }

    printf("  %-26s 预分配 %7.2f MB | 并发请求 %5ld | 池内数据 %7.2f MB | 利用率 %5.1f%% | %s\n",
           label,(double)pre / 1048576.0,reqs,(double)poolBytes / 1048576.0,
           100.0 * (double)poolBytes / (double)pre,
           mallocs ? "有块走 malloc（不在池内）" : "全部在池内");
    if(r->kind != 2 && mallocs)
        printf("  %-26s   ↑ 其中 %ld 个块是 malloc 兜底，池内实际只用 %ld 个条\n",
               "",mallocs,pooledBlocks);

    for(long i = 0;i < reqs;i++)
        for(int k = 0;k < n;k++) pr_free(r,keep[i][k],keepB[i][k]);
}

static void case_utilization(void)
{
    const int load1KB[1]  = {1024};
    const int loadWeb[6]  = {63 * 1024,70 * 1024,1024,1024,1024,1024};   /* cWeb 真实请求形态 */
    const int loadIn[5]   = {32 * 1024,1024,1024,1024,1024};             /* 全部 ≤64KB，留在池内 */

    printf("\n== J. 容量利用率：同预算下各池能同时服务多少数据（老池初始化属性已授权调参）==\n");

    printf("\n  负载①：只用 1KB 块\n");
    { PoolRef r; mk_old_locked(&r,120); r.name = "老池"; OldPoolDestroy(r.h);
      r.h = NULL; OldPoolCreate(&r.h,7,7,200,120);
      util_one("老池 (7,7,200,120) 线上", &r,load1KB,1); OldPoolDestroy(r.h); }
    { PoolRef r; r.name = "老池"; r.kind = 0; r.h = NULL;
      /* 调参：预算全给 1KB 级。注意老池的隐藏约束 init_strip_num <= strip_num_future+50，
       * 否则它自己的空闲栈会被写爆（见 ASAN 实测），所以 strip_num_future 要一起放大 */
      OldPoolCreate(&r.h,1,1,15240,15240);
      util_one("老池 (1,1,15240,15240) 调参", &r,load1KB,1); OldPoolDestroy(r.h); }
    { PoolRef r; mk_new(&r,15 * 1024 * 1024,1); r.name = "新池";
      util_one("新池 (15MB × 1 池)", &r,load1KB,1); pr_destroy(&r); }

    printf("\n  负载②：cWeb 真实请求形态（63KB 头 + 70KB 打包 + 4×1KB）\n");
    { PoolRef r; mk_old_locked(&r,120);
      util_one("老池 (7,7,200,120) 线上", &r,loadWeb,6); pr_destroy(&r); }
    { PoolRef r; mk_old_locked(&r,1200);           /* 调参：预算 ×10（栈槽也要一起放大） */
      util_one("老池 (7,7,1250,1200) 调参", &r,loadWeb,6); pr_destroy(&r); }
    { PoolRef r; mk_new(&r,15 * 1024 * 1024,1); r.name = "新池";
      util_one("新池 (15MB × 1 池)", &r,loadWeb,6); pr_destroy(&r); }
    { PoolRef r; mk_new(&r,1024 * 1024,15); r.name = "新池";
      util_one("新池 (1MB × 15 池)", &r,loadWeb,6); pr_destroy(&r); }

    printf("\n  负载③：每请求 32KB + 4×1KB（全部留在池内，避免 malloc 干扰）\n");
    { PoolRef r; mk_old_locked(&r,120);
      util_one("老池 (7,7,200,120) 线上", &r,loadIn,5); pr_destroy(&r); }
    { PoolRef r; mk_new(&r,15 * 1024 * 1024,1); r.name = "新池";
      util_one("新池 (15MB × 1 池)", &r,loadIn,5); pr_destroy(&r); }

    printf("\n  说明：老池预分配 = 每级条数 × Σ(前 init_max 级的大小)——它只能“每级一样多”，\n");
    printf("        无法按负载分布给不同级配不同条数（API 不支持），这是它的结构性上限。\n");
}

/*==========================================================================
 * K. 按 cWeb 四个池的真实负载，验证推荐配置（新池）与线上配置（老池）
 *    “单位”= 一个请求（txt_pool / http_pool）或一个连接（store_area）
 *========================================================================*/
static void case_recommend(void)
{
    /* 每单位要分配的块大小（按 c_web 的调用点推导） */
    static const int l_txt_small[2] = {64512, 4 * 1024};        /* 头 63KB + 小响应打包 4KB */
    static const int l_txt_big[2]   = {64512, 48 * 1024};       /* 头 63KB + 大响应打包 48KB */
    static const int l_whttp_small[1] = {8 * 1024};             /* 1KB 请求的分析器 ≈ 8KB */
    static const int l_whttp_big[1]   = {24 * 1024};            /* 4KB 请求的分析器 ≈ 24KB */
    static const int l_rhttp[1]       = {1024};                 /* 请求原文 1KB 级 */
    static const int l_store[2]       = {2 * 1024, 4 * 1024};   /* 暂存区 2KB + 发送指针区 4KB */

    printf("\n== K. 四个池的真实负载：老池线上配置 vs 新池推荐配置 ==\n");

    printf("\n  [1] worker txt_pool —— 每响应 [63KB 头 + 4KB 打包]\n");
    { PoolRef r; mk_old_locked(&r,120); util_one("老池 (7,7,200,120) 线上", &r,l_txt_small,2); OldPoolDestroy(r.h); }
    { PoolRef r; mk_new(&r,256 * 1024,32); r.name = "新池";
      util_one("新池 (256KB × 32 = 8MiB)", &r,l_txt_small,2); pr_destroy(&r); }
    printf("  [1b] 同上但打包区 48KB（大响应）\n");
    { PoolRef r; mk_old_locked(&r,120); util_one("老池 (7,7,200,120) 线上", &r,l_txt_big,2); OldPoolDestroy(r.h); }
    { PoolRef r; mk_new(&r,256 * 1024,32); r.name = "新池";
      util_one("新池 (256KB × 32 = 8MiB)", &r,l_txt_big,2); pr_destroy(&r); }

    printf("\n  [2] worker http_pool —— 每请求 [分析器 8KB]（1KB 请求）\n");
    { PoolRef r; r.name = "老池"; r.kind = 0; r.h = NULL; OldPoolCreate(&r.h,6,6,10,10);
      util_one("老池 (6,6,10,10) 线上", &r,l_whttp_small,1); OldPoolDestroy(r.h); }
    { PoolRef r; mk_new(&r,64 * 1024,16); r.name = "新池";
      util_one("新池 (64KB × 16 = 1MiB)", &r,l_whttp_small,1); pr_destroy(&r); }
    printf("  [2b] 同上但请求 4KB（分析器 24KB）\n");
    { PoolRef r; r.name = "老池"; r.kind = 0; r.h = NULL; OldPoolCreate(&r.h,6,6,10,10);
      util_one("老池 (6,6,10,10) 线上", &r,l_whttp_big,1); OldPoolDestroy(r.h); }
    { PoolRef r; mk_new(&r,64 * 1024,16); r.name = "新池";
      util_one("新池 (64KB × 16 = 1MiB)", &r,l_whttp_big,1); pr_destroy(&r); }

    printf("\n  [3] receiver http_pool —— 每请求 [请求原文 1KB]\n");
    { PoolRef r; r.name = "老池"; r.kind = 0; r.h = NULL; OldPoolCreate(&r.h,6,6,500,400);
      util_one("老池 (6,6,500,400) 线上", &r,l_rhttp,1); OldPoolDestroy(r.h); }
    { PoolRef r; mk_new(&r,64 * 1024,64); r.name = "新池";
      util_one("新池 (64KB × 64 = 4MiB)", &r,l_rhttp,1); pr_destroy(&r); }

    printf("\n  [4] receiver store_area —— 每连接 [暂存区 2KB + 发送指针区 4KB]\n");
    { PoolRef r; r.name = "老池"; r.kind = 0; r.h = NULL; OldPoolCreate(&r.h,10,6,500,400);
      util_one("老池 (10,6,500,400) 线上", &r,l_store,2); OldPoolDestroy(r.h); }
    { PoolRef r; mk_new(&r,64 * 1024,64); r.name = "新池";
      util_one("新池 (64KB × 64 = 4MiB)", &r,l_store,2); pr_destroy(&r); }

    printf("\n  注：老池 (10,6,500,400) 的第 7~10 级（64/128/256/512KB）条数为 0，\n");
    printf("      所以暂存区一旦扩容到 64KB 就会 notfull=0（线上隐患）。\n");
}

int main(void)
{
    printf("== bench_other_pool : 老池(otherPool，cWeb 现在列装) vs 新池(CWebMemoryPool) ==\n");
    printf("老池：1KB~2^(n-1)KB 分级 / 每级固定条 / 每级一把 pthread_mutex / 超最大级 malloc 兜底\n");
    printf("新池：连续大块切 perSize 子池 / 池内 bump / 计数归零才整体回收 / 无锁（每线程一个池）\n");
    printf("两边都是 -O2；老池源码未改一个字节（只补了独立编译用的 include 替身）\n");

    case_single();
    case_request();
    case_cweb_shape();
    case_footprint();
    case_threads();
    case_mutex_cost();
    case_capacity();
    case_random_pressure();
    case_threads3();
    case_utilization();
    case_recommend();

    printf("\n读表提示：老池每次 alloc/free 都要 lock+unlock 一次 mutex，且按 1KB 起跳的固定条会浪费小块；\n");
    printf("          新池每次约 3 ns，但无锁、要求每线程/每连接一个池（cWeb 已经是这个用法）。\n");
    return 0;
}

