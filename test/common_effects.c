/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "test.h"
#include <assert.h>

/*-----------------------------------------------------------------
  Standard effect implementations
-----------------------------------------------------------------*/

/*-----------------------------------------------------------------
  Reader
-----------------------------------------------------------------*/
MPE_DEFINE_EFFECT1(reader, ask)
MPE_DEFINE_OP0(reader, ask, long)

// Tail optimized reader 
static void* handle_reader_ask(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(arg);
  return mpe_resume_tail(r, local, local);
}
 
void* reader_handle(mpe_actionfun_t action, long init, void* arg) {
  static const mpe_handlerdef_t reader_hdef = { MPE_EFFECT(reader), NULL, {
    { MPE_OP_TAIL_NOOP, MPE_OPTAG(reader,ask), &handle_reader_ask },
    { MPE_OP_NULL, mpe_op_null, NULL }
  } };
  return mpe_handle(&reader_hdef, mpe_voidp_long(init), action, arg);
}

// General reader that uses full resumptions
static void* handle_greader_ask(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(arg);
  return mpe_resume_tail(r, local, mpe_voidp_long(42));
}
 
void* greader_handle(mpe_actionfun_t action, long init, void* arg) {
  static const mpe_handlerdef_t greader_hdef = { MPE_EFFECT(reader), NULL, {
    { MPE_OP_SCOPED_ONCE, MPE_OPTAG(reader,ask), &handle_greader_ask },
    { MPE_OP_NULL, mpe_op_null, NULL }
  } };
  return mpe_handle(&greader_hdef, mpe_voidp_long(init), action, arg);
}


/*-----------------------------------------------------------------
  Exception
-----------------------------------------------------------------*/
MPE_DEFINE_EFFECT1(exn, raise)
MPE_DEFINE_VOIDOP1(exn, raise, mpe_string_t)

static void* handle_exn_raise(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(local); UNUSED(r);
  fprintf(stderr, "exn raised: %s\n", (const char*)arg);
  return NULL;
}

void* exn_handle(mpe_actionfun_t action, void* arg) {
  static const mpe_handlerdef_t exn_hdef = { MPE_EFFECT(exn), NULL, {
    { MPE_OP_NEVER, MPE_OPTAG(exn,raise), &handle_exn_raise },
    { MPE_OP_NULL, mpe_op_null, NULL }
  } };
  return mpe_handle(&exn_hdef, NULL, action, arg);
}


/*-----------------------------------------------------------------
  State
-----------------------------------------------------------------*/
MPE_DEFINE_EFFECT2(state, get, set)
MPE_DEFINE_OP0(state, get, long)
MPE_DEFINE_VOIDOP1(state, set, long)

static void* handle_state_get(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(arg);
  return mpe_resume_tail(r, local, local);
}

static void* handle_state_set(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(local);
  return mpe_resume_tail(r, arg, NULL);
}

static const mpe_handlerdef_t state_hdef = { MPE_EFFECT(state), NULL, {
  { MPE_OP_TAIL_NOOP, MPE_OPTAG(state,get), &handle_state_get },
  { MPE_OP_TAIL_NOOP, MPE_OPTAG(state,set), &handle_state_set },
  { MPE_OP_NULL, mpe_op_null, NULL }
}};

void* state_handle(mpe_actionfun_t action, long init, void* arg) {
  return mpe_handle(&state_hdef, mpe_voidp_long(init), action, arg);
}


// Variants

static const mpe_handlerdef_t ustate_hdef = { MPE_EFFECT(state), NULL, {
  { MPE_OP_TAIL, MPE_OPTAG(state,get), &handle_state_get },
  { MPE_OP_TAIL, MPE_OPTAG(state,set), &handle_state_set },
  { MPE_OP_NULL, mpe_op_null, NULL }
} };

void* ustate_handle(mpe_actionfun_t action, long init, void* arg) {
  return mpe_handle(&ustate_hdef, mpe_voidp_long(init), action, arg);
}

static const mpe_handlerdef_t ostate_hdef = { MPE_EFFECT(state), NULL, {
  { MPE_OP_SCOPED_ONCE, MPE_OPTAG(state,get), &handle_state_get },
  { MPE_OP_SCOPED_ONCE, MPE_OPTAG(state,set), &handle_state_set },
  { MPE_OP_NULL, mpe_op_null, NULL }
} };

void* ostate_handle(mpe_actionfun_t action, long init, void* arg) {
  return mpe_handle(&ostate_hdef, mpe_voidp_long(init), action, arg);
}

static const mpe_handlerdef_t gstate_hdef = { MPE_EFFECT(state), NULL, {
  { MPE_OP_MULTI, MPE_OPTAG(state,get), &handle_state_get },
  { MPE_OP_MULTI, MPE_OPTAG(state,set), &handle_state_set },
  { MPE_OP_NULL, mpe_op_null, NULL }
} };

void* gstate_handle(mpe_actionfun_t action, long init, void* arg) {
  return mpe_handle(&gstate_hdef, mpe_voidp_long(init), action, arg);
}


/*-----------------------------------------------------------------
   ambiguity handler
-----------------------------------------------------------------*/

MPE_DEFINE_EFFECT1(amb, flip)
MPE_DEFINE_OP0(amb, flip, bool)

// return(x){ [x] }
static void* handle_amb_result(void* local, void* arg) {
  UNUSED(local);  
  return mpe_voidp_blist( blist_single(arg) );
}

// control amb(){ resume(False) ++ resume(True) }
static void* handle_amb_flip(mpe_resume_t* rc, void* local, void* arg) {
  UNUSED(rc);
  UNUSED(local);
  UNUSED(arg);
  blist xs = mpe_blist_voidp( mpe_resume(rc,local,mpe_voidp_bool(false)));
  blist ys = mpe_blist_voidp( mpe_resume_final(rc,local,mpe_voidp_bool(true)));
  return mpe_voidp_blist(blist_appendto(xs,ys));
}
  

static const mpe_handlerdef_t amb_def = { MPE_EFFECT(amb), &handle_amb_result, {
  { MPE_OP_SCOPED, MPE_OPTAG(amb,flip), &handle_amb_flip },
  { MPE_OP_NULL, mpe_op_null, NULL }
}};

blist amb_handle(mpe_actionfun_t* action, void* arg) {
  return mpe_blist_voidp( mpe_handle(&amb_def, NULL, action, arg) );
}

/*-----------------------------------------------------------------
  choice handler
-----------------------------------------------------------------*/

MPE_DEFINE_EFFECT2(choice, choose, fail)
MPE_DEFINE_OP1(choice, choose, long, long)
MPE_DEFINE_VOIDOP0(choice, fail)


static void* handle_choice_result(void* local, void* arg) {
  UNUSED(local);  
  return mpe_voidp_blist( blist_single(arg) );
}

static void* handle_choice_fail(mpe_resume_t* rc, void* local, void* arg) {
  UNUSED(rc);
  UNUSED(local);
  UNUSED(arg);
  //assert(rc == NULL);
  mpe_resume_release(rc);
  return mpe_voidp_blist( NULL );
}

static void* handle_choice_choose(mpe_resume_t* rc, void* local, void* arg) {
  UNUSED(rc);
  UNUSED(local);
  long max = mpe_long_voidp(arg);
  blist xss = blist_nil;
  for( long i = 1; i <= max; i++) {
    blist yss = mpe_blist_voidp( (i<max ? mpe_resume(rc,local,mpe_voidp_long(i))
                                        : mpe_resume_final(rc, local, mpe_voidp_long(i))) );
    xss = blist_appendto(yss,xss); // hmm, reversed order
  }
  return mpe_voidp_blist(xss);
}
  

static const mpe_handlerdef_t choice_def = { MPE_EFFECT(choice), &handle_choice_result, {
  { MPE_OP_SCOPED, MPE_OPTAG(choice,choose), &handle_choice_choose },
  { MPE_OP_ABORT,  MPE_OPTAG(choice,fail), &handle_choice_fail },
  //{ MPE_OP_NEVER,  MPE_OPTAG(choice,fail), &handle_choice_fail },  // very slow in C++: nqueens(12) is about 15s vs. 0.6s with abort.
  { MPE_OP_NULL, mpe_op_null, NULL }
}};

blist choice_handle(mpe_actionfun_t* action, void* arg) {
  return mpe_blist_voidp( mpe_handle(&choice_def, mpe_voidp_null, action, arg) );
}
