CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -O3
SRCS = field.c link.c source.c relay.c dest.c codec.c sim.c main.c psi_opt.c
LIB_SRCS = field.c source.c dest.c codec.c

.PHONY: all lib run clean cpu_bench walk

all: bats_sim psi_opt

bats_sim: $(SRCS) bats.h bats_codec.h bats_internal.h psi_builtin.h
	$(CC) $(CFLAGS) -o bats_sim $(SRCS) -lm

lib: libbats.a

libbats.a: $(LIB_SRCS) bats_codec.h bats_internal.h psi_builtin.h
	$(CC) $(CFLAGS) -c $(LIB_SRCS)
	ar rcs libbats.a field.o source.o dest.o codec.o

psi_opt: psi_opt.c
	$(CC) $(CFLAGS) -DPSI_OPT_MAIN -o psi_opt psi_opt.c -lm

run: bats_sim
	./bats_sim input.txt recovered.txt

cpu_bench: cpu_bench.c libbats.a
	$(CC) $(CFLAGS) -I../multi-flow-rate-control/apps/bats_line -o cpu_bench cpu_bench.c libbats.a

walk: walk_pattern.c libbats.a
	$(CC) $(CFLAGS) -I../multi-flow-rate-control/apps/bats_line -o walk_pattern walk_pattern.c libbats.a

clean:
	rm -f bats_sim bats_sim.exe psi_opt psi_opt.exe cpu_bench walk_pattern recovered.txt libbats.a *.o
