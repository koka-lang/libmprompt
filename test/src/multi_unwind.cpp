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

MPE_DEFINE_EFFECT1(multi, unwind)
MPE_DEFINE_OP0(multi, unwind, long)



/*-----------------------------------------------------------------
  Benchmark
-----------------------------------------------------------------*/
static bool d3_destructed;

static void* bench_main(void* arg) {
  test_raii_t d3("d3", &d3_destructed);
  UNUSED(arg);
  long i = multi_unwind() + multi_unwind();
  return mpe_voidp_long(i);
}


/*-----------------------------------------------------------------
   multi-unwind handler
-----------------------------------------------------------------*/



static void* handle_multi_unwind(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(local);
  UNUSED(arg);
  // void* res = mpe_resume(r, mpe_voidp_long(1), mpe_voidp_long(42));
  mpe_resume_release(r);
  return mpe_voidp_long(42);
}
  

static const mpe_handlerdef_t multi_hdef = { MPE_EFFECT(multi), NULL, NULL, NULL, {
  { MPE_OP_MULTI, MPE_OPTAG(multi,unwind), &handle_multi_unwind },
  { MPE_OP_NULL, mpe_op_null, NULL }
}};

void* multi_handle(mpe_actionfun_t* action, void* arg) {
  return mpe_handle(&multi_hdef, NULL, action, arg);
}

/*-----------------------------------------------------------------
  Run
-----------------------------------------------------------------*/

static void test(void) {
  long res = 0; 
  mpt_bench{ res = mpe_long_voidp(multi_handle(&bench_main,NULL)); }
  printf("test-multi-unwind  : %ld\n", res);
  mpt_assert(res == 42 && d3_destructed, "test-multi-unwind");  
}


void multi_unwind_run(void) {
  test();
}

