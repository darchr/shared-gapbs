# See LICENSE.txt for license details.

CXX_FLAGS += -std=c++11 -O3 -Wall -DM5OP_ADDR=0xFFFF0000 -g
PAR_FLAG = -fopenmp
CC = gcc

CCOMPILE = $(CC)  -c $(C_INC) $(CFLAGS)
COMMON = src
GEM5DIR = gem5

ifneq (,$(findstring icpc,$(CXX)))
	PAR_FLAG = -openmp
endif

ifneq (,$(findstring sunCC,$(CXX)))
	CXX_FLAGS = -std=c++11 -xO3 -m64 -xtarget=native
	PAR_FLAG = -xopenmp
endif

ifneq ($(SERIAL), 1)
	CXX_FLAGS += $(PAR_FLAG)
endif

KERNELS = allocator bc bfs cc cc_sv pr sssp tc
SUITE = $(KERNELS) converter

.PHONY: all
all: $(SUITE)


${COMMON}/hooks.o: ${COMMON}/hooks.c
	cd ${COMMON}; ${CCOMPILE} hooks.c -Wno-implicit-function-declaration -I ${GEM5DIR}/include/
${COMMON}/m5op_x86.o: ${COMMON}/m5op_x86.S
	cd ${COMMON}; gcc -O2 m5op_x86.S -DM5OP_ADDR=0xFFFF0000 -I ${GEM5DIR}/include/ -o ../$@ -c 
${COMMON}/m5_mmap.o: ${COMMON}/m5_mmap.c
	cd ${COMMON}; ${CCOMPILE} ${COMMON}/m5_mmap.c -Wno-implicit-function-declaration -I ${GEM5DIR}/include/
OBJS =
ifeq (${HOOKS}, 1)
        OBJS += ${COMMON}/m5op_x86.o
endif

ifeq (${HOOKS}, 1)
       CXX_FLAGS += -DHOOKS
endif

SHM_ALLOC_DIR = ext/disagg-shmem-allocator
SHM_ALLOC_INC = -I$(SHM_ALLOC_DIR)/include -D_POSIX_C_SOURCE=200809L
SHM_ALLOC_OBJS = $(SHM_ALLOC_DIR)/build/shm_alloc.o $(SHM_ALLOC_DIR)/build/shm_ns.o
SHM_ALLOC_LIBS = -pthread -lrt
GRAPH_SHMEM_SRC = $(COMMON)/graph_shmem.cc

$(SHM_ALLOC_OBJS): $(SHM_ALLOC_DIR)/Makefile
	$(MAKE) -C $(SHM_ALLOC_DIR) build/shm_alloc.o build/shm_ns.o

% : src/%.cc src/*.h $(OBJS) $(SHM_ALLOC_OBJS) $(GRAPH_SHMEM_SRC)
	$(CXX) $(CXX_FLAGS) $(SHM_ALLOC_INC) $< $(GRAPH_SHMEM_SRC) -o $@ \
		$(COMMON)/m5_mmap.c $(OBJS) $(SHM_ALLOC_OBJS) $(SHM_ALLOC_LIBS) -no-pie

# Testing
include test/test.mk

# Benchmark Automation
include benchmark/bench.mk


.PHONY: clean
clean:
	rm -f $(SUITE) test/out/*
	rm -f src/hooks.o
	rm -r src/m5op_x86.o
