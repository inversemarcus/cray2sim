# Build the CRAY-2 emulator and run its self-tests.
ARCH := $(shell uname -m)
ifeq ($(ARCH),arm64)
  CPUFLAG := -mcpu=native
else
  CPUFLAG := -march=native
endif
CC ?= cc
CFLAGS ?= -O3 $(CPUFLAG)

all: sim/cray2sim

sim/cray2sim: sim/cray2sim.c sim/cray2fp.c sim/cray2fp.h
	$(CC) $(CFLAGS) -pthread -o $@ sim/cray2sim.c sim/cray2fp.c -lm

tests/test_fp: tests/test_fp.c sim/cray2fp.c sim/cray2fp.h
	$(CC) -O2 -Isim -o $@ tests/test_fp.c sim/cray2fp.c -lm

test: sim/cray2sim tests/test_fp
	./tests/test_fp
	python3 tests/test_asm.py

clean:
	rm -f sim/cray2sim tests/test_fp

.PHONY: all test clean
