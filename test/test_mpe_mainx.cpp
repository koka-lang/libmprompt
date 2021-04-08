/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
  
  Test various effect handlers with C++ exceptions and threads
-----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdint.h>

#include <mprompt.h>
#include <mpeff.h>
#include "test.h"

int main(int argc, char** argv) {
  printf("main\n");
  
  mp_config_t config = { };
  #if defined(_MSC_VER)
  config.stack_use_overcommit = true;    // don't use gpools or a page-fault handler
  #endif
  //config.stack_use_overcommit = true;  // easier debugging in gdb/lldb as no SEGV signals are used
  //config.gpool_disable = true;
  //config.stack_grow_linear = true;
  //config.stack_max_size = 1 * 1024 * 1024L;
  //config.stack_initial_commit = 64 * 1024L; 
  //config.stack_cache_count = -1; // disable per-thread cache
  mp_init(&config);

  size_t start_rss = 0;
  mpt_timer_t start = mpt_show_process_info_start(&start_rss);
  
   
  // effect handlers
  reader_run();
  counter_run();
  countern_run();
  mstate_run();
  rehandle_run();

  // multi-shot tests
  amb_run();
  amb_state_run();
  nqueens_run();

  // C++ tests
  exn_run();
  multi_unwind_run();
  throw_run();
  
  // threaded test 
  thread_rehandle_run();
    
  printf("done\n");
  mpt_show_process_info(stdout, start, start_rss);
}
