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


static void* bench_counter(void* arg) {
  UNUSED(arg);
  long i = state_get() + state_get();
  if (i > 42) {
    mp_throw std::logic_error("ouch!");
  }
  return mpe_voidp_long(i);
}


/*-----------------------------------------------------------------
  Run
-----------------------------------------------------------------*/
static void* bench_reader(void* arg) {
  return reader_handle(&bench_counter, 42, arg);
}

static void test(long count) {
  long res = 0;
  try {
    mpt_bench{ res = mpe_long_voidp(state_handle(&bench_reader,count,NULL)); }
    printf("test-exn : %ld\n", res);
    mpt_assert(res == count, "test-exn");
  }
  catch (const std::exception& e) {
    printf("exception caught: %s\n", e.what());
  }
}


void throw_run(void) {
  test(100);
}

