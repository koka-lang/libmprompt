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


class test_raii_t {
private:
  const char* msg;
public:
  test_raii_t(const char* s) : msg(s) { }
  ~test_raii_t() {
    fprintf(stderr, "destruct: %s\n", msg);
  }
};

/*-----------------------------------------------------------------
  Benchmark
-----------------------------------------------------------------*/


static void* bench_exn(void* arg) {
  test_raii_t("d1");
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
  test_raii_t("d2");
  return state_handle(&bench_exn, 42, arg);
}

static void test(void) {
  long res = 0; 
  mpt_bench{ res = mpe_long_voidp(exn_handle(&bench_state,NULL)); }
  printf("test-exn  : %ld\n", res);
  mpt_assert(res == 0, "test-exn");  
}


void exn_run(void) {
  test();
}

