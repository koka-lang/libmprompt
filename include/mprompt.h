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

// Initialize explicitly:
//  gstack_max_size : pass 0 to use default (8MiB virtual)
//  gpool_max_size  : pass 0 to not use gpools if possible (only on Windows and Linux with overcommit enabled)
//                    use 1 to enable gpools with the default maximum size (256GiB virtual)
#include <stddef.h>
mp_decl_export void mp_mprompt_init(size_t gstack_max_size, size_t gpool_max_size);



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