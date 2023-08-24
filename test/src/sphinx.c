/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  The sphinx effect will only allow performers to continue execution
  when they provide the correct answer to their riddle
-----------------------------------------------------------------------------*/
#include "test.h"
#include <string.h>


/*-----------------------------------------------------------------
  Define operations
-----------------------------------------------------------------*/
MPE_DEFINE_EFFECT1(sphinx, answer)
MPE_DEFINE_VOIDOP1(sphinx, answer, mpe_string_t)

/*-----------------------------------------------------------------
  Example programs
-----------------------------------------------------------------*/

void* brian(void* arg){
    UNUSED(arg);
    // Arrive at Thebes
    sphinx_answer("Scooters");
    // Die
    mpt_assert(false, "Brian should have been eaten by the sphinx");
    return mpe_voidp_int(1);
}


void* oedipus(void* arg){
    UNUSED(arg);
    // Arrive at Thebes
    sphinx_answer("Person");
    // Free Thebes
    return mpe_voidp_int(1);
}

/*-----------------------------------------------------------------
  Sphinx handler
-----------------------------------------------------------------*/

static void* _sphinx_answer(mpe_resume_t* r, void* local, void* arg){
    if (strcmp(mpe_mpe_string_t_voidp(arg), "Person") == 0) {
        return mpe_resume_tail(r, local, NULL);
    } else {
        // I couldn't find a way to release it, this one is not intended for MPE_RESUMPTION_INPLACE
        // mpe_resume_release(r);
        return mpe_voidp_int(0);
    }
}

static const mpe_handlerdef_t sphinx_hdef = { MPE_EFFECT(sphinx), NULL, {
    { MPE_OP_TAIL, MPE_OPTAG(sphinx, answer), &_sphinx_answer },
    { MPE_OP_NULL, mpe_op_null, NULL}
}};

static void* sphinx_handle(mpe_actionfun_t action) {
    return mpe_handle(&sphinx_hdef, NULL, action, NULL);
}

static const mpe_handlerdef_t sphinx_noop_hdef = { MPE_EFFECT(sphinx), NULL, {
    { MPE_OP_TAIL_NOOP, MPE_OPTAG(sphinx, answer), &_sphinx_answer },
    { MPE_OP_NULL, mpe_op_null, NULL}
}};

static void* sphinx_noop_handle(mpe_actionfun_t action) {
    return mpe_handle(&sphinx_noop_hdef, NULL, action, NULL);
}

/*-----------------------------------------------------------------
  testing
-----------------------------------------------------------------*/

static void oedipus_tail() {
    int res = mpe_int_voidp(sphinx_handle(&oedipus));
    mpt_printf("state: %d\n", res);
    mpt_assert(res == 1, "Oedipus should pass");
}

static void oedipus_tail_noop() {
    int res = mpe_int_voidp(sphinx_noop_handle(&oedipus));
    mpt_printf("state: %d\n", res);
    mpt_assert(res == 1, "Oedipus should pass");
}

static void brian_tail() {
    int res = mpe_int_voidp(sphinx_handle(&brian));
    mpt_printf("state: %d\n", res);
    mpt_assert(res == 0, "Brian shouldn't pass");
}

static void brian_tail_noop() {
    int res = mpe_int_voidp(sphinx_noop_handle(&brian));
    mpt_printf("state: %d\n", res);
    mpt_assert(res == 0, "Brian shouldn't pass");
}

void sphinx_run(void) {
    oedipus_tail();
    oedipus_tail_noop();
    brian_tail();
    brian_tail_noop();
}