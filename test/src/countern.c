/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
   Counter10: state under 10 readers
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
static void* bench_reader1(void* arg) {
  return reader_handle(&bench_counter, 1, arg);
}
static void* bench_reader2(void* arg) {
  return reader_handle(&bench_reader1, 2, arg);
}
static void* bench_reader3(void* arg) {
  return reader_handle(&bench_reader2, 3, arg);
}
static void* bench_reader4(void* arg) {
  return reader_handle(&bench_reader3, 4, arg);
}
static void* bench_reader5(void* arg) {
  return reader_handle(&bench_reader4,5,arg);
}
static void* bench_reader6(void* arg) {
  return reader_handle(&bench_reader5, 6, arg);
}
static void* bench_reader7(void* arg) {
  return reader_handle(&bench_reader6, 7, arg);
}
static void* bench_reader8(void* arg) {
  return reader_handle(&bench_reader7, 8, arg);
}
static void* bench_reader9(void* arg) {
  return reader_handle(&bench_reader8, 9, arg);
}
static void* bench_reader10(void* arg) {
  return reader_handle(&bench_reader9, 10, arg);
}

static void test(long count) {
  long res = 0;
  /*mpt_bench{ res = mpe_long_voidp(state_handle(&bench_reader1,count,NULL)); }
  mpt_printf("counter1  : %ld\n", res);
  mpt_assert(res == count, "counter1");*/

  mpt_bench{ res = mpe_long_voidp(ostate_handle(&bench_reader1,count,NULL)); }
  mpt_printf("ocounter1 : %ld\n", res);
  mpt_assert(res == count, "ocounter1");

  mpt_bench{ res = mpe_long_voidp(state_handle(&bench_reader10,count,NULL)); }  
  mpt_printf("counter10 : %ld\n", res);
  mpt_assert(res == count, "counter10");
  
  mpt_bench{ res = mpe_long_voidp(ostate_handle(&bench_reader10,count,NULL)); }
  mpt_printf("ocounter10: %ld\n", res);
  mpt_assert(res == count, "ocounter10");
}

void countern_run(void) {
  #ifdef NDEBUG
  test(10010010L);
  #else
  test(100100L);
  #endif
}

