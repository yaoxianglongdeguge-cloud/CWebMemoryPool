# CWebMemoryPool

连续大块内存切成多个小池子 + 池内 bump 追加 + 计数归零整体回收的内存池（设计背景见 `whyNeed.txt`）。

## 测试

```bash
make -C test run            # 单元测试（84 个断言，30s 超时保护，卡死判为 HANG）
make -C test run-asan       # 单元测试 + AddressSanitizer/UndefinedBehaviorSanitizer
make -C test stress         # 压力/性质测试：影子模型逐操作校验 8 条不变式（local + shared）
make -C test stress-asan    # 压力测试 + ASAN/UBSAN
make -C test stress-tsan    # 共享池 + 外部锁 的线程安全验证（ThreadSanitizer，应报 0 竞争）
make -C test bench          # 性能基准（-O2，同进程对照 glibc malloc/free）
make -C test bench-other    # 与老池子（otherPool = cWeb 现在列装那套）的同进程对比
make -C test run-all        # 单元 + 压力 + 基准 + 老池对比 全跑
make -C test clean
```

> 全部测试结果、方法学与结论汇总在 **`TEST_REPORT.md`**；
> 性能优化分析（O1~O5、位图对照、推荐配置）在 **`PERF_REPORT.md`**；
> O1/O2 的可应用补丁在 **`PATCH_O1_O2.md`**；
> **cWeb 四个池的较优配置（含调用点改造清单）在 `CONFIG_CWEB.md`**；
> **一页交代全部结论请看 `SUMMARY.md`。**

环境变量：`STRESS_THREADS`（默认 4）、`STRESS_OPS`（默认 1000000）、`BENCH_MUL`（默认 1）。
单独跑压力测试（可选模式/线程/操作数/seed）：

```bash
./test/build/stress_memory_pool local  4 5000000          # 每线程独立池（与 c_web 用法一致）
./test/build/stress_memory_pool shared 4 500000           # 共享池 + 外部 mutex
./test/build/stress_memory_pool shared-unlocked 4 100000   # 反例：故意不加锁，预期失败
./test/build/bench_memory_pool 2                           # 规模倍数 2
```

### 单元用例（`test/src/test_memory_pool.c`，84 断言）

1. 初始化布局（小池子连续排列、游标/空闲量/计数/空闲标记）
2. 小池子内部 bump 追加与空闲量扣减（每块尾部 4B 护栏，一次申请实际占 needSize + 4）
3. 计数归零 → 小池子整体回收并可复用
3b. 不变式：某次申请“装不下”不等于池子“满了”（否则会出现池子里有空间却 `notfull=0` → 上层误 503）
4. 单池可用上限 = `perSize - 4`（1021B/1024B 被拒，1020B 正好占满）；剩 0~3B 的池子永久闲置且护栏不越界
5. 归还时“落在哪个小池子”的边界判定
6. 200 轮申请/全部归还的地址稳定性（无碎片增长）
7. 重复释放的违约后果（INFO）+ 正确用法的回收验证
8. 真实用法：一个请求拆解后连续分配 40 个 64B 块 → 全部归还 → 下个请求复用同一段内存
9. MB 级小池子布局与 int 容量上限提醒
10. 边界/健壮性 INFO（NULL / 池外指针 / 重复释放计数）

### 压力测试（`test/src/stress_memory_pool.c`）

用影子模型逐操作校验 8 条不变式（I1 bump 偏移一致 / I2 在用块不重叠 / I3 账目自洽 /
I4 计数与模型一致 / I5 “满”标记一致 / I6 归零必复位 / I7 不许“有空间却 notfull=0” / I8 结束全复位），
另有 2000 轮“耗尽 → 全部归还 → 复位”循环验证容量无漂移。

### 性能基线（本机快照：2 vCPU Ryzen 9 8945HX，-O2，每项 9 次重复取最快；**O1+O2 已应用**）

绝对值只作参考（VM 噪声可达 ±30%），**同一次运行内的相对关系**才可靠：

| 项目 | 内存池 | glibc malloc | 结论 |
|---|---|---|---|
| 单块 32B~4KB 申请+释放 | **3.2~3.4 ns** | 4.3~4.8 ns | 内存池快 1.33~1.37 倍 |
| 单块 4096B | 3.3~3.4 ns | 11.0~11.9 ns | 快 3.1~3.5 倍 |
| 每请求 40 个 64B 块（顺序分配→整请求释放） | **0.16 µs/请求（630~642 万请求/s）** | 0.54~0.71 µs/请求 | **快 3.5 倍**，且相邻块 100%（malloc 0%） |
| 一次申请 8 个 64B 再全释放 | 3.23~3.35 ns/块 | 4.6~6.3 ns/块 | 内存池最快 |
| 残量池扫描（256 个池都装不下） | 3.20~3.34 ns/op | — | O2 把最坏情况压回常规水平 |
| 内存占用（16B/64B/256B/1KB 数据） | 20/68/260/1028 B/块 | 24/72/264/1032 B/块 | 每块略省（护栏 4B vs malloc 头部+对齐） |

> 单位申请成本还能再降一半：`MemoryPoolFree` 里的 `gapSize / perSize` 是热路径上唯一的整数除法，
> 现已换成 `gapSize >> __builtin_ctz(perSize)`（要求 `perSize` 为 2 的幂），单块从 6.2~7.6 ns 降到
> **3.2~3.4 ns**；再加"分配提示 hint"后最坏情况（256 个残量池）从 118~208 ns 降到 **3.2~3.3 ns**。
> 完整分析、patch 与实测矩阵见 **`PERF_REPORT.md`**，测试汇总见 **`TEST_REPORT.md`**。

> **与老池子（`otherPool`，cWeb 现在列装的那套）的对比**（`make -C test bench-other`，
> 老池源码一个字节未改）：
>
> | 场景 | 老池（分级+空闲栈+每级 mutex） | 本池（O1+O2） | 差异 |
> |---|---|---|---|
> | 单块 申请+释放 64B~64KB | 12.96~15.07 ns | 3.42~3.58 ns | 本池快 **3.7~4.3 倍** |
> | 一个请求 40×64B（一起分配/释放） | 13.12 ns/块 | 3.91 ns/块 | **3.35 倍**；每请求内存 40960 B → 2720 B（省 15 倍） |
> | cWeb 真实形态（63KB 头 + 70KB 打包 + 4×1KB） | 0.078 µs/请求 | 0.019 µs/请求 | **4.02 倍** |
> | 内存占用（1000 块 64B） | 1024 B/块（最小条 1KB） | 68 B/块 | 省 15 倍 |
> | 参考：一次 `pthread_mutex` lock+unlock | 4.35 ns（老池每次 alloc/free 各一次 → 8.7 ns/对） | — | 老池固定开销的大头 |
>
> 老池超过最大级（cWeb 是 64KB）会走 `malloc` 兜底（永远成功），本池是 `notfull=0`；
> 老池"每级固定条数"（worker `txt_pool` 120 条/级、`http_pool` 只有 10 条/级）vs 本池"总容量 + 每池上限"。


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
7. **返回地址只保证 1 字节对齐**：每块步长是 `needSize + 4`，因此相邻块起点可能不是 4 的倍数
   （`stress-asan` 的 UBSan 曾在此报 misaligned access）。上层若要把池内存当结构体/整型用，
   必须自行按对齐边界申请，或把步长向上取整到 8/16 字节。当前测试只用字节/memcpy 访问。
8. **池本身无锁**：多线程共享同一个池必须外部加锁。实测反例（`shared-unlocked`，4 线程 10 万次操作）
   出现 6847 处不变式错误，含“两块拿到同一内存”；加 `pthread_mutex` 后 `stress-tsan` 报 0 竞争。
   c_web 的用法（`profes->txt_pool` / `w->pool` / 连接私有暂存池 = 每线程/每连接一个池）本身安全。
9. **降级率（上层 503）与 `perSize` 无关，取决于释放模式**：池子空间只在"该池在用块计数归零"时整体回收，
   随机交错释放会让已释放的空间收不回来、已用空间停在历史高水位。实测（4 线程 50 万次操作）
   `perSize` 4KB→64KB、池数 16→64、请求全变小块，降级率都稳定在 **27.7%~33.3%**；
   而"一个请求的块一起申请、一起释放"（200k 请求 × 40 块 = 800 万次分配）降级 **0 次**。
   结论：坚持请求级整块释放；跨请求长持有的块不要和请求块共用池子；容量按"并发请求数 × 单请求块总量"估。
   详见 `PERF_REPORT.md` §4。

## 性能优化建议（按收益排序）

1. **去掉 `MemoryPoolFree` 里的整数除法**（`whichEntry = gapSize / perSize`）：它是热路径唯一的除法，
   实测约占单块申请+释放成本的一半。`perSize` 保持 2 的幂时可存 `shift` 用 `gapSize >> shift`；
   允许任意值时可预计算倒数（libdivide 风格）用乘+移位代替除法。
2. **减少“残量池”被反复扫描**：某池剩余 `>= 5B` 但装不下当前请求时不会被标记为满，每次申请都要重新扫到它。
   256 个这种池会让单次申请从 5.7 ns 涨到 118~208 ns（近似线性，每多一个池 ~0.45 ns）。
3. **一次请求尽量落在同一个池内**（当前设计天然如此，实测相邻块 100%、缓存友好），
   并据“单请求最大字节数”定 `perSize`，可同时降低 503 率与跨池扫描开销。
