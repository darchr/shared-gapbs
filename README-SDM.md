---
title: GAPBS Benchamark for Shared Disaggregated Memory for X86 ISA
shortdoc: >
    A modified GAPBS benchmark to separate graph allocation and graph kernels
    This work is based on the original GAPBS reference implementation by S. Beamer et al.
authors: ["kg"]
---

The original README of GAPBS can be found in `README.md`.

This work is based on GAPBS by S. Beamer et al. [1].
The version of GAPBS used is forked from https://github.com/darchr/gapbs
It already has gem5 hooks for architecture simulations.
Instead of a single system doing both the generation, allocation of the graph, and the graph kernel, we separate the allocation and the kernel.
The use case for this setup is in shared disaggregated memory (like CXL 3.0+), where there will be multiple hosts sharing memory.
Coherence across the hosts are software managed.
The allocator makes sure that the data is coherently written to the shared memory.
The reader hosts read the graph from the remote/shared memory and allocates data structures needed for doing the kernel locally.

The allocator can be configured to zero out the shared memory space in the beginning.
Shared memory is managed by the [`disagg-shmem-allocator`](disagg-shmem-allocator/) submodule (capability API). The writer persists graph data to the backing store after every write; see [disagg-shmem-allocator integration](#disagg-shmem-allocator-integration) for full details.

See the example section on how to get started ASAP!

## Limitations

1. This version of GAPBS only work with synthetic graphs.
2. The graphs cannot have weights (sssp cannot be executed).

## An explanation on how GAPBS work

TL;DR: This section is only important if you want to understand/make changes to the source.

I hope this section will be a good documentation for people who want to make changes to the GAPBS benchmark.

1. Each graph kernel has a separate source file (`src/bc.cc` `src/bfs.cc` `src/cc.cc` `src/pr.cc` `src/tc.cc` `src/sssp.cc`)
2. A graph is generated for each kernel first, based on the input provided by the user (`defined in src/command_line.h`).
3. To understand how a new command line argument is added, see the comments for specifying the host ID (-x).
4. The file `src/builder.cc` is pretty much responsible for creating the graph.
    1. There are a bunch of utility functions to get the meta-data of the graph.
    2. `pvector` is the data structures that GAPBS use everywhere which is defined in `src/pvector.h`.
    3. The template `DestID_` stores most of the graph information in *active* memory.
    4. The first method called in builder class (`BuilderBase`; will just refer to as the builder class) to create a graph is `MakeGraph()`.
    5. An `EdgeList` data type is used to create and store edge information of the graph.
    6. If you want to use a synthetic graph, the Generator class' `GenerateEL()` method randomly generates an edge list.
    7. In my understanding, the version of GAPBS that I used (from/for gem5), the random seeds are the same, making the graph same across different runs.
    8. The method `MakeGraphFromEL()` is called with the el object, which was just created.
    9. The variable `el` is simply passed around until the neighbors (neighs, `DestID_`) and index (index, `DestID_` x `DestID_`) arrays are generated.
    10. The neighs and index are created in `MakeGraphFromEL()` as null pointers in the beginning.
    11. A synthetically generated graph will NOT have the inv_* variables used (unless specified I guess).
    12. A graph read from a file WILL have them set.
    13. `needs_weights` is pretty much relevant for `sssp`.
    14. `MakeCSR()` method assigns values to the neighs and index arrays.
    15. The size of neighs is `offsets[num_nodes_]`.
    16. The number of rows that index has is `offsets.size()`.
    17. The length of each row is variable.
    18. `MakeCSR()` method creates an unoptimized copy of the graph in the memory.
    19. The flow then goes back to the `MakeGraph()` method.
    20. The `el` variable is destroyed as soon as the arrays are stored in the memory.
    21. The Graph class object `g` is initially initialized to pretty much NULL (see `src/graph.h` `Graph::CSRGraph()` constructor).
    22. Command line parameters that are relevant are: graph file name (filename()), uniform(), `in_place_`, directed(), and weights.
    23. There is an extra scope added to make sure that the `EdgeList` object `el` is deleted after the arrays are generated for the first time.
    24. From what I understand, the Graph object stores the neighs and index within its scope.
    25. `SquishGraph(Graph object)` is called with the Graph object `g`, which then calls `SquishCSR` method since the graph is in CSR format.
    26. Note that there are DestID_ pointers to the neighs and index arrays, which are used to squish the graph.
    27. By squishing (in `SquishCSR()`), the author means to create an *optimized* copy of the graph to store in the memory (I might be wrong).
    28. The final versions of neighs and index are generated in `SquishCSR()` method.
    29. The builder class' objectives are done.
5. The graph's structure as a class is written in Graph class.
    1. The flow then goes to the Graph class in `src/graph.c`.
    2. The specific constructor that gets called is `CSRGraph::CSRGraph(int64_t num_nodes, DestID_** index, DestID_* neighs)` for this specific example.
    3. The in_* and out_* variables are the same in this case as the graph is undirected.
    4. The number of rows in index is +1 than the number of nodes.
    5. The number of edges *should* be half of the neighs' size.
    6. The number of edges is calculated as the difference of the last + 1 row of the index array and the base of the same array.
6. The algorithm then kicks in!

## Making GAPBS disaggregated!

> **Historical note:** This section describes the original flat-mmap design. The **current** implementation uses `disagg-shmem-allocator` — see [disagg-shmem-allocator integration](#disagg-shmem-allocator-integration).

TL;DR: The idea here to separate the allocation and the kernel part of the benchmark.

Here is what I did:
1. A new command line parameter `-x` is added to allow the user to specify whether a given host is a writer (allocator) or a worker (reader) node.
2. This is defined in `command_line.h` file. See the comments for more details.
3. TODO: The allocator *only* allocates the graph. In the current implementation, the allocator also executes a kernel (low priority, but I'll separate them).
4. Suppose the allocator host (referred to as the writer) wants to allocate and run `bfs`, it'll generate the edge list and create the neighs and index arrays.
5. Instead of simply starting the kernel, the Graph class is extended to write the graph into a shared memory space.
6. For single host testing, we use Linux shmem.
7. An allocator specific file is added to the source in `src/dmalloc.h`.
8. Function `dmalloc(size, host_id)` mounts the shared memory region in the /dev/dax region in dmalloc.h, `hmalloc(size, host_id)` uses huge pages to allocate a 1 GiB huge page for the allocation and `shmalloc(size, host_id)` uses shmem.
9. The Graph class constructor will store the pointer to the shared memory region as a private class variable.
10. The builder class is modified such that only the writer is allowed to generate the edge list, populate the neighs and index arrays and squish the CSR graph.
11. The worker nodes (or hosts or processes) initialized the Graph class object `g` as an empty object.
12. In both cases, the Graph constructor is called with host information.
13. The pointer to the mmapped region initialized when the Graph class constructor with the final neighs and index is created (at the end of the builder class).
14. If the writer calls the constructor, it writes some metadata, the neigh array, length of each row of the index array and finally a 2-D index array. See Figure 1 for a better visualization.
15. The `synch. variable` is unset when the allocator is writing the graph. It is set only when everything is written into the shared memory region.
16. The worker nodes will wait until the graph is fully written into the shared memory region (pooling into the (int) of the start of the mmap pointer). A potential optimization is to use something like `m_wait`.
17. The allocator needs to have the `neigh_size` as the length of C pointers are not defined. Same thing for the index array. This is passed as an argument to the class constructor.
18. If the aforementioned values are undefined or -1 and the host ID > 0 (aka worker node), the sizes are read out of the shared memory.
19. The writer figures out the size of each row of the index array and then writes them into the shared memory (column size for each index row array).
20. The reader reads out the same values before assigning pointers to the index array.
21. In both of these cases, the neighs are read out from `out_neighbors_` class variable and the index is read out from `in_index_` class variable when performing kernel related operations.

```
Figure 1. Map of the mmapped region.
__________________________________________________________________________________________________________________________________________________ .. __
|                 |           |              |            |         |                                 |          |    |          |            |        |
| synch. variable | num_nodes | index_x size | neigh_size | *neighs | *column size for each index row | index[0] | .. | index[N] | index[N+1] | unused |
|                 |           |              |            |  array  |               array             |   row 0  |    |   row N  |   row N+1  |        |
|                 |           |              |            |         |                                 |__________|____|__________|____________|        |
|      int        |  int64_t  |    size_t    |   size_t   | DestID_ |               size_t            |         DestID_ x DestID_             |        |
|_________________|___________|______________|____________|_________|_________________________________|_______________________________________|________|
<---------------- metadata of the graph -----------------><-------------------------------- graph data -------------------------------------->
```

<---------------- metadata of the graph -----------------><-------------------------------- graph data -------------------------------------->
```

## disagg-shmem-allocator integration

This section documents the current shared-memory stack. It replaces the older hardcoded `dmalloc` / `shmalloc` / flat-mmap layout described above in sections *Making GAPBS disaggregated!* and *Figure 1*. The old approach manually `mmap`'d `/dev/dax0.0` or `/my_shmem2`, laid out graph fields at fixed byte offsets, and flushed the entire region with `clflushopt`. The new approach delegates region management, object discovery, permissions, and cache persistence to the [`disagg-shmem-allocator`](disagg-shmem-allocator/) git submodule.

### Summary of what changed

| Topic | Old (`dmalloc.h`) | New (`shm_graph.h` + submodule) |
|-------|-------------------|----------------------------------|
| Region backend | Hardcoded `/dev/dax0.0` or `shm_open("/my_shmem2")` | `SHM_BACKEND_DAX` or `SHM_BACKEND_POSIX` via `shm_region_open()` |
| Graph layout | Single flat `mmap` with manual offset math | Five named heap objects in an object directory |
| Cross-host handles | Raw pointers into `mmap` base | `shm_cap_t` 128-bit capabilities (`shm_cap_derive` / `shm_cap_deref`) |
| Worker discovery | Spin on `int` at byte offset 0 | Lookup `gapbs_sync` by name; spin on `GapbsSync::ready` |
| Coherence / persist | `clflushopt` over entire GiB region | `shm_persist()` + `shm_drain()` per written range (configurable `CLWB` / `cbo.clean`) |
| Build | Header-only `dmalloc.h` | Links `shm_alloc.o`, `shm_ns.o`, `shm_cap.o` |

Key source files:

| File | Role |
|------|------|
| `disagg-shmem-allocator/` | Submodule: region allocator, object directory, capability layer, cache persist |
| `src/shm_graph.h` | GAPBS-specific wrapper: region open, object names, capability helpers |
| `src/graph.h` | CSR graph constructor: allocates objects, writes graph, derives capabilities |
| `src/dmalloc.h` | Legacy shim; `munmap_memory()` now calls `gapbs_shm::ReinitRegion()` |
| `Makefile` | Builds and links the allocator library; optional `CACHE=` flag |

### Submodule setup

The allocator is a git submodule. After cloning the repository:

```sh
git submodule update --init --recursive
```

Build everything (GAPBS + allocator objects):

```sh
make -j
```

Build allocator tests only:

```sh
cd disagg-shmem-allocator
make test
```

### Region backends

The `-T` flag selects the backend. All processes participating in a run **must use the same backend and region path**.

| `-T` | Backend | Region path | Typical use |
|------|---------|-------------|-------------|
| `0` (default) | `SHM_BACKEND_DAX` | `/dev/dax0.0` | CXL / disaggregated persistent memory (production) |
| `1` | `SHM_BACKEND_POSIX` | `/gapbs_sdm` → `/dev/shm/gapbs_sdm` | Single-host development and CI-style testing |

Constants are defined in `src/shm_graph.h`:

```c
#define GAPBS_SHM_POSIX_NAME "/gapbs_sdm"   // appears as /dev/shm/gapbs_sdm
#define GAPBS_SHM_DAX_PATH   "/dev/dax0.0"
```

**Writer (host 0)** opens the region with `SHM_OPEN_CREATE`, which initializes the region header and object directory.

**Workers (host ≥ 1)** open the same path **without** `SHM_OPEN_CREATE` and discover existing objects through the directory.

### Graph object layout

Instead of one contiguous mmap with fixed offsets (old Figure 1), the graph is split into five separately allocated, named objects:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  shm_region  (/dev/shm/gapbs_sdm  or  /dev/dax0.0)                          │
│  ┌──────────────┬────────────────────┬──────────────────────────────────┐ │
│  │ region hdr   │ object directory   │ heap (5 live blocks)             │ │
│  └──────────────┴────────────────────┴──────────────────────────────────┘ │
│                                                                             │
│  Named objects (registered in directory):                                   │
│                                                                             │
│    "gapbs_sync"       GapbsSync   { volatile int ready; }                   │
│                       ready = 0 while writer is populating; 1 when done     │
│                                                                             │
│    "gapbs_meta"       GapbsMeta   { int64_t num_nodes;                      │
│                                     size_t index_x;                         │
│                                     size_t neigh_size; }                    │
│                                                                             │
│    "gapbs_row_lens"   size_t[index_x]   per-row length of index rows        │
│                                                                             │
│    "gapbs_neighs"     DestID_[neigh_size]   flat neighbor array             │
│                                                                             │
│    "gapbs_index"      DestID_[sum of row lengths]   flat index row data     │
└─────────────────────────────────────────────────────────────────────────────┘
```

Workers do **not** need hardcoded offsets. They:

1. Open the region.
2. Look up `gapbs_sync` and spin until `ready == 1`.
3. Look up `gapbs_meta`, `gapbs_row_lens`, `gapbs_neighs`, `gapbs_index` by name.
4. Build local `out_index_` pointer rows pointing into the shared `gapbs_index` payload.

The local `out_index_` pointer array itself is still `malloc`'d per process (array of pointers into shared memory). Only the CSR payload arrays live in the shared region.

### Capability-based access control

All reads and writes into shared graph payloads go through the **capability API** (`include/shm_cap.h`). A `shm_cap_t` is a 128-bit value (offset + sealed permissions/bounds/epoch). `shm_cap_deref()` validates the capability on every access.

**Writer (host_id = 0)**

- `shm_user_id_t` = `0`
- Caller permissions: `SHM_PERM_DEFAULT` (`READ | WRITE | FREE | RESIZE`)
- Allocates objects with `gapbs_shm::AllocObject()`, derives read-write capabilities

**Worker (host_id ≥ 1)**

- `shm_user_id_t` = `host_id`
- Caller permissions: `SHM_PERM_READ | SHM_PERM_ADMIN` (required to read objects owned by host 0)
- Looks up objects by name, derives **read-only** capabilities (`SHM_PERM_READ` grant)

Example from `src/graph.h` (writer dereference):

```cpp
GapbsMeta *meta = shm_cap_deref_as(region_, meta_cap_, GapbsMeta,
                                   gapbs_shm::RwRequired());
meta->num_nodes = num_nodes;
gapbs_shm::PersistRange(meta, sizeof(GapbsMeta));
```

Workers never receive write capabilities on graph data. Kernel code (`bfs.cc`, `cc.cc`, etc.) only traverses the graph through `out_neigh()` / `in_neigh()` iterators and does **not** write into the shared CSR arrays.

### Cache persistence (CLWB / CBO)

On DRAM-backed POSIX shared memory, hardware keeps caches coherent and `shm_persist()` compiles to a compiler barrier only (default build).

On disaggregated / persistent memory (`/dev/dax`, CXL PMEM), stores may sit in CPU caches and must be explicitly written back. The allocator's `src/shm_persist.h` supports:

| `make CACHE=` | Instruction | Architecture |
|---------------|-------------|--------------|
| *(empty)* | No-op persist (barrier only) | Any; fine for POSIX `/dev/shm` testing |
| `CLFLUSH` | `clflush` | x86 |
| `CLFLUSHOPT` | `clflushopt` + `sfence` | x86 (Broadwell+) |
| `CLWB` | `clwb` + `sfence` | x86 (Cannon Lake+); **recommended for DAX** |
| `CBO_FLUSH` | `cbo.flush` + `fence` | RISC-V (Zicbom) |
| `CBO_CLEAN` | `cbo.clean` + `fence` | RISC-V (Zicbom); **recommended for RISC-V PMEM** |

**Where persists happen**

1. **Allocator metadata** — `disagg-shmem-allocator/src/shm_alloc.c` persists block headers, directory entries, and free-list mutations automatically.
2. **Graph writer** — `CSRGraph::assign_data()` in `src/graph.h` calls `gapbs_shm::PersistRange()` after writing:
   - row-length array (`gapbs_row_lens`)
   - flat index data (`gapbs_index`)
   - neighbor array (`gapbs_neighs`)
   - sync flag (`gapbs_sync`, set to `1` last)
3. **Kernels / workers** — **no writes** to shared graph memory; no persist required on the read path.

`gapbs_shm::PersistRange()` is a thin wrapper:

```cpp
inline void PersistRange(const void *addr, size_t len) {
  shm_persist(addr, len);
  shm_drain();
}
```

### Build flags

```sh
# Default: POSIX-safe, no explicit cache flush instructions
make -j

# DAX / CXL on x86 with CLWB support
make -j CACHE=CLWB

# DAX / CXL on RISC-V with Zicbom
make -j CACHE=CBO_CLEAN

# Serial build (no OpenMP)
make -j SERIAL=1

# gem5 hooks
make -j HOOKS=1
```

**Important:** If you build with `CACHE=CLWB` on a CPU that does not support `CLWB`, the binary will compile but **fault at runtime** with `SIGILL`. Use the default build for local `/dev/shm` testing; use `CACHE=CLWB` or `CACHE=CBO_CLEAN` only on hardware that supports the chosen instruction.

To force a clean rebuild after changing `CACHE=`:

```sh
rm -f disagg-shmem-allocator/build/*.o allocator bfs cc pr
make -j CACHE=CLWB
```

### Command-line flags (unchanged interface)

```
 -S <int>    : shared region size in GiB (e.g. 1, 16)
 -T <int>    : test mode — 0 = DAX /dev/dax0.0 [default], 1 = POSIX /dev/shm
 -l <int>    : reinitialize region before allocate — 0 [default], 1 = yes (writer only)
 -x <int>    : host id — 0 = writer/allocator, 1..n = worker/reader
 -g <scale>  : generate 2^scale Kronecker graph (synthetic)
 -u <scale>  : generate 2^scale uniform-random graph
```

All kernels and `allocator` require `-x` and `-S`. Workers must pass the same `-g`/`-u` and `-S` as the writer (they still enter `MakeGraph()` but skip edge-list generation).

---

### Example A — local testing with POSIX shared memory (no DAX)

Use this on any Linux machine for development. No special hardware required.

**1. Build**

```sh
git submodule update --init --recursive
make -j
```

**2. Remove any stale region from a previous run**

```sh
rm -f /dev/shm/gapbs_sdm
```

**3. Start the writer (allocator)**

Generates a scale-10 graph (~1024 nodes), allocates into a 1 GiB POSIX region, and publishes it:

```sh
./allocator -S 1 -l 1 -x 0 -g 10 -T 1
```

Expected log lines:

```
info: zeroing out the memory
info: opened shared region /gapbs_sdm (POSIX, writer)
info: shared region reinitialized
info: I am a writer/allocator node!
...
info: writing graph index into shared memory...
info: graph ready flag = 1
```

**4. Start workers (separate terminals or background)**

Wait until the allocator finishes before launching workers:

```sh
./bfs    -S 1 -x 1 -g 10 -T 1 -n1
./cc     -S 1 -x 2 -g 10 -T 1 -n1
./cc_sv  -S 1 -x 3 -g 10 -T 1 -n1
./bc     -S 1 -x 4 -g 10 -T 1 -n1
./pr     -S 1 -x 5 -g 10 -T 1 -n1
./tc     -S 1 -x 6 -g 10 -T 1 -n1
```

Or run several in parallel after the allocator completes:

```sh
./allocator -S 1 -l 1 -x 0 -g 10 -T 1
./bfs -S 1 -x 1 -g 10 -T 1 -n1 & \
./cc  -S 1 -x 2 -g 10 -T 1 -n1 & \
./pr  -S 1 -x 3 -g 10 -T 1 -n1 & \
wait
```

Expected worker log lines:

```
info: I am a worker node!
info: opened shared region /gapbs_sdm (POSIX, reader)
info: waiting for master
info: master has published the graph
info: graph ready flag = 1
```

**5. Larger local smoke test (scale 20, 16 GiB region)**

```sh
rm -f /dev/shm/gapbs_sdm
./allocator -S 16 -l 1 -x 0 -g 20 -T 1
./bfs -S 16 -x 1 -g 20 -T 1 -n4 &
./cc  -S 16 -x 2 -g 20 -T 1 -n4 &
wait
```

**6. Verify the allocator library**

```sh
cd disagg-shmem-allocator && make test
```

---

### Example B — production multi-host with `/dev/dax` (CXL / DAX)

Use this when a DAX device (e.g. `dax0.0`) is mapped on every host and all hosts can `mmap` the same device with cache-coherent or software-managed coherence.

**1. Prerequisites (on each host)**

- `/dev/dax0.0` exists and is mapped (check `ls -l /dev/dax0.0`)
- Sufficient device capacity for `-S <GiB>` (region size is fixed by the device; `ftruncate` is not used)
- CPU supports your chosen persist instruction if `CACHE=` is set

**2. Build with cache write-back for persistent memory**

```sh
git submodule update --init --recursive
make -j CACHE=CLWB        # x86 DAX / CXL PMEM
# make -j CACHE=CBO_CLEAN   # RISC-V alternative
```

**3. On the writer host (host 0)**

Do **not** pass `-T 1` (defaults to DAX). `-l 1` re-creates the region metadata:

```sh
./allocator -S 16 -l 1 -x 0 -g 20
```

Expected log:

```
info: opened shared region /dev/dax0.0 (DAX, writer)
...
info: graph ready flag = 1
```

**4. On each worker host**

Same graph parameters, unique `-x`, no `-T` flag:

```sh
# host 1
./bfs -S 16 -x 1 -g 20 -n16

# host 2
./cc -S 16 -x 2 -g 20 -n16

# host 3
./pr -S 16 -x 3 -g 20 -n16
```

Workers on remote machines open `/dev/dax0.0` locally; the allocator's object directory (at the start of the device image) tells them where each graph object lives. No hardcoded payload offsets are required.

**5. Single-machine DAX smoke test (if `/dev/dax0.0` is present locally)**

```sh
make -j CACHE=CLWB
./allocator -S 16 -l 1 -x 0 -g 16
./bfs -S 16 -x 1 -g 16 -n1
```

If `/dev/dax0.0` is missing, `shm_region_open` fails with an error pointing at the path.

---

### End-to-end control flow

```
Writer (host 0)                          Workers (host ≥ 1)
─────────────────                        ────────────────────
MakeGraph()
  Generate EL → CSR → SquishCSR
  OpenRegion(CREATE)
  AllocObject("gapbs_sync")     ───┐
  AllocObject("gapbs_meta")        │ objects registered in
  AllocObject("gapbs_row_lens")    │ shared object directory
  AllocObject("gapbs_neighs")      │
  AllocObject("gapbs_index")   ───┘
  assign_data() — write CSR
  PersistRange() per object
  sync.ready = 1
  PersistRange(sync)            ───►  OpenRegion(no CREATE)
                                      Lookup("gapbs_sync")
                                      spin until ready == 1
                                      Lookup(meta, row_lens,
                                             neighs, index)
                                      assign_sizes() — local
                                        pointer rows into
                                        shared payloads
                                      Run kernel (read-only
                                        graph access)
```

### `-l 1` (zero / reinit) behavior

With the new allocator, `-l 1` on the writer calls `gapbs_shm::ReinitRegion()`, which closes any existing handle and re-opens the region with `SHM_OPEN_CREATE`. This clears the object directory and heap — equivalent to the old "zero the mmap" behavior, but without manually memset'ing the entire GiB range.

Only host 0 may reinitialize. Workers passing `-l 1` are ignored with a warning.

### Troubleshooting

| Symptom | Likely cause | Fix |
|---------|--------------|-----|
| `shm_region_open(/dev/dax0.0) failed` | DAX device missing or permissions | Check `ls -l /dev/dax0.0`; run on a host with the device |
| `shm_region_open(/gapbs_sdm) failed: No such file` | Worker started before writer created region | Run `./allocator ...` first; ensure writer finished |
| `error: worker could not find gapbs_sync` | Stale or wrong region | `rm -f /dev/shm/gapbs_sdm` and rerun writer with `-l 1` |
| `Illegal instruction` after `CACHE=CLWB` build | CPU lacks CLWB | Rebuild without `CACHE=` for POSIX testing |
| Workers see wrong graph size | Mismatched `-g`/`-S`/`-T` vs writer | Use identical `-g`, `-S`, and `-T` on all processes |
| `make` does not rebuild after `CACHE=` change | Stale `.o` files | `rm -f disagg-shmem-allocator/build/*.o` then rebuild |

### Migrating from the old `dmalloc` mental model

| Old concept | New equivalent |
|-------------|----------------|
| `_mmap_pointer[0]` sync int | `gapbs_sync` object, `GapbsSync::ready` |
| `_mmap_pointer[1]` num_nodes | `gapbs_meta.num_nodes` |
| `dmalloc()` / `shmalloc()` | `gapbs_shm::OpenRegion()` |
| `flush_x86_cache(entire region)` | `gapbs_shm::PersistRange()` per written object |
| Fixed offset into `int[]` array | `shm_lookup_by_name("gapbs_neighs")` + capability deref |
| `PROT_READ`-only worker `mmap` | Read-only `shm_cap_t` + `SHM_PERM_ADMIN` caller perms |

The legacy header `src/dmalloc.h` is retained only so `allocator.cc` can still call `munmap_memory()`; all real logic lives in `src/shm_graph.h`.

---

## Example

> **Note:** The examples below are kept for quick reference. For full detail (build flags, object layout, capabilities, cache persist, troubleshooting), see [disagg-shmem-allocator integration](#disagg-shmem-allocator-integration) above.

### Quick start — POSIX `/dev/shm` (single machine, no DAX)

```sh
make -j
rm -f /dev/shm/gapbs_sdm

# Writer
./allocator -S 1 -l 1 -x 0 -g 10 -T 1

# Workers (after allocator finishes)
./bfs -S 1 -x 1 -g 10 -T 1 -n1 &
./cc  -S 1 -x 2 -g 10 -T 1 -n1 &
wait
```

### Quick start — `/dev/dax` (CXL / multi-host)

```sh
make -j CACHE=CLWB

# Writer (host 0) — no -T flag
./allocator -S 16 -l 1 -x 0 -g 20

# Workers — same -S and -g, unique -x, no -T flag
./bfs -S 16 -x 1 -g 20 &
./cc  -S 16 -x 2 -g 20 &
./pr  -S 16 -x 3 -g 20 &
```

A kernel can also allocate the graph, **however** it is recommended to use the `allocator` binary on host 0.

Command-line flags:

```sh
 -S <int>    : specify the size in GiB of the shared memory     # Eg. 1, 16
 -T <int>    : enable test mode /dev/shmem is mounted [0]/1     # 1 = POSIX; 0 = DAX (default)
 -l <int>    : reinitialize shared region before allocate [0]/1 # writer only
 -x <int>    : specify the host id. 0 -> master                 # 0 = writer; 1..n = workers
```

To allocate a graph on DAX (default backend):

```sh
./allocator -S 16 -l 1 -x 0 -g 20
```

To allocate a graph on POSIX shared memory (local testing):

```sh
rm -f /dev/shm/gapbs_sdm
./allocator -S 16 -l 1 -x 0 -g 20 -T 1
```

Worker processes for other graph kernels (DAX example):

```sh
./bfs -S 16 -x 1 -g 20 &
./cc -S 16 -x 2 -g 20 &
./cc_sv -S 16 -x 3 -g 20 &
./bc -S 16 -x 4 -g 20 &
./pr -S 16 -x 5 -g 20 &
./tc -S 16 -x 6 -g 20 &
```

POSIX local-testing equivalent (add `-T 1` everywhere):

```sh
./bfs -S 16 -x 1 -g 20 -T 1 &
./cc -S 16 -x 2 -g 20 -T 1 &
```

## Using this framework in simulations

TODO.

## References
[1] S. Beamer, K. Asanovi´ c, and D. Patterson, “The GAP Benchmark Suite,” arXiv preprint arXiv:1508.03619, 2015.