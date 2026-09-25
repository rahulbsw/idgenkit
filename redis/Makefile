CC     ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c99 -D_DEFAULT_SOURCE -fPIC -Wall -Wextra -I../c
BUILD  := build

ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -bundle -undefined dynamic_lookup
else
  LDFLAGS += -shared
endif

.PHONY: all test bench clean

all: $(BUILD)/idgenkit.so

$(BUILD)/idgenkit.so: idgenkit_module.c redismodule_min.h ../c/idgenkit.c ../c/idgenkit.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ idgenkit_module.c ../c/idgenkit.c $(LDFLAGS)

test: all
	./test.sh

bench: all
	./bench.sh

clean:
	rm -rf $(BUILD)
