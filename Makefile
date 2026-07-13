# See LICENSE.txt for license details.

SHM_ALLOC_DIR = disagg-shmem-allocator
SHM_ALLOC_BUILD = $(SHM_ALLOC_DIR)/build
SHM_ALLOC_OBJS = $(SHM_ALLOC_BUILD)/shm_alloc.o $(SHM_ALLOC_BUILD)/shm_ns.o \
                 $(SHM_ALLOC_BUILD)/shm_cap.o
SHM_ALLOC_INC = -I$(SHM_ALLOC_DIR)/include -I$(SHM_ALLOC_DIR)/src
SHM_LDFLAGS = -pthread -lrt

# Cache persist for DAX/CXL (optional): make CACHE=CLWB or CACHE=CBO_CLEAN
CACHE ?=
ifneq ($(CACHE),)
  SHM_CACHE_FLAG = -DSHM_CACHE_$(CACHE)
else
  SHM_CACHE_FLAG =
endif

CXX_FLAGS += -std=c++11 -O3 -Wall -DM5OP_ADDR=0xFFFF0000 -g $(SHM_ALLOC_INC) $(SHM_CACHE_FLAG)
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

$(SHM_ALLOC_BUILD)/shm_alloc.o:
	$(MAKE) -C $(SHM_ALLOC_DIR) build/shm_alloc.o CACHE=$(CACHE)

$(SHM_ALLOC_BUILD)/shm_ns.o:
	$(MAKE) -C $(SHM_ALLOC_DIR) build/shm_ns.o CACHE=$(CACHE)

$(SHM_ALLOC_BUILD)/shm_cap.o:
	$(MAKE) -C $(SHM_ALLOC_DIR) build/shm_cap.o CACHE=$(CACHE)

% : src/%.cc src/*.h $(OBJS) $(SHM_ALLOC_OBJS)
	$(CXX) $(CXX_FLAGS) $< -o $@ ${COMMON}/m5_mmap.c $(OBJS) $(SHM_ALLOC_OBJS) $(SHM_LDFLAGS) -no-pie

# Testing
include test/test.mk

# Benchmark Automation
include benchmark/bench.mk


.PHONY: clean
clean:
	rm -f $(SUITE) test/out/*
	rm -f src/hooks.o src/m5op_x86.o
	$(MAKE) -C $(SHM_ALLOC_DIR) clean
