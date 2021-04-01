/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MP_MPROMPT_H
#define MP_MPROMPT_H

//------------------------------------------------------
// Compiler specific attributes
//------------------------------------------------------
#if defined(_MSC_VER) || defined(__MINGW32__)
#if !defined(MP_SHARED_LIB)
#define mp_decl_export      
#elif defined(MP_SHARED_LIB_EXPORT)
#define mp_decl_export      __declspec(dllexport)
#else
#define mp_decl_export      __declspec(dllimport)
#endif
#elif defined(__GNUC__) // includes clang and icc      
#define mp_decl_export      __attribute__((visibility("default")))
#else
#define mp_decl_export      
#endif


//---------------------------------------------------------------------------
// Multi-prompt interface
//---------------------------------------------------------------------------

// Types
typedef struct mp_prompt_s   mp_prompt_t;     // resumable "prompts" (in-place growable stack chain)
typedef struct mp_resume_s   mp_resume_t;     // single-shot resume
typedef struct mp_mresume_s  mp_mresume_t;    // multi-shot resume

// Function types
typedef void* (mp_start_fun_t)(mp_prompt_t*, void* arg); 
typedef void* (mp_yield_fun_t)(mp_resume_t*, void* arg);  

// Continue with `fun(p,arg)` under a fresh prompt `p`.
mp_decl_export void* mp_prompt(mp_start_fun_t* fun, void* arg);

// Yield back up to a parent prompt `p` and run `fun(r,arg)` from there, where `r` is a `mp_resume_t` resumption.
mp_decl_export void* mp_yield(mp_prompt_t* p, mp_yield_fun_t* fun, void* arg);

// Resume back to the yield point with a result; can be used at most once.
mp_decl_export void* mp_resume(mp_resume_t* resume, void* arg);         // resume once
mp_decl_export void* mp_resume_tail(mp_resume_t* resume, void* arg);    // resume once as the last action in a `mp_yield_fun_t`
mp_decl_export void  mp_resume_drop(mp_resume_t* resume);               // drop the resume object without resuming



//---------------------------------------------------------------------------
// Multi-shot resumptions; use with care in combination with linear resources.
//---------------------------------------------------------------------------

typedef void* (mp_myield_fun_t)(mp_mresume_t*, void* arg);

mp_decl_export void* mp_myield(mp_prompt_t* p, mp_myield_fun_t* fun, void* arg);
mp_decl_export void* mp_mresume(mp_mresume_t* r, void* arg);
mp_decl_export void* mp_mresume_tail(mp_mresume_t* r, void* arg);
mp_decl_export void  mp_mresume_drop(mp_mresume_t* r);
mp_decl_export mp_mresume_t* mp_mresume_dup(mp_mresume_t* r);



//---------------------------------------------------------------------------
// Initialization
//---------------------------------------------------------------------------
#include <stddef.h>
#include <stdbool.h>

// Configuration settings; any zero value uses the default setting.
typedef struct mp_config_s {
  bool      gpool_enable;         // enable gpool by default (on systems without overcommit gpools may still be enabled even if this is false)
  ptrdiff_t gpool_max_size;       // maximum virtual size per gpool (256 GiB)
  ptrdiff_t stack_max_size;       // maximum virtual size of a gstack (8 MiB)
  ptrdiff_t stack_exn_guaranteed; // guaranteed extra stack space available during exception unwinding (Windows only) (16 KiB)
  ptrdiff_t stack_initial_commit; // initial commit size of a gstack (OS page size, 4 KiB)
  ptrdiff_t stack_gap_size;       // virtual no-access gap between stacks for security (64 KiB)
  ptrdiff_t stack_cache_count;    // count of gstacks to keep in a thread-local cache (4)
} mp_config_t;

// Initialize with `config`; use NULL for default settings.
// Call at most once from the main thread before using any other functions. 
// Overwrites the `config` with the actual used settings.
//
// Use as: `mp_config_t config = { }; config.<setting> = <N>; mp_init(&config);`.
mp_decl_export void mp_init(mp_config_t* config);




//---------------------------------------------------------------------------
// Handler interface
// A higher level abstraction on top of mprompt that maintains a stack
// of handlers (so one can yield to a parent without needing a specific marker)
// This also integrates unwinding and exception propagation.
//---------------------------------------------------------------------------

typedef struct mph_handler_s mph_handler_t;
typedef struct mph_resume_s  mph_resume_t;

typedef void* (mph_start_fun_t)(void* hdata, void* arg);
typedef void* (mph_yield_fun_t)(mph_resume_t* resume, void* hdata, void* arg);
typedef void* (mph_unwind_fun_t)(void* hdata, void* arg);

typedef const char* mph_kind_t;      // user extensible

mp_decl_export void*          mph_prompt_handler(mph_kind_t kind, size_t hdata_size, mph_start_fun_t* fun, void* arg);
mp_decl_export mph_handler_t* mph_find(mph_kind_t kind);
mp_decl_export void*          mph_yield_to(mph_handler_t* handler, mph_yield_fun_t fun, void* arg);
mp_decl_export void*          mph_unwind_to(mph_handler_t* handler, mph_unwind_fun_t fun, void* arg);

mp_decl_export mph_kind_t     mph_kind(mph_handler_t* handler);
mp_decl_export void*          mph_data(mph_handler_t* handler);


// Light weight linear handlers; cannot be yielded to (or unwound to)
// the finally, under, and mask handlers are linear. Effect handlers that always tail-resume are linear as well.
// todo: provide inline macros
mp_decl_export void*          mph_linear_handler(mph_kind_t kind, void* hdata, mph_start_fun_t* fun, void* arg);
mp_decl_export void*          mph_under(mph_kind_t under, void* (*fun)(void*), void* arg);
mp_decl_export void*          mph_mask(mph_kind_t mask, int from);


// low-level access
mp_decl_export mph_handler_t* mph_top(void);
mp_decl_export mph_handler_t* mph_parent(mph_handler_t* handler);

extern mph_kind_t MPH_FINALLY;
extern mph_kind_t MPH_UNDER;
extern mph_kind_t MPH_MASK;


// multi-shot
//typedef void* (mp_handler_myield_fun_t)(mp_mresume_t* resume, mph_handler_t* handler, void* arg);
//mp_decl_export void* mp_handler_myield_to(mph_handler_t* handler, mp_handler_myield_fun_t* fun, void* arg);



//---------------------------------------------------------------------------
// Low-level access  
// (only `mp_mresume_should_unwind` is required by `libmphandler`)
//---------------------------------------------------------------------------

mp_decl_export long         mp_mresume_resume_count(mp_mresume_t* r);
mp_decl_export int          mp_mresume_should_unwind(mp_mresume_t* r);  // refcount==1 && resume_count==0

mp_decl_export mp_prompt_t* mp_prompt_create(void);
mp_decl_export void*        mp_prompt_enter(mp_prompt_t* p, mp_start_fun_t* fun, void* arg);
mp_decl_export mp_prompt_t* mp_prompt_parent(mp_prompt_t* p);



//---------------------------------------------------------------------------
// Misc
//---------------------------------------------------------------------------

// to be fixed... 
#define mp_throw    throw
#ifdef _WIN32
#include <stdint.h>
mp_decl_export void mp_win_trace_stack_layout(uint8_t* base, uint8_t* base_limit);
#endif


#endif