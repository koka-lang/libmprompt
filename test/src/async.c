#include "test.h"

// Simulate async scheduler with an effect
// that can either "fork" new strands of control,
// or where a strand can "sleep" for a while (ie. a blocking operation)

// Types
typedef void (*mpt_void_fun_t)(void) 
#define mpe_voidp_mpt_void_fun_t(v)   ((void*)(v))

// A list of actions to schedule
enum mpt_action_kind_t {
  MPT_RESUME
  MPT_FORK
};

typedef struct mpt_action_s {
  long            time;    // wait until this time; 0 is uninitialized
  mpe_resume_t*   resume;
  mpt_void_fun_t  start;
} mpt_action_t;

#define MAX_ACTIONS 100

static long now = 0        // simulate time ticks


// Effect that returns its resumption
MPE_DEFINE_EFFECT2(async, fork, sleep)
MPE_DEFINE_VOIDOP1(fork, mpt_void_fun_t)
MPE_DEFINE_VOIDOP0(sleep)

static void* op_async_sleep(mpe_resume_t* r, void* local, void* arg) {
  // insert in scheduling queue
  mpt_action_t* queue = (mpt_action_t*)local;
  mpt_void_fun_t action = (mpt_void_fun_t)(arg);
  for(int i = 0; i < MAX_ACTIONS; i++) {
    if (queue[i].time == 0) {
      queue[i].time = now;
      queue[i].resume = NULL;
      queue[i].start = action;
      break;
    }
  }
  return mpe_resume_tail(r,NULL); // and resume normally 
}

static void* op_async_sleep(mpe_resume_t* r, void* local, void* arg) {
  // insert in scheduling queue
  mpt_action_t* queue = (mpt_action_t*)local;
  long upto = now + mpe_long_voidp(arg);
  for(int i = 0; i < MAX_ACTIONS; i++) {
    if (queue[i].time == 0) {
      queue[i].time = upto;
      queue[i].resume = r;
      queue[i].start = NULL;
      break;
    }
  }
  // and find the next action to schedule instead
  while (true) {
    now++;
    for(int i = 0; i < MAX_ACTIONS; i++) {
      if (queue[i].time != 0 && queue[i].time <= now) {
        // schedule it
        queue[i].time = 0;
        if (queue[i].resume != NULL) {
          return mpe_resume_tail(queue[i].resume, NULL); // resume sleeping strand
        }
      }
    }
  }
  // unreachable
  return NULL; 
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