/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Tests various standard effect handlers
-----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdint.h>

#include <mprompt.h>
#include <mpeff.h>
#include "test.h"

static void test_c(void);
static void test_cpp(void);
static void test_cpp_threaded(void);

int main(int argc, char** argv) {
  printf("testing..\n");
  
  mp_config_t config = mp_config_default();
  //config.stack_use_overcommit = true;  // easier debugging in gdb/lldb as no SEGV signals are used
  config.gpool_enable = true;
  config.stack_grow_fast = true;
  //config.stack_max_size = 1 * 1024 * 1024L;
  //config.stack_initial_commit = 64 * 1024L; 
  config.stack_cache_count = 0; // disable per-thread cache
  mp_init(&config);

  size_t start_rss = 0;
  mpt_timer_t start = mpt_show_process_info_start(&start_rss);
  
  test_c();
  test_cpp();
  test_cpp_threaded();
  
  printf("done.\n");
  mpt_show_process_info(stdout, start, start_rss);
}

static void test_c(void) {
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
}


#ifdef __cplusplus

static void test_cpp(void) {
  // C++ exception tests  
  exn_run();
  multi_unwind_run();
  throw_run();
}


#include <thread>
static void thread_test() {
  printf("\n-----------------------------\nrun tests again in a separate thread:\n\n");
  test_c();
  test_cpp();
  printf("done testing in separate thread.\n");
}
void test_cpp_threaded(void) {
  auto t = std::thread(&thread_test);
  t.join();  
}

#else // C
static void test_cpp(void) {
  // nothing
}
static void test_cpp_threaded(void) {
  // nothing
}
#endif  