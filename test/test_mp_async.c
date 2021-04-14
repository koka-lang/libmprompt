/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Test async prompts where each worker use stack space
-----------------------------------------------------------------------------*/
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <mprompt.h>
#include "test.h"

#define N         10000    // max active async workers
#define M      10000000    // total number of requests
#define USE_KB       32    // use 32KiB stack per request

static void async_workers(void);

int main() {
  mp_config_t config = mp_config_default();
  //config.stack_use_overcommit = true;  // easier debugging in gdb/lldb as no SEGV signals are used
  //config.gpool_enable = true;
  //config.stack_grow_fast = true;
  //config.stack_cache_count = -1; // disable per-thread cache
  mp_init(&config);

  async_workers();
  return 0;
}


// -------------------------------
// Helper to use stack space

#if defined(_MSC_VER) && !defined(__clang__) && !defined(__GNUC__)
# define __noinline     __declspec(noinline)
#else
# define __noinline     __attribute__((noinline))
#endif

static __noinline void* as_stack_address(void* p) {
  return p;
}

static __noinline void* get_stack_top(void) {
  void* top = NULL;
  return as_stack_address(&top);
}

static void stack_use(intptr_t totalkb) {
  uint8_t* sp = (uint8_t*)get_stack_top();
  size_t page_size = 4096;
  size_t total_pages = ((size_t)totalkb*1024 + page_size - 1) / page_size;
  for (size_t i = 0; i < total_pages; i++) {
    volatile uint8_t b = *(sp - (i * page_size));
  }
}


// -------------------------------
// Async workers

static void* await_result(mp_resume_t* r, void* arg) {
  (void)(arg);
  return r;  // instead of resuming ourselves, we return the resumption as a "suspended async computation" (A)
}

static void* async_worker(mp_prompt_t* parent, void* arg) {
  (void)(arg);
  // start a fresh worker
  // ... do some work
  intptr_t partial_result = 0;
  // and await some request; we do this by yielding up to our prompt and running `await_result` (in the parent context!)
  intptr_t kb =(intptr_t)mp_yield( parent, &await_result, NULL );
  // when we are resumed at some point, we do some more work 
  // ... do more work
  stack_use(kb);
  partial_result++;
  // and return with the result (B)
  return (void*)(partial_result);
}

static void async_workers(void) {
  mp_resume_t** workers = (mp_resume_t**)calloc(N,sizeof(mp_resume_t*));  // allocate array of N resumptions
  intptr_t count = 0;
  printf("run requests...\n");  
  size_t start_rss;
  mpt_timer_t start = mpt_show_process_info_start(&start_rss);
  for( int i = 0; i < M+N; i++) {  // perform M connections
    int j = i % N;               // pick an active worker
    // if the worker is actively waiting (suspended), resume it
    if (workers[j] != NULL) {  
      count += (intptr_t)mp_resume(workers[j], (void*)((intptr_t)USE_KB));  // (B)
      workers[j] = NULL;
    }
    // and start a fresh worker and wait for its first yield (suspension). 
    // the worker returns its own resumption as a result.
    if (i < M) {
      workers[j] = (mp_resume_t*)mp_prompt( &async_worker, NULL );  // (A)
    }
    //if (j == 0) mpt_printf("%6i todo\n", M + N - i);
  }
  mpt_show_process_info(stdout, start, start_rss);
  size_t total_kb = M * USE_KB;
  double total_mb = (double)total_kb / 1024.0;
  printf("Total of %d prompts with %d active at a time\nUsing %dkb stack per request, total stack used: %.3fmb, count=%zd\n", M, N, USE_KB, total_mb, count);
}

