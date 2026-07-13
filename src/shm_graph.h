#ifndef SHM_GRAPH_H_
#define SHM_GRAPH_H_

// Shared-graph region management via disagg-shmem-allocator (capability API).
// Backends: POSIX shm (/dev/shm) for local testing, /dev/dax for CXL.

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sched.h>

extern "C" {
#include "shm_alloc.h"
#include "shm_cap.h"
#include "shm_persist.h"
}

#define GAPBS_ONE_G          0x40000000ULL
#define GAPBS_SHM_POSIX_NAME "/gapbs_sdm"
#define GAPBS_SHM_DAX_PATH   "/dev/dax0.0"

#define GAPBS_OBJ_SYNC      "gapbs_sync"
#define GAPBS_OBJ_META      "gapbs_meta"
#define GAPBS_OBJ_ROW_LENS  "gapbs_row_lens"
#define GAPBS_OBJ_NEIGHS    "gapbs_neighs"
#define GAPBS_OBJ_INDEX     "gapbs_index"

struct GapbsSync {
  volatile int ready;
};

struct GapbsMeta {
  int64_t num_nodes;
  size_t index_x;
  size_t neigh_size;
};

namespace gapbs_shm {

inline shm_user_id_t HostUid(int host_id) {
  return static_cast<shm_user_id_t>(host_id);
}

inline shm_perm_t WriterCallerPerms() {
  return static_cast<shm_perm_t>(SHM_PERM_DEFAULT);
}

inline shm_perm_t ReaderCallerPerms() {
  return static_cast<shm_perm_t>(SHM_PERM_READ | SHM_PERM_ADMIN);
}

inline shm_perm_t RwRequired() {
  return static_cast<shm_perm_t>(SHM_PERM_READ | SHM_PERM_WRITE);
}

inline shm_perm_t RoRequired() {
  return SHM_PERM_READ;
}

inline void PersistRange(const void *addr, size_t len) {
  shm_persist(addr, len);
  shm_drain();
}

class Region {
 public:
  shm_region_t *handle;
  int host_id;
  bool is_writer;

  Region() : handle(nullptr), host_id(-1), is_writer(false) {}

  static Region &Get() {
    static Region instance;
    return instance;
  }

  shm_user_id_t Uid() const { return HostUid(host_id); }

  shm_perm_t CallerPerms() const {
    return is_writer ? WriterCallerPerms() : ReaderCallerPerms();
  }
};

inline const char *RegionPath(int test_mode) {
  return test_mode ? GAPBS_SHM_POSIX_NAME : GAPBS_SHM_DAX_PATH;
}

inline void OpenRegion(int host_id, int test_mode, size_t size_gib) {
  Region &reg = Region::Get();
  reg.host_id = host_id;
  reg.is_writer = (host_id == 0);

  shm_region_open_opts_t opts = {};
  opts.backend = test_mode ? SHM_BACKEND_POSIX : SHM_BACKEND_DAX;
  opts.flags = reg.is_writer ? SHM_OPEN_CREATE : 0;
  opts.dir_capacity = 64;

  const size_t size_bytes = size_gib * GAPBS_ONE_G;
  const char *path = RegionPath(test_mode);

  int rc = shm_region_open(path, size_bytes, &opts, &reg.handle);
  if (rc != 0) {
    std::cerr << "error: shm_region_open(" << path << ") failed: "
              << strerror(rc) << " (code " << rc << ")\n";
    std::exit(EXIT_FAILURE);
  }

  std::cout << "info: opened shared region " << path
            << " (" << (test_mode ? "POSIX" : "DAX") << ", "
            << (reg.is_writer ? "writer" : "reader") << ")\n";
}

inline void ReinitRegion(int host_id, int test_mode, size_t size_gib) {
  if (host_id != 0) {
    std::cout << "warn: cannot reinit region; needs to be the allocator\n";
    return;
  }
  Region &reg = Region::Get();
  if (reg.handle) {
    shm_region_close(reg.handle, false);
    reg.handle = nullptr;
  }
  OpenRegion(host_id, test_mode, size_gib);
  std::cout << "info: shared region reinitialized\n";
}

inline shm_cap_t DeriveCap(shm_region_t *region, shm_off_t off, int host_id,
                           bool writer, shm_perm_t grant) {
  const shm_user_id_t uid = HostUid(host_id);
  const shm_perm_t caller = writer ? WriterCallerPerms() : ReaderCallerPerms();
  return shm_cap_derive(region, off, uid, caller, grant);
}

inline int AllocObject(shm_region_t *region, size_t payload_size,
                       const char *name, uint64_t *out_id, shm_off_t *out_off,
                       shm_cap_t *out_cap) {
  int rc = shm_alloc(region, payload_size, name, 0, WriterCallerPerms(), 0,
                     out_id, out_off);
  if (rc != 0)
    return rc;
  *out_cap = DeriveCap(region, *out_off, 0, true, RwRequired());
  if (shm_cap_is_null(*out_cap))
    return EINVAL;
  return 0;
}

inline int LookupObjectByName(shm_region_t *region, const char *name,
                              int host_id, shm_cap_t *out_cap,
                              shm_perm_t grant) {
  const shm_user_id_t uid = HostUid(host_id);
  const shm_perm_t caller = ReaderCallerPerms();
  uint64_t id = 0;
  shm_off_t off = 0;

  for (int retry = 0; retry < 500000; retry++) {
    int rc = shm_lookup_by_name(region, name, uid, caller, &id, &off);
    if (rc == 0) {
      *out_cap = DeriveCap(region, off, host_id, false, grant);
      if (shm_cap_is_null(*out_cap))
        return EINVAL;
      return 0;
    }
    sched_yield();
  }
  return ENOENT;
}

struct GraphCaps {
  shm_cap_t sync;
  shm_cap_t meta;
  shm_cap_t row_lens;
  shm_cap_t neighs;
  shm_cap_t index;
};

inline GraphCaps OpenGraphCaps(int host_id) {
  Region &reg = Region::Get();
  GraphCaps caps = {};
  const shm_perm_t grant = reg.is_writer ? RwRequired() : RoRequired();

  if (reg.is_writer)
    return caps;

  int rc = LookupObjectByName(reg.handle, GAPBS_OBJ_SYNC, host_id, &caps.sync,
                              grant);
  if (rc != 0) {
    std::cerr << "error: worker could not find " << GAPBS_OBJ_SYNC << "\n";
    std::exit(EXIT_FAILURE);
  }

  GapbsSync *sync = static_cast<GapbsSync *>(
      shm_cap_deref(reg.handle, caps.sync, RoRequired()));
  if (!sync) {
    std::cerr << "error: invalid sync capability\n";
    std::exit(EXIT_FAILURE);
  }

  std::cout << "info: waiting for master\n";
  while (sync->ready != 1)
    sched_yield();
  std::cout << "info: master has published the graph\n";

  const char *names[] = {GAPBS_OBJ_META, GAPBS_OBJ_ROW_LENS, GAPBS_OBJ_NEIGHS,
                         GAPBS_OBJ_INDEX};
  shm_cap_t *cap_slots[] = {&caps.meta, &caps.row_lens, &caps.neighs,
                            &caps.index};

  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    rc = LookupObjectByName(reg.handle, names[i], host_id, cap_slots[i],
                            grant);
    if (rc != 0) {
      std::cerr << "error: worker could not find " << names[i] << "\n";
      std::exit(EXIT_FAILURE);
    }
  }

  return caps;
}

}  // namespace gapbs_shm

#endif  // SHM_GRAPH_H_
