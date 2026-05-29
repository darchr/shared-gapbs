// Shared-memory backing for the CSR graph via ext/disagg-shmem-allocator.

#include "graph_shmem.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace {

shm_region_t *g_region = nullptr;

shm_user_id_t graph_uid(int host_id) {
  return host_id == 0 ? GRAPH_SHMEM_UID_ALLOCATOR
                      : static_cast<shm_user_id_t>(host_id);
}

shm_perm_t worker_lookup_perms(void) {
  return static_cast<shm_perm_t>(SHM_PERM_READ | SHM_PERM_ADMIN);
}

shm_perm_t worker_ptr_perms(void) {
  return static_cast<shm_perm_t>(SHM_PERM_READ | SHM_PERM_ADMIN);
}

shm_perm_t allocator_perms(void) {
  return static_cast<shm_perm_t>(SHM_PERM_READ | SHM_PERM_WRITE);
}

const char *region_path(int test_mode) {
  return test_mode ? GRAPH_SHMEM_REGION_POSIX : "/dev/dax0.0";
}

shm_backend_t region_backend(int test_mode) {
  return test_mode ? SHM_BACKEND_POSIX : SHM_BACKEND_DAX;
}

size_t region_bytes(size_t size_gib) {
  return static_cast<size_t>(size_gib) * GRAPH_SHMEM_ONE_G;
}

void die(const char *msg, int rc) {
  std::fprintf(stderr, "graph_shmem: %s (rc=%d)\n", msg, rc);
  std::exit(EXIT_FAILURE);
}

}  // namespace

extern "C" {

shm_region_t *graph_shmem_region(void) { return g_region; }

int graph_shmem_open(int host_id, size_t size_gib, int test_mode, int create) {
  if (g_region != nullptr)
    return 0;

  shm_region_open_opts_t opts = {};
  opts.backend = region_backend(test_mode);
  opts.flags = create ? SHM_OPEN_CREATE : 0u;
  opts.dir_capacity = 64;

  int rc = shm_region_open(region_path(test_mode), region_bytes(size_gib),
                           &opts, &g_region);
  if (rc != 0)
    die("shm_region_open failed", rc);

  if (host_id == 0 && create)
    std::printf("info: opened shared region (%s, %zu GiB)\n",
                region_path(test_mode), size_gib);
  else if (host_id != 0)
    std::printf("info: attached to shared region (%s)\n",
                region_path(test_mode));

  return 0;
}

int graph_shmem_graph_base(int host_id, size_t payload_bytes, int **out_base) {
  if (!g_region || !out_base)
    return EINVAL;

  shm_off_t off = SHM_OFF_NULL;
  shm_user_id_t uid = graph_uid(host_id);

  if (host_id == 0) {
    uint64_t id = 0;
    int rc = shm_alloc(g_region, payload_bytes, GRAPH_SHMEM_GRAPH_NAME, uid,
                       allocator_perms(), 0, &id, &off);
    if (rc != 0)
      die("shm_alloc graph failed", rc);
    std::printf("info: allocated graph object id=%llu (%zu bytes)\n",
                static_cast<unsigned long long>(id),
                payload_bytes);
  } else {
    uint64_t id = 0;
    int rc = ENOENT;
    for (int retry = 0; retry < 600000 && rc == ENOENT; retry++) {
      rc = shm_lookup_by_name(g_region, GRAPH_SHMEM_GRAPH_NAME, uid,
                              worker_lookup_perms(), &id, &off);
      if (rc == ENOENT) {
        if (retry == 0)
          std::printf("info: waiting for graph object in shared memory...\n");
        usleep(1000);
      }
    }
    if (rc != 0)
      die("shm_lookup_by_name graph failed", rc);
    std::printf("info: attached to graph object id=%llu\n",
                static_cast<unsigned long long>(id));
  }

  void *ptr = nullptr;
  if (host_id == 0) {
    ptr = shm_ptr(g_region, off, uid, allocator_perms(), SHM_PERM_WRITE);
  } else {
    ptr = shm_ptr(g_region, off, uid, worker_ptr_perms(), SHM_PERM_READ);
  }
  if (!ptr)
    die("shm_ptr graph failed", EINVAL);

  *out_base = static_cast<int *>(ptr);
  return 0;
}

void graph_shmem_close_region(void) {
  if (!g_region)
    return;
  shm_region_close(g_region, false);
  g_region = nullptr;
}

void graph_shmem_reset_region(size_t size_gib, int test_mode) {
  if (g_region) {
    shm_region_close(g_region, test_mode != 0);
    g_region = nullptr;
  }
  graph_shmem_open(0, size_gib, test_mode, 1);
}

void graph_shmem_flush_cache(int *base, size_t nbytes) {
  if (!base || nbytes == 0)
    return;
  auto *start = reinterpret_cast<volatile char *>(base);
#pragma omp parallel for
  for (size_t i = 0; i < nbytes; i += 64)
    asm volatile("clflushopt (%0)\n" ::"r"(start + i) : "memory");
}

}  // extern "C"
