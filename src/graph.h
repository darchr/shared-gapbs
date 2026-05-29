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

#include "graph_shmem.h"

#define INT64_T sizeof(int64_t)
#define INT sizeof(int)
#define SIZE_T sizeof(size_t)
#define DESTID_ sizeof(DestID_)

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
    in_index_(nullptr), in_neighbors_(nullptr),
    _mmap_pointer(nullptr), _shm_attached(false) {}

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

  // kg: we need our own CSRGraph constructor that uses the host_ids correctly.
  // further, a method is required to validate whether the graph generated and
  // read from the /dev is the same.
  // TODO
  CSRGraph(int64_t num_nodes, DestID_*** index, size_t index_x,
        DestID_** neighs, size_t neigh_size, int host_id, int validate_graph,
        int size_of_shmem, int test_mode) :
    directed_(false), num_nodes_(num_nodes) {
      // kg: Make sure that the shared graph will be stored here and nothing
      // else.
      // goal: if this is the host then copy the contencts of the arrays to the
      // the mmapped backed memory and notify the user.

      const int create_region = (host_id == 0) ? 1 : 0;
      if (test_mode == 1)
        std::cout << "info: test mode! Using POSIX shm_alloc backend" << std::endl;
      graph_shmem_open(host_id, (size_t) size_of_shmem, test_mode, create_region);
      const size_t layout_bytes = SharedLayoutBytes(
          index_x, neigh_size, host_id == 0 ? *index : nullptr);
      graph_shmem_graph_base(host_id, layout_bytes, &_mmap_pointer);
      _shm_attached = true;

      // kg: if i am a worker node, then I need to wait for the synch variable

      // the start of the mmap array will be allocated to the start of index
      _synch_var = (int *) &_mmap_pointer[0];
      
      if (host_id == 0) {
        // set the metadata of the graph. There are 3 more metadata values
        // the number of nodes
        _num_nodes = (int64_t *) &_mmap_pointer[1];
        _num_nodes[0] = num_nodes;
        // size of the index_x.
        _index_x = (size_t *) &_mmap_pointer[1 + INT64_T/INT];
        _index_x[0] = index_x;
        // size of neighs
        _neigh_size = (size_t *) &_mmap_pointer[1 + INT64_T/INT + SIZE_T/INT];
        _neigh_size[0] = neigh_size;
      }
      // kg: if i am a worker, then I pool on the synch. variable until it is
      // set to 1.
      else {
        std::cout << "info: waiting for master!" << std::endl;
        while (_synch_var[0] != 1) {}
        std::cout << "info: master has set the variable!" << std::endl;
        std::cout << "info: reading graph metadata!" << std::endl;
        _num_nodes = (int64_t *) &_mmap_pointer[1];
        // num_nodes is a function argument but num_nodes_ was set when the
        // contructor was called for the first time. The worker nodes does not
        // know anything about it!
        num_nodes = num_nodes_ = _num_nodes[0];
        // size of the index_x.
        _index_x = (size_t *) &_mmap_pointer[1 + INT64_T/INT];
        index_x = _index_x[0];
        // size of neighs
        _neigh_size = (size_t *) &_mmap_pointer[1 + INT64_T/INT + SIZE_T/INT];
        neigh_size = _neigh_size[0];

        // I'm ready!!
      }
      
      // need to assign size and data
      assign_sizes(_mmap_pointer, index_x, neigh_size, host_id);
      
      if (host_id == 0) {
        // The master needs to allocate and then populate the data.
        assign_data(_mmap_pointer, *index, index_x, *neighs, neigh_size);
        // the master has done everything, just clflush the cache!
        graph_shmem_flush_cache(_mmap_pointer, layout_bytes);
      }
      // update: We don't need to free these data structures in the allocator.
      // The allocator will do some algo as well.

      // kg: once the data is assigned to the mmap space, we can freeup the
      // arrays that the program uses. Ideally this is correct but I'm just
      // lazy to free index and neigh and re-read them from the shared memory
      // for the allocator node. So I'd assume that THIS IS INCORRECT!!

      // update: In case this is a worker node, it'll read the index and the
      // neighs from the mmap and then allocate the correct data into these
      // variables. ** This is not the right place **
      else {

        // Now read the index and the neighs from the pointer. This needs
        // to work for the host and also for the workers as well. index and
        // neighs are both free pointers right now. Just reassign them from the
        // mmaped region
        index = &out_index_;
        neighs = &out_neighbors_;
      }

      std::cout << "info: value at synch. location = " << _mmap_pointer[0]
                                                                  << std::endl;

      // validation is done already by dumping the data into separate files.
      // I'm putting this on a low priority to do item.
      if (validate_graph == true) {
        // TODO
        std::cout << "fatal: NotImplementedError: Validation is pending!" <<
            std::endl;
        exit(-1);

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
    index_index_array(other.index_index_array),
    out_index_(other.out_index_), out_neighbors_(other.out_neighbors_),
    in_index_(other.in_index_), in_neighbors_(other.in_neighbors_),
    _mmap_pointer(other._mmap_pointer), _shm_attached(other._shm_attached),
    _synch_var(other._synch_var), _num_nodes(other._num_nodes),
    _index_x(other._index_x), _neigh_size(other._neigh_size) {
      other.num_edges_ = -1;
      other.num_nodes_ = -1;
      other.out_index_ = nullptr;
      other.out_neighbors_ = nullptr;
      other.in_index_ = nullptr;
      other.in_neighbors_ = nullptr;
      other._mmap_pointer = nullptr;
      other._shm_attached = false;
  }

  ~CSRGraph() {
    ReleaseResources();
    if (_shm_attached) {
      graph_shmem_close_region();
      _shm_attached = false;
      _mmap_pointer = nullptr;
    }
  }

  static size_t SharedLayoutBytes(size_t index_x, size_t neigh_size,
                                  DestID_** index) {
    size_t sum_index = 0;
    if (index != nullptr) {
      for (size_t i = 0; i + 1 < index_x; i++)
        sum_index += static_cast<size_t>(index[i + 1] - index[i]);
    }
    const size_t total_ints =
        1 + INT64_T / INT + (2 * SIZE_T) / INT +
        (neigh_size * DESTID_) / INT + (index_x * SIZE_T) / INT +
        (sum_index * DESTID_) / INT;
    return total_ints * INT;
  }

  // kg: defining new methods to manage the memory
  void assign_sizes(int *_mmap, size_t index_x, size_t neigh_size, int host_id) {
    // need to assign 2d pointers. regardless of the host_id, this step is true
    // for all hosts.

    // see the figure in the README to understand this mapping.
    out_neighbors_ = (DestID_ *) &_mmap[1 + INT64_T/INT + (2 * SIZE_T)/INT];
    in_neighbors_ = out_neighbors_;

    // next we need to keep a track of all the y indices
    index_index_array = (size_t *) &_mmap[1 + INT64_T/INT + (2 * SIZE_T)/INT +
                                                   (neigh_size * DESTID_)/INT];


    // This is a 2d array. we need a holder for the pointer pointer
    out_index_ = (DestID_ **) malloc (index_x * sizeof(DestID_ *));
    in_index_ = (DestID_ **) malloc (index_x * sizeof(DestID_ *));

    // If i am the host, IDK the size of each row for the index array. We'll
    // only know that once the data is allocated!
    if (host_id > 0) {
      // workers always have the sizes!
      //
      // now that i have the sizes, i can reassign them. remember the first
      // element will start at the base i.e. +0!
      out_index_[0] = (DestID_ *) &_mmap[1 + INT64_T/INT + (2 * SIZE_T)/INT +
                          (neigh_size * DESTID_)/INT + (index_x * SIZE_T)/INT];
      // the in_index_ is the same for these graphs! I'm doing another line for
      // this step for better understanding.
      in_index_[0] = out_index_[0];


      // WE ONLY CARE ABOUT INDEX_X - 1 i.e. NUM_NODES rows!
      // A new variable local_sum is needed to figure out the length of each
      // row
      size_t sum_local = 0;
      
      for (size_t i = 0 ; i < index_x ; i++) {
        out_index_[i] = (DestID_ *) &_mmap[1 + INT64_T/INT + (2 * SIZE_T)/INT
            + (neigh_size * DESTID_)/INT + (index_x * SIZE_T)/INT +
            (sum_local * DESTID_)/INT];
        in_index_[i] = out_index_[i];
        if (i < index_x - 1)
          sum_local += index_index_array[i];
      }

    }

  }

  void assign_data(int *_mmap, DestID_** index, size_t index_x,
                                          DestID_ *neighs, size_t neigh_size) {
    // once every sinvle variable size is set, we only need to assign the data
    // only the master calls this function
    
    // kg: the problem here is that each row of the index array is of variable
    // size. The client/worker has no way of knoing this unless this
    // information is stored in the shared memory.

    // The index_index_array needs to store the number of elements each
    // row has, i.e. elements_in_row_i
    // This can be calculated by subtrating the base of the i th row
    // from i + 1 th row.
    // The final index is not necessary as there are index_x - 1 number
    // of nodes.
    #pragma omp parallel for
    for (size_t i = 0 ; i < index_x - 1; i++)
      index_index_array[i] = (index[i + 1] - index[i]);
    
    // now that i have the sizes, i can reassign them. remember the first elem-
    // ent will start at the base i.e. +0!
    out_index_[0] = (DestID_ *) &_mmap[1 + INT64_T/INT + (2 * SIZE_T)/INT
                        + (neigh_size * DESTID_)/INT + (index_x * SIZE_T)/INT];
    // the in_index_ is the same for these graphs! I'm doing another line for
    // this step for better understanding.
    in_index_[0] = out_index_[0];

    // WE ONLY CARE ABOUT INDEX_X - 1 i.e. NUM_NODES rows!
    // A new variable local_sum is needed to figure out the length of each row
    size_t sum_local = 0;
    std::cout << "info: the allocator is writing the index array to the shared"
            << " memory! This may take some time..." << std::endl;

    for (size_t i = 0 ; i < index_x ; i++) {
      out_index_[i] = (DestID_ *) &_mmap[1 + INT64_T/INT + (2 * SIZE_T)/INT
          + (neigh_size * DESTID_)/INT + (index_x * SIZE_T)/INT +
          (sum_local * DESTID_)/INT];
      in_index_[i] = out_index_[i];
      if (i < index_x - 1)
        sum_local += index_index_array[i];
    }

    // ready to write the memory!!!
    #pragma omp parallel for
    for (size_t i = 0 ; i < index_x - 1; i++) {

      for (size_t j = 0; j < index_index_array[i]; j++)
        out_index_[i][j] = index[i][j];
      
      // making absolutely sure at this point.
      in_index_[i] = out_index_[i];
    }

    // this is the simple step of writing an one-dimensional array
    for (size_t i = 0 ; i < neigh_size ; i++) {
      out_neighbors_[i] = neighs[i];
      in_neighbors_[i] = neighs[i];
    }

    // make sure that the synchronization variable is finally set
    _synch_var[0] = 1;
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
    std::cout << INT << " " << DESTID_ << " " << SIZE_T << std::endl;
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
  bool directed_;
  int64_t num_nodes_;
  int64_t num_edges_;
  size_t *index_index_array;
  DestID_** out_index_;
  DestID_*  out_neighbors_;
  DestID_** in_index_;
  DestID_*  in_neighbors_;
  int *_mmap_pointer = nullptr;
  bool _shm_attached = false;
  int *_synch_var = nullptr;
  int64_t *_num_nodes;
  size_t *_index_x;
  size_t *_neigh_size;
};

#endif  // GRAPH_H_
