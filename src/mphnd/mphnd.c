#include <stdint.h>
#include <assert.h>

#include <mprompt.h>
#include "mphnd.h"

#ifdef __cplusplus
#define MP_HAS_TRY  (1)
#include <exception>
#else
#define MP_HAS_TRY  (0)
#endif


#if defined(_MSC_VER)
#define mph_decl_noinline        __declspec(noinline)
#define mph_decl_thread          __declspec(thread)
#elif (defined(__GNUC__) && (__GNUC__>=3))  // includes clang and icc
#define mph_decl_noinline        __attribute__((noinline))
#define mph_decl_thread          __thread
#else
#define mph_decl_noinline
#define mph_decl_thread          __thread  
#endif

#if defined(__GNUC__) || defined(__clang__)
#define mph_unlikely(x)          __builtin_expect((x),0)
#define mph_likely(x)            __builtin_expect((x),1)
#else
#define mph_unlikely(x)          (x)
#define mph_likely(x)            (x)
#endif

#define mph_assert(x)            assert(x)
#define mph_assert_internal(x)   mph_assert(x)


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
  mp_prompt_t*           prompt;   // NULL for (non-yieldable) linear handlers
  mph_kind_t             kind;
  void*                  hdata;
  void*                  local;
} mph_handler_t;


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


/*
static bool mph_is_prompt_handler(mph_handler_t* h) {
  return (h->prompt != NULL);
}
*/

//---------------------------------------------------------------------------
// Shadow stack
//---------------------------------------------------------------------------

// Top of the handlers in the current execution stack
mph_decl_thread mph_handler_t* _mph_top;

mph_handler_t* mph_get_top(void) {
  return _mph_top;  
}

mph_handler_t* mph_get_parent(mph_handler_t* handler) {
  return (mph_unlikely(handler == NULL) ? mph_get_top() : handler->parent);
}

mph_kind_t mph_get_kind(mph_handler_t* handler) {
  return handler->kind;
}
void* mph_get_data(mph_handler_t* handler) {
  return handler->hdata;
}

void* mph_get_local(mph_handler_t* handler) {
  return handler->local;
}

void** mph_get_local_byref(mph_handler_t* handler) {
  return &handler->local;
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
    mph_assert_internal(_mph_top == f);
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

void* mph_linear_handler(mph_kind_t kind, void* hdata, void* local, mph_start_fun_t* fun, void* arg) {
  mph_handler_t h;
  h.kind = kind;
  h.prompt = NULL;
  h.hdata = hdata;
  h.local = local;
  void* result = NULL;
  {mph_with_handler(&h){
    result = fun(&h,arg);
  }}
  return result;
}





//---------------------------------------------------------------------------
// Unwind
//---------------------------------------------------------------------------

#if MP_HAS_TRY
class mph_unwind_exception : public std::exception {
public:
  mph_handler_t* target;
  mph_unwind_fun_t* fun;
  void* arg1;
  void* arg2;
  mph_unwind_exception(mph_handler_t* h, mph_unwind_fun_t* fun, void* arg1, void* arg2) : target(h), fun(fun), arg1(arg1), arg2(arg2) {  }
  mph_unwind_exception(const mph_unwind_exception& e) : target(e.target), fun(e.fun), arg1(e.arg1), arg2(e.arg2) { }
  mph_unwind_exception& operator=(const mph_unwind_exception& e) {
    target = e.target; fun = e.fun; arg1 = e.arg1; arg2 = e.arg2;
    return *this;
  }

  virtual const char* what() const throw() {
    return "libmpeff: unwinding the stack; do not catch this exception!";
  }
};

void mph_unwind_to(mph_handler_t* target, mph_unwind_fun_t* fun, void* arg1, void* arg2) {
  throw mph_unwind_exception(target, fun, arg1, arg2);
}
#else
void mph_unwind_to(mph_handler_t* target, mph_unwind_fun_t* fun, void* arg1, void* arg2) {
  // TODO: walk the handlers and invoke finally frames
  // and finally yield up to abort
  mph_abort_to(target, fun, arg1, arg2);
}
#endif



//---------------------------------------------------------------------------
// Full prompt handler: can be yielded to (or unwound to)
//---------------------------------------------------------------------------


// Pass arguments down in a closure environment
struct mph_start_env {
  mph_kind_t       kind;
  void*            hdata;
  void*            local;
  mph_start_fun_t* body;
  void*            arg;
};

// Start a handler
static void* mph_start(mp_prompt_t* prompt, void* earg) {
  // init
  struct mph_start_env* env = (struct mph_start_env*)earg;  
  mph_handler_t h;
  h.kind = env->kind;
  h.prompt = prompt;
  h.hdata = env->hdata;
  h.local = env->local;
#if MP_HAS_TRY
  try {  // catch unwind exceptions
#endif
    void* result = NULL;
    {mph_with_handler(&h) {
      result = (env->body)(&h, env->arg);
    }}
    return result;
#if MP_HAS_TRY
  }     // handle unwind exceptions
  catch (const mph_unwind_exception& e) {
    if (e.target != &h) {
      throw;  // rethrow 
    }
    return (e.fun)(h.local, e.arg1, e.arg2);  // run here before dropping the prompt (unlike abort does)
  }
#endif  
}


void* mph_prompt_handler(mph_kind_t kind, void* hdata, void* local, mph_start_fun_t* fun, void* arg) {
  struct mph_start_env env = { kind, hdata, local, fun, arg };
  return mp_prompt(&mph_start, &env);
}


//---------------------------------------------------------------------------
// Yield
//---------------------------------------------------------------------------

typedef struct mph_yield_env_s {
  void*            local;
  mph_yield_fun_t* fun;
  void*            arg;
} mph_yield_env_t;

typedef struct mph_resume_env_s {
  void* local;
  void* result;
  bool  unwind;
} mph_resume_env_t;


static void* mph_yield_fun(mp_resume_t* r, void* envarg) {
  mph_yield_env_t* env = (mph_yield_env_t*)envarg;
  return (env->fun)((mph_resume_t*)r, env->local, env->arg);
}


static void* mph_unwind_fun(void* local, void* arg1, void* arg2) {
  (void)(local); (void)(arg2);
  return arg1;
}

// Yield to a prompt without unwinding
static void* mph_yield_to_internal(bool once, mph_handler_t* h, mph_yield_fun_t fun, void* arg) {
  mph_assert(mph_is_prompt_handler(h));
  
  // unlink the current handler top
  mph_handler_t* yield_top = _mph_top; 
  _mph_top = h->parent;                

  // yield
  mph_yield_env_t yenv = { h->local, fun, arg };
  mph_resume_env_t* renv = (mph_resume_env_t*)
                           (mph_likely(once) ? mp_yield(h->prompt, &mph_yield_fun, &yenv) 
                                             : mp_myield(h->prompt, &mph_yield_fun, &yenv));
 
  // and relink handlers once resumed
  h->parent = _mph_top;
  _mph_top = yield_top;
  
  // unwind or return?
  if (mph_unlikely(renv->unwind)) {
    mph_unwind_to(h, &mph_unwind_fun, renv->result, NULL);
    return NULL;
  }
  else {
    // new local
    h->local = renv->local;
    return renv->result;
  }
}

// Yield to a prompt without unwinding
void* mph_yield_to(mph_handler_t* h, mph_yield_fun_t fun, void* arg) {
  return mph_yield_to_internal(true, h, fun, arg);
}

// Multi-shot Yield to a prompt without unwinding
void* mph_myield_to(mph_handler_t* h, mph_yield_fun_t fun, void* arg) {
  return mph_yield_to_internal(false, h, fun, arg);
}

//---------------------------------------------------------------------------
// Abort
//---------------------------------------------------------------------------


typedef struct mph_abort_env_s {
  void* local;
  mph_unwind_fun_t* fun;
  void* arg1;
  void* arg2;
} mph_abort_env_t;

static void* mph_abort_fun(mp_resume_t* r, void* envarg) {
  mph_abort_env_t yenv = *((mph_abort_env_t*)envarg); // copy as the drop can discard the memory
  mp_resume_drop(r);
  return (yenv.fun)(yenv.local,yenv.arg1,yenv.arg2);
}


// Yield to a prompt without unwinding
void mph_abort_to(mph_handler_t* h, mph_unwind_fun_t* fun, void* arg1, void* arg2) {
  mph_assert(mph_is_prompt_handler(h));
  mph_abort_env_t env = { h->local, fun, arg1, arg2 };
  mp_yield(h->prompt, &mph_abort_fun, &env);
}



//---------------------------------------------------------------------------
// Resuming
//---------------------------------------------------------------------------

// mph_resume_t* is always cast to mp_resume_t*
struct mph_resume_s {
  void* abstract;
};

void* mph_resume(mph_resume_t* resume, void* local, void* arg) {
  mph_resume_env_t renv = { local, arg, false };
  return mp_resume((mp_resume_t*)resume, &renv);
}

void* mph_resume_tail(mph_resume_t* resume, void* local, void* arg) {
  mph_resume_env_t renv = { local, arg, false };
  return mp_resume_tail((mp_resume_t*)resume, &renv);
}

void mph_resume_unwind(mph_resume_t* resume) {
  mph_resume_env_t renv = { NULL, NULL, true /* unwind */ };
  mp_resume((mp_resume_t*)resume, &renv);
}


void mph_resume_drop(mph_resume_t* resume) {
  mp_resume_t* r = (mp_resume_t*)resume;
  if (mp_resume_should_unwind(r)) {
    mph_resume_unwind(resume);
  }
  else {
    mp_resume_drop(r);
  }
}


mph_resume_t* mph_resume_dup(mph_resume_t* resume) {
  mp_resume_t* r = (mp_resume_t*)resume;
  mp_resume_dup(r);
  return resume;
}

//---------------------------------------------------------------------------
// Under
//---------------------------------------------------------------------------

void* mph_under(mph_kind_t under, void* (*fun)(void*), void* arg) {
  mph_handler_under_t h;
  h.under = under;
  h.handler.kind = MPH_UNDER;
  h.handler.prompt = NULL;
  h.handler.hdata = NULL;
  h.handler.local = NULL;
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
  h.handler.kind = MPH_MASK;
  h.handler.prompt = NULL;
  h.handler.hdata = NULL;
  h.handler.local = NULL;
  void* result = NULL;
  {mph_with_handler(&h.handler) {
    result = fun(arg);
  }}
  return result;
}

