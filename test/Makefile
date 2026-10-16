# CWebMemoryPool 单元测试
# 每个用例独立可执行，只链接被测模块（MemoryPool.c）。
# 用法： make -C test run        （构建 + 运行，30s 超时保护，卡死判定为 HANG）
#        make -C test run-asan   （AddressSanitizer + UndefinedBehaviorSanitizer）
#        make -C test clean

ROOT   := ..
CC     := gcc
CFLAGS := -g -O0 -Wall -I$(ROOT)
ASAN   := -g -O0 -Wall -I$(ROOT) -fsanitize=address,undefined
SRC    := src
BUILD  := build
POOL   := $(ROOT)/MemoryPool.c
TESTS  := test_memory_pool

all: $(addprefix $(BUILD)/,$(TESTS))

$(BUILD)/%: $(SRC)/%.c $(POOL) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD):
	mkdir -p $(BUILD)

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

clean:
	rm -f $(BUILD)/*

.PHONY: all run run-asan clean
