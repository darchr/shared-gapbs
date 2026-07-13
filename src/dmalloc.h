#ifndef DMALLOC_H_
#define DMALLOC_H_

// Legacy header — shared memory is managed by disagg-shmem-allocator via shm_graph.h.
#include "shm_graph.h"

inline void munmap_memory(size_t size_gib, int test_mode, int host_id) {
  gapbs_shm::ReinitRegion(host_id, test_mode, size_gib);
}

#endif  // DMALLOC_H_
