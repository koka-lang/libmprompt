/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#include "mprompt.h"
#include "internal/util.h"
#include "internal/longjmp.h"
#include "internal/gstack.h"

#ifdef __cplusplus
#include <exception>
#include <utility>
#endif


//-----------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------

typedef enum mp_return_kind_e {
  MP_RETURN,         // normal return
  MP_EXCEPTION,      // return with an exception
  MP_YIELD_ONCE,     // yield that can be resumed at most once.
  MP_YIELD_MULTI,    // yield that can be resumed multiple times (or not at all).
} mp_return_kind_t;


typedef struct mp_resume_point_s {   // allocated on the suspended stack (which performed a yield)
  mp_jmpbuf_t      jmp;     
  void*            result;  // the yield result (= resume argument)
} mp_resume_point_t;

typedef struct mp_return_point_s {   // allocated on the parent stack (which performed an enter/resume)
  mp_jmpbuf_t      jmp;     // must be the first field (see `mp_stack_enter`)
  mp_return_kind_t kind;    
  void*            fun;     // if yielding, the function to execute
  void*            arg;     // if yielding, the argument to the function; if returning, the result.
  #ifdef __cplusplus
  std::exception_ptr exn;   // returning with an exception to propagate
  #endif
} mp_return_point_t;


// Prompt:
// Represents a piece of stack and can be yielded to.
//
// A prompt can be in 2 states:
// _active_:    top == NULL
//              means the prompt and its gstack is part of propmt stack chain.
// _suspended_: top != NULL, resume_point != NULL
//              This when being captured as a resumption. The `top` points to the end of the captured resumption.
//              and the prompt (and children) are not part of the current stack chain.
//              note that the prompt children are still in the _active_ state (but not part of a current execution stack chain)

struct mp_prompt_s {  
  mp_prompt_t*  parent;     // parent: previous prompt up in the stack chain (towards bottom of the stack)
  mp_prompt_t*  top;        // top of a suspended prompt chain.
  intptr_t      refcount;   // free when drops to zero
  mp_gstack_t*  gstack;     // the growable stacklet associated with this prompt;
                            // note: the prompt structure itself is allocated at the base of the `gstack` to avoid a separate allocation.
  mp_return_point_t* return_point;  // return point in the parent (if not suspended..)
  mp_resume_point_t* resume_point;  // resume point for a suspended prompt chain. (the resume will be in the `top->gstack`)
};


// A resumption can only be used at most once. 
// This allows for allocation free yield and resume
struct mp_resume_s {
  mp_prompt_t p;
};

// If resuming multiple times, the original stack is saved in a corresponding chain of prompt_save structures.
typedef struct mp_prompt_save_s {
  struct mp_prompt_save_s* next;
  mp_prompt_t*  prompt;
  mp_gsave_t*   gsave;  
} mp_prompt_save_t;


// A general resumption that can be resumed multiple times; needs a small allocation and is reference counted.
// Only copies the original stack if it is actually being resumed more than once.
struct mp_mresume_s {
  intptr_t           refcount;
  long               resume_count;       // count number of resumes.
  mp_prompt_t*       prompt;
  mp_prompt_save_t*  save;
  mp_return_point_t* tail_return_point;  // need to save this as the one in the prompt may be overwritten by earlier resumes
};



//-----------------------------------------------------------------------
// Prompt chain
//-----------------------------------------------------------------------

// The top of the prompts chain; points to the prompt on whose stack we currently execute.
mp_decl_thread mp_prompt_t* _mp_prompt_top;

// get the top of the chain; ensures proper initialization
static inline mp_prompt_t* mp_prompt_top(void) {  
  return _mp_prompt_top;
}

// walk the prompt chain; returns NULL when done.
// with initial argument `NULL` the first prompt returned is the current top.
mp_prompt_t* mp_prompt_parent(mp_prompt_t* p) {
  return (p == NULL ? mp_prompt_top() : p->parent);
}

#ifndef NDEBUG
// An _active_ prompt is currently part of the stack.
static bool mp_prompt_is_active(mp_prompt_t* p) {
  return (p != NULL && p->top == NULL);
}

// Is a prompt an ancestor in the chain?
static bool mp_prompt_is_ancestor(mp_prompt_t* p) {
  mp_prompt_t* q = NULL;
  while ((q = mp_prompt_parent(q)) != NULL) {
    if (q == p) return true;
  }
  return false;
}
#endif

// Allocate a fresh (suspended) prompt
mp_prompt_t* mp_prompt_create(void) {
  // allocate a fresh growable stack
  mp_gstack_t* gstack = mp_gstack_alloc();
  if (gstack == NULL) mp_fatal_message(ENOMEM, "unable to allocate a stack\n");
  // allocate the prompt structure at the base of the new stack
  mp_prompt_t* p = (mp_prompt_t*)mp_gstack_reserve(gstack, sizeof(mp_prompt_t));
  p->parent = NULL;
  p->top = p;
  p->refcount = 1;
  p->gstack = gstack;
  p->resume_point = NULL;
  p->return_point = NULL;
  return p;
}

// Free a prompt and drop its children
static void mp_prompt_free(mp_prompt_t* p) {
  mp_assert_internal(!mp_prompt_is_active(p));
  p = p->top;
  while (p != NULL) {
    mp_assert_internal(p->refcount == 0);
    mp_prompt_t* parent = p->parent;    
    mp_gstack_free(p->gstack);
    if (parent != NULL) {
      mp_assert_internal(parent->refcount == 1);
      parent->refcount--;
    }
    p = parent;
  }
}

// Decrement the refcount (and free when it becomes zero).
static void mp_prompt_drop(mp_prompt_t* p) {
  int64_t i = p->refcount--;
  if (i <= 1) {
    mp_prompt_free(p);
  }
}

// Increment the refcount
static mp_prompt_t* mp_prompt_dup(mp_prompt_t* p) {
  p->refcount++;
  return p;
}

// Link a suspended prompt to the current prompt chain and set the new prompt top
static inline mp_resume_point_t* mp_prompt_link(mp_prompt_t* p, mp_return_point_t* ret) {
  mp_assert_internal(!mp_prompt_is_active(p));
  p->parent = mp_prompt_top();
  _mp_prompt_top = p->top;
  p->top = NULL;
  if (ret != NULL) { p->return_point = ret; }                         
              else { mp_assert_internal(p->return_point != NULL); }  // used for tail resumes
  mp_assert_internal(mp_prompt_is_active(p));  
  return p->resume_point;
}

// Unlink a prompt from the current chain and make suspend it (and set the new prompt top to its parent)
static inline mp_return_point_t* mp_prompt_unlink(mp_prompt_t* p, mp_resume_point_t* res) {
  mp_assert_internal(mp_prompt_is_active(p));
  mp_assert_internal(mp_prompt_is_ancestor(p)); // ancestor of current top?
  p->top = mp_prompt_top();
  _mp_prompt_top = p->parent;
  p->parent = NULL;  
  p->resume_point = res;
  // note: leave return_point as-is for potential reuse in tail resumes
  mp_assert_internal(!mp_prompt_is_active(p));
  return p->return_point;
}



//-----------------------------------------------------------------------
// Create an initial prompt
//-----------------------------------------------------------------------

// Initial stack entry

typedef struct mp_entry_env_s {
  mp_prompt_t* prompt;
  mp_start_fun_t* fun;
  void* arg;
} mp_entry_env_t;

static  void mp_prompt_stack_entry(void* penv, void* trap_frame) {
  MP_UNUSED(trap_frame);
  mp_entry_env_t* env = (mp_entry_env_t*)penv;
  mp_prompt_t* p = env->prompt;
  //mp_prompt_stack_entry(p, env->fun, env->arg);
  #ifdef __cplusplus
  try {
  #endif
    void* result = (env->fun)(p, env->arg);
    // RET: return from a prompt
    mp_return_point_t* ret = mp_prompt_unlink(p, NULL);
    ret->arg = result;
    ret->fun = NULL;
    ret->kind = MP_RETURN;
    mp_longjmp(&ret->jmp);
  #ifdef __cplusplus
  }
  catch (...) {
    mp_trace_message("catch exception to propagate across the prompt %p..\n", p);
    mp_return_point_t* ret = mp_prompt_unlink(p, NULL);
    ret->exn = std::current_exception();
    ret->arg = NULL;
    ret->fun = NULL;
    ret->kind = MP_EXCEPTION;
    mp_longjmp(&ret->jmp);
  }
  #endif  
}

// Execute the function that is yielded or return normally.
static mp_decl_noinline void* mp_prompt_exec_yield_fun(mp_return_point_t* ret, mp_prompt_t* p) {
  mp_assert_internal(!mp_prompt_is_active(p));
  if (ret->kind == MP_YIELD_ONCE) {
    return ((mp_yield_fun_t*)ret->fun)((mp_resume_t*)p, ret->arg);
  }
  else if (ret->kind == MP_RETURN) {
    void* result = ret->arg;
    mp_prompt_drop(p);
    return result;
  }
  else if (ret->kind == MP_YIELD_MULTI) {
    mp_mresume_t* r = mp_malloc_safe_tp(mp_mresume_t);
    r->prompt = p;
    r->refcount = 1;
    r->resume_count = 0;
    r->save = NULL;
    r->tail_return_point = p->return_point;
    return ((mp_myield_fun_t*)ret->fun)(r, ret->arg);    
  }
  else {
    #ifdef __cplusplus
    mp_assert_internal(ret->kind == MP_EXCEPTION);
    mp_trace_message("rethrow propagated exception again (from prompt %p)..\n", p);
    mp_prompt_drop(p);
    mp_throw_prepare(); 
    std::rethrow_exception(ret->exn);
    #else
    mp_unreachable("invalid return kind");
    #endif
  }
}

// Resume a prompt: used for the initial entry as well as for resuming in a suspended prompt.
static mp_decl_noinline void* mp_prompt_resume(mp_prompt_t * p, void* arg) {
  mp_return_point_t ret;  
  // save our return location for yields and regular return  
  if (mp_setjmp(&ret.jmp)) {
    // P: return from yield (YR), or a regular return (RET)
    // printf("%s to prompt %p\n", (ret.kind == MP_RETURN ? "returned" : "yielded"), p);    
    return mp_prompt_exec_yield_fun(&ret, p);  // must be under the setjmp to preserve the stack
  }
  else {
    mp_assert(p->parent == NULL);
    mp_resume_point_t* res = mp_prompt_link(p,&ret);  // make active
    if (res != NULL) {
      // PR: resume to yield point
      res->result = arg;
      mp_longjmp(&res->jmp);
    }
    else {
      // PI: initial entry, switch to the new stack with an initial function      
      mp_gstack_enter(p->gstack, (mp_jmpbuf_t**)&p->return_point, &mp_prompt_stack_entry, arg);
    }
    mp_unreachable("mp_prompt_resume");    // should never return
  }
}

void* mp_prompt_enter(mp_prompt_t* p, mp_start_fun_t* fun, void* arg) {
  mp_assert_internal(!mp_prompt_is_active(p) && p->resume_point == NULL);
  mp_entry_env_t env;
  env.prompt = p;
  env.fun = fun;
  env.arg = arg;
  return mp_prompt_resume(p, &env);
}

// Install a fresh prompt `p` with a growable stack and start running `fun(p,arg)` on it.
void* mp_prompt(mp_start_fun_t* fun, void* arg) {
  mp_prompt_t* p = mp_prompt_create();
  return mp_prompt_enter(p, fun, arg);  // enter the initial stack with fun(arg)
}



//-----------------------------------------------------------------------
// Resume from a yield (once)
//-----------------------------------------------------------------------

// Resume (once)
void* mp_resume(mp_resume_t* resume, void* arg) {
  mp_prompt_t* p = &resume->p;
  mp_assert_internal(p->refcount == 1);
  mp_assert_internal(p->resume_point != NULL);
  return mp_prompt_resume(p, arg);  // resume back to yield point
}

// Resume in tail position to a prompt `p`
// Uses longjmp back to the `return_jump` as if it is yielding; this
// makes the tail-recursion use no stack as they keep getting back (P)
// and then into the exec_yield_fun function.
static void* mp_resume_tail_to(mp_prompt_t* p, void* arg, mp_return_point_t* ret) {
  mp_assert_internal(p->refcount == 1);
  mp_assert_internal(!mp_prompt_is_active(p));
  mp_assert_internal(p->resume_point != NULL);
  mp_resume_point_t* res = mp_prompt_link(p,ret);   // make active using the given return point!
  res->result = arg;
  mp_longjmp(&res->jmp);
}

// Resume in tail position (last and only resume in scope)
void* mp_resume_tail(mp_resume_t* resume, void* arg) {
  mp_prompt_t* p = &resume->p;
  return mp_resume_tail_to(p, arg, p->return_point);  // reuse return-point of the original entry
}

void mp_resume_drop(mp_resume_t* resume) {
  mp_prompt_t* p = &resume->p;
  mp_prompt_drop(p);
}


//-----------------------------------------------------------------------
// Yield up to a prompt
//-----------------------------------------------------------------------

// Yield to a prompt with a certain resumption kind. Once yielded back up, execute `fun(arg)`
static void* mp_yield_internal(mp_return_kind_t rkind, mp_prompt_t* p, void* fun, void* arg) {
  mp_assert_internal(mp_prompt_is_active(p)); // can only yield to an active prompt
  mp_assert_internal(mp_prompt_is_ancestor(p));
  // set our resume point (Y)
  mp_resume_point_t res;
  if (mp_setjmp(&res.jmp)) {
    // Y: resuming with a result (from PR)
    mp_assert_internal(mp_prompt_is_active(p));  // when resuming, we should be active again
    mp_assert_internal(mp_prompt_is_ancestor(p));
    return res.result;
  }
  else {
    // YR: yielding to prompt, or resumed prompt (P)
    mp_return_point_t* ret = mp_prompt_unlink(p, &res);
    ret->fun = fun;
    ret->arg = arg;
    ret->kind = rkind;
    mp_longjmp(&ret->jmp);
  }
}

// Yield back to a prompt with a `mp_resume_once_t` resumption.
void* mp_yield(mp_prompt_t* p, mp_yield_fun_t* fun, void* arg) {
  return mp_yield_internal(MP_YIELD_ONCE, p, (void*)fun, arg);
}

// Yield back to a prompt with a `mp_resume_t` resumption.
void* mp_myield(mp_prompt_t* p, mp_myield_fun_t* fun, void* arg) {
  return mp_yield_internal(MP_YIELD_MULTI, p, (void*)fun, arg);
}



//-----------------------------------------------------------------------
// General resume's that are first-class (and need allocation)
//-----------------------------------------------------------------------

// Increment the reference count of a resumption.
mp_mresume_t* mp_mresume_dup(mp_mresume_t* r) {
  r->refcount++;  
  return r;
}

long mp_mresume_resume_count(mp_mresume_t* r) {
  return r->resume_count;
}

int mp_mresume_should_unwind(mp_mresume_t* r) {  
  return (r->refcount == 1 && r->resume_count == 0);
}

// Decrement the reference count of a resumption.
void mp_mresume_drop(mp_mresume_t* r) {
  int64_t i = r->refcount--;
  if (i <= 1) {
    // free saved stacklets
    mp_prompt_save_t* s = r->save;
    while (s != NULL) {
      mp_prompt_save_t* next = s->next;
      mp_prompt_t* p = s->prompt;
      mp_gsave_free(s->gsave);
      mp_free(s);
      mp_prompt_drop(p);
      s = next;
    }
    mp_prompt_drop(r->prompt);
    //mp_trace_message("free resume: %p\n", r);
    mp_free(r);
  }
}


// Save a full prompt chain started at `p`
static mp_prompt_save_t* mp_prompt_save(mp_prompt_t* p) {
  mp_assert_internal(!mp_prompt_is_active(p));  
  mp_prompt_save_t* savep = NULL;
  uint8_t* sp = (uint8_t*)p->resume_point->jmp.reg_sp;
  p = p->top;
  do {
    mp_prompt_save_t* save = mp_malloc_tp(mp_prompt_save_t);
    save->prompt = mp_prompt_dup(p);
    save->next = savep;
    save->gsave = mp_gstack_save(p->gstack,sp);
    savep = save;
    sp = (uint8_t*)(p->parent == NULL ? NULL : p->return_point->jmp.reg_sp);  // set to parent's sp
    p = p->parent;    
  } while (p != NULL);
  mp_assert_internal(savep != NULL);
  return savep;
}

// Restore all prompt stacks from a save.
static void mp_prompt_restore(mp_prompt_t* p, mp_prompt_save_t* save) {
  mp_assert_internal(!mp_prompt_is_active(p));
  mp_assert_internal(p == save->prompt);
  MP_UNUSED(p);
  do {
    //mp_assert_internal(p == save->prompt);
    mp_gsave_restore(save->gsave);  // TODO: restore refcount?
    save = save->next;
  } while (save != NULL);
}


// Ensure proper refcount and pristine stack
static mp_prompt_t* mp_resume_get_prompt(mp_mresume_t* r) {
  mp_prompt_t* p = r->prompt;
  if (r->save != NULL) {
    mp_prompt_restore(p, r->save);
  }
  else if (r->refcount > 1 || p->refcount > 1) {
    r->save = mp_prompt_save(p);
  }
  mp_prompt_dup(p);
  mp_mresume_drop(r);
  return p;
}

// Resume with a regular resumption (and consumes `r` so dup if it needs to used later on)
void* mp_mresume(mp_mresume_t* r, void* arg) {
  r->resume_count++;
  mp_prompt_t* p = mp_resume_get_prompt(r);
  return mp_prompt_resume(p, arg);  // set a fresh prompt 
}

// Resume in tail position 
// Note: this only works if all earlier resumes were in-scope -- which should hold
// or otherwise the tail resumption wasn't in tail position anyways.
void* mp_mresume_tail(mp_mresume_t* r, void* arg) {
  mp_return_point_t* ret = r->tail_return_point;
  if (ret == NULL) {
    return mp_mresume(r, arg);  // resume normally as the return_point may not be preserved correctly
  }
  else {
    r->tail_return_point = NULL;
    r->resume_count++;
    mp_prompt_t* p = mp_resume_get_prompt(r);
    return mp_resume_tail_to(p, arg, ret);      // resume tail by reusing the original entry return point
  }
}

//-----------------------------------------------------------------------
// Initialize
//-----------------------------------------------------------------------

//#ifdef _WIN32
//#include <windows.h>
//void mp_throw_prepare(void) {
//  NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
//  tib->StackBase = ((uint64_t*)NULL - 1);
//  tib->StackLimit = NULL;
//  *((void**)((uint8_t*)tib + 5240)) = NULL;
//}
//#else
void mp_throw_prepare(void) {}
//#endif

void mp_mprompt_init(size_t gstack_size, size_t gpool_max_size) {
  /*uint8_t* sp = mp_win_sp();
  mp_stack_enter(sp - 256, NULL, NULL, &mp_test_start, NULL);
  */
  //ULONG guarantee = 32 * MP_KIB;
  //SetThreadStackGuarantee(&guarantee);
  // mp_throw_prepare();
  mp_gstack_init((ssize_t)gstack_size, (ssize_t)gpool_max_size);
}
