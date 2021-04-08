/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <mprompt.h>
#include "test.h"

#ifdef __cplusplus
#include <stdexcept>
#endif



/*-----------------------------------------------------------------
  Benchmark
-----------------------------------------------------------------*/
static bool d1_destructed;
static bool d2_destructed;

static void* bench_exn(void* arg) {
  test_raii_t d1("d1", &d1_destructed);
  UNUSED(arg);
  long i = state_get() + state_get();
  if (i > 42) {
    exn_raise("i > 42");
  }
  return mpe_voidp_long(i);
}


/*-----------------------------------------------------------------
  Run
-----------------------------------------------------------------*/
static void* bench_state(void* arg) {
  test_raii_t d2("d2", &d2_destructed);
  return state_handle(&bench_exn, 42, arg);
}

static void test(void) {
  long res = 0; 
  mpt_bench{ res = mpe_long_voidp(exn_handle(&bench_state,NULL)); }
  mpt_printf("test-exn  : %ld\n", res);
  mpt_assert(res == 0 && d1_destructed && d2_destructed, "test-exn");  
}


void exn_run(void) {
  test();
}

