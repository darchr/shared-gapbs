// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

// This is the allocator program that allocates a graph.

#include <iostream>
#include <vector>

#include "benchmark.h"
#include "command_line.h"
#include "graph_shmem.h"

int main(int argc, char* argv[]) {
  CLApp cli(argc, argv, "allocator");
  if (!cli.ParseArgs())
    return -1;
  Builder b(cli);
  // kg: invoking MakeGraph with a valid host_id
  assert(cli.host_id() >= 0 &&
                "The host ID (-x <int>) is not specified...");
  // make sure that the size of the shared memory is specified.
  assert(cli.get_size() > 0 &&
                "The shared memory size (-S <GiB>) is not specified...");

  // we don't overwrite the graph or munmap it unless the user specifies.
  if (cli.munmap_me() == 1) {
      std::cout << "info: zeroing out the memory" << std::endl;
      graph_shmem_reset_region((size_t) cli.get_size(), cli.is_test());
  }
  
  Graph g = b.MakeGraph(cli.host_id());

  // pretty much done here!

  return 0;
}


