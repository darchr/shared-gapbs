#ifndef GRAPH_SHMEM_H_
#define GRAPH_SHMEM_H_

#include <stddef.h>
#include <stdint.h>

#include "shm_alloc.h"

#define GRAPH_SHMEM_ONE_G 0x40000000ull

#define GRAPH_SHMEM_REGION_POSIX "/gapbs_pool"
#define GRAPH_SHMEM_GRAPH_NAME   "gapbs_csr"

#define GRAPH_SHMEM_UID_ALLOCATOR 0u

#ifdef __cplusplus
extern "C" {
#endif

shm_region_t *graph_shmem_region(void);

int graph_shmem_open(int host_id, size_t size_gib, int test_mode, int create);

int graph_shmem_graph_base(int host_id, size_t payload_bytes, int **out_base);

void graph_shmem_close_region(void);

void graph_shmem_reset_region(size_t size_gib, int test_mode);

void graph_shmem_flush_cache(int *base, size_t nbytes);

#ifdef __cplusplus
}
#endif

#endif  // GRAPH_SHMEM_H_
