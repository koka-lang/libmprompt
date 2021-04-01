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

#include <mprompt.h>
#include "mphandler.h"


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

// Frame types
typedef enum mpe_frame_type_e {
  MPE_FRAME_HANDLER,
  MPE_FRAME_UNDER,
  MPE_FRAME_FINALLY,
  MPE_FRAME_END
} mpe_frame_type_t;


// A general frame
typedef struct mpe_frame_s {
  mpe_effect_t        effect;     // every frame has an effect (to speed up tests)
  struct mpe_frame_s* parent;
  mpe_frame_type_t    ftype;
} mpe_frame_t;


// A handler frame
typedef struct mpe_frame_handle_s {
  mpe_frame_t             frame;
  mp_prompt_t*            prompt;
  const mpe_handlerdef_t* hdef;
  void*                   local;
  mpe_frame_t*            resume_top;
} mpe_frame_handle_t;


// An under frame (used for tail-resumptive optimization)
typedef struct mpe_frame_under_s {
  mpe_frame_t   frame;    
  mpe_effect_t  under;    // ignore frames until the innermost `under` effect
} mpe_frame_under_t;

// For search efficiency, non-handler frames are identified by a unique effect tag
MPE_DEFINE_EFFECT0(mpe_under)


// Resumption kinds: used to avoid allocation etc.
typedef enum mpe_resumption_kind_e {
  MPE_RESUMPTION_NEVER,
  MPE_RESUMPTION_INPLACE,
  MPE_RESUMPTION_SCOPED_ONCE,
  MPE_RESUMPTION_ONCE,
  MPE_RESUMPTION_MULTI
} mpe_resumption_kind_t;


// A resumption
struct mpe_resume_s {
  mpe_resumption_kind_t kind;       // todo: encode kind in lower bits so we can avoid allocating resumes?
  union {
    void**        plocal;           // kind == MPE_RESUMPTION_INPLACE
    mp_resume_t*  resume;           // kind == MPE_RESUMPTION_SCOPED_ONCE || MPE_RESUMPTION_ONCE || MPE_RESUME_MULTI
  } mp;
};


/*-----------------------------------------------------------------
  Handler shadow stack
-----------------------------------------------------------------*/

// Top of the frames in the current execution stack
mpe_decl_thread mpe_frame_t* mpe_frame_top;


// use as: `{mpe_with_frame(f){ <body> }}`
#if MPE_HAS_TRY
#define mpe_with_frame(f)   mpe_raii_with_frame_t _with_frame(f); 
// This class ensures a handler-stack will be properly unwound even when exceptions are raised.
class mpe_raii_with_frame_t {
private:
  mpe_frame_t* f;
public:
  mpe_raii_with_frame_t(mpe_frame_t* f) {
    this->f = f;
    f->parent = mpe_frame_top;
    mpe_frame_top = f;
  }
  ~mpe_raii_with_frame_t() {
    mpe_assert_internal(mpe_frame_top == f);
    mpe_frame_top = f->parent;
  }
};
#else
// C version
#define mpe_with_frame(f) \
  for( bool _once = ((f)->parent = mpe_frame_top, mpe_frame_top = (f), true); \
       _once; \
       _once = (mpe_frame_top = (f)->parent, false) ) 
#endif


#if MPE_HAS_TRY
// in some cases (like MPE_OP_NORESUME) we need to unwind to the effect handler operation
// while calling destructors. We do this using a special unwind exception.
class mpe_unwind_exception : public std::exception {
public:
  mpe_frame_handle_t* target;
  const mpe_operation_t* op;
  void* arg;
  mpe_unwind_exception(mpe_frame_handle_t* h, const mpe_operation_t* op, void* arg) : target(h), op(op), arg(arg) {  }
  mpe_unwind_exception(const mpe_unwind_exception& e) : target(e.target), op(e.op), arg(e.arg) {  
    fprintf(stderr, "copy exception\n");
  }
  mpe_unwind_exception& operator=(const mpe_unwind_exception& e) { 
    target = e.target; op = e.op; arg = e.arg; 
    return *this; 
  }

  virtual const char* what() const throw() {
    return "libmpeff: unwinding the stack; do not catch this exception!";
  }
};

static void mpe_unwind_to(mpe_frame_handle_t* target, const mpe_operation_t* op, void* arg) {
  //fprintf(stderr, "throw unwind..\n");
  mp_throw mpe_unwind_exception(target, op, arg);
}
#else
static void* mpe_perform_yield_to_abort(mpe_frame_handle_t* h, const mpe_operation_t* op, void* arg);
static void mpe_unwind_to(mpe_frame_handle_t* target, const mpe_operation_t* op, void* arg) {
  // TODO: walk the handlers and invoke finally frames
  mpe_perform_yield_to_abort(target, op, arg);
}
#endif

MPE_DEFINE_EFFECT1(mpe_unwind, mpe_unwind);

static void* mpe_handle_op_unwind(mpe_resume_t* r, void* local, void* arg) {
  (void)(r); (void)(local);
  mpe_assert_internal(r == NULL);
  return arg;
}

static mpe_operation_t mpe_op_unwind = {
  MPE_OP_ABORT,
  MPE_OPTAG(mpe_unwind,mpe_unwind),
  &mpe_handle_op_unwind
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




/*-----------------------------------------------------------------
  Perform
-----------------------------------------------------------------*/

typedef struct mpe_perform_env_s {
  mpe_opfun_t* opfun;
  void* local;
  void* oparg;
} mpe_perform_env_t;

typedef struct mpe_resume_env_s {
  void* local;
  void* result;
  bool  unwind;
} mpe_resume_env_t;


// Regular once resumption
static void* mpe_perform_op_clause(mp_resume_t* mpr, void* earg) {
  mpe_perform_env_t* env = (mpe_perform_env_t*)earg;
  mpe_resume_t* resume = mpe_malloc_tp(mpe_resume_t);
  resume->kind = MPE_RESUMPTION_ONCE;
  resume->mp.resume = mpr;
  return (env->opfun)(resume, env->local, env->oparg);
}

static void* mpe_perform_yield_to(mpe_frame_handle_t* h, const mpe_operation_t* op, void* arg) {
  mpe_frame_t* resume_top = mpe_frame_top; // save current top
  mpe_frame_top = h->frame.parent;           // and unlink handlers
  mpe_perform_env_t penv = { op->opfun, h->local, arg };
  // yield up
  mpe_resume_env_t* renv = (mpe_resume_env_t*)mp_yield(h->prompt, &mpe_perform_op_clause, &penv);
  // resumed!
  h->local = renv->local;           // set new state
  h->frame.parent = mpe_frame_top;  // relink handlers
  mpe_frame_top = resume_top;
  if (renv->unwind) {
    mpe_unwind_to(h, &mpe_op_unwind, renv->result);
  }
  return renv->result;
}

// Multi-shot resumption
static void* mpe_perform_op_clause_multi(mp_resume_t* mpr, void* earg) {
  mpe_perform_env_t* env = (mpe_perform_env_t*)earg;
  mpe_resume_t* resume = mpe_malloc_tp(mpe_resume_t);
  //mpe_trace_message("alloc resume: %p\n", resume);
  resume->kind = MPE_RESUMPTION_MULTI;
  resume->mp.resume = mpr;
  return (env->opfun)(resume, env->local, env->oparg);  
}

static void* mpe_perform_yield_to_multi(mpe_frame_handle_t* h, const mpe_operation_t* op, void* arg) {
  mpe_frame_t* resume_top = mpe_frame_top; // save current top
  mpe_frame_top = h->frame.parent;           // and unlink handlers
  mpe_perform_env_t penv = { op->opfun, h->local, arg };
  // yield up
  mpe_resume_env_t* renv = (mpe_resume_env_t*)mp_myield(h->prompt, &mpe_perform_op_clause_multi, &penv);
  // resumed!
  h->local = renv->local;         // set new state
  h->frame.parent = mpe_frame_top;  // relink handlers
  mpe_frame_top = resume_top;
  if (renv->unwind) {
    mpe_unwind_to(h, &mpe_op_unwind, renv->result);
  }
  return renv->result;
}

// Scoped-once resumption
static void* mpe_perform_op_clause_scoped_once(mp_resume_t* mpr, void* earg) {
  mpe_perform_env_t* env = (mpe_perform_env_t*)earg;
  mpe_resume_t resume;
  resume.kind = MPE_RESUMPTION_SCOPED_ONCE;
  resume.mp.resume = mpr;
  return (env->opfun)(&resume, env->local, env->oparg);
}

static void* mpe_perform_yield_to_scoped_once(mpe_frame_handle_t* h, const mpe_operation_t* op, void* arg) {
  mpe_frame_t* resume_top = mpe_frame_top; // save current top
  mpe_frame_top = h->frame.parent;           // and unlink handlers
  mpe_perform_env_t penv = { op->opfun, h->local, arg };
  // yield up
  mpe_resume_env_t* renv = (mpe_resume_env_t*)mp_yield(h->prompt, &mpe_perform_op_clause_scoped_once, &penv);
  // resumed!
  h->local = renv->local;         // set new state
  h->frame.parent = mpe_frame_top;  // relink handlers
  mpe_frame_top = resume_top;
  if (renv->unwind) {
    mpe_unwind_to(h, &mpe_op_unwind, renv->result);
  }
  return renv->result;
}


// Never resumption
static void* mpe_perform_op_clause_abort(mp_resume_t* mpr, void* earg) {
  mpe_perform_env_t env = *((mpe_perform_env_t*)earg);  // copy out all args before dropping the prompt
  mp_resume_drop(mpr);
  return (env.opfun)(NULL, env.local, env.oparg);
}

static void* mpe_perform_yield_to_abort(mpe_frame_handle_t* h, const mpe_operation_t* op, void* arg) {
  mpe_perform_env_t env = { op->opfun, h->local, arg };
  return mp_yield(h->prompt, &mpe_perform_op_clause_abort, &env);
}


// ------------------------------------------------------------------------------
// Perform
// ------------------------------------------------------------------------------

static void* mpe_perform_at(mpe_frame_handle_t* h, const mpe_operation_t* op, void* arg) {
  mpe_opkind_t opkind = op->opkind;
  mpe_assert_internal(opkind == op->opkind);
  if (mpe_likely(opkind == MPE_OP_TAIL_NOOP)) {
    // tail resumptive, calls no operations, execute in place
    mpe_resume_t resume = { MPE_RESUMPTION_INPLACE, { &h->local } };
    return (op->opfun)(&resume, h->local, arg);
  }
  else if (mpe_likely(opkind == MPE_OP_TAIL)) {
    // tail resumptive; execute in place under an "under" frame
    mpe_frame_under_t f;
    f.frame.ftype = MPE_FRAME_UNDER;
    f.frame.effect = MPE_EFFECT(mpe_under);
    f.under = h->frame.effect;
    void* result = NULL;
    {mpe_with_frame(&f.frame) {
      mpe_resume_t resume = { MPE_RESUMPTION_INPLACE, { &h->local } };
      result = (op->opfun)(&resume, h->local, arg);
    }}
    return result;
  }
  else if (opkind == MPE_OP_SCOPED_ONCE) {
    return mpe_perform_yield_to_scoped_once(h, op, arg);
  }
  else if (opkind == MPE_OP_ONCE) {
    return mpe_perform_yield_to(h, op, arg);
  }
  else if (opkind == MPE_OP_NEVER) {
    mpe_unwind_to(h, op, arg);
    return NULL; // never reached
  }
  else if (opkind == MPE_OP_ABORT) {
    return mpe_perform_yield_to_abort(h, op, arg);
  }
  else {
    return mpe_perform_yield_to_multi(h, op, arg);    
  }
}



static mpe_decl_noinline void* mpe_unhandled_operation(mpe_optag_t optag) {
  fprintf(stderr, "unhandled operation: %s\n", mpe_optag_name(optag));
  return NULL;
}


// Perform finds the innermost handler and performs the operation
void* mpe_perform(mpe_optag_t optag, void* arg) {
  mpe_frame_t* f = mpe_frame_top;
  mpe_effect_t opeff = optag->effect;
  while (mpe_likely(f != NULL)) {
    mpe_effect_t eff = f->effect;
    if (mpe_likely(eff == opeff)) {
      // found the effect
      mpe_frame_handle_t* h = (mpe_frame_handle_t*)f;
      const mpe_operation_t* op = &h->hdef->operations[optag->opidx];
      return mpe_perform_at(h, op, arg);      
    }
    else if (mpe_unlikely(eff == MPE_EFFECT(mpe_under))) {
      // skip to the matching handler of this `under` frame
      mpe_effect_t ueff = ((mpe_frame_under_t*)f)->under;
      do {
        f = f->parent;
      } while (f != NULL && f->effect != ueff);
      if (f == NULL) break;
    }
    f = f->parent;
  }
  return mpe_unhandled_operation(optag);
}



/*-----------------------------------------------------------------
  Handle
-----------------------------------------------------------------*/

// Pass arguments down in a closure environment
struct mpe_handle_start_env {
  const mpe_handlerdef_t* hdef;
  void* local;
  mpe_actionfun_t* body;
  void* arg;
};

// Start a handler
static mpe_decl_noinline void* mpe_handle_start(mp_prompt_t* prompt, void* earg) {
  // init
  struct mpe_handle_start_env* env = (struct mpe_handle_start_env*)earg;
  mpe_frame_handle_t h;
  h.prompt = prompt;
  h.hdef = env->hdef;
  h.local = env->local;
  h.frame.ftype = MPE_FRAME_HANDLER;
  h.frame.effect = env->hdef->effect;
  void* result = NULL;
  #if MPE_HAS_TRY
  try {  // catch unwind exceptions
  #endif
    // push frame on top
    {mpe_with_frame(&h.frame) {
      // and call the action
      result = (env->body)(env->arg);
    }}
  #if MPE_HAS_TRY
  }   // handle unwind exceptions
  catch (const mpe_unwind_exception& e) {
    if (e.target != &h) {
      //fprintf(stderr, "rethrow unwind\n");
      mp_throw;  // rethrow 
    }
    //fprintf(stderr, "catch unwind\n");
    return mpe_perform_yield_to_abort(e.target, e.op, e.arg);  // yield to ourselves (exiting this prompt)
  }
  #endif
  // potentially run return function
  if (h.hdef->resultfun != NULL) {
    result = h.hdef->resultfun(h.local, result);
  }
  return result;
}

/// Handle a particular effect.
/// Handles operations yielded in `body(arg)` with the given handler definition `def`.
void* mpe_handle(const mpe_handlerdef_t* hdef, void* local, mpe_actionfun_t* body, void* arg) {
  struct mpe_handle_start_env env = { hdef, local, body, arg };
  return mp_prompt(&mpe_handle_start, &env);
}


/*-----------------------------------------------------------------
  Resume
-----------------------------------------------------------------*/

static void* mpe_resume_internal(bool final, mpe_resume_t* resume, void* local, void* arg, bool unwind) {
  mpe_assert(resume->kind >= MPE_RESUMPTION_SCOPED_ONCE);
  mpe_resume_env_t renv = { local, arg, unwind };
  // and resume
  if (resume->kind == MPE_RESUMPTION_SCOPED_ONCE) {
    mp_resume_t* mpr = resume->mp.resume;
    return mp_resume(mpr, &renv);
  }
  else if (resume->kind == MPE_RESUMPTION_ONCE) {
    mp_resume_t* mpr = resume->mp.resume;
    mpe_assert_internal(final);
    //mpe_trace_message("free resume: %p\n", resume);
    mpe_free(resume);
    return mp_resume(mpr, &renv);
  }
  else {
    mp_resume_t* mpr = resume->mp.resume;
    if (final) {
      //mpe_trace_message("free resume: %p\n", resume);
      mpe_free(resume);
    }
    else {
      mp_resume_dup(mpr); 
    }
    return mp_resume(mpr, &renv);
  }
}

// Resume to unwind (e.g. run destructors and finally clauses)
static void mpe_resume_unwind(mpe_resume_t* r) {
  mpe_resume_internal(true, r, NULL, NULL, true);
}

// Last use of a resumption
void* mpe_resume_final(mpe_resume_t* resume, void* local, void* arg) {
  return mpe_resume_internal(true, resume, local, arg, false);
}

// Regular resume
void* mpe_resume(mpe_resume_t* resume, void* local, void* arg) {
  return mpe_resume_internal(false, resume, local, arg, false);
}

// Last resume in tail-position
void* mpe_resume_tail(mpe_resume_t* resume, void* local, void* arg) {  
  if (mpe_likely(resume->kind == MPE_RESUMPTION_INPLACE)) {
    *resume->mp.plocal = local;
    return arg;
  }
  mpe_resume_env_t renv = { local, arg, false };
  // and tail resume
  if (resume->kind == MPE_RESUMPTION_SCOPED_ONCE) {
    mp_resume_t* mpr = resume->mp.resume;
    return mp_resume_tail(mpr, &renv);
  }
  else {
    mp_resume_t* mpr = resume->mp.resume;    
    mpe_free(resume); // always assume final?
    return mp_resume_tail(mpr, &renv);
  }
}


// Release without resuming 
void mpe_resume_release(mpe_resume_t* resume) {
  if (resume == NULL) return; // in case someone tries to release a NULL (OP_NEVER or OP_ABORT) resumption
  if (resume->kind == MPE_RESUMPTION_ONCE) {
    mpe_resume_unwind(resume);    
  }
  else {
    mpe_assert_internal(resume->kind == MPE_RESUMPTION_MULTI);
    mp_resume_t* mpr = resume->mp.resume;
    if (mp_resume_should_unwind(mpr)) {
      mpe_resume_unwind(resume);
    }
    else {
      mpe_free(resume);
      mp_resume_drop(mpr);
    }
  }
}
