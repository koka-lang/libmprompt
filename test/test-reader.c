/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "test.h"


static long stack_use(uint8_t* p, long kb) {
  if (kb <= 4) return reader_ask();

  if (p != NULL) p[4095] = 1;
  uint8_t arr[4096];
  arr[4095] = 0;
  return stack_use(arr,kb - 4);
}

/*-----------------------------------------------------------------
  Example programs
-----------------------------------------------------------------*/

void* reader_action(void* arg) {
  UNUSED(arg);
  long i = stack_use(NULL,64);
  return mpe_voidp_long( i + reader_ask());
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

