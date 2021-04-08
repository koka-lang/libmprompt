/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
   Ambiguity with state
-----------------------------------------------------------------------------*/

#include "test.h"

/*-----------------------------------------------------------------
  Benchmark
-----------------------------------------------------------------*/

static bool xxor(void) {
  bool x = amb_flip();
  bool y = amb_flip();
  return ((x && !y) || (!x && y));
}


static void* foo(void* arg) {
  UNUSED(arg);
  bool p = amb_flip();
  long i = state_get();
  state_set(i + 1);
  bool b = ((i > 0 && p) ? xxor() : false);
  return mpe_voidp_bool(b);
}


/*-----------------------------------------------------------------
  Test
-----------------------------------------------------------------*/

static void print_bool(void* arg) {
  mpt_printf("%s", mpe_bool_voidp(arg) ? "true" : "false" );
}

static void* hstate( void* arg ) {
  return state_handle( &foo, 0, arg );
}

static blist amb_state(void) {
  return amb_handle( &hstate, NULL );
}

static void* hamb(void* arg) {
  return amb_handle(&foo, arg);
}

static blist state_amb(void) {
  return mpe_blist_voidp(state_handle(&hamb, 0, NULL));
}

static void test() {
  blist xs = NULL;
  mpt_bench{ xs = amb_state(); }
  mpt_printf("amb-state : "); blist_println(xs, &print_bool); 
  mpt_assert(blist_length(xs)==2, "amb-state");
  blist_free(xs);

  mpt_bench{ xs = state_amb(); }
  mpt_printf("state-amb : "); blist_println(xs, &print_bool);
  mpt_assert(blist_length(xs) == 5, "state-amb");
  blist_free(xs);
}


void amb_state_run(void) {
  test();
}

