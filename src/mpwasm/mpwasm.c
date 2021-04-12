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
#include "mpwasm.h"


/*-----------------------------------------------------------------
  Defines
-----------------------------------------------------------------*/

#ifdef __cplusplus
#define MPW_HAS_TRY  (1)
#include <exception>
#else
#define MPW_HAS_TRY  (0)
#endif

#if defined(_MSC_VER)
#define mpw_decl_noinline        __declspec(noinline)
#define mpw_decl_thread          __declspec(thread)
#elif (defined(__GNUC__) && (__GNUC__>=3))  // includes clang and icc
#define mpw_decl_noinline        __attribute__((noinline))
#define mpw_decl_thread          __thread
#else
#define mpw_decl_noinline
#define mpw_decl_thread          __thread  
#endif

#if defined(__GNUC__) || defined(__clang__)
#define mpw_unlikely(x)          __builtin_expect((x),0)
#define mpw_likely(x)            __builtin_expect((x),1)
#else
#define mpw_unlikely(x)          (x)
#define mpw_likely(x)            (x)
#endif

#define mpw_assert(x)            assert(x)
#define mpw_assert_internal(x)   mpw_assert(x)
#define mpw_malloc_tp(tp)        (tp*)mpw_malloc_safe(sizeof(tp))


static inline void* mpw_malloc_safe(size_t size) {
  void* p = malloc(size);
  if (p != NULL) return p;
  fprintf(stderr,"out of memory\n");
  abort();
}

static inline void mpw_free(void* p) {
  free(p);
}


/*-----------------------------------------------------------------
  Types
-----------------------------------------------------------------*/


// A general frame
typedef struct mpw_frame_s {
  mpw_effect_t        effect;     // every frame has an effect (to speed up tests)
  struct mpw_frame_s* parent;
} mpw_frame_t;


// A handler frame
typedef struct mpw_frame_handle_s {
  mpw_frame_t             frame;
  mp_prompt_t*            prompt;
} mpw_frame_handle_t;


// An under frame (used for tail-resumptive optimization)
typedef struct mpw_frame_under_s {
  mpw_frame_t   frame;    
  mpw_effect_t  under;    // ignore frames until the innermost `under` effect
} mpw_frame_under_t;


// An mask frame 
typedef struct mpw_frame_mask_s {
  mpw_frame_t   frame;
  mpw_effect_t  mask;    
  size_t        from;
} mpw_frame_mask_t;


// Finally frame
typedef struct mpw_frame_finally_s {
  mpw_frame_t        frame;
  mpw_release_fun_t* fun;
  void*              local;
} mpw_frame_finally_t;


// For search efficiency, non-handler frames are identified by a unique effect tag
MPW_DEFINE_EFFECT0(mpw_frame_under)
MPW_DEFINE_EFFECT0(mpw_frame_mask)
MPW_DEFINE_EFFECT0(mpw_frame_finally)


// A resumption
typedef mp_resume_t mpw_cont_t;


/*-----------------------------------------------------------------
  Handler shadow stack
-----------------------------------------------------------------*/

// Top of the frames in the current execution stack
mpw_decl_thread mpw_frame_t* mpw_frame_top;


// use as: `{mpw_with_frame(f){ <body> }}`
#if MPW_HAS_TRY
#define mpw_with_frame(f)   mpw_raii_with_frame_t _with_frame(f); 
// This class ensures a handler-stack will be properly unwound even when exceptions are raised.
class mpw_raii_with_frame_t {
private:
  mpw_frame_t* f;
public:
  mpw_raii_with_frame_t(mpw_frame_t* f) {
    this->f = f;
    f->parent = mpw_frame_top;
    mpw_frame_top = f;
  }
  ~mpw_raii_with_frame_t() {
    mpw_assert_internal(mpw_frame_top == f);
    mpw_frame_top = f->parent;
  }
};
#else
// C version
#define mpw_with_frame(f) \
  for( bool _once = ((f)->parent = mpw_frame_top, mpw_frame_top = (f), true); \
       _once; \
       _once = (mpw_frame_top = (f)->parent, false) ) 
#endif


/*-----------------------------------------------------------------
  Unwind and abort
-----------------------------------------------------------------*/


static void* mpw_abort_clause(mp_resume_t* r, void* arg) {
  mp_resume_drop(r);
  return arg;
}

static void* mpw_abort_to(mpw_frame_handle_t* h, void* arg) {
  mp_yield(h->prompt, &mpw_abort_clause, arg);
  return NULL;
}

#if MPW_HAS_TRY
// in some cases (like MPW_OP_NORESUME) we need to unwind to the effect handler operation
// while calling destructors. We do this using a special unwind exception.
class mpw_unwind_exception : public std::exception {
public:
  mpw_frame_handle_t* target;
  void* arg;
  mpw_unwind_exception(mpw_frame_handle_t* h, void* arg) : target(h), arg(arg) {  }
  mpw_unwind_exception(const mpw_unwind_exception& e) : target(e.target), arg(e.arg) {  
    fprintf(stderr, "copy exception\n");
  }
  mpw_unwind_exception& operator=(const mpw_unwind_exception& e) { 
    target = e.target; arg = e.arg; 
    return *this; 
  }

  virtual const char* what() const throw() {
    return "libmpeff: unwinding the stack -- do not catch this exception!";
  }
};

static void mpw_unwind_to(mpw_frame_handle_t* target, void* arg) {
  //fprintf(stderr, "throw unwind..\n");
  throw mpw_unwind_exception(target, arg);
}
#else
static void* mpw_perform_yield_to_abort(mpw_frame_handle_t* h, void* arg);
static void mpw_unwind_to(mpw_frame_handle_t* target, void* arg) {
  // TODO: walk the handlers and invoke finally frames
  mpw_abort_to(target, arg);
}
#endif



// Simulate operation definition for resume_unwind
MPW_DEFINE_EFFECT1(mpw_unwind, mpw_unwind);



/*-----------------------------------------------------------------
  Effect and optag names
-----------------------------------------------------------------*/

const char* mpw_effect_name(mpw_effect_t effect) {
  return (effect == NULL ? "<null>" : effect[0]);
}

const char* mpw_optag_name(mpw_optag_t optag) {
  return (optag == NULL ? "<null>" : optag->effect[optag->opidx + 1]);
}


/*-----------------------------------------------------------------
  Search for an innermost handler
-----------------------------------------------------------------*/


static mpw_decl_noinline void* mpw_unhandled_operation(mpw_optag_t optag) {
  fprintf(stderr, "unhandled operation: %s\n", mpw_optag_name(optag));
  return NULL;
}


// Perform finds the innermost handler and performs the operation
// note: this is performance sensitive code
static mpw_frame_handle_t* mpw_find(mpw_optag_t optag) {
  mpw_frame_t* f = mpw_frame_top;
  mpw_effect_t opeff = optag->effect;
  size_t mask_level = 0;
  while (mpw_likely(f != NULL)) {
    mpw_effect_t eff = f->effect;
    // handle
    if (mpw_likely(eff == opeff)) {
      if (mpw_likely(mask_level == 0)) {
        return (mpw_frame_handle_t*)f;    // found our handler
      }
      else {
        mask_level--;
      }
    }
    // under
    else if (mpw_unlikely(eff == MPW_EFFECT(mpw_frame_under))) {
      mpw_effect_t ueff = ((mpw_frame_under_t*)f)->under;
      do {
        f = f->parent;
      } while (f != NULL && f->effect != ueff);
      if (f == NULL) break;
    }
    // mask
    else if (mpw_unlikely(eff == MPW_EFFECT(mpw_frame_mask))) {
      mpw_frame_mask_t* mf = (mpw_frame_mask_t*)f;
      if (mpw_unlikely(mf->mask == opeff && mf->from <= mask_level)) {
        mask_level++;
      }
    }
    f = f->parent;
  }
  return NULL;
}


/*-----------------------------------------------------------------
  Resume
-----------------------------------------------------------------*/

/*-----------------------------------------------------------------
  Perform
-----------------------------------------------------------------*/

typedef struct mpw_perform_env_s {
  mpw_opidx_t idx;
  void* arg;
  mp_resume_t* resume;
} mpw_perform_env_t;

typedef struct mpw_resume_env_s {
  mpw_effect_t eff;
  void* result;
  bool  unwind;
} mpw_resume_env_t;


static void* mpw_perform_op_clause(mp_resume_t* r, void* envarg) {
  mpw_perform_env_t* env = (mpw_perform_env_t*)envarg;
  env->resume = r;
  return env;
}

static void* mpw_perform_at(mpw_frame_handle_t* h, mpw_optag_t op, void* arg) {
  mpw_frame_t* resume_top = mpw_frame_top;   // save current top
  mpw_frame_top = h->frame.parent;           // and unlink handlers
  // yield up
  mpw_perform_env_t env = { op->opidx, arg, NULL };
  mpw_resume_env_t* renv = (mpw_resume_env_t*)mp_yield(h->prompt, &mpw_perform_op_clause, &env);
  // resumed!                     
  h->frame.parent = mpw_frame_top;  // relink handlers
  mpw_frame_top = resume_top;
  h->frame.effect = renv->eff;
  if (renv->unwind) {
    mpw_unwind_to(h, renv->result);
  }
  return renv->result;
}

void* mpw_suspend(mpw_optag_t optag, void* arg) {
  mpw_frame_handle_t* h = mpw_find(optag);
  if (mpw_unlikely(h == NULL)) return mpw_unhandled_operation(optag);
  return mpw_perform_at(h, optag, arg);
}



/*-----------------------------------------------------------------
  Resume
-----------------------------------------------------------------*/

mpw_opidx_t mpw_resume(mpw_effect_t eff, mpw_cont_t** resume, void* arg, void** result) {
  mpw_assert(resume != NULL);
  mpw_resume_env_t renv = { eff, arg, false };
  mpw_perform_env_t* env = (mpw_perform_env_t*)mp_resume(*resume, &renv);
  if (env != NULL) {
    *resume = env->resume; 
    *result = env->arg;
    return env->idx;
  }
  else {
    *resume = NULL;
    *result = renv.result;
    return (-1);
  }
}

/*-----------------------------------------------------------------
   Create
-----------------------------------------------------------------*/

// Pass arguments down in a closure environment
typedef struct mpw_start_env_s {
  mpw_effect_t eff;
  void*        arg;
} mpw_start_env_t;

// Start a handler
static void* mpw_start_fun(mp_prompt_t* prompt, void* efun, void* earg) {
  // init
  mpw_action_fun_t* fun = (mpw_action_fun_t*)efun;
  mpw_resume_env_t* renv = (mpw_resume_env_t*)earg;
  mpw_frame_handle_t h;
  h.prompt = prompt;
  h.frame.effect = renv->eff;
  void* result = NULL;
  #if MPW_HAS_TRY
  try {  // catch unwind exceptions
  #endif
    // push frame on top
    {mpw_with_frame(&h.frame) {
      // and call the action
      if (renv->unwind) {
        mpw_unwind_to(&h, renv->result);
      }
      else {
        result = fun(renv->result);
      }
    }}
  #if MPW_HAS_TRY
  }   // handle unwind exceptions
  catch (const mpw_unwind_exception& e) {
    if (e.target != &h) {
      //fprintf(stderr, "rethrow unwind\n");
      throw;  // rethrow 
    }
    //fprintf(stderr, "catch unwind\n");
    return mpw_abort_to(e.target, e.arg);  // yield to ourselves (exiting this prompt)
  }
  #endif  
  renv->result = result;
  return NULL;
}

mpw_cont_t* mpw_new(mpw_action_fun_t fun) {
  return mp_prompt_create(&mpw_start_fun, (void*)fun);
}
