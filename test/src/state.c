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
MPE_DEFINE_EFFECT2(state, get, set)
MPE_DEFINE_OP0(state, get, int)
MPE_DEFINE_VOIDOP1(state, set, int)

/*-----------------------------------------------------------------
  Example programs
-----------------------------------------------------------------*/

void* state_action(void* arg) {
  UNUSED(arg);
  int i;
  int count = 0;
  while ((i = state_get()) > 0) {
    //printf("counter: %i\n", i);
    count++;
    state_set(i-1);
  }
  return mpe_voidp_int(count);
}

/*-----------------------------------------------------------------
  Reader handler
-----------------------------------------------------------------*/

static void* _state_get(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(arg);
  return mpe_resume_tail(r, local, local);
}

static void* _state_set(mpe_resume_t* r, void* local, void* arg) {
  return mpe_resume_tail(r, arg, NULL);
}

static const mpe_handlerdef_t state_hdef = { MPE_EFFECT(state), NULL, NULL, NULL, {
  { MPE_OP_SCOPED_ONCE, MPE_OPTAG(state,get), &_state_get },
  { MPE_OP_SCOPED_ONCE, MPE_OPTAG(state,set), &_state_set },
  //  { MPE_OP_GENERAL, MPE_OPTAG(state,get), &_state_get },
  //  { MPE_OP_GENERAL, MPE_OPTAG(state,set), &_state_set },
  //  { MPE_OP_TAIL_NOOP, MPE_OPTAG(state,get), &_state_get },
  //  { MPE_OP_TAIL_NOOP, MPE_OPTAG(state,set), &_state_set },
  { MPE_OP_NULL, mpe_op_null, NULL }
}};

static void* state_handle(mpe_actionfun_t action, int init, void* arg) {
  return mpe_handle(&state_hdef, mpe_voidp_int(init), action, arg);
}

/*-----------------------------------------------------------------
  testing
-----------------------------------------------------------------*/
static void test(int init) {
  int res = mpe_int_voidp(state_handle(&state_action, init, NULL));
  printf("state: %d\n", res);
}
void state_run(void) {
#ifdef NDEBUG
  test(10100100L);
#else
  test(100);
#endif
}

/*
void test_state()
{
  test("state", run,
    "final result counter: 42\n"
  );
}
*/
