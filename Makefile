# mini-rtos —— 单核抢占式调度 / 优先级反转复现
#
# make          编译
# make run      跑一遍，输出 trace 到 out/
# make report   用 trace 生成 docs/report.html（需要 python3）
# make all      编译 + 跑 + 出报告
# make clean    清理

# 注意：make 内置 CC=cc，用 ?= 覆盖不掉，所以这里直接赋 gcc（命令行传 CC=... 仍可覆盖）
CC      := gcc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
LDFLAGS ?= -lpthread
PY      := python

ifeq ($(OS),Windows_NT)
  BIN := build/mini_rtos.exe
else
  BIN := build/mini_rtos
endif

SRC := src/rtos.c src/main.c

all: run report

$(BIN): $(SRC) src/rtos.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDFLAGS)

build: $(BIN)

run: $(BIN)
	@mkdir -p out
	./$(BIN) --out out

report:
	$(PY) tools/report.py out docs/report.html

# 确定性自检：连跑 5 次，指标必须完全一致（不一致时 make 直接失败）
soak: $(BIN)
	@mkdir -p out
	./$(BIN) --out out --repeat 5

clean:
	rm -rf build out

.PHONY: all build run report soak clean
