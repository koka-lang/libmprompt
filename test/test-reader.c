/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "test.h"

/*-----------------------------------------------------------------
  Example programs
-----------------------------------------------------------------*/

void* reader_action(void* arg) {
  UNUSED(arg);
  return mpe_voidp_long( reader_ask() + reader_ask() );
}

/*-----------------------------------------------------------------
  testing
-----------------------------------------------------------------*/
void reader_run(void) {
  long init = 42;
  long res = 0;
  mpt_bench{ res = mpe_long_voidp(reader_handle(reader_action, init, NULL)); }
  printf("reader    : %ld\n", res);
  mpt_assert(res == 2*init, "reader");
  mpt_bench{ res = mpe_long_voidp(greader_handle(reader_action, init, NULL)); }
  printf("greader   : %ld\n", res);
  mpt_assert(res == 2*init, "greader");
}

