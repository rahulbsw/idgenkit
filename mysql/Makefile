CC           ?= cc
MYSQL_CONFIG ?= mysql_config
CFLAGS       ?= -O2
CFLAGS       += -std=gnu99 -fPIC -Wall -Wextra -I../c $(shell $(MYSQL_CONFIG) --include)
BUILD        := build

ifeq ($(shell uname -s),Darwin)
  LDFLAGS += -bundle -undefined dynamic_lookup
else
  LDFLAGS += -shared
endif

.PHONY: all install test bench clean

all: $(BUILD)/idgenkit_udf.so

$(BUILD)/idgenkit_udf.so: idgenkit_udf.c ../c/idgenkit.c ../c/idgenkit.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ idgenkit_udf.c ../c/idgenkit.c $(LDFLAGS)

install: all
	cp $(BUILD)/idgenkit_udf.so "$$($(MYSQL_CONFIG) --plugindir)/"

test: all
	./test.sh

bench: all
	./test.sh --bench

clean:
	rm -rf $(BUILD)
