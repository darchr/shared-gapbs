// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef GRAPH_H_
#define GRAPH_H_

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <iostream>
#include <type_traits>

#include <fstream>

#include "pvector.h"
#include "util.h"

#include "shm_graph.h"

// There are four values, storing the metadata information of the graph!
#define METADATA 4

/*
GAP Benchmark Suite
Class:  CSRGraph
Author: Scott Beamer

Simple container for graph in CSR format
 - Intended to be constructed by a Builder
 - To make weighted, set DestID_ template type to NodeWeight
 - MakeInverse parameter controls whether graph stores its inverse
*/


// Used to hold node & weight, with another node it makes a weighted edge
template <typename NodeID_, typename WeightT_>
struct NodeWeight {
  NodeID_ v;
  WeightT_ w;
  NodeWeight() {}
  NodeWeight(NodeID_ v) : v(v), w(1) {}
  NodeWeight(NodeID_ v, WeightT_ w) : v(v), w(w) {}

  bool operator< (const NodeWeight& rhs) const {
    return v == rhs.v ? w < rhs.w : v < rhs.v;
  }

  // doesn't check WeightT_s, needed to remove duplicate edges
  bool operator== (const NodeWeight& rhs) const {
    return v == rhs.v;
  }

  // doesn't check WeightT_s, needed to remove self edges
  bool operator== (const NodeID_& rhs) const {
    return v == rhs;
  }

  operator NodeID_() {
    return v;
  }
};

template <typename NodeID_, typename WeightT_>
std::ostream& operator<<(std::ostream& os,
                         const NodeWeight<NodeID_, WeightT_>& nw) {
  os << nw.v << " " << nw.w;
  return os;
}

template <typename NodeID_, typename WeightT_>
std::istream& operator>>(std::istream& is, NodeWeight<NodeID_, WeightT_>& nw) {
  is >> nw.v >> nw.w;
  return is;
}



// Syntatic sugar for an edge
template <typename SrcT, typename DstT = SrcT>
struct EdgePair {
  SrcT u;
  DstT v;

  EdgePair() {}

  EdgePair(SrcT u, DstT v) : u(u), v(v) {}

  bool operator< (const EdgePair& rhs) const {
    return u == rhs.u ? v < rhs.v : u < rhs.u;
  }

  bool operator== (const EdgePair& rhs) const {
    return (u == rhs.u) && (v == rhs.v);
  }
};

// SG = serialized graph, these types are for writing graph to file
typedef int32_t SGID;
typedef EdgePair<SGID> SGEdge;
typedef int64_t SGOffset;



template <class NodeID_, class DestID_ = NodeID_, bool MakeInverse = true>
class CSRGraph {
  // Used for *non-negative* offsets within a neighborhood
  typedef std::make_unsigned<std::ptrdiff_t>::type OffsetT;

  // Used to access neighbors of vertex, basically sugar for iterators
  class Neighborhood {
    // kg: For some reasons the neighbors are not getting accessed correctly.
    NodeID_ n_;
    DestID_** g_index_;
    OffsetT start_offset_;
   public:
    Neighborhood(NodeID_ n, DestID_** g_index, OffsetT start_offset) :
        n_(n), g_index_(g_index), start_offset_(0) {
      OffsetT max_offset = end() - begin();
      start_offset_ = std::min(start_offset, max_offset);
    }
    typedef DestID_* iterator;
    iterator begin() { return g_index_[n_] + start_offset_; }
    iterator end()   { return g_index_[n_+1]; }
  };

  void ReleaseResources() {
    // These graphs aren't allocated using malloc or new. so we can skip these
    // for now.
    /*
    if (out_index_ != nullptr)
      delete[] out_index_;
    if (out_neighbors_ != nullptr)
      delete[] out_neighbors_;
    if (directed_) {
      if (in_index_ != nullptr)
        delete[] in_index_;
      if (in_neighbors_ != nullptr)
        delete[] in_neighbors_;
    }
    */
  }


 public:
  CSRGraph() : directed_(false), num_nodes_(-1), num_edges_(-1),
    out_index_(nullptr), out_neighbors_(nullptr),
    in_index_(nullptr), in_neighbors_(nullptr) {}

  CSRGraph(int64_t num_nodes, DestID_** index, DestID_* neighs) :
    directed_(false), num_nodes_(num_nodes),
    out_index_(index), out_neighbors_(neighs),
    in_index_(index), in_neighbors_(neighs) {
      // kg: relabel function. some kernels call this.

      // not terribly important to notify the user.
      // std::cout << "info: relabel function called!" << std::endl;

      // we cannot calculate num_edges_ right now as these structures are all
      // nullptr for the workers.

      // TODO: does this change in TC?
      // num_edges_ = (out_index_[num_nodes_] - out_index_[0]) / 2;
    }

  CSRGraph(int64_t num_nodes, DestID_*** index, size_t index_x,
        DestID_** neighs, size_t neigh_size, int host_id, int validate_graph,
        int size_of_shmem, int test_mode) :
    host_id_(host_id), directed_(false), num_nodes_(num_nodes) {
      gapbs_shm::OpenRegion(host_id, test_mode, static_cast<size_t>(size_of_shmem));
      region_ = gapbs_shm::Region::Get().handle;

      if (host_id == 0) {
        size_t index_elems = 0;
        for (size_t i = 0; i + 1 < index_x; i++)
          index_elems += static_cast<size_t>((*index)[i + 1] - (*index)[i]);

        uint64_t id = 0;
        shm_off_t off = 0;
        int rc = 0;

        rc = gapbs_shm::AllocObject(region_, sizeof(GapbsSync), GAPBS_OBJ_SYNC,
                                    &id, &off, &sync_cap_);
        if (rc != 0) { std::cerr << "error: alloc sync failed\n"; std::exit(EXIT_FAILURE); }

        rc = gapbs_shm::AllocObject(region_, sizeof(GapbsMeta), GAPBS_OBJ_META,
                                    &id, &off, &meta_cap_);
        if (rc != 0) { std::cerr << "error: alloc meta failed\n"; std::exit(EXIT_FAILURE); }

        rc = gapbs_shm::AllocObject(region_, index_x * sizeof(size_t),
                                    GAPBS_OBJ_ROW_LENS, &id, &off, &row_lens_cap_);
        if (rc != 0) { std::cerr << "error: alloc row_lens failed\n"; std::exit(EXIT_FAILURE); }

        rc = gapbs_shm::AllocObject(region_, neigh_size * sizeof(DestID_),
                                    GAPBS_OBJ_NEIGHS, &id, &off, &neighs_cap_);
        if (rc != 0) { std::cerr << "error: alloc neighs failed\n"; std::exit(EXIT_FAILURE); }

        rc = gapbs_shm::AllocObject(region_, index_elems * sizeof(DestID_),
                                    GAPBS_OBJ_INDEX, &id, &off, &index_cap_);
        if (rc != 0) { std::cerr << "error: alloc index failed\n"; std::exit(EXIT_FAILURE); }

        GapbsSync *sync = shm_cap_deref_as(region_, sync_cap_, GapbsSync,
                                           gapbs_shm::RwRequired());
        GapbsMeta *meta = shm_cap_deref_as(region_, meta_cap_, GapbsMeta,
                                           gapbs_shm::RwRequired());
        sync->ready = 0;
        gapbs_shm::PersistRange(sync, sizeof(GapbsSync));

        meta->num_nodes = num_nodes;
        meta->index_x = index_x;
        meta->neigh_size = neigh_size;
        gapbs_shm::PersistRange(meta, sizeof(GapbsMeta));
      } else {
        gapbs_shm::GraphCaps caps = gapbs_shm::OpenGraphCaps(host_id);
        sync_cap_ = caps.sync;
        meta_cap_ = caps.meta;
        row_lens_cap_ = caps.row_lens;
        neighs_cap_ = caps.neighs;
        index_cap_ = caps.index;

        const GapbsMeta *meta = shm_cap_deref_as(region_, meta_cap_, GapbsMeta,
                                                 gapbs_shm::RoRequired());
        num_nodes = num_nodes_ = meta->num_nodes;
        index_x = meta->index_x;
        neigh_size = meta->neigh_size;
      }

      assign_sizes(index_x, neigh_size, host_id);

      if (host_id == 0)
        assign_data(*index, index_x, *neighs, neigh_size);
      else {
        index = &out_index_;
        neighs = &out_neighbors_;
      }

      GapbsSync *sync = shm_cap_deref_as(region_, sync_cap_, GapbsSync,
                                         host_id == 0 ? gapbs_shm::RwRequired()
                                                      : gapbs_shm::RoRequired());
      std::cout << "info: graph ready flag = " << sync->ready << std::endl;

      if (validate_graph) {
        std::cout << "fatal: NotImplementedError: Validation is pending!\n";
        std::exit(EXIT_FAILURE);
      }
      num_edges_ = (out_index_[num_nodes_] - out_index_[0]) / 2;
    }

  // kg: we need our own CSRGraph constructor that uses the host_ids correctly.
  // further, a method is required to validate whether the graph generated and
  // read from the /dev is the same.

  CSRGraph(int64_t num_nodes, DestID_** out_index, DestID_* out_neighs,
        DestID_** in_index, DestID_* in_neighs) :
    directed_(true), num_nodes_(num_nodes),
    out_index_(out_index), out_neighbors_(out_neighs),
    in_index_(in_index), in_neighbors_(in_neighs) {
      num_edges_ = out_index_[num_nodes_] - out_index_[0];
    }

  CSRGraph(CSRGraph&& other) : directed_(other.directed_),
    num_nodes_(other.num_nodes_), num_edges_(other.num_edges_),
    out_index_(other.out_index_), out_neighbors_(other.out_neighbors_),
    in_index_(other.in_index_), in_neighbors_(other.in_neighbors_) {
      other.num_edges_ = -1;
      other.num_nodes_ = -1;
      other.out_index_ = nullptr;
      other.out_neighbors_ = nullptr;
      other.in_index_ = nullptr;
      other.in_neighbors_ = nullptr;
  }

  ~CSRGraph() {
    ReleaseResources();
  }

  // kg: defining new methods to manage the memory
  void assign_sizes(size_t index_x, size_t neigh_size, int host_id) {
    const shm_perm_t required = (host_id == 0) ? gapbs_shm::RwRequired()
                                               : gapbs_shm::RoRequired();

    out_neighbors_ = shm_cap_deref_as(region_, neighs_cap_, DestID_, required);
    in_neighbors_ = out_neighbors_;

    index_index_array = shm_cap_deref_as(region_, row_lens_cap_, size_t, required);

    DestID_ *index_base = shm_cap_deref_as(region_, index_cap_, DestID_, required);

    out_index_ = static_cast<DestID_ **>(malloc(index_x * sizeof(DestID_ *)));
    in_index_ = static_cast<DestID_ **>(malloc(index_x * sizeof(DestID_ *)));

    if (host_id > 0) {
      out_index_[0] = index_base;
      in_index_[0] = index_base;

      size_t sum_local = 0;
      for (size_t i = 0; i < index_x; i++) {
        out_index_[i] = index_base + sum_local;
        in_index_[i] = out_index_[i];
        sum_local += index_index_array[i];
      }
    }
  }

  void assign_data(DestID_** index, size_t index_x,
                   DestID_ *neighs, size_t neigh_size) {
    #pragma omp parallel for
    for (size_t i = 0; i < index_x - 1; i++)
      index_index_array[i] = static_cast<size_t>(index[i + 1] - index[i]);
    gapbs_shm::PersistRange(index_index_array,
                            (index_x > 0 ? index_x - 1 : 0) * sizeof(size_t));

    DestID_ *index_base = shm_cap_deref_as(region_, index_cap_, DestID_,
                                           gapbs_shm::RwRequired());
    out_index_[0] = index_base;
    in_index_[0] = index_base;

    size_t sum_local = 0;
    std::cout << "info: writing graph index into shared memory...\n";

    for (size_t i = 0; i < index_x; i++) {
      out_index_[i] = index_base + sum_local;
      in_index_[i] = out_index_[i];
      sum_local += index_index_array[i];
    }

    #pragma omp parallel for
    for (size_t i = 0; i < index_x - 1; i++) {
      for (size_t j = 0; j < index_index_array[i]; j++)
        out_index_[i][j] = index[i][j];
      in_index_[i] = out_index_[i];
    }
    gapbs_shm::PersistRange(index_base, sum_local * sizeof(DestID_));

    for (size_t i = 0; i < neigh_size; i++) {
      out_neighbors_[i] = neighs[i];
      in_neighbors_[i] = neighs[i];
    }
    gapbs_shm::PersistRange(out_neighbors_, neigh_size * sizeof(DestID_));

    GapbsSync *sync = shm_cap_deref_as(region_, sync_cap_, GapbsSync,
                                       gapbs_shm::RwRequired());
    sync->ready = 1;
    gapbs_shm::PersistRange(sync, sizeof(GapbsSync));
  }

  void print_data(size_t index_x) {
    // kg: useful for debugging!
    for (size_t i = 0 ; i < index_x - 1 ; i++) {
      std::cout << i << "," << index_index_array[i] << "  ::  ";
      for (size_t j = 0 ; j < index_index_array[i]; j++) {
        
        std::cout <<  out_index_[i][j] << " ";
        
      }
      std::cout << std::endl;
    }
    std::cout << sizeof(int) << " " << sizeof(DestID_) << " " << sizeof(size_t)
              << std::endl;
  }

  void print_neighs(size_t neigh_size) {
    // kg: useful for debugging!
    for (size_t i = 0 ; i < neigh_size ; i++) {
      std::cout << out_neighbors_[i] << " ";
      if (i % 1000 == 0)
        std::cout << std::endl;
    }
  }

  CSRGraph& operator=(CSRGraph&& other) {
    if (this != &other) {
      ReleaseResources();
      directed_ = other.directed_;
      num_edges_ = other.num_edges_;
      num_nodes_ = other.num_nodes_;
      out_index_ = other.out_index_;
      out_neighbors_ = other.out_neighbors_;
      in_index_ = other.in_index_;
      in_neighbors_ = other.in_neighbors_;
      other.num_edges_ = -1;
      other.num_nodes_ = -1;
      other.out_index_ = nullptr;
      other.out_neighbors_ = nullptr;
      other.in_index_ = nullptr;
      other.in_neighbors_ = nullptr;
    }
    return *this;
  }

  bool directed() const {
    return directed_;
  }

  int64_t num_nodes() const {
    return num_nodes_;
  }

  int64_t num_edges() const {
    return num_edges_;
  }

  int64_t num_edges_directed() const {
    return directed_ ? num_edges_ : 2*num_edges_;
  }

  int64_t out_degree(NodeID_ v) const {
    return out_index_[v+1] - out_index_[v];
  }

  int64_t in_degree(NodeID_ v) const {
    static_assert(MakeInverse, "Graph inversion disabled but reading inverse");
    return in_index_[v+1] - in_index_[v];
  }

  Neighborhood out_neigh(NodeID_ n, OffsetT start_offset = 0) const {
    return Neighborhood(n, out_index_, start_offset);
  }

  Neighborhood in_neigh(NodeID_ n, OffsetT start_offset = 0) const {
    static_assert(MakeInverse, "Graph inversion disabled but reading inverse");
    return Neighborhood(n, in_index_, start_offset);
  }

  void PrintStats() const {
    std::cout << "Graph has " << num_nodes_ << " nodes and "
              << num_edges_ << " ";
    if (!directed_)
      std::cout << "un";
    std::cout << "directed edges for degree: ";
    std::cout << num_edges_/num_nodes_ << std::endl;
  }

  void PrintTopology() const {
    for (NodeID_ i=0; i < num_nodes_; i++) {
      std::cout << i << ": ";
      for (DestID_ j : out_neigh(i)) {
        std::cout << j << " ";
      }
      std::cout << std::endl;
    }
  }

  static DestID_** GenIndex(const pvector<SGOffset> &offsets, DestID_* neighs) {
    // x of index is offset.size()
    NodeID_ length = offsets.size();
    DestID_** index = new DestID_*[length];
    #pragma omp parallel for
    for (NodeID_ n=0; n < length; n++)
      index[n] = neighs + offsets[n];
    return index;
  }

  pvector<SGOffset> VertexOffsets(bool in_graph = false) const {
    pvector<SGOffset> offsets(num_nodes_+1);
    for (NodeID_ n=0; n < num_nodes_+1; n++)
      if (in_graph)
        offsets[n] = in_index_[n] - in_index_[0];
      else
        offsets[n] = out_index_[n] - out_index_[0];
    return offsets;
  }

  Range<NodeID_> vertices() const {
    return Range<NodeID_>(num_nodes());
  }

 private:
  shm_region_t *region_;
  shm_cap_t sync_cap_;
  shm_cap_t meta_cap_;
  shm_cap_t row_lens_cap_;
  shm_cap_t neighs_cap_;
  shm_cap_t index_cap_;
  int host_id_;
  bool directed_;
  int64_t num_nodes_;
  int64_t num_edges_;
  size_t *index_index_array;
  DestID_** out_index_;
  DestID_*  out_neighbors_;
  DestID_** in_index_;
  DestID_*  in_neighbors_;
};

#endif  // GRAPH_H_
