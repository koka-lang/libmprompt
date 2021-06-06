/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "test.h"


/*-----------------------------------------------------------------
  Define operations
-----------------------------------------------------------------*/
MPE_DEFINE_EFFECT1(yield, yield)
MPE_DEFINE_VOIDOP1(yield, yield, long)


/*-----------------------------------------------------------------
  Benchmark
-----------------------------------------------------------------*/

static void triples( long n, long s ) {
  long x = choice_choose(n);
  long y = choice_choose(x-1);
  long z = choice_choose(y-1);
  if (x+y+z == s) yield_yield(x);
             else choice_fail();
}

static void* do_triples(void* arg) {
  long l = mpe_long_voidp(arg);
  long n = (l >> 16);
  long s = (l & 0xFFFF);
  triples(n,s);
  return mpe_voidp_int(0);
}

/*-----------------------------------------------------------------
 choice handler
-----------------------------------------------------------------*/

static void* _choice_result(void* local, void* arg) {
  UNUSED(local);  
  return arg;
}

static void* _choice_fail(mpe_resume_t* rc, void* local, void* arg) {
  UNUSED(local);
  UNUSED(arg);  
  mpe_resume_release(rc);
  return mpe_voidp_long(0);
}

static void* _choice_choose(mpe_resume_t* rc, void* local, void* arg) {
  UNUSED(local);
  long n = mpe_long_voidp(arg);  
  for( long i = 1; i <= n; i++) {
    if (i!=n) mpe_resume(rc,local,mpe_voidp_long(i));
         else mpe_resume_final(rc,local,mpe_voidp_long(i));
  }
  //mpe_release(rc);
  return mpe_voidp_long(0);
}
  


static const mpe_handlerdef_t choice_def = { MPE_EFFECT(choice), &_choice_result, {
  { MPE_OP_SCOPED, MPE_OPTAG(choice,choose), &_choice_choose },
  { MPE_OP_ABORT,  MPE_OPTAG(choice,fail), &_choice_fail },
  { MPE_OP_NULL, mpe_op_null, NULL }
} };

static void* xchoice_handle(void*(*action)(void*), void* arg) {
  return mpe_handle(&choice_def, mpe_voidp_null, action, arg);
}



static void* _yield_result(void* local, void* arg) {
  UNUSED(arg);
  //trace_printf("reader result: %i, %li\n", *((long*)local), (long)(x));
  return local;
} 

static void* _yield_yield(mpe_resume_t* rc, void* local, void* arg) {
  UNUSED(arg);
  //trace_printf("reader ask: %i\n", *((long*)local));
  return mpe_resume_tail(rc, mpe_voidp_long( mpe_long_voidp(local)+1 ), local);
}

static const mpe_handlerdef_t yield_def = { MPE_EFFECT(yield),  &_yield_result, {
  { MPE_OP_TAIL_NOOP, MPE_OPTAG(yield,yield), &_yield_yield },
  { MPE_OP_NULL, mpe_op_null, NULL }
} };

static void* yield_handle(void*(*action)(void*), long val, void* arg) {
  return mpe_handle(&yield_def, mpe_voidp_long(val), action, arg);
}

/*-----------------------------------------------------------------
 bench
-----------------------------------------------------------------*/

static void* do_choose_triples( void* arg ) {
  return xchoice_handle( &do_triples, arg );
}

static void test(int n, int s, long expect) {
  long count = 0;
  mpt_bench{ 
    count = mpe_long_voidp( yield_handle( &do_choose_triples, 0, mpe_voidp_long(n << 16 | s) ) );
  }
  printf("triples %2d,%2d: %ld\n", n, s, count);
  mpt_assert(expect == count, "triples");
}


void triples_run(void) {
#ifdef NDEBUG
  test(500,127,1281);
#else
  test(100, 27, 48);
#endif
}
