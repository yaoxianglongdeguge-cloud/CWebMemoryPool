# CWebMemoryPool

连续大块内存切成多个小池子 + 池内 bump 追加 + 计数归零整体回收的内存池（设计背景见 `whyNeed.txt`）。

## 测试

```bash
make -C test run        # 构建并运行单元测试（30s 超时保护，卡死判为 HANG）
make -C test run-asan   # AddressSanitizer + UndefinedBehaviorSanitizer 下运行
make -C test clean
```

用例文件：`test/src/test_memory_pool.c`，覆盖：

1. 初始化布局（小池子连续排列、游标/空闲量/计数/空闲标记）
2. 小池子内部 bump 追加与空闲量扣减（每块尾部 4B 护栏，一次申请实际占 needSize + 4）
3. 计数归零 → 小池子整体回收并可复用
3b. 不变式：某次申请“装不下”不等于池子“满了”（还有 520B 的池子不能被一次 600B 申请废掉，
    否则会出现池子里明明有空间却 `notfull=0` → 上层误 503）
4. 小池子装满 → 顺延到下一个装得下的池子；全部占满 → `notfull=0`（容量按 needSize+4 计算）
5. 单池可用上限 = `perSize - 4`（1021B / 1024B 被拒，1020B 正好占满）；剩 3B 的池子永久闲置，护栏不越池边界
6. 归还时“落在哪个小池子”的边界判定
7. 重复释放的违约后果（INFO，非失败项）+ 正确用法的回收验证
8. 真实用法：一个请求拆解后连续分配 40 个 64B 块 → 全部归还 → 下个请求复用同一段内存
9. MB 级小池子的布局与容量上限提醒
10. 边界/健壮性 INFO（NULL / 池外指针 / 重复释放计数）

## 已知限制与使用前提

1. **每块 4B 护栏按次计费**：一次申请实际占用 `needSize + 4`，用于容忍调用方少量越界写。
   HTTP 请求拆解后每块几十字节时约 6% 开销（64B 块 → 68B），可接受；块越小开销占比越高。
2. **单池可用上限 = `perSize - 4`**：判定为 `needSize + 4 ≤ freeSize`，好处是 4B 护栏一定落在本池内，
   不会越界到下一个池子（池尾也不会）。代价：需要 `perSize ≥ 单请求最大字节数 + 4`，否则该请求直接
   `notfull=0`（如 perSize=1MB 时单个请求最多 1MB-4B）；池子剩余 0~3B 时永久闲置。
3. **不校验重复释放**：多减一次计数会导致池子在仍有块在用时就整体回收（两个在用请求共用内存）。
   本池有意不做检查，由调用方保证“只释放 `MemoryPoolAlloc` 返回过的指针，且每块只释放一次”；
   `MemoryPoolFree` 只校验指针是否落在池范围内。
4. **int 容量上限**：`MemoryPoolInit` 的 `totalSize = perSize * entryNum + 4` 与 `MemoryPoolFree` 的 `gapSize`
   都是 `int`，池总容量 ≥ 2GB 时会溢出（1GB / 1024×1MB 配置安全），需要时改成 `size_t`。
5. **无销毁接口**：`Memory_Pool` / `manageEntry` / `freeEntry` / 大块内存没有对应的释放函数。
6. `MemoryPoolFree(p, NULL)` 返回 1（“成功”），与池外指针返回 0 的语义不一致；`MemoryPool.h` 缺 include guard。
4. **int 容量上限**：`MemoryPoolInit` 的 `totalSize = perSize * entryNum + 4` 与 `MemoryPoolFree` 的 `gapSize`
   都是 `int`，池总容量 ≥ 2GB 时会溢出（1GB / 1024×1MB 配置安全），需要时改成 `size_t`。
5. **无销毁接口**：`Memory_Pool` / `manageEntry` / `freeEntry` / 大块内存没有对应的释放函数。
6. `MemoryPoolFree(p, NULL)` 返回 1（“成功”），与池外指针返回 0 的语义不一致；`MemoryPool.h` 缺 include guard。
