# CWebMemoryPool 单元测试 / 压力测试 / 性能基准
# 用法：
#   make -C test run           单元测试（30s 超时保护，卡死判为 HANG）
#   make -C test run-asan      单元测试（AddressSanitizer + UndefinedBehaviorSanitizer）
#   make -C test stress        压力/性质测试（影子模型校验 8 条不变式，多线程）
#   make -C test stress-asan   压力测试 + ASAN/UBSAN
#   make -C test stress-tsan   共享池 + 外部锁 的线程安全验证（ThreadSanitizer）
#   make -C test bench         性能基准（-O2，对照 glibc malloc/free）
#   make -C test run-all       单元 + 压力 + 基准 全跑
#   make -C test clean

ROOT   := ..
CC     := gcc
CFLAGS := -g -O0 -Wall -I$(ROOT)
ASAN   := -g -O0 -Wall -I$(ROOT) -fsanitize=address,undefined
BENCHF := -O2 -g -Wall -I$(ROOT)
SRC    := src
BUILD  := build
POOL   := $(ROOT)/MemoryPool.c
TESTS  := test_memory_pool

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD)/%: $(SRC)/%.c $(POOL) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD): 
	mkdir -p $(BUILD)

#------------------------------- 单元测试 -------------------------------
run: all
	@fail=0; \
	for t in $(TESTS); do \
	  echo "========== $$t =========="; \
	  timeout -k 5 30 stdbuf -o0 -e0 ./$(BUILD)/$$t; rc=$$?; \
	  if [ $$rc -eq 124 ] || [ $$rc -eq 137 ]; then \
	    echo "   [HANG] $$t 超过 30s 未返回，已强制终止"; fail=1; \
	  elif [ $$rc -ne 0 ]; then \
	    fail=1; \
	  fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "==== 全部单元测试通过 ===="; \
	else echo "==== 存在失败/卡死用例 ===="; exit 1; fi

run-asan: | $(BUILD)
	@fail=0; \
	for t in $(TESTS); do \
	  echo "========== $$t (ASAN+UBSAN) =========="; \
	  $(CC) $(ASAN) -o $(BUILD)/$$t.asan $(SRC)/$$t.c $(POOL) || exit 1; \
	  ASAN_OPTIONS=verify_asan_link_order=0 timeout -k 5 60 stdbuf -o0 -e0 ./$(BUILD)/$$t.asan; rc=$$?; \
	  if [ $$rc -ne 0 ]; then fail=1; fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "==== ASAN 构建下全部通过 ===="; \
	else echo "==== ASAN 构建下存在失败 ===="; exit 1; fi

#------------------------------- 压力测试 -------------------------------
$(BUILD)/stress_memory_pool: $(SRC)/stress_memory_pool.c $(POOL) | $(BUILD)
	$(CC) $(CFLAGS) -pthread -o $@ $^

stress: $(BUILD)/stress_memory_pool
	@echo "========== stress_memory_pool: local（每线程独立池，与 c_web 用法一致）=========="
	@timeout -k 5 120 stdbuf -o0 -e0 ./$(BUILD)/stress_memory_pool local $${STRESS_THREADS:-4} $${STRESS_OPS:-1000000}
	@echo "========== stress_memory_pool: shared（共享池 + 外部 mutex）=========="
	@timeout -k 5 120 stdbuf -o0 -e0 ./$(BUILD)/stress_memory_pool shared $${STRESS_THREADS:-4} $${STRESS_OPS:-300000}

stress-asan: | $(BUILD)
	@echo "========== stress_memory_pool local (ASAN+UBSAN) =========="
	$(CC) $(ASAN) -pthread -o $(BUILD)/stress_memory_pool.asan $(SRC)/stress_memory_pool.c $(POOL)
	ASAN_OPTIONS=verify_asan_link_order=0 timeout -k 5 300 stdbuf -o0 -e0 ./$(BUILD)/stress_memory_pool.asan local 2 200000

stress-tsan: | $(BUILD)
	@echo "========== stress_memory_pool shared (ThreadSanitizer，验证外部锁足够) =========="
	$(CC) $(CFLAGS) -fsanitize=thread -pthread -o $(BUILD)/stress_memory_pool.tsan $(SRC)/stress_memory_pool.c $(POOL)
	timeout -k 5 300 stdbuf -o0 -e0 ./$(BUILD)/stress_memory_pool.tsan shared 4 50000

#------------------------------- 性能基准 -------------------------------
$(BUILD)/bench_memory_pool: $(SRC)/bench_memory_pool.c $(POOL) | $(BUILD)
	$(CC) $(BENCHF) -o $@ $^

bench: $(BUILD)/bench_memory_pool
	@./$(BUILD)/bench_memory_pool $${BENCH_MUL:-1}

#-------- 与老池子（otherPool = cWeb 现在列装的那套）的对比基准 --------
# 两套池子的同名符号靠 -D 重命名区分：老池子那两 TU 单独编译，bench 主程序不参与重命名
#   老池(原样)  → Old*          老池(去锁) → OldNoLock*   （去锁见 noop_mutex.h，仅测量用）
OTHER   := $(ROOT)/otherPool/MemoryPool/MemoryPool.c
# $(call REN,前缀) → 老池那一套符号的重命名（前缀取 Old / OldNoLock）
REN = -DMemoryPoolInit=$(1)PoolInit -DMemoryPoolAlloc=$(1)PoolAlloc -DMemoryPoolFree=$(1)PoolFree \
      -DMemoryEntryInit=$(1)EntryInit -DMemoryEntryAlloc=$(1)EntryAlloc -DMemoryEntryAllocTwo=$(1)EntryAllocTwo \
      -DMemoryEntryFree=$(1)EntryFree -DMemoryEntryExpend=$(1)EntryExpend \
      -DMemoryStackInit=$(1)StackInit -DMemoryStackTopAndPop=$(1)StackTopAndPop \
      -DMMemoryStackPush=$(1)StackPush -DMemoryStackExpend=$(1)StackExpend

$(BUILD)/old_pool_renamed.o: $(OTHER) | $(BUILD)
	$(CC) $(BENCHF) $(call REN,Old) -c $< -o $@

$(BUILD)/old_pool_nolock.o: $(OTHER) $(SRC)/noop_mutex.h | $(BUILD)
	$(CC) $(BENCHF) $(call REN,OldNoLock) -include $(SRC)/noop_mutex.h -c $< -o $@

$(BUILD)/old_pool_shim.o: $(SRC)/old_pool_shim.c | $(BUILD)
	$(CC) $(BENCHF) $(call REN,Old) -DSHIM_PREFIX=Old -c $< -o $@

$(BUILD)/old_pool_shim_nolock.o: $(SRC)/old_pool_shim.c | $(BUILD)
	$(CC) $(BENCHF) $(call REN,OldNoLock) -DSHIM_PREFIX=OldNoLock -c $< -o $@

$(BUILD)/bench_other_pool: $(SRC)/bench_other_pool.c $(BUILD)/old_pool_renamed.o $(BUILD)/old_pool_nolock.o \
                           $(BUILD)/old_pool_shim.o $(BUILD)/old_pool_shim_nolock.o $(POOL) | $(BUILD)
	$(CC) $(BENCHF) -o $@ $(SRC)/bench_other_pool.c $(BUILD)/old_pool_renamed.o $(BUILD)/old_pool_nolock.o \
	      $(BUILD)/old_pool_shim.o $(BUILD)/old_pool_shim_nolock.o $(POOL) -lpthread

bench-other: $(BUILD)/bench_other_pool
	@./$(BUILD)/bench_other_pool

run-all: run stress bench bench-other

clean:
	rm -f $(BUILD)/*

.PHONY: all run run-asan stress stress-asan stress-tsan bench bench-other run-all clean
