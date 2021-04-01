#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include "mprompt.h"
#include "internal/util.h"

#ifdef __cplusplus
#define MP_HAS_TRY  (1)
#include <exception>
#else
#define MP_HAS_TRY  (0)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define mph_unlikely(x)          __builtin_expect((x),0)
#define mph_likely(x)            __builtin_expect((x),1)
#else
#define mph_unlikely(x)          (x)
#define mph_likely(x)            (x)
#endif

//---------------------------------------------------------------------------
// Builtin handler kinds
//---------------------------------------------------------------------------

mph_kind_t MPH_FINALLY = "mph_finally";
mph_kind_t MPH_UNDER   = "mph_under";
mph_kind_t MPH_MASK    = "mph_mask";


//---------------------------------------------------------------------------
// Internal handlers
//---------------------------------------------------------------------------

// Internal base handler
typedef struct mph_handler_s {
  struct mph_handler_s*  parent;
  mph_kind_t             kind;
  void*                  hdata;
} mph_handler_t;


// Prompt handler
typedef struct mph_handler_prompt_s {
  mph_handler_t handler;
  mp_prompt_t*  prompt;
} mph_handler_prompt_t;


// An under handler (used for tail-resumptive optimization)
typedef struct mph_handler_under_s {
  mph_handler_t  handler;
  mph_kind_t     under;     // ignore handlers until the innermost `under` kind
} mph_handler_under_t;


// A mask handler
typedef struct mph_handler_mask_s {
  mph_handler_t  handler;
  mph_kind_t     mask;    
  size_t         from;
} mph_handler_mask_t;



//---------------------------------------------------------------------------
// Shadow stack
//---------------------------------------------------------------------------

// Top of the handlers in the current execution stack
mp_decl_thread mph_handler_t* _mph_top;

mph_handler_t* mph_top(void) {
  return _mph_top;  
}

mph_handler_t* mph_parent(mph_handler_t* handler) {
  return (mph_unlikely(handler == NULL) ? mph_top() : handler->parent);
}

mph_kind_t mph_kind(mph_handler_t* handler) {
  return handler->kind;
}
void* mph_data(mph_handler_t* handler) {
  return handler->hdata;
}



//---------------------------------------------------------------------------
// Find innermost handler
//---------------------------------------------------------------------------

// Find the innermost handler
mph_handler_t* mph_find(mph_kind_t kind) {
  mph_handler_t* h = _mph_top;
  size_t mask_level = 0;
  while (mph_likely(h != NULL)) {
    mph_kind_t hkind = h->kind;
    if (mph_likely(kind == hkind)) {
      if (mph_likely(mask_level <= 0)) {
        return h;
      }
      else {
        mask_level--;
      }
    }
    else if (mph_unlikely(hkind == MPH_UNDER)) {
      // skip to the matching handler of this `under` frame
      mph_kind_t ukind = ((mph_handler_under_t*)h)->under;
      do {
        h = h->parent;
      } while (h!= NULL && h->kind != ukind);
      if (h == NULL) break;
    }
    else if (mph_unlikely(hkind == MPH_MASK)) {
      // increase masking level 
      mph_handler_mask_t* m = ((mph_handler_mask_t*)h);
      if (m->mask == kind && m->from <= mask_level) {
        mask_level++;
      }
    }
    h = h->parent;
  }
  return NULL;
}



//---------------------------------------------------------------------------
// Linear handlers without a prompt
//---------------------------------------------------------------------------


// use as: `{mpe_with_handler(f){ <body> }}`
#if MP_HAS_TRY
#define mph_with_handler(f)   mph_raii_with_handler_t _with_handler(f); 
// This class ensures a handler-stack will be properly unwound even when exceptions are raised.
class mph_raii_with_handler_t {
private:
  mph_handler_t* f;
public:
  mph_raii_with_handler_t(mph_handler_t* f) {
    this->f = f;
    f->parent = _mph_top;
    _mph_top = f;
  }
  ~mph_raii_with_handler_t() {
    mp_assert_internal(_mph_top == f);
    _mph_top = f->parent;
  }
};
#else
// C version
#define mph_with_handler(f) \
  for( bool _once = ((f)->parent = _mph_top, _mph_top = (f), true); \
       _once; \
       _once = (_mph_top = (f)->parent, false) ) 
#endif

void* mph_linear(mph_kind_t kind, void* hdata, mph_start_fun_t* fun, void* arg) {
  mph_handler_t h;
  h.kind = kind;
  h.hdata = hdata;
  void* result = NULL;
  {mph_with_handler(&h){
    result = fun(h.hdata,arg);
  }}
  return result;
}



//---------------------------------------------------------------------------
// Abort
//---------------------------------------------------------------------------

static void* mph_abort_fun(mp_resume_t* r, void* arg) {
  mp_resume_drop(r);
  return arg;
}

// Yield to a prompt without unwinding
static void* mph_abort_to(mph_handler_prompt_t* h, void* arg) {
  return mp_yield(h->prompt, &mph_abort_fun, arg);
}



//---------------------------------------------------------------------------
// Unwind
//---------------------------------------------------------------------------

#if MP_HAS_TRY
class mph_unwind_exception : public std::exception {
public:
  mph_handler_prompt_t* target;
  mph_unwind_fun_t* fun;
  void* arg;
  mph_unwind_exception(mph_handler_prompt_t* h, mph_unwind_fun_t* fun, void* arg) : target(h), fun(fun), arg(arg) {  }
  mph_unwind_exception(const mph_unwind_exception& e) : target(e.target), fun(e.fun), arg(e.arg) {
    fprintf(stderr, "copy exception\n");
  }
  mph_unwind_exception& operator=(const mph_unwind_exception& e) {
    target = e.target; fun = e.fun; arg = e.arg;
    return *this;
  }

  virtual const char* what() const throw() {
    return "libmpeff: unwinding the stack; do not catch this exception!";
  }
};

static void mph_unwind_to(mph_handler_prompt_t* target, mph_unwind_fun_t* fun, void* arg) {
  throw mph_unwind_exception(target, fun, arg);
}
#else
static void mph_unwind_to(mph_handler_prompt_t* target, mph_unwind_fun_t* fun, void* arg) {
  // TODO: walk the handlers and invoke finally frames
  // invoke the unwind function under an "under" frame
  // and finally yield up to abort
  mph_abort_to(target, arg);
}
#endif



//---------------------------------------------------------------------------
// Full prompt handler: can be yielded to (or unwound to)
//---------------------------------------------------------------------------


// Pass arguments down in a closure environment
struct mph_start_env {
  mph_kind_t       kind;
  size_t           hdata_size;
  mph_start_fun_t* body;
  void*            arg;
};

// Start a handler
static void* mph_start(mp_prompt_t* prompt, void* earg) {
  // init
  struct mph_start_env* env = (struct mph_start_env*)earg;  
  void* hdata = alloca(env->hdata_size);
  mph_handler_prompt_t h;
  h.prompt = prompt;  
#if MP_HAS_TRY
  try {  // catch unwind exceptions
#endif
    return mph_linear(env->kind, hdata, env->body, env->arg);    
#if MP_HAS_TRY
  }     // handle unwind exceptions
  catch (const mph_unwind_exception& e) {
    if (e.target != &h) {
      throw;  // rethrow 
    }
    return e.fun(hdata, e.arg);  // execute the unwind function here while hdata is valid 
  }
#endif  
}


void* mph_prompt(mph_kind_t kind, size_t hdata_size, mph_start_fun_t* fun, void* arg) {
  struct mph_start_env env = { kind, hdata_size, fun, arg };
  return mp_prompt(&mph_start, &env);
}


//---------------------------------------------------------------------------
// Yield
//---------------------------------------------------------------------------

typedef struct mph_yield_env_s {
  void*            hdata;
  mph_yield_fun_t* fun;
  void*            arg;
} mph_yield_env_t;

static void* mph_yield_fun(mp_resume_t* r, void* envarg) {
  mph_yield_env_t* env = (mph_yield_env_t*)envarg;
  return (env->fun)((mph_resume_t*)r, env->hdata, env->arg);
}

// Yield to a prompt without unwinding
static void* mph_yield_to(mph_handler_prompt_t* h, mph_yield_fun_t fun, void* arg) {
  mph_yield_env_t env = { &h->handler.hdata, fun, arg };
  return mp_yield(h->prompt, &mph_yield_fun, &env);
}


//---------------------------------------------------------------------------
// Multi-shot Yield
//---------------------------------------------------------------------------
/*
typedef struct mph_myield_env_s {
  mph_handler_t* handler;
  mph_yield_fun_t* fun;
  void* arg;
} mph_myield_env_t;

static void* mph_myield_fun(mp_mresume_t* r, void* envarg) {
  mph_myield_env_t* env = (mph_myield_env_t*)envarg;
  return (env->fun)(r, env->handler, env->arg);
}

// Yield to a prompt without unwinding
static void* mph_myield_to(mph_handler_prompt_t* h, mph_yield_fun_t fun, void* arg) {
  mph_myield_env_t env = { &h->handler.handler, fun, arg };
  return mp_myield(h->prompt, &mph_myield_fun, &env);
}
*/



//---------------------------------------------------------------------------
// Under
//---------------------------------------------------------------------------

void* mph_under(mph_kind_t under, void* (*fun)(void*), void* arg) {
  mph_handler_under_t h;
  h.under = under;
  h.handler.kind = MPH_UNDER;
  h.handler.hdata = NULL;
  void* result = NULL;
  {mph_with_handler(&h.handler){
    result = fun(arg);
  }}
  return result;
}


//---------------------------------------------------------------------------
// Mask
//---------------------------------------------------------------------------

void* mph_mask(mph_kind_t mask, size_t from, void* (*fun)(void*), void* arg) {
  mph_handler_mask_t h;
  h.mask = mask;
  h.from = from;
  h.handler.kind = MPH_UNDER;
  h.handler.hdata = NULL;
  void* result = NULL;
  {mph_with_handler(&h.handler) {
    result = fun(arg);
  }}
  return result;
}

