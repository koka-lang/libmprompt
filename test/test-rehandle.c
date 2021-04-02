#include "test.h"


// Effect that returns its resumption
MPE_DEFINE_EFFECT1(exit, capture)
MPE_DEFINE_VOIDOP0(exit, capture)

static void* op_exit_capture(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(arg); UNUSED(local);
  return r; // return the resumption as is
}
 
static void* exit_handle(mpe_actionfun_t action, void* arg) {
  static const mpe_handlerdef_t exit_hdef = { MPE_EFFECT(exit), NULL, NULL, NULL, {
    { MPE_OP_ONCE, MPE_OPTAG(exit,capture), &op_exit_capture },
    { MPE_OP_NULL, mpe_op_null, NULL }
  } };
  return mpe_handle(&exit_hdef, NULL, action, arg);
}


// Ask twice with an exit_capture in between
static void* body(void* arg) {
  UNUSED(arg); 
  long x = reader_ask();   // return 1
  exit_capture();          // exit and resume under a new reader
  long y = reader_ask();   // now it returns 2
  return mpe_voidp_long(x + y);
}

static void* with_exit_handle(void* arg) {
  return exit_handle(&body, arg);
}

static void* with_resume(void* arg) {
  mpe_resume_t* r = (mpe_resume_t*)arg;
  return mpe_resume_final(r, NULL, NULL);
}

static void test(void) {
  long res;
  mpt_bench{
    void* r = reader_handle(&with_exit_handle, 1, NULL);        // reader returns 1  -- final return is a resumption from with_exit_handle
    res = mpe_long_voidp(reader_handle(&with_resume, 2, r));    // new reader returns 2 -- this resumes the resumption under a new reader
  }
  printf("rehandle  : %ld\n", res);
  mpt_assert(res == 3, "test-rehandle");
}

void rehandle_run(void) {
  test();
}