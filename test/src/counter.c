/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "test.h"


/*-----------------------------------------------------------------
  Benchmark
-----------------------------------------------------------------*/

static void* bench_counter(void* arg) {
  UNUSED(arg);
  long count = 0;
  long i;
  while ((i = state_get()) > 0) {
    //trace_printf("counter: %i\n", i);
    state_set(i-1);
    count++;
  }
  return mpe_voidp_long(count);
}


/*-----------------------------------------------------------------
  Run
-----------------------------------------------------------------*/
static void test(long count) {
  long res = 0;
  mpt_bench{ res = mpe_long_voidp(state_handle(&bench_counter,count,NULL)); }
  mpt_printf("counter   : %ld\n", res);
  mpt_assert(res == count, "counter");

  mpt_bench{ res = mpe_long_voidp(ustate_handle(&bench_counter, count, NULL)); }
  mpt_printf("ucounter  : %ld\n", res);
  mpt_assert(res == count, "ucounter");
  
  mpt_bench{ res = mpe_long_voidp(ostate_handle(&bench_counter, count, NULL)); }
  mpt_printf("ocounter  : %ld\n", res);
  mpt_assert(res == count, "ocounter");

  mpt_bench{ res = mpe_long_voidp(gstate_handle(&bench_counter, count/10, NULL)); }  
  mpt_printf("gcounter  : %ld\n", res);
  mpt_assert(res == count/10, "gcounter");
}


void counter_run(void) {
#ifdef NDEBUG
  test(10010010L);
#else
  test(100100L);
#endif
}

