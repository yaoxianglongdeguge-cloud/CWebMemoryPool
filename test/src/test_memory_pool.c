/*
 * test_memory_pool.c —— CWebMemoryPool 单元测试
 *
 * 用例语义依据 whyNeed.txt 的最终方案 + MemoryPool.c 当前实现约定：
 *   1) 一整块连续大内存切成 entryNum 个 perSize 字节的“小池子”；        MemoryPoolInit
 *   2) 分配只在小池子内部 bump 追加，O(1)；                             MemoryEntryAlloc
 *      每块尾部额外占 4B 护栏（tailSize）：一次申请实际消耗 needSize + 4 字节，
 *      护栏不会再分配给任何人，用来容忍调用方少量越界写。
 *   3) 每个小池子维护分配计数，计数归零才整体回收（游标/空闲量复位）；    MemoryEntryFree
 *   4) 一个小池子装不下就顺延找下一个小池子；全部不行 → notfull=0（上层降级 503）。
 *   5) 一次申请需满足 needSize + 4 ≤ 单池剩余量 → 单池可用上限 = perSize - 4，
 *      每块的 4B 护栏一定落在本池内（池尾也不会越界）；剩 0~3B 的池子永久闲置。
 *   6) 不校验重复释放：调用方契约 = 只释放 MemoryPoolAlloc 返回过的指针，且每块只释放一次
 *      （省掉分配/释放路径上的检查开销；违约后果见第 7 节的 INFO）。
 *   7) “满”的判定 = 剩余空间不足一次最小分配（1B 数据 + 4B 护栏）；某次申请装不下 **不算** 满，
 *      该池子仍要留给后面更小的请求用（第 3b 节，判错会造成上层误 503）。
 *
 * 运行方式： make -C test run        （构建 + 运行，30s 超时保护）
 *            make -C test run-asan   （AddressSanitizer + UBSan）
 */
#include "MemoryPool.h"
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                                             \
    do {                                                                             \
        if (cond) { g_pass++; printf("  [PASS] %s\n", msg); }                         \
        else { g_fail++; printf("  [FAIL] %s  (%s:%d)\n", msg, __FILE__, __LINE__); }  \
    } while (0)

#define PER 1024 /* 单个小池子的字节数 */
#define GUARD 4  /* 每个分配块尾部预留的护栏字节数（对应 MemoryPool.c 里的 tailSize） */

/* 真实部署形态的近似参数：MB 级小池子 + 一个请求拆解后连续申请几十字节的多个块 */
#define SRV_POOL (1024 * 1024)
#define SRV_ENTRY 8
#define CHUNK 64
#define CHUNKS 40

/* 库本身没有 destroy 接口，这里本地释放内部结构，
 * 目的是让 valgrind / ASAN 只报内存池本身的真实问题，而不是测试自身漏掉 free。 */
static void local_destroy(Memory_Pool* p)
{
    if (p == NULL) return;
    free(p->beginPtr);
    free(p->manageEntry);
    free(p->freeEntry);
    free(p);
}

static int in_pool(Memory_Pool* p, void* ptr)
{
    char* c = (char*)ptr;
    return c >= p->beginPtr && c < p->endPtr;
}

int main(void)
{
    printf("== test_memory_pool : 连续小池子 + 计数回收 内存池 ==\n");

    /*================ 1. 初始化布局 ================*/
    Memory_Pool* pool = NULL;
    int r = MemoryPoolInit(&pool, PER, 4);
    CHECK(r == 1, "MemoryPoolInit 返回 1");
    CHECK(pool != NULL, "内存池对象创建成功");
    CHECK(pool->perSize == PER && pool->entryNum == 4, "perSize=1024 / entryNum=4 记录正确");
    CHECK(pool->beginPtr != NULL && pool->manageEntry != NULL && pool->freeEntry != NULL,
          "大块内存与管理结构均已分配");
    CHECK(pool->endPtr == pool->beginPtr + PER * 4 + 4, "endPtr = beginPtr + 4*1024 + 4（尾部预留 4B）");

    int layoutOk = 1;
    for (int i = 0; i < pool->entryNum; i++) {
        Memory_Entry* e = &pool->manageEntry[i];
        if (e->beginPtr != pool->beginPtr + i * PER) layoutOk = 0;    /* 小池子连续排列 */
        if (e->allocPtr != e->beginPtr) layoutOk = 0;                 /* 分配游标在起点 */
        if (e->freeSize != PER || e->totalSize != PER) layoutOk = 0;  /* 空闲量 = 总量 */
        if (e->hadAllocNum != 0) layoutOk = 0;                        /* 分配计数为 0 */
        if (pool->freeEntry[i] != true) layoutOk = 0;                 /* 4 个小池子都空闲 */
    }
    CHECK(layoutOk == 1, "4 个小池子首尾相接，游标/空闲量/计数/空闲标记初始化正确");
    local_destroy(pool);

    /*================ 2. 小池子内部 bump 追加 / 计数归零回收 ================*/
    Memory_Pool* a = NULL;
    MemoryPoolInit(&a, PER, 4);
    void* p1 = NULL;
    void* p2 = NULL;
    int nf = -1;

    CHECK(MemoryPoolAlloc(a, 16, &p1, &nf) == 1, "MemoryPoolAlloc 返回 1");
    CHECK(nf == 1, "申请 16B → notfull=1");
    CHECK(p1 == (void*)a->beginPtr, "首块落在第 0 号小池子起点");
    CHECK(a->manageEntry[0].hadAllocNum == 1, "第 0 号小池子分配计数 = 1");
    CHECK(a->manageEntry[0].freeSize == PER - (16 + GUARD), "空闲量按 needSize + 4B 护栏扣减 = 1004");

    MemoryPoolAlloc(a, 16, &p2, &nf);
    CHECK(nf == 1 && p2 != NULL, "同一小池子内第二次申请成功");
    CHECK(p2 == (char*)p1 + 16 + GUARD, "第二块 = 第一块 + 16B + 4B 护栏（bump 追加，O(1)）");
    CHECK(a->manageEntry[0].hadAllocNum == 2, "不同大小请求共用同一小池子，计数 = 2");
    CHECK(a->manageEntry[0].allocPtr == (char*)p2 + 16 + GUARD, "分配游标指向已用空间末尾（含护栏）");
    CHECK(a->freeEntry[0] == true, "小池子还有空间 → 仍参与分配");

    if (p1 != NULL) memset(p1, 0xA1, 16);
    if (p2 != NULL) memset(p2, 0xB2, 16);
    CHECK(p1 != NULL && ((unsigned char*)p1)[15] == 0xA1, "第 1 块 16B 全可写");
    CHECK(p2 != NULL && ((unsigned char*)p2)[0] == 0xB2 && ((unsigned char*)p2)[15] == 0xB2,
          "第 2 块 16B 全可写且未被第 1 块覆盖（中间隔着 4B 护栏）");
    CHECK(in_pool(a, p1) && in_pool(a, p2), "返回的地址都在池范围内");

    CHECK(MemoryPoolFree(a, p1) == 1, "归还第 1 块");
    CHECK(MemoryPoolFree(a, p2) == 1, "归还第 2 块");
    CHECK(a->manageEntry[0].hadAllocNum == 0, "两块都归还 → 计数归零");
    CHECK(a->manageEntry[0].allocPtr == a->manageEntry[0].beginPtr, "计数归零 → 游标复位到小池子起点");
    CHECK(a->manageEntry[0].freeSize == PER, "计数归零 → 空闲量复位为 1024");
    CHECK(a->freeEntry[0] == true, "回收后小池子重新可用（freeEntry 置回空闲）");

    void* p3 = NULL;
    MemoryPoolAlloc(a, 16, &p3, &nf);
    CHECK(nf == 1 && p3 == p1, "回收后 16B 复用同一地址（无碎片增长）");
    MemoryPoolFree(a, p3);
    local_destroy(a);

    /*================ 3. 装满 → 顺延下一个小池子；容量按 needSize + 4B 护栏算 ================*/
    Memory_Pool* b = NULL;
    MemoryPoolInit(&b, 2048, 2); /* 512 + 4 = 516，2048B 的池子正好放 3 块 */
    void* q[6];
    int allocOk = 1;
    for (int i = 0; i < 6; i++) {
        q[i] = NULL;
        MemoryPoolAlloc(b, 512, &q[i], &nf);
        if (nf != 1 || q[i] == NULL) allocOk = 0;
    }
    CHECK(allocOk == 1, "2 个 2048B 小池子共容纳 6 块 512B（每池 3 块）");
    CHECK(q[0] == (void*)(b->beginPtr + 0), "第 1 块在第 0 号池偏移 0");
    CHECK(q[1] == (void*)(b->beginPtr + 516), "第 2 块偏移 512 + 4 护栏 = 516");
    CHECK(q[2] == (void*)(b->beginPtr + 1032), "第 3 块偏移 1032");
    CHECK(q[3] == (void*)(b->beginPtr + 2048), "第 0 号只剩 500B 装不下 → 第 4 块顺延到第 1 号池起点");
    CHECK(q[4] == (void*)(b->beginPtr + 2048 + 516), "第 5 块在第 1 号池偏移 516");
    CHECK(q[5] == (void*)(b->beginPtr + 2048 + 1032), "第 6 块在第 1 号池偏移 1032");

    CHECK(b->manageEntry[0].freeSize == 2048 - 3 * 516, "第 0 号池剩余 500B（4B 护栏带来的容量损失）");
    CHECK(b->manageEntry[0].hadAllocNum == 3, "第 0 号池分配计数 = 3");
    CHECK(b->freeEntry[0] == true, "剩余 500B 的池子仍被视为空闲（只是装不下 512B）");

    for (int i = 0; i < 6; i++) {
        if (q[i] != NULL) memset(q[i], 0x10 + i, 512); /* 分配失败时不写，避免测试自己崩溃 */
    }
    int dataOk = 1;
    for (int i = 0; i < 6; i++) {
        if (q[i] == NULL) { dataOk = 0; continue; }
        unsigned char* c = (unsigned char*)q[i];
        if (c[0] != (unsigned char)(0x10 + i) || c[511] != (unsigned char)(0x10 + i)) dataOk = 0;
    }
    CHECK(dataOk == 1, "6 块 512B 各自写满后内容互不干扰（有护栏，无重叠）");

    void* over = NULL;
    MemoryPoolAlloc(b, 512, &over, &nf);
    CHECK(nf == 0, "两个池子都用尽 → notfull=0（上层可据此降级 503）");
    CHECK(over == NULL, "分配失败时不返回地址");

    for (int i = 0; i < 6; i++) MemoryPoolFree(b, q[i]);
    int resetOk = 1;
    for (int i = 0; i < 2; i++) {
        if (b->manageEntry[i].hadAllocNum != 0) resetOk = 0;
        if (b->manageEntry[i].allocPtr != b->manageEntry[i].beginPtr) resetOk = 0;
        if (b->manageEntry[i].freeSize != 2048) resetOk = 0;
        if (b->freeEntry[i] != true) resetOk = 0;
    }
    CHECK(resetOk == 1, "6 块全部归还 → 两个池子整体回收（计数 0 / 游标复位 / 空闲 2048）");

    void* again = NULL;
    MemoryPoolAlloc(b, 512, &again, &nf);
    /* 只要求“落在已复位池的起点”：具体复用哪个池允许实现自己决定
     * （例如带“分配提示 hint”的实现会复用上次成功的池，而不再固定用下标最小的池） */
    CHECK(nf == 1 && (again == q[0] || again == q[3]),
          "整体回收后再次申请，落在已复位池的起点（地址稳定、无碎片增长）");
    MemoryPoolFree(b, again);
    local_destroy(b);

    /* --- 3b. “这次装不下”不等于“池子满了”：有剩余空间的池子不能被标记为不可分配 --- */
    Memory_Pool* cap = NULL;
    MemoryPoolInit(&cap, PER, 2);
    void* m1 = NULL;
    void* m2 = NULL;
    void* m3 = NULL;
    MemoryPoolAlloc(cap, 500, &m1, &nf);
    CHECK(nf == 1 && m1 == (void*)cap->beginPtr, "第 0 号先放一个 500B（用 504B，剩 520B）");
    MemoryPoolAlloc(cap, 600, &m2, &nf);
    CHECK(nf == 1 && m2 == (char*)cap->beginPtr + PER, "600B 装不下第 0 号（604 > 520）→ 落到第 1 号池");
    CHECK(cap->manageEntry[0].hadAllocNum == 1, "第 0 号的计数仍是 1（这次申请与它无关）");
    CHECK(cap->freeEntry[0] == true, "第 0 号还剩 520B → 仍然可用（不能因一次装不下就被标记为满）");
    MemoryPoolAlloc(cap, 500, &m3, &nf);
    CHECK(nf == 1 && m3 == (char*)cap->beginPtr + 504, "500B 请求回到还有 520B 的第 0 号池（偏移 504）");
    if (m3 != NULL) MemoryPoolFree(cap, m3);
    if (m2 != NULL) MemoryPoolFree(cap, m2);
    if (m1 != NULL) MemoryPoolFree(cap, m1);
    local_destroy(cap);

    /*================ 4. 单池可用上限 = perSize - 4（护栏必须留在本池内） ================*/
    Memory_Pool* t = NULL;
    MemoryPoolInit(&t, PER, 2);
    void* s1 = NULL;
    void* s2 = NULL;

    MemoryPoolAlloc(t, PER - GUARD + 1, &s1, &nf); /* 1021 + 4 护栏 = 1025 > 1024 */
    CHECK(nf == 0, "申请 1021B（+4 护栏 = 1025 > perSize）必须被拒绝");
    CHECK(t->manageEntry[0].freeSize == PER, "被拒绝的申请不消耗小池子空间");
    CHECK(s1 == NULL, "被拒绝时不返回地址");

    MemoryPoolAlloc(t, PER, &s1, &nf);
    CHECK(nf == 0, "申请 1024B（= perSize）同样被拒绝：单池可用上限 = perSize - 4");
    CHECK(s1 == NULL, "被拒绝时不返回地址");

    MemoryPoolAlloc(t, PER - GUARD, &s1, &nf); /* 1020 + 4 护栏 = 1024，正好占满 */
    CHECK(nf == 1 && s1 == (void*)t->beginPtr, "申请 1020B（+4 护栏 = 1024）正好占满一个小池子");
    CHECK(t->manageEntry[0].freeSize == 0, "占满后 freeSize = 0（不会变成负数）");

    MemoryPoolAlloc(t, 1, &s2, &nf);
    CHECK(nf == 1 && s2 == (char*)s1 + PER, "1B 申请顺延到第 2 个小池子起点");
    CHECK(t->freeEntry[0] == false, "第 0 号被拒一次后标记为不可再分配");
    if (s1 != NULL) memset(s1, 0x11, PER - GUARD);
    if (s2 != NULL) memset(s2, 0x22, 1);
    CHECK(s1 != NULL && ((unsigned char*)s1)[PER - GUARD - 1] == 0x11, "写满第 1 块 1020B 不会踩到第 2 块首字节");
    CHECK(s2 != NULL && ((unsigned char*)s2)[0] == 0x22, "第 2 块内容完好");
    MemoryPoolFree(t, s2);
    MemoryPoolFree(t, s1);
    local_destroy(t);

    /* 不足最小分配（1B + 4B 护栏）的剩余空间会永久闲置；护栏一定落在本池内 */
    Memory_Pool* u = NULL;
    MemoryPoolInit(&u, PER, 2);
    void* ua = NULL;
    void* ub = NULL;
    void* uc = NULL;
    MemoryPoolAlloc(u, 1017, &ua, &nf); /* 1017 + 4 = 1021 ≤ 1024 → 第 0 号剩 3B */
    MemoryPoolAlloc(u, 1020, &ub, &nf); /* 第 0 号剩 3B 装不下 → 第 1 号池起点 */
    CHECK(ua == (void*)u->beginPtr && ub == (char*)u->beginPtr + PER, "ua 在第 0 号偏移 0，ub 在第 1 号起点");
    CHECK(u->manageEntry[0].freeSize == 3, "第 0 号还剩 3B，但小于最小分配 5B");
    MemoryPoolAlloc(u, 3, &uc, &nf);
    CHECK(nf == 0 && uc == NULL, "剩余 3B 的池子永远用不上（护栏必须留在本池内）→ notfull=0");
    if (ua != NULL && ub != NULL) {
        *(unsigned char*)ub = 0x5A;         /* 第 1 号池首字节的标记 */
        memset(ua, 0x77, 1017 + GUARD);     /* 越界写满本块 + 4B 护栏（仍在第 0 号池内） */
        CHECK(*(unsigned char*)ub == 0x5A, "护栏一定落在本池内：越界 4B 不会碰到下一个池子");
    }
    MemoryPoolFree(u, ub);
    MemoryPoolFree(u, ua);
    local_destroy(u);

    /*================ 5. 归还时“落在哪个小池子”的边界判定 ================*/
    Memory_Pool* d = NULL;
    MemoryPoolInit(&d, PER, 2);
    void* first = NULL;
    void* last = NULL;

    MemoryPoolAlloc(d, 1015, &first, &nf); /* 1015 + 4 = 1019 → 第 0 号剩 5B */
    CHECK(nf == 1 && first == (void*)d->beginPtr, "申请 1015B 落在第 0 号小池子起点");
    MemoryPoolAlloc(d, 1, &last, &nf);     /* 1 + 4 = 5，正好用掉剩的 5B → 偏移 1019 */
    CHECK(nf == 1 && last == (char*)d->beginPtr + 1019, "剩余 5B 被 1B 申请用掉（偏移 1019）");
    CHECK(d->manageEntry[0].freeSize == 0, "第 0 号 freeSize = 0");

    MemoryPoolFree(d, last);
    CHECK(d->manageEntry[0].hadAllocNum == 1, "归还池内靠后的块只扣第 0 号小池子的计数");
    CHECK(d->manageEntry[1].hadAllocNum == 0, "第 1 号小池子计数不受影响（仍为 0）");
    MemoryPoolFree(d, first);
    CHECK(d->manageEntry[0].hadAllocNum == 0, "两块都归还 → 第 0 号计数归零");
    CHECK(d->freeEntry[0] == true, "归零后第 0 号小池子重新可用");
    local_destroy(d);

    /*================ 6. 多轮请求：反复申请/整体回收，地址稳定无碎片增长 ================*/
    Memory_Pool* w = NULL;
    MemoryPoolInit(&w, PER, 4);
    int sizes[3] = {100, 200, 300};
    void* base[3] = {NULL, NULL, NULL};
    int stable = 1;
    for (int round = 0; round < 200; round++) {
        void* cur[3] = {NULL, NULL, NULL};
        for (int i = 0; i < 3; i++) {
            MemoryPoolAlloc(w, sizes[i], &cur[i], &nf);
            if (nf != 1 || cur[i] == NULL) stable = 0;
        }
        if (round == 0) {
            base[0] = cur[0];
            base[1] = cur[1];
            base[2] = cur[2];
        } else if (cur[0] != base[0] || cur[1] != base[1] || cur[2] != base[2]) {
            stable = 0;
        }
        for (int i = 0; i < 3; i++) MemoryPoolFree(w, cur[i]);
    }
    CHECK(stable == 1, "200 轮“申请 3 块(100/200/300B) → 全部归还”地址完全稳定");
    CHECK(base[0] == (void*)w->beginPtr && base[1] == (char*)w->beginPtr + 104 && base[2] == (char*)w->beginPtr + 308,
          "三块按 100+4 / 200+4 / 300+4 的步距排列，复用同一段连续内存");
    CHECK(w->manageEntry[0].freeSize == PER && w->freeEntry[0] == true && w->manageEntry[0].hadAllocNum == 0,
          "循环结束后第 0 号小池子回到完全空闲状态");
    local_destroy(w);

    /*================ 7. 重复释放：本池不校验，属于调用方契约 ================*/
    Memory_Pool* rp = NULL;
    MemoryPoolInit(&rp, PER, 1);
    void* A = NULL;
    void* B = NULL;
    void* C = NULL;
    MemoryPoolAlloc(rp, 100, &A, &nf);
    MemoryPoolAlloc(rp, 900, &B, &nf);
    CHECK(A == (void*)rp->beginPtr && B == (char*)A + 104, "A/B 两块同在第 0 号池（偏移 0 / 104）");
    CHECK(rp->manageEntry[0].hadAllocNum == 2, "两块在用 → 计数 = 2");

    MemoryPoolFree(rp, A);
    CHECK(rp->manageEntry[0].hadAllocNum == 1, "归还 A 后计数 = 1（B 仍在用）");

    MemoryPoolFree(rp, A); /* 违约：同一指针释放两次（本池不检查，靠调用方保证） */
    MemoryPoolAlloc(rp, 900, &C, &nf);
    if (B != NULL && C != NULL) {
        printf("  [INFO] 违约（同一指针释放两次）后计数 = %d，新块 C = 池起点 + %ld，B = 池起点 + %ld，"
               "重叠 %ld B —— 本池不做重复释放校验以省掉开销\n",
               rp->manageEntry[0].hadAllocNum, (long)((char*)C - rp->beginPtr),
               (long)((char*)B - rp->beginPtr), (long)(((char*)B + 900) - (char*)C));
    } else {
        printf("  [INFO] 违约（同一指针释放两次）后计数 = %d（B/C 分配结果异常，见上方失败项）\n",
               rp->manageEntry[0].hadAllocNum);
    }
    local_destroy(rp);

    /* 正确用法（每块只释放一次）下，计数制回收完全正常 */
    Memory_Pool* rp2 = NULL;
    MemoryPoolInit(&rp2, PER, 1);
    void* A2 = NULL;
    void* B2 = NULL;
    MemoryPoolAlloc(rp2, 100, &A2, &nf);
    MemoryPoolAlloc(rp2, 900, &B2, &nf);
    MemoryPoolFree(rp2, A2);
    MemoryPoolFree(rp2, B2);
    CHECK(rp2->manageEntry[0].hadAllocNum == 0 && rp2->freeEntry[0] == true &&
          rp2->manageEntry[0].allocPtr == rp2->manageEntry[0].beginPtr,
          "正确用法（每块只释放一次）→ 两块归还后池子整体回收、可复用");
    local_destroy(rp2);

    /*================ 8. 真实用法：一个请求拆解后连续分配几十字节的多个块 ================*/
    Memory_Pool* srv = NULL;
    MemoryPoolInit(&srv, SRV_POOL, SRV_ENTRY);
    void* chunk[CHUNKS];
    int consecutive = 1;
    for (int i = 0; i < CHUNKS; i++) {
        chunk[i] = NULL;
        MemoryPoolAlloc(srv, CHUNK, &chunk[i], &nf);
        if (nf != 1 || chunk[i] == NULL) consecutive = 0;
        if (chunk[i] != (char*)srv->beginPtr + i * (CHUNK + GUARD)) consecutive = 0;
    }
    CHECK(consecutive == 1, "一个请求的 40 个 64B 块按 68B 步距连续落在同一个池子里");
    CHECK(srv->manageEntry[0].hadAllocNum == CHUNKS, "第 0 号池计数 = 40");
    CHECK(srv->manageEntry[1].hadAllocNum == 0, "第 1 号池未被使用（连续分配只吃一个池子，缓存友好）");
    printf("  [INFO] 每块占 %dB = %dB 数据 + %dB 护栏（%d%% 开销），1MB 池可放 %d 块，本次用掉 %dB\n",
           CHUNK + GUARD, CHUNK, GUARD, 100 * GUARD / CHUNK,
           SRV_POOL / (CHUNK + GUARD), CHUNKS * (CHUNK + GUARD));

    for (int i = 0; i < CHUNKS; i++) {
        if (chunk[i] != NULL) memset(chunk[i], 0x30 + (i % 8), CHUNK);
    }
    int chunkOk = 1;
    for (int i = 0; i < CHUNKS; i++) {
        if (chunk[i] == NULL) { chunkOk = 0; continue; }
        unsigned char* c = (unsigned char*)chunk[i];
        if (c[0] != (unsigned char)(0x30 + (i % 8)) || c[CHUNK - 1] != (unsigned char)(0x30 + (i % 8))) chunkOk = 0;
    }
    CHECK(chunkOk == 1, "40 个块各自写满 64B 后内容互不干扰");

    for (int i = 0; i < CHUNKS; i++) MemoryPoolFree(srv, chunk[i]);
    CHECK(srv->manageEntry[0].hadAllocNum == 0 && srv->freeEntry[0] == true &&
          srv->manageEntry[0].allocPtr == srv->manageEntry[0].beginPtr,
          "请求处理完 → 40 块全部归还，池子整体回收（计数 0 / 游标复位）");

    void* re1 = NULL;
    void* re2 = NULL;
    MemoryPoolAlloc(srv, CHUNK, &re1, &nf);
    MemoryPoolAlloc(srv, CHUNK, &re2, &nf);
    CHECK(re1 == chunk[0] && re2 == chunk[1], "下一个请求复用同一段连续内存（无碎片、缓存友好）");
    MemoryPoolFree(srv, re2);
    MemoryPoolFree(srv, re1);
    local_destroy(srv);

    /*================ 9. MB 级小池子的布局与容量上限提醒 ================*/
    Memory_Pool* big = NULL;
    MemoryPoolInit(&big, 1024 * 1024, 8); /* 1MB × 8 = 8MB（真实部署是 1MB × entryNum） */
    CHECK(big->perSize == 1024 * 1024 && big->entryNum == 8, "1MB 小池子 × 8 初始化成功");
    CHECK(big->manageEntry[7].beginPtr == big->beginPtr + 7 * 1024 * 1024, "第 8 个池子起点 = 大块起点 + 7MB");
    void* b1 = NULL;
    void* b2 = NULL;
    MemoryPoolAlloc(big, 4096, &b1, &nf);
    MemoryPoolAlloc(big, 4096, &b2, &nf);
    CHECK(nf == 1 && b1 == (void*)big->beginPtr && b2 == (char*)b1 + 4100, "1MB 池里的 4KB 请求按 4100 步距分配");
    MemoryPoolFree(big, b2);
    MemoryPoolFree(big, b1);
    printf("  [INFO] 容量上限：总池容量 ≥ 2GB 时 MemoryPoolInit 的 int totalSize 与 "
           "MemoryPoolFree 的 int gapSize 会溢出（1GB(1024×1MB) 配置下 totalSize = %lld，仍安全）\n",
           1024LL * 1024 * 1024 + 4);
    local_destroy(big);

    /*================ 10. 边界/健壮性信息（仅记录当前行为，不纳入通过判定） ================*/
    Memory_Pool* g = NULL;
    MemoryPoolInit(&g, PER, 2);
    int stack_var = 0;
    printf("  [INFO] 释放 NULL          → %d（库内直接 return 1，即“成功释放空指针”）\n",
           MemoryPoolFree(g, NULL));
    printf("  [INFO] 释放栈上地址        → %d（池外地址返回 0，符合预期）\n",
           MemoryPoolFree(g, &stack_var));
    printf("  [INFO] 释放池尾之后地址    → %d（越界拦截）\n", MemoryPoolFree(g, g->endPtr));
    void* x = NULL;
    MemoryPoolAlloc(g, 64, &x, &nf);
    MemoryPoolFree(g, x);
    MemoryPoolFree(g, x); /* 重复释放 */
    printf("  [INFO] 同一块重复释放后计数 → %d（未做重复释放校验）\n",
           g->manageEntry[0].hadAllocNum);
    local_destroy(g);


    printf("---- test_memory_pool: PASS %d / FAIL %d ----\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
