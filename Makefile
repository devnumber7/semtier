CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -std=c11
CPPFLAGS := -Iinclude -Isrc -D_DARWIN_C_SOURCE -D_GNU_SOURCE $(CPPFLAGS)
LDFLAGS ?=
LDLIBS ?= -pthread

SRC := src/semtier.c src/ring.c src/numa_policy.c
OBJ := $(SRC:.c=.o)

.PHONY: all clean test

all: libsemtier.a tools/semtier-drain bench/pointer-chase

libsemtier.a: $(OBJ)
	$(AR) rcs $@ $(OBJ)

tools/semtier-drain: tools/semtier_drain.o libsemtier.a
	$(CC) $(LDFLAGS) -o $@ tools/semtier_drain.o libsemtier.a $(LDLIBS)

bench/pointer-chase: bench/pointer_chase.o libsemtier.a
	$(CC) $(LDFLAGS) -o $@ bench/pointer_chase.o libsemtier.a $(LDLIBS)

test: all
	SEMTIER_EVENT_LOG=/tmp/semtier-events.jsonl ./bench/pointer-chase --mode semtier --bench chase --nodes 10000 --iters 2
	./bench/pointer-chase --mode malloc --bench stream --nodes 10000 --iters 2

clean:
	rm -f $(OBJ) tools/semtier_drain.o bench/pointer_chase.o
	rm -f libsemtier.a tools/semtier-drain bench/pointer-chase
