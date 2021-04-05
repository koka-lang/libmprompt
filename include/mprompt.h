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
typedef struct mp_resume_s   mp_resume_t;     // abstract resumption

// Function types
typedef void* (mp_start_fun_t)(mp_prompt_t*, void* arg); 
typedef void* (mp_yield_fun_t)(mp_resume_t*, void* arg);  

// Continue with `fun(p,arg)` under a fresh prompt `p`.
mp_decl_export void* mp_prompt(mp_start_fun_t* fun, void* arg); 

// Yield back up to a parent prompt `p` and run `fun(r,arg)` from there, where `r` is a `mp_resume_t` resumption.
mp_decl_export void* mp_yield(mp_prompt_t* p, mp_yield_fun_t* fun, void* arg);

// Resume back to the yield point with a result; can be used at most once.
mp_decl_export void* mp_resume(mp_resume_t* resume, void* arg);      // resume 
mp_decl_export void* mp_resume_tail(mp_resume_t* resume, void* arg); // resume as the last action in a `mp_yield_fun_t`
mp_decl_export void  mp_resume_drop(mp_resume_t* resume);            // drop the resume object without resuming


//---------------------------------------------------------------------------
// Multi-shot resumptions; use with care in combination with linear resources.
//---------------------------------------------------------------------------

mp_decl_export void* mp_yieldm(mp_prompt_t* p, mp_yield_fun_t* fun, void* arg);
mp_decl_export mp_resume_t* mp_resume_dup(mp_resume_t* r);    // only myield resumptions can be dup'd



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
// Low-level access  
// (only `mp_mresume_should_unwind` is required by `libmphandler`)
//---------------------------------------------------------------------------

// Get a portable backtrace
mp_decl_export int          mp_backtrace(void** backtrace, int len);

// How often is this resumption resumed?
mp_decl_export long         mp_resume_resume_count(mp_resume_t* r);
mp_decl_export int          mp_resume_should_unwind(mp_resume_t* r);  // refcount==1 && resume_count==0

// Separate prompt creation
mp_decl_export mp_prompt_t* mp_prompt_create(void);
mp_decl_export void* mp_prompt_enter(mp_prompt_t* p, mp_start_fun_t* fun, void* arg) ;

// Walk the chain of prompts.
mp_decl_export mp_prompt_t* mp_prompt_top(void);
mp_decl_export mp_prompt_t* mp_prompt_parent(mp_prompt_t* p);


#endif