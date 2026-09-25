CC      ?= cc
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic
BUILD   := build

.PHONY: all test bench sanitize clean

all: $(BUILD)/test_idgenkit $(BUILD)/bench_idgenkit

$(BUILD):
	mkdir -p $@

$(BUILD)/test_idgenkit: test_idgenkit.c idgenkit.c idgenkit.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_idgenkit.c idgenkit.c -lpthread

$(BUILD)/bench_idgenkit: bench_idgenkit.c idgenkit.c idgenkit.h | $(BUILD)
	$(CC) $(CFLAGS) -O3 -o $@ bench_idgenkit.c idgenkit.c

test: $(BUILD)/test_idgenkit
	$(BUILD)/test_idgenkit ../testdata

sanitize: | $(BUILD)
	$(CC) -std=c99 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o $(BUILD)/test_sanitize test_idgenkit.c idgenkit.c -lpthread
	$(BUILD)/test_sanitize ../testdata

bench: $(BUILD)/bench_idgenkit
	$(BUILD)/bench_idgenkit

clean:
	rm -rf $(BUILD)
