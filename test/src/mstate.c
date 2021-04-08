/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

/* ----------------------------------------------------------------------------
   Monadic state
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
    state_set(i-1);
    count++;
  }
  return mpe_voidp_long(count);
}

/*-----------------------------------------------------------------
  functions from long -> long
-----------------------------------------------------------------*/

typedef struct function_s {
  void* env;
  long (*fun)(void*,long);
} function_t;

static long function_apply( function_t f, long arg ) {
  return (f.fun)(f.env,arg);
}

static function_t function_create( void* env, long (*f)(void*,long) ) {
  function_t fun = { env, f };
  return fun;
}

static function_t mpe_fun_voidp( void* v ) {
  function_t* p  = (function_t*)(mpe_ptr_voidp(v));
  function_t fun = *p;
  free(p);
  return fun;
}

static void* mpe_voidp_fun( function_t fun ) {
  function_t* p = (function_t*)malloc( sizeof(function_t) );
  if (p!=NULL) *p = fun;
  return mpe_voidp_ptr(p);
}

/*-----------------------------------------------------------------
  monadic state handler
-----------------------------------------------------------------*/

// fn(x:long){ fn(s){ x } }
static long fun_result( void* env, long st ) {
  UNUSED(env);
  UNUSED(st);
  return mpe_long_voidp(env);
}

static void* _mstate_result(void* local, void* arg) {
  UNUSED(local);
  //trace_prlongf("state result: %i, %li\n", *((long*)local), (long)(x));
  return mpe_voidp_fun( function_create(arg,&fun_result) ); 
} 

// control get(){ fn(s){ resume(s)(s) } }
static long fun_get( void* venv, long st ) {
  mpe_resume_t* rc = (mpe_resume_t*)(venv);
  void* res = mpe_resume_final(rc, NULL, mpe_voidp_long(st));
  function_t f = mpe_fun_voidp( res );
  return function_apply( f, st );
}

static void* _mstate_get(mpe_resume_t* rc, void* local, void* arg) {
  UNUSED(arg);
  UNUSED(local);
  //trace_prlongf("state get: %i\n", *((long*)local));
  return mpe_voidp_fun( function_create( rc, &fun_get ) );
}


struct env_put_t {
  long newst;
  mpe_resume_t* rc;
};

// control put(s'){ fn(s){ resume(())(s') } }
static long fun_put( void* venv, long st ) {
  UNUSED(st);
  struct env_put_t env = *((struct env_put_t*)(venv));
  free(venv);
  function_t f = mpe_fun_voidp( mpe_resume_final( env.rc, NULL, NULL ) );
  return function_apply( f, env.newst );
}

static void* _mstate_set(mpe_resume_t* rc, void* local, void* st) {
  UNUSED(local);
  struct env_put_t* env = (struct env_put_t*)malloc( sizeof(struct env_put_t) );
  env->newst = mpe_long_voidp(st);
  env->rc = rc;
  return mpe_voidp_fun( function_create( env, &fun_put ) );
}

static const mpe_handlerdef_t mstate_def = { MPE_EFFECT(state), NULL, NULL, &_mstate_result, {
  { MPE_OP_ONCE, MPE_OPTAG(state,get), &_mstate_get },
  { MPE_OP_ONCE, MPE_OPTAG(state,set), &_mstate_set },
  { MPE_OP_NULL, mpe_op_null, NULL }
} };

static void* mstate_handle(mpe_actionfun_t action, long st, void* arg) {
  function_t f = mpe_fun_voidp( mpe_handle(&mstate_def, NULL, action, arg) );
  return mpe_voidp_long( function_apply( f, st ) );
}

/*-----------------------------------------------------------------
  Run
-----------------------------------------------------------------*/
static void test(long count) {
  long res = 0;
  mpt_bench{
    res = mpe_int_voidp(mstate_handle(&bench_counter, count, NULL));
  }
  printf("mstate    : %ld\n", res);
  mpt_assert(res == count, "mstate");
}


void mstate_run(void) {
#ifdef NDEBUG
  test(1000L);
  // test(10100100L);  // only if the compiler does tail-call optimization
#else
  test(100L);
#endif
}

