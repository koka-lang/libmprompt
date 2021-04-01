#pragma once
#ifndef MPH_HND_H
#define MPH_HND_H


//------------------------------------------------------
// Compiler specific attributes
//------------------------------------------------------
#if defined(_MSC_VER) || defined(__MINGW32__)
#if !defined(MPH_SHARED_LIB)
#define mp_decl_export      
#elif defined(MPH_SHARED_LIB_EXPORT)
#define mp_decl_export      __declspec(dllexport)
#else
#define mp_decl_export      __declspec(dllimport)
#endif
#elif defined(__GNUC__) // includes clang and icc      
#define mp_decl_export      __attribute__((visibility("default")))
#else
#define mp_decl_export      
#endif

#include <stddef.h>     // size_t

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

// Set up a handler
mp_decl_export void*          mph_prompt_handler(mph_kind_t kind, size_t hdata_size, mph_start_fun_t* fun, void* arg);
mp_decl_export mph_handler_t* mph_find(mph_kind_t kind);
mp_decl_export void*          mph_yield_to(mph_handler_t* handler, mph_yield_fun_t fun, void* arg);
mp_decl_export void           mph_unwind_to(mph_handler_t* handler, mph_unwind_fun_t fun, void* arg);

mp_decl_export mph_kind_t     mph_kind(mph_handler_t* handler);
mp_decl_export void*          mph_data(mph_handler_t* handler);


// Resume back to the yield point with a result; can be used at most once.
mp_decl_export void*          mph_resume(mph_resume_t* resume, void* arg);         // resume 
mp_decl_export void*          mph_resume_tail(mph_resume_t* resume, void* arg);    // resume as the last action in a `mph_yield_fun_t`
mp_decl_export void           mph_resume_drop(mph_resume_t* resume);               // drop the resume object without resuming (but it will unwind if never resumed before)


// Light weight linear handlers; cannot be yielded to (or unwound to)
// The finally, under, and mask handlers are linear. Effect handlers that always tail-resume are linear as well.
// Todo: provide inline macros
mp_decl_export void*          mph_linear_handler(mph_kind_t kind, void* hdata, mph_start_fun_t* fun, void* arg);
mp_decl_export void*          mph_under(mph_kind_t under, void* (*fun)(void*), void* arg);
mp_decl_export void*          mph_mask(mph_kind_t mask, int from);


// Multi-shot
mp_decl_export void*          mph_myield_to(mph_handler_t* handler, mph_yield_fun_t* fun, void* arg);
mp_decl_export mph_resume_t*  mph_resume_dup(mph_resume_t* r);              // only myield resumptions can be dup'd


// low-level access
mp_decl_export mph_handler_t* mph_top(void);
mp_decl_export mph_handler_t* mph_parent(mph_handler_t* handler);

extern mph_kind_t MPH_FINALLY;
extern mph_kind_t MPH_UNDER;
extern mph_kind_t MPH_MASK;




#endif // include guard
