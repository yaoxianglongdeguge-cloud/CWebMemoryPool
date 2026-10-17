# O1 / O2 改动清单（待你审阅后再决定是否应用）

> 这份文件只包含 **O1（去掉热路径的整数除法）** 和 **O2（分配提示 hint）** 两处改动，
> 都是我已在 `/tmp` 副本里实测验证过的版本，**当前仓库的 `MemoryPool.c/.h` 没有被改动**。
> 背景与完整分析见 `PERF_REPORT.md`（本文件是它的"可执行部分"）。

## 0. 速览

| | 改什么 | 实测收益（同机） | 风险 | 验证 |
|---|---|---|---|---|
| **O1** | `MemoryPoolFree` 里 `gapSize / perSize` → 移位 | 单块 申请+释放 **6.2~7.6 → 3.1~3.5 ns**；真实请求模式 **0.29~0.34 → 0.16~0.23 µs** | 极低，但要求 `perSize` 是 2 的幂（或改用 §2.3 的倒数乘法） | 84/84 断言 + 800 万次压力操作 0 不变式错误 |
| **O2** | `MemoryPoolAlloc` 先试"上次成功的池子"（`hint`） | 残量池最坏情况 **118~208 → 3.2 ns**；请求模式再快 ~5~10% | 低：会改变"优先用哪个池"；`Memory_Pool` 多 4 字节 | 84/84 断言 + 800 万次压力操作 0 不变式错误 |

叠加后所有实测场景都比 glibc malloc 快 1.27~3.4 倍（原版在单块场景是 0.6~0.8 倍）。

---

## 1. 改动 O1：去掉 `MemoryPoolFree` 的整数除法

### 1.1 补丁（可直接 `patch -p1`，只有 1 行）

```diff
--- a/MemoryPool.c
+++ b/MemoryPool.c
@@ -103,7 +103,7 @@
 
     int perSize = p->perSize;
     int gapSize = pt - p->beginPtr;
-    int whichEntry = gapSize / perSize;
+    int whichEntry = gapSize >> __builtin_ctz((unsigned)perSize);//perSize 为 2 的幂时等价于 /perSize（热路径去除法）
     Memory_Entry* e = &(p->manageEntry[whichEntry]);
     bool Cleaned = false;
     MemoryEntryFree(e,&Cleaned);
```

### 1.2 为什么要动它

`MemoryPoolFree` 是每次归还都要走的路径，而 `gapSize / perSize` 是整条热路径上唯一的整数除法
（32 位 `idiv`：延迟 20~26 周期、流水线很差）。实测把它换成移位后：

| | 原版 | O1 后 |
|---|---|---|
| 单块 32B/64B/256B/1024B 申请+释放 | 6.2~7.6 ns | **3.17~3.47 / 3.20~3.35 / 3.30~3.45 / 3.12~3.39 ns** |
| 单块 4096B | 6.2~16.8 ns | **3.11~3.36 ns** |
| 真实请求模式（40 个 64B 块） | 0.29~0.34 µs | **0.16~0.23 µs（430~628 万请求/s）** |

其中"4096B"那栏顺带说明了一句：malloc 在 ≥1040B 时掉出 `tcache` 快路径（实测 4.5 → 12 ns），
所以大块上池子本来就赢，O1 只是把优势从 1.7 倍拉到 3.4 倍。

### 1.3 前置要求：`perSize` 必须是 2 的幂

`__builtin_ctz` 对 0 是未定义行为，且 `perSize` 不是 2 的幂时移位会算出**错误的下标**（静默越界）。
两个办法：

- **办法一（推荐，你的配置本来满足）**：`MemoryPoolInit` 里加一句校验 + 注释说明约束：
  ```c
  if(perSize <= 0 || (perSize & (perSize - 1)) != 0){
      return 0;   /* 约束：perSize 必须是 2 的幂（1KB/2KB/4KB/1MB 都满足） */
  }
  ```
  （当前 `MemoryPoolInit` 没有任何参数校验，顺手把 `perSize > 0` 也一起补上更稳。）
- **办法二（想支持任意 `perSize`）**：用倒数乘法代替除法，见下节。

### 1.4 备选：支持任意 `perSize` 的倒数乘法（已做边界穷举验证）

在 `Memory_Pool` 里加一个常量，`MemoryPoolInit` 里算一次：

```c
    (*p)->divMagic = ((~0ULL) / (unsigned long long)perSize) + 1;   // = floor(2^64/d) + 1
```

`MemoryPoolFree` 里：

```c
    int whichEntry = (int)(((unsigned __int128)(unsigned long long)gapSize * p->divMagic) >> 64);
```

正确性验证（穷举所有会"跳变"的边界点 `k*d-1 / k*d / k*d+1`，`d ∈ {1000,1024,3000,65535,100000,1000000,1024000,3145728}`，
共约 3200 万个点）：**0 处不匹配**。同样正确的另一个写法是"32 位 magic + 一次修正"：

```c
    /* Init: magic32 = (1ULL<<32)/perSize + 1; */
    int whichEntry = (int)(((unsigned long long)gapSize * p->magic32) >> 32);
    if((unsigned long long)whichEntry * perSize > (unsigned long long)gapSize) whichEntry--;  /* 修正 +1 误差 */
```

> 提醒：**不加修正的 `(gapSize * ((1<<32)/perSize + 1)) >> 32` 是错的**（我在验证时先写错、被边界穷举抓出来），
> 别用那个简化写法。两种正确写法的性能我没单独测，但都只是"乘 + 移位（+ 比较）"，远快于 `idiv`。

---

## 2. 改动 O2：分配提示 `hint`（先试上次成功的池子）

### 2.1 补丁（两个文件，可直接 `patch -p1`）

```diff
--- a/MemoryPool.h
+++ b/MemoryPool.h
@@ -20,6 +20,7 @@
     char* endPtr;
     Memory_Entry* manageEntry;
     bool* freeEntry;
+    int hint;//O2：上次成功分配的小池子下标（快路径先试它）
 
 }Memory_Pool;
 
```

```diff
--- a/MemoryPool.c
+++ b/MemoryPool.c
@@ -64,6 +64,7 @@
     (*p)->perSize = perSize;
     (*p)->manageEntry = malloc(entryNum*sizeof(Memory_Entry));
     MemoryEntryInit(*p);
+    (*p)->hint = 0;//O2
     (*p)->freeEntry = malloc(entryNum*sizeof(bool));
     for(int i=0;i < entryNum;i++){
         (*p)->freeEntry[i] = true;
@@ -74,7 +75,25 @@
 
 int MemoryPoolAlloc(Memory_Pool* p,int needSize,void** ptr,int* notfull)
 {
+    const int totalSize = needSize + 4;//与 tailSize 一致
     *notfull = 0;
+
+    /* O2 快路径：先试上次成功的那个小池子。同一次请求拆出的块大小接近，
+     * “继续往同一个池子里放”也是主路径，命中时只需 1 次判断（原来是每次从 0 号池扫）。 */
+    int h = p->hint;
+    if(h >= 0 && h < p->entryNum && p->freeEntry[h] == true &&
+       p->manageEntry[h].freeSize >= totalSize){
+        Memory_Entry* e = &p->manageEntry[h];
+        *ptr = (void*)e->allocPtr;
+        e->allocPtr += totalSize;
+        e->freeSize -= totalSize;
+        e->hadAllocNum++;
+        p->freeEntry[h] = (e->freeSize >= 1 + 4);//剩余不足以再来一个最小块才算满
+        *notfull = 1;
+        return 1;
+    }
+
+    /* 慢路径：原来的顺序扫描，语义与标记逻辑完全不变 */
     bool isFull = true;
     for(int i = 0;i < p->entryNum;i++){
         if(p->freeEntry[i] == true){
@@ -85,6 +104,7 @@
             if(canPutInto == true){
                 isFull = false;
                 *notfull = 1;
+                p->hint = i;//记住这个池子
                 break;
             }
             
```

### 2.2 它解决的是什么

原来的 `MemoryPoolAlloc` **每次都从 0 号池开始扫**。当一串池子"没满、但装不下当前请求"（残量池）时，
每次申请都要把它们全部比较一遍（实测 **0.45 ns/个**），256 个残量池就是 **118~208 ns/次**。
加了 hint 之后，这类申请变成"先看上次成功的池子"，命中即返回：

| 场景 | 原版 | O1 | **O1+O2** |
|---|---|---|---|
| 残量池 A) 1 个池 | 5.7~6.5 ns | 3.06~3.16 ns | 3.03~3.36 ns |
| 残量池 B) 256 个池 | **118~208 ns** | 117~121 ns | **3.19~3.23 ns（B/A ≈ 1.0~1.1）** |
| 真实请求模式 | 0.29~0.34 µs | 0.16~0.23 µs | **0.16~0.17 µs（590~638 万请求/s）** |
| 单块 64B（对 malloc） | 0.6~0.8 倍 | 1.36 倍 | **1.27~1.36 倍** |

### 2.3 两个必须注意的点

1. **快路径里的"满"标记方向不能写反**：必须是
   ```c
   p->freeEntry[h] = (e->freeSize >= 1 + 4);   // 剩余不足以再来一个最小块才算满
   ```
   我第一次写成 `(e->freeSize < 1 + 4)`（把"满"当"空闲"），单元用例立刻抓到：第二个 16B 申请被赶到别的池子。
   这个坑值得写进注释，因为快路径绕过了 `MemoryEntryAlloc`，语义要自己保持一致。
2. **分配顺序会变**：原来"优先用下标最小的可用池"，现在"优先复上次的池"。影响面：
   - 不改变连续性：同一个请求内的后续块依然紧跟在同一池里（实测相邻块仍 **100%**）；
   - 不改变任何不变式（8 条压力不变式全过）；
   - 但**任何断言具体地址/池下标的测试或代码都要理解这一点**：本仓库的单元用例里有一条
     （"整体回收后再次申请，复用第 1 块地址"）依赖顺序，我已改成"落在已复位池的起点（地址稳定）"，
     改完当前仓库与 O2 变体都是 **84/84**。

### 2.4 如果只想要 O1

O2 与 O1 相互独立：只应用 §1 的补丁即可（那时 `MemoryPoolAlloc` 一行都不用改）。
但要知道："残量池反复扫描"这个最坏情况**只有 O2 能解决**；位图方案我实测过，
要么解决不了（只标满/不满），要么要付 6 ns/次的维护成本（分档位图，见 `PERF_REPORT.md` §5.7）。


---

## 3. 这两处改动是怎么验证的

我这次的验证流程（你可以原样复跑）：

```bash
# 1) 复制一份到 /tmp，改在那里（不动仓库）
cp -r . /tmp/o1 && cd /tmp/o1        # 应用 §1 → O1 变体
# 2) 依次跑：单元断言 / 压力（8 条不变式）/ 基准
make -C test run                     # 期望 84 PASS / 0 FAIL
make -C test build/stress_memory_pool && ./test/build/stress_memory_pool local 4 2000000
make -C test bench                   # 对比基准
```

本次结果（O1 与 O1+O2 两个变体都跑过）：

| 变体 | 单元 | 压力（4 线程 × 200 万次） | 基准 |
|---|---|---|---|
| O1 | **84/84** | 8,000,353 次操作，**8 条不变式 0 错误** | 单块 3.1~3.5 ns；请求模式 0.16~0.23 µs；残量池 B) 117~121 ns（未改善，O1 管不到） |
| O1+O2 | **84/84** | 8,000,311 次操作，**8 条不变式 0 错误** | 单块 3.2~3.5 ns；请求模式 0.16~0.17 µs；**残量池 B) 3.19~3.23 ns** |

补丁可用性也验证过：把这两个 diff 打到一份**全新的仓库副本**上，
`patch -p1` 干净应用 → 单元 84/84、压力 400 万次 0 不变式错误。

## 4. 应用方式（三选一）

```bash
# 方式 A：用补丁（推荐，把上面两段 diff 分别存成 p_o1.patch / p_o2.patch）
patch -p1 --dry-run < p_o1.patch && patch -p1 < p_o1.patch
patch -p1 --dry-run < p_o2.patch && patch -p1 < p_o2.patch
# 或 git apply --check p_o1.patch && git apply p_o1.patch

# 方式 B：手动改（就 3 处）
#   MemoryPool.c:106   gapSize / perSize        → gapSize >> __builtin_ctz((unsigned)perSize)
#   MemoryPool.h:~23   加 int hint;
#   MemoryPool.c      Init 里加 (*p)->hint = 0; / Alloc 里加 hint 快路径 + p->hint = i;

# 方式 C：先只做 O1（改一行），O2 以后再决定
```

## 5. 应用后必须通过的验证（判定标准）

```bash
make -C test run            # 必须 84 PASS / 0 FAIL（重点：3b「装不下≠满」、I5 满标记、I6 归零复位）
make -C test run-asan       # ASAN+UBSAN 必须 0 报错
make -C test stress         # local + shared 两段都必须 8 条不变式 0 错误
make -C test stress-asan    # 压力 + ASAN/UBSAN，0 报错
make -C test stress-tsan    # 共享池 + 外部锁，0 data race
make -C test bench          # 与基线对比：单块 ns/op、请求模式 µs/请求、残量池 A/B
```

**只看性能、不看语义**：任何"改进"只要让上述语义类断言变红，一律回退。

## 6. 附：本文件对应变体的位置与实测原始输出

| 变体 | 内容 | 位置（临时目录，系统会清） | 原始日志 |
|---|---|---|---|
| O1 | 只在 `MemoryPool.c:106` 换移位 | `/tmp/o1` | `/tmp/o1*.log` |
| O1+O2 | 再加 `hint` 快路径 | `/tmp/o1o2` | `/tmp/o1o2_*.log` |
| 补丁文件 | 上面两段 diff | `/tmp/p_o1.patch`、`/tmp/p_o2.patch` | — |
| 边界穷举验证 | 倒数乘法正确性 | `/tmp/recip2.c`（0 处不匹配） | — |
| 位图对照实验 | 三种位图方案 | `/tmp/mp_bm` | `/tmp/bm*_bench.log` |

> 注意：`/tmp` 里的东西会被系统清理（我这轮就丢过一次），要长期保留请把变体复制到仓库外你自己的目录，
> 或者直接按本文件应用改动后用 `git diff` 留档。

