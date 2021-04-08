/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "test.h"


static bool safe( int queen, blist xs ) {
  for( int diag = 1; xs != NULL; diag++, xs = xs->next) {
    int q = mpe_int_voidp(xs->value);
    if (queen == q || queen == q + diag || queen == q - diag) return false;    
  }
  return true;
}

static blist find_solution( int n, int col ) {
  if (col == 0) return NULL;
  blist sol = find_solution( n, col - 1);
  int queen = choice_choose( n );
  if (safe(queen,sol)) {
    return blist_cons(mpe_voidp_int(queen),sol);
  }
  else {
    choice_fail();
    return NULL; 
  }
}

static void* bench_nqueens(void* arg) {
  int n = mpe_int_voidp(arg);
  return mpe_voidp_blist( find_solution(n,n) );
}


/*-----------------------------------------------------------------
 bench
-----------------------------------------------------------------*/

static void test(int n, int expect) {
  blist xss = NULL;
  mpt_bench{
    xss = mpe_blist_voidp(choice_handle(&bench_nqueens, mpe_voidp_int(n)));
  }
  int len = blist_length(xss);
  mpt_printf("nqueens %2d: %d\n", n, len);
  mpt_assert(expect == len, "nqueens");
}


void nqueens_run(void) {
#ifdef NDEBUG
  test(12, 14200);
#else
  test(8, 92);
#endif
}

