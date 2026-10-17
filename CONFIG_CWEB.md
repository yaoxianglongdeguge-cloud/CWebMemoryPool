# cWeb 四个池的较优配置（新池，含调用点改造清单）

> 依据：本仓库 `TEST_REPORT.md` §5.7 / §9 的实测数据（老池 = `c_web` 现列装版，
> 与 `otherPool` md5 一致）；新池已合入 **O1**（移位代替除法）与 **O2**（分配提示 hint）。
> cWeb 当前规模：**2 个 worker 线程 + 1 个 receiver 线程**（`StaterMain/StarterMain.c:64,70,77`），
> fd 表 `FdTableInit(&fd_table, 4)` 动态扩容、无固定连接上限。

---

## 1. 推荐配置（直接照抄）

```c
/* ---- 每个 Worker 线程（WorkerThreadInit，目前 2 个） ---- */
/* 响应文本/打包区：每响应 63KB 头 + 打包区(≤~960KB)。1MB 单池 → 一个请求的块落在同一池，
 * 请求结束池子计数归零、立刻整体复用。8 池 = 120 个小响应 或 64 个大响应（与老池 120 相当）。 */
MemoryPoolInit(&((*profes)->txt_pool), 1024*1024, 8);   /* 1MB × 8   = 8 MiB */

/* 请求分析器：allocsize ≈ 4×请求长度 + 结构体（1KB 请求≈8KB、4KB 请求≈24KB）。
 * 256KB 单池覆盖到 ~63KB 的请求；4 池 = 124 个小分析器 或 40 个大分析器（老池只有 10）。 */
MemoryPoolInit(&((*profes)->http_pool), 256*1024, 4);   /* 256KB × 4 = 1 MiB */

/* ---- 每个 Receiver 线程（ReceiverThreadInit，目前 1 个） ---- */
/* 请求原文（1KB 级）：1MB 单池可放 1020 个 1KB 块 → 4 池 ≈ 4080 个并发请求
 * （老池 (6,6,500,400) 只有 400 个）。 */
MemoryPoolInit(&((*w)->http_pool), 1024*1024, 4);       /* 1MB × 4   = 4 MiB */

/* 连接级暂存区(2KB，可扩容) + 发送指针区(4KB)：连接级块是“长持有”，
 * 所以故意用“小池多池”——一个被钉住的池只损失 64KB，而不是 1MB。 */
MemoryPoolInit(&((*w)->store_area), 64*1024, 64);       /* 64KB × 64 = 4 MiB */
```

### 总量对比（按 2 worker + 1 receiver 算）

| 池 | 老池线上配置 | 老池内存 | **新池配置** | **新池内存** | 并发能力（实测） |
|---|---|---|---|---|---|
| worker `txt_pool` ×2 | `(7,7,200,120)` | 2×14.88 MiB | `1MB × 8` | **2×8 = 16 MiB** | 120 小响应/64 大响应（老 120/60） |
| worker `http_pool` ×2 | `(6,6,10,10)` | 2×0.62 MiB | `256KB × 4` | **2×1 = 2 MiB** | 124 小/40 大分析器（老 **10**） |
| receiver `http_pool` | `(6,6,500,400)` | 24.61 MiB | `1MB × 4` | **4 MiB** | **4080** 并发请求（老 400） |
| receiver `store_area` | `(10,6,500,400)` | 24.61 MiB | `64KB × 64` | **4 MiB** | **640** 并发连接（老 400） |
| **合计** | | **80.2 MiB** | | **26 MiB（32%）** | **全面提升** |

### 实测验证（`make -C test bench-other` 的 K 段，同负载、同进程）

| 负载 | 老池：内存 / 并发 / 利用率 | 新池推荐：内存 / 并发 / 利用率 |
|---|---|---|
| txt_pool：63KB 头 + 4KB 打包 | 14.88 MiB / 120 / 52.8% | **8 MiB / 120 / 98.2%** |
| txt_pool：63KB 头 + 48KB 打包 | 14.88 MiB / 60 / 43.7% | **8 MiB / 72 / 97.6%** |
| worker http_pool：分析器 8KB | 0.62 MiB / **10** / 12.7% | **1 MiB / 124 / 96.9%** |
| worker http_pool：分析器 24KB | 0.62 MiB / 10 / 38.1% | **1 MiB / 40 / 93.8%** |
| receiver http_pool：请求原文 1KB | 24.61 MiB / 400 / **1.6%** | **4 MiB / 4080 / 100%** |
| store_area：暂存 2KB + 发送指针 4KB | 24.61 MiB / 400 / 9.5% | **4 MiB / 640 / 93.9%** |

---

## 2. 三条定参规则（以后自己调也按这个来）

1. **`perSize ≥ 单块最大字节数 + 4`**（硬约束，否则该类请求直接 `notfull=0`）
2. **`perSize ≥ 该池一个“单位”（请求/连接）的块总量 + 4`** —— 让一个请求的块落进**同一个池**，
   请求结束时该池计数归零、立刻整体复用（这正是计数回收高效、且保持连续分配的前提）
3. **`entryNum ≈ 目标并发单位数 ÷ 每池可容纳的单位数`**，再留 1.5~2 倍余量
   （每池单位数 ≈ `perSize ÷ 单单位块总量`）

**另外一个取舍**：
- **请求级池（txt_pool / http_pool）→ perSize 取大**（尾料占比小、打包效率高、扫描更少）
- **连接级/长持有池（store_area）→ perSize 取小、池数多**（一个被"钉住"的池只损失小池子的空间）

**`perSize` 必须是 2 的幂**（`MemoryPoolInit` 会校验，非 2 的幂直接返回 0）。


---

## 3. 调用点改造清单（**单位必须换算：老池的 size 是 KB 级，新池是字节**）

> ⚠️ 这是迁移最容易出事的地方：老池 `MemoryPoolAlloc(pool, 63, ...)` 表示 **63KB 级**，
> 新池的 `needSize` 是**字节**。照抄就会"申请 63 字节却写 64512 字节"→ 越界踩内存。
> 另外新池的 `MemoryPoolFree` **只需要 `(pool, ptr)`**，老池的第三个 size 参数要删掉。

### 3.1 初始化（4 处）

| 文件:行 | 老 | 新 |
|---|---|---|
| `Threads/WorkerThread.c:82` | `MemoryPoolInit(&txt_pool,7,7,200,120)` | `MemoryPoolInit(&txt_pool,1024*1024,8)` |
| `Threads/WorkerThread.c:83` | `MemoryPoolInit(&http_pool,6,6,10,10)` | `MemoryPoolInit(&http_pool,256*1024,4)` |
| `Threads/ReceiverThread.c:32` | `MemoryPoolInit(&http_pool,6,6,500,400)` | `MemoryPoolInit(&http_pool,1024*1024,4)` |
| `Threads/ReceiverThread.c:33` | `MemoryPoolInit(&store_area,10,6,500,400)` | `MemoryPoolInit(&store_area,64*1024,64)` |

> `MemoryPoolInit` 现在会校验 `perSize > 0 && 是 2 的幂`，返回 0 表示参数非法 —— 建议在初始化处判一下返回值。

### 3.2 分配 / 释放（逐调用点）

| 文件:行 | 老（KB 级 / 带 size 的 free） | 新（字节 / 无 size） |
|---|---|---|
| `Threads/WorkerThread.c:12` `RspHeaderAlloc` | `MemoryPoolAlloc(txt_pool, 63, &ptr, &notfull)` | `MemoryPoolAlloc(txt_pool, sizeof(Response_Header_Entry) + 64512, &ptr, &notfull)` |
| `Threads/WorkerThread.c:52` `PackTotal` | `MemoryPoolAlloc(txt_pool, *allocsize/1024+1, ...)` | `MemoryPoolAlloc(txt_pool, *allocsize, ...)` |
| `Threads/WorkerThread.c:69` `CleanMemoryQueue` | `MemoryPoolFree(txt_pool, m.char_ptr, (m.size/1024+1)*1024)` | `MemoryPoolFree(txt_pool, m.char_ptr)` |
| `Threads/WorkerThread.c:223` `PackBackHead` | `MemoryPoolFree((Memory_Pool*)(w->pool), w->body, (w->body_size/1024+1)*1024)` | `MemoryPoolFree((Memory_Pool*)(w->pool), w->body)` |
| `Threads/WorkerThread.c:227/242` | `MemoryPoolFree((Memory_Pool*)(w->pool), w->headers, 64512)` | `MemoryPoolFree((Memory_Pool*)(w->pool), w->headers)` |
| `Threads/WorkerThread.c:262` `RequestGet` | `MemoryPoolFree(w->http_pool, ptr, 1024*(size/1024+1))` | `MemoryPoolFree(w->http_pool, ptr)` |
| `RequestCome/RequestComeMain/RequestComeMain.c:63` | `MemoryPoolAlloc(http_pool, h_size/1024+1, ...)` | `MemoryPoolAlloc(http_pool, h_size, ...)` |
| `RequestCome/RequestAnalysis/RequestAnalysiser.c:19` `RequestAnalysisInit` | `MemoryPoolAlloc(pool, (h_size/1024)+1, ...)`；`(*h)->size = 1024*((h_size/1024)+1)` | `MemoryPoolAlloc(pool, h_size, ...)`；`(*h)->size = h_size` |
| `RequestCome/RequestAnalysis/RequestAnalysiser.c:48` `RequestAnalysisFree` | `MemoryPoolFree(pool, h, h->size)` | `MemoryPoolFree(pool, h)` |
| `FdConnectResource/RequestTempPool/RequestTempPool.c:47` `RequestTempPoolAlloc` | `MemoryPoolAlloc(pool, size, ...)`（size 是 KB） | `MemoryPoolAlloc(pool, size * 1024, ...)` |
| `.../RequestTempPool.c:64` `RequestTempPoolFree` | `MemoryPoolFree(pool, h->begin, size)` | `MemoryPoolFree(pool, h->begin)` |
| `.../RequestTempPool.c:84` `RequestTempPoolExpend` | `MemoryPoolAlloc(pool, expendsize, ...)`（expendsize 是 KB） | `MemoryPoolAlloc(pool, expendsize * 1024, ...)` |
| `FdConnectResource/SendPointerPool/SendPointerPool.c:18` | `int size = sizeof(entry)*blocknum/1024+1;` → `MemoryPoolAlloc(pool, size, ...)` | `int size = sizeof(entry)*blocknum;` → `MemoryPoolAlloc(pool, size, ...)`（已是字节） |
| `.../SendPointerPool.c:72` | `MemoryPoolFree(pool, s->store, size)` | `MemoryPoolFree(pool, s->store)` |

**换算口诀**：老代码里凡是"`/1024+1`、`*1024`、直接写 KB 数字"的地方，都是 KB 语义 → 新池一律用**字节**；
`MemoryPoolFree` 的第三个参数直接删掉。

---

## 4. 迁移时必须补的两个兜底

1. **大响应（打包区 > `perSize - 4`）**
   老池超过最大级（64KB）走 `malloc` 兜底、永远成功；新池超过 `perSize-4` 会返回 `notfull=0`（上层 503）。
   - 推荐配置 `txt_pool = 1MB × 8` 能覆盖到 **约 960KB 的单个块**；
   - 若响应体可能更大：要么把 `txt_pool` 拆成"普通池 + 大响应专用池"，要么在 `PackTotal` 里对
     `allocsize*1024 + 4 > 1024*1024` 的请求**保留 malloc 路径**（和现在的兜底逻辑一样）。
2. **暂存区扩容（`RequestTempPoolExpend`）**
   老池 `store_area = (10,6,500,400)` 的第 7~10 级（64/128/256/512KB）**条数为 0** → 暂存区一旦扩到
   64KB 就必然 `notfull=0`（线上隐患）。新配置 `64KB × 64` 能覆盖到 **64KB-4**；若要支持更大的暂存区，
   把 `store_area` 改成 `128KB × 32`（仍是 4MiB，但连接容量减半到 ~320）。

---

## 5. 扩容公式（按目标连接数/并发数放大）

| 需要 | 调哪个 | 公式（按推荐配置） |
|---|---|---|
| 更多并发连接 | `store_area` 的 `entryNum` | 每池 ≈10 个连接 → `entryNum ≈ 目标连接数 / 10`（再留 1.5~2 倍） |
| 更多并发请求原文 | receiver `http_pool` 的 `entryNum` | 每 1MB 池 ≈1020 个 1KB 块 → `entryNum ≈ 目标并发 / 1020` |
| 更多 in-flight 响应 | `txt_pool` 的 `entryNum` | 每 1MB 池 ≈15 个小响应（或 8 个大响应） → `entryNum ≈ 目标 / 15` |
| 更多并发解析 | worker `http_pool` 的 `entryNum` | 每 256KB 池 ≈31 个 8KB 分析器 → `entryNum ≈ 目标 / 31` |

例：要做到 **2000 并发连接** → `store_area = 64KB × 200`（12.5MiB）+ receiver `http_pool = 1MB × 4`（4080 够）；
总内存约 2×8 + 2×1 + 4 + 12.5 = **34.5 MiB**，仍不到老池 80.2MiB 的一半。

---

## 6. 怎么验证（改完后跑这两条）

```bash
make -C test bench-other     # K 段：四池真实负载 vs 老池（并发/利用率）
make -C test run && make -C test stress   # 新池自身语义/不变式（87 断言 + 8 条不变式）
```

判定标准：
- K 段新池的"并发请求"≥ 上面的目标值、"利用率"≥ 90%（剩下的就是每块 4B 护栏）；
- 若某个池出现 `池内放不下` 或 `notfull=0` → 说明 `perSize` 小于该类请求，按 §4 补兜底或调大 `perSize`。

