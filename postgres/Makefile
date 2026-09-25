EXTENSION  = idgenkit
MODULE_big = idgenkit
OBJS       = src/idgenkit_pg.o src/idgenkit_core.o
DATA       = idgenkit--0.1.0.sql
PG_CPPFLAGS = -I$(CURDIR)/../c

REGRESS      = idgenkit
REGRESS_OPTS = --inputdir=test --outputdir=test

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

src/idgenkit_core.o: ../c/idgenkit.c ../c/idgenkit.h
src/idgenkit_pg.o: ../c/idgenkit.h
