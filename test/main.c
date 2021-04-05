/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <stdio.h>
#include <stdint.h>

#include <mprompt.h>
#include <mpeff.h>
#include "test.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable" 
#endif

static void mp_test2(void);
static void mp_test1M(void);
static void mp_async_test1M(void);


int main(int argc, char** argv) {
  printf("main\n");
  
  mp_config_t config = { };
  //config.gpool_enable = true;
  //config.stack_max_size = 1 * 1024 * 1024L;
  //config.stack_initial_commit = 64 * 1024L;   // use when debugging with lldb on macOS
  //config.stack_cache_count = -1;
  mp_init(&config);

  size_t start_rss = 0;
  mpt_timer_t start = mpt_show_process_info_start(&start_rss);

  // effect handlers
  reader_run();
  counter_run();
  countern_run();
  mstate_run();
  rehandle_run();

  // C++ 
  exn_run();
  multi_unwind_run();
  throw_run();
  
  // multi-shot tests
  amb_run();
  amb_state_run();
  nqueens_run();

  // threaded test (C++ only)
  thread_rehandle_run();
  
  // direct mprompt tests
  //mp_async_test1M();  // async workers

  // low-level mprompt tests
  //mp_test1()
  //mp_test2();
  //mp_test1M();

  printf("done\n");
  mpt_show_process_info(stdout, start, start_rss);
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

static void stack_use(long totalkb) {
  uint8_t* sp = (uint8_t*)get_stack_top();
  size_t page_size = 4096;
  size_t total_pages = ((size_t)totalkb*1024 + page_size - 1) / page_size;
  for (size_t i = 0; i < total_pages; i++) {
    volatile uint8_t b = *(sp - (i * page_size));
  }
}

// -----------------------------------------------------------------------
// Test "asynchronous" connections:
// set up N active connections (suspended prompts) that get resumed and
// then perform "work" (use stack space) and finish; and are then replaced
// by a fresh connection.

static void* await_resultx(mp_resume_t* r, void* arg) {
  return r;  // just return the resumption as a "suspended async computation"
}


static void* async_workerx(mp_prompt_t* p, void* arg) {
  long kb = mpe_long_voidp( mp_yield(p, &await_resultx, arg) );  // wait for some result
  stack_use(kb);                                                // and perform "work"
  return mpe_voidp_long(1);
}
 

static void mp_async_test1M(void) {
  const size_t totalN = 10 * (1000000 /* 1M */);
  const size_t activeN = 10000;
  const long stack_kb = 32;
  printf("async_test1M set up...\n");
  mp_resume_t** rs = (mp_resume_t**)calloc(activeN, sizeof(mp_prompt_t*));
  for (size_t i = 0; i < activeN; i++) {
    rs[i] = (mp_resume_t*)mp_prompt(&async_workerx, NULL);
  }

  printf("run %zuM connections with %zu active at a time, each using %ldkb stack...\n", totalN / 1000000, activeN, stack_kb);
  //size_t start_rss;
  //mpt_timer_t start = mpt_show_process_info_start(&start_rss);
  long count = 0;
  for (size_t i = 0; i < totalN; i++) {
    size_t j = i % activeN;
    count += mpe_long_voidp(mp_resume(rs[j], mpe_voidp_long(stack_kb)));  // do the work
    rs[j] = (mp_resume_t*)mp_prompt(&async_workerx, NULL); // and create a new one
    //if (i % 1000 == 0) printf("todo: %5zu ...\n", N - i);
  }
  //mpt_show_process_info(stdout, start, start_rss);
  size_t total_kb = totalN * stack_kb;
  double total_mb = (double)(totalN*stack_kb) / 1024.0;
  printf("total stack used: %.3fmb, count=%ld\n", total_mb, count);
}




/*
static void worker() {
  uint8_t* p = (uint8_t*)mp_gstack_alloc();
  size_t sz = mp_gstack_size();
  for (size_t i = sz; i > 0; i--) {
    p[i - 1] = (uint8_t)(i - 1);
  }
  mp_gstack_free(p);
}


static void mp_test1(void) {
  auto t1 = std::thread(worker);
  t1.join();
}
*/



// ------------------------------------
// Creating large number of prompts (active at the same time)

static void* worker1M(mp_prompt_t* p, void* arg) {
  stack_use(mpe_long_voidp(arg));
  return mpe_voidp_long(1);
}


static void mp_test1M(void) {
  const size_t totalN = 10000000;
  const size_t activeN = 10000;
  const long stack_kb = 32;
  printf("start some workers...\n");
  mp_prompt_t** ps = (mp_prompt_t**)calloc(activeN, sizeof(mp_prompt_t*));
  for (size_t i = 0; i < activeN; i++) {
    ps[i] = mp_prompt_create();
  }

  printf("run prompts...\n");
  size_t start_rss;
  mpt_timer_t start = mpt_show_process_info_start(&start_rss);
  long count = 0;
  for (size_t i = 0; i < totalN; i++) {
    size_t j = i % activeN;
    count += mpe_long_voidp(mp_prompt_enter(ps[j], &worker1M, mpe_voidp_long(stack_kb)));
    ps[j] = mp_prompt_create();
    //if (i % 1000 == 0) printf("todo: %5zu ...\n", N - i);
  }
  mpt_show_process_info(stdout, start, start_rss);
  size_t total_kb = totalN * stack_kb;
  double total_mb = (double)(totalN * stack_kb) / 1024.0;
  printf("total %zu prompts, %zu active at a time, using %ldkb stack: total stack used: %.3fmb, count=%ld\n", totalN, activeN, stack_kb, total_mb, count);
}


// ------------------------------
// Induce stack overflows

static int deep2(int n) {
  if (n <= 1) {
    return 1;
  }
  return (1 + deep2(n - 1));
}

static void* fun_exit(mp_resume_t* r, void* arg) {
  printf("operation exit\n");
  return NULL;
}

static void* fun_get(mp_resume_t* r, void* arg) {
  printf("operation get\n");
  //mp_resume_once(r, (void*)((size_t)10000));
  return mp_resume_tail(r, (void*)((size_t)10000));
}

static void* pentry2(mp_prompt_t* p, void* arg) {
  printf("pentry2\n");
  mp_prompt_t* p1 = mp_prompt_parent(p);
  int n = mpe_int_voidp(mp_yield(p1, &fun_get, NULL));
  return mpe_voidp_int(deep2(n));
}

static int deep(int n) {
  if (n <= 1) {
    int result = 0;
    mp_prompt(&pentry2, &result);
    return 1 + result;
  }
  return (1 + deep(n - 1));
}

//#include <windows.h>

static void* pentry(mp_prompt_t* p, void* arg) {
  printf("\npentry\n");
  /*mp_trace_stack_layout(NULL, NULL);
  ULONG stk_size = 4*4096;
  if (!SetThreadStackGuarantee(&stk_size)) {
    printf("cannot set thread guarantee\n");
  };*/
  //mp_trace_stack_layout(NULL, NULL);
  return mpe_voidp_int(deep(100000));
}

static void mp_test2(void) {
  printf("\nmp_test2\n");
  /* mp_trace_stack_layout(NULL, NULL);
   ULONG stk_size = 4 * 4096;
   if (!SetThreadStackGuarantee(&stk_size)) {
     printf("cannot set thread guarantee\n");
   };
   mp_trace_stack_layout(NULL, NULL);
  */
  printf("\n");
  int result = mpe_int_voidp(mp_prompt(&pentry, &result));
  printf("done with %i\n", result);
}
