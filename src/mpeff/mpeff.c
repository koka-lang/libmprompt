/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <mphnd.h>
#include "mpeff.h"


/*-----------------------------------------------------------------
  Defines
-----------------------------------------------------------------*/

#ifdef __cplusplus
#define MPE_HAS_TRY  (1)
#include <exception>
#else
#define MPE_HAS_TRY  (0)
#endif

#if defined(_MSC_VER)
#define mpe_decl_noinline        __declspec(noinline)
#define mpe_decl_thread          __declspec(thread)
#elif (defined(__GNUC__) && (__GNUC__>=3))  // includes clang and icc
#define mpe_decl_noinline        __attribute__((noinline))
#define mpe_decl_thread          __thread
#else
#define mpe_decl_noinline
#define mpe_decl_thread          __thread  
#endif

#if defined(__GNUC__) || defined(__clang__)
#define mpe_unlikely(x)          __builtin_expect((x),0)
#define mpe_likely(x)            __builtin_expect((x),1)
#else
#define mpe_unlikely(x)          (x)
#define mpe_likely(x)            (x)
#endif

#define mpe_assert(x)            assert(x)
#define mpe_assert_internal(x)   mpe_assert(x)
#define mpe_malloc_tp(tp)        (tp*)mpe_malloc_safe(sizeof(tp))


static inline void* mpe_malloc_safe(size_t size) {
  void* p = malloc(size);
  if (p != NULL) return p;
  fprintf(stderr,"out of memory\n");
  abort();
}

static inline void mpe_free(void* p) {
  free(p);
}


/*-----------------------------------------------------------------
  Types
-----------------------------------------------------------------*/

// Resumption kinds: used to avoid allocation etc.
typedef enum mpe_resumption_kind_e {
  MPE_RESUMPTION_INPLACE,           
  MPE_RESUMPTION_SCOPED_ONCE,       
  MPE_RESUMPTION_ONCE,              
  MPE_RESUMPTION_MULTI
} mpe_resumption_kind_t;


// A resumption
struct mpe_resume_s {
  mpe_resumption_kind_t rkind;      // todo: encode rkind in lower bits so we can avoid allocating resumes?
  union {
    void** plocal;
    mph_resume_t* resume;
  } mp;
};




/*-----------------------------------------------------------------
  Effect and optag names
-----------------------------------------------------------------*/

const char* mpe_effect_name(mpe_effect_t effect) {
  return (effect == NULL ? "<null>" : effect[0]);
}

const char* mpe_optag_name(mpe_optag_t optag) {
  return (optag == NULL ? "<null>" : optag->effect[optag->opidx + 1]);
}


static mph_kind_t mpe_effect_kind(mpe_effect_t effect) {
  return effect[0];
}


/*-----------------------------------------------------------------
  Perform
-----------------------------------------------------------------*/

static mpe_handlerdef_t* mpe_hdef(mph_handler_t* h) {
  return (mpe_handlerdef_t*)(mph_get_data(h));
}


typedef struct mpe_perform_env_s {
  mpe_opfun_t* opfun;
  void* arg;
} mpe_perform_env_t;


typedef struct mpe_perform_under_env_s {
  void** plocal;
  mpe_opfun_t* opfun;
  void* arg;
} mpe_perform_under_env_t;


// Under
static void* mpe_perform_under(void* envarg) {
  mpe_perform_under_env_t* env = (mpe_perform_under_env_t*)envarg;
  mpe_resume_t resume = { MPE_RESUMPTION_INPLACE, { env->plocal } };
  return (env->opfun)(&resume, *env->plocal, env->arg);
}


// Never
static void* mpe_perform_never(void* local, void* funarg, void* arg) {
  return ((mpe_opfun_t*)funarg)(NULL, local, arg);
}


// Scoped once
static void* mpe_perform_op_scoped_once(mph_resume_t* r, void* local, void* envarg) {
  mpe_perform_env_t* env = (mpe_perform_env_t*)envarg;
  mpe_resume_t resume;
  resume.rkind = MPE_RESUMPTION_SCOPED_ONCE; // scoped so we can allocate on the stack
  resume.mp.resume = r;
  return (env->opfun)(&resume, local, env->arg);
}

static void* mpe_perform_yield_to_scoped_once(mph_handler_t* mph, const mpe_operation_t* op, void* arg) {
  mpe_perform_env_t env = { op->opfun, arg };
  return mph_yield_to(mph, &mpe_perform_op_scoped_once, &env);
}

// Once
static void* mpe_perform_op(mph_resume_t* r, void* local, void* envarg) {
  mpe_perform_env_t* env = (mpe_perform_env_t*)envarg;
  mpe_resume_t* resume = mpe_malloc_tp(mpe_resume_t);
  resume->rkind  = MPE_RESUMPTION_ONCE;
  resume->mp.resume = r;
  return (env->opfun)(resume, local, env->arg);
}

static void* mpe_perform_yield_to(mph_handler_t* mph, const mpe_operation_t* op, void* arg) {
  mpe_perform_env_t env = { op->opfun, arg };
  return mph_yield_to(mph, &mpe_perform_op, &env);
}

// Multi
static void* mpe_perform_op_multi(mph_resume_t* r, void* local, void* envarg) {
  mpe_perform_env_t* env = (mpe_perform_env_t*)envarg;
  mpe_resume_t* resume = mpe_malloc_tp(mpe_resume_t);
  resume->rkind = MPE_RESUMPTION_MULTI;
  resume->mp.resume = r;
  return (env->opfun)(resume, local, env->arg);
}

static void* mpe_perform_yield_to_multi(mph_handler_t* mph, const mpe_operation_t* op, void* arg) {
  mpe_perform_env_t env = { op->opfun, arg };
  return mph_myield_to(mph, &mpe_perform_op_multi, &env);
}



// ------------------------------------------------------------------------------
// Perform
// ------------------------------------------------------------------------------

static void* mpe_perform_at(mph_handler_t* mph, mpe_optag_t optag, void* arg) {
  const mpe_operation_t* op = &mpe_hdef(mph)->operations[optag->opidx];
  mpe_opkind_t opkind = op->opkind;  
  if (mpe_likely(opkind == MPE_OP_TAIL_NOOP)) {
    // tail resumptive, calls no operations, execute in place
    void** plocal = mph_get_local_byref(mph);
    mpe_resume_t resume = { MPE_RESUMPTION_INPLACE, {plocal} };
    return (op->opfun)(&resume, *plocal, arg);
  }
  else if (mpe_likely(opkind == MPE_OP_TAIL)) {
    // tail resumptive; execute in place under an "under" frame
    mpe_perform_under_env_t env = { mph_get_local_byref(mph), op->opfun, arg };
    return mph_under( mph_get_kind(mph), &mpe_perform_under, &env);
  }
  else if (opkind == MPE_OP_SCOPED_ONCE) {
    return mpe_perform_yield_to_scoped_once(mph, op, arg);
  }
  else if (opkind == MPE_OP_ONCE) {
    return mpe_perform_yield_to(mph, op, arg);
  }
  else if (opkind == MPE_OP_NEVER) {
    mph_unwind_to(mph, &mpe_perform_never, (void*)(op->opfun), arg);
    return NULL; // never reached
  }
  else if (opkind == MPE_OP_ABORT) {
    mph_abort_to(mph, &mpe_perform_never, (void*)(op->opfun), arg);
    return NULL;
  }
  else {
    return mpe_perform_yield_to_multi(mph, op, arg);    
  }
}

static mpe_decl_noinline void* mpe_unhandled_operation(mpe_optag_t optag) {
  fprintf(stderr, "unhandled operation: %s\n", mpe_optag_name(optag));
  return NULL;
}


// Perform finds the innermost handler and performs the operation
void* mpe_perform(mpe_optag_t optag, void* arg) {
  mph_handler_t* h = mph_find(mpe_effect_kind(optag->effect));
  if (mpe_unlikely(h == NULL)) return mpe_unhandled_operation(optag);  
  return mpe_perform_at(h, optag, arg);
}



/*-----------------------------------------------------------------
  Handle
-----------------------------------------------------------------*/

// Pass arguments down in a closure environment
struct mpe_handle_start_env {
  const mpe_handlerdef_t* hdef;
  void*            local;
  mpe_actionfun_t* body;
  void*            arg;
};

// Start a handler
static void* mpe_handle_start(mph_handler_t* h, void* earg) {
  // init
  struct mpe_handle_start_env* env = (struct mpe_handle_start_env*)earg;
  void* result = (env->body)(env->arg);
  // potentially run return function
  // todo: this is wrong at operations in the return function are still executed under the handler
  if (mpe_hdef(h)->resultfun != NULL) {
    result = mpe_hdef(h)->resultfun(mph_get_local(h), result);
  }
  return result;
}

/// Handle a particular effect.
/// Handles operations yielded in `body(arg)` with the given handler definition `def`.
void* mpe_handle(const mpe_handlerdef_t* hdef, void* local, mpe_actionfun_t* body, void* arg) {
  struct mpe_handle_start_env env = { hdef, local, body, arg };
  return mph_prompt_handler(mpe_effect_kind(hdef->effect), (void*)hdef, local, &mpe_handle_start, &env);
}


/*-----------------------------------------------------------------
  Resume
-----------------------------------------------------------------*/

static void* mpe_resume_internal(bool final, mpe_resume_t* resume, void* local, void* arg) {
  mpe_assert(resume->rkind >= MPE_RESUMPTION_SCOPED_ONCE);
  // and resume
  if (resume->rkind == MPE_RESUMPTION_SCOPED_ONCE) {
    mph_resume_t* mpr = resume->mp.resume;
    return mph_resume(mpr, local, arg);
  }
  else if (resume->rkind == MPE_RESUMPTION_ONCE) {
    mph_resume_t* mpr = resume->mp.resume;
    mpe_assert_internal(final);
    //mpe_trace_message("free resume: %p\n", resume);
    mpe_free(resume);
    return mph_resume(mpr, local, arg);
  }
  else {
    mph_resume_t* mpr = resume->mp.resume;
    if (final) {
      //mpe_trace_message("free resume: %p\n", resume);
      mpe_free(resume);
    }
    else {
      mph_resume_dup(mpr); 
    }
    return mph_resume(mpr, local, arg);
  }
}


// Last use of a resumption
void* mpe_resume_final(mpe_resume_t* resume, void* local, void* arg) {
  return mpe_resume_internal(true, resume, local, arg);
}

// Regular resume
void* mpe_resume(mpe_resume_t* resume, void* local, void* arg) {
  return mpe_resume_internal(false, resume, local, arg);
}

// Last resume in tail-position
void* mpe_resume_tail(mpe_resume_t* resume, void* local, void* arg) {  
  if (mpe_likely(resume->rkind == MPE_RESUMPTION_INPLACE)) {
    *resume->mp.plocal = local;
    return arg;
  }
  // and tail resume
  if (resume->rkind == MPE_RESUMPTION_SCOPED_ONCE) {
    mph_resume_t* mpr = resume->mp.resume;
    return mph_resume_tail(mpr, local, arg);
  }
  else {
    mph_resume_t* mpr = resume->mp.resume;    
    mpe_free(resume); // always assume final?
    return mph_resume_tail(mpr, local, arg);
  }
}


// Resume to unwind (e.g. run destructors and finally clauses)
static void mpe_resume_unwind(mpe_resume_t* resume) {
  mpe_assert(resume->rkind >= MPE_RESUMPTION_SCOPED_ONCE);
  // and resume
  if (resume->rkind == MPE_RESUMPTION_SCOPED_ONCE) {
    mph_resume_t* mpr = resume->mp.resume;
    return mph_resume_unwind(mpr);
  }
  else {
    mph_resume_t* mpr = resume->mp.resume;
    //mpe_trace_message("free resume: %p\n", resume);
    mpe_free(resume);
    return mph_resume_unwind(mpr);
  }  
}


// Release without resuming 
void mpe_resume_release(mpe_resume_t* resume) {
  if (resume == NULL) return; // in case someone tries to release a NULL (OP_NEVER or OP_ABORT) resumption
  if (resume->rkind == MPE_RESUMPTION_ONCE) {
    mpe_resume_unwind(resume);    
  }
  else {
    mpe_assert_internal(resume->rkind == MPE_RESUMPTION_MULTI);
    mph_resume_t* mpr = resume->mp.resume;
    mpe_free(resume);
    mph_resume_drop(mpr); // will unwind if needed
  }
}



