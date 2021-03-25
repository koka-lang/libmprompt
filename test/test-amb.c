/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
   Ambiguity
-----------------------------------------------------------------------------*/

#include "test.h"

/*-----------------------------------------------------------------
  Benchmark
-----------------------------------------------------------------*/

static void* bench_xor(void* arg) {
  UNUSED(arg);
  bool x = amb_flip();
  bool y = amb_flip();
  return mpe_voidp_bool((x && !y) || (!x && y));
}

/*-----------------------------------------------------------------
  Bench
-----------------------------------------------------------------*/

static void print_bool(void* arg) {
  printf("%s", mpe_bool_voidp(arg) ? "true" : "false" );
}

static void test() {
  blist xs = NULL;
  mpt_bench{ xs = mpe_blist_voidp(amb_handle(&bench_xor, NULL)); }
  printf("amb:      : "); blist_println(xs, &print_bool); 
  mpt_assert(blist_length(xs)==4, "ambxor");
  blist_free(xs);  
}


void amb_run(void) {
  test();
}

