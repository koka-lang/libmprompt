/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MP_WASM_H
#define MP_WASM_H

#include <stdbool.h>
#include <stdint.h>
#include <mprompt.h>

//------------------------------------------------------
// Compiler specific attributes
//------------------------------------------------------

#if defined(_MSC_VER) || defined(__MINGW32__)
#if !defined(MP_SHARED_LIB)
#define mpw_decl_export     
#elif defined(MP_SHARED_LIB_EXPORT)
#define mpw_decl_export      __declspec(dllexport)
#else
#define mpw_decl_export      __declspec(dllimport)
#endif
#elif defined(__GNUC__) // includes clang and icc      
#define mpw_decl_export      __attribute__((visibility("default")))
#else
#define mpw_decl_export      
#endif


//------------------------------------------------------
// Interface
//------------------------------------------------------

/// A generic action 
typedef void* (mpw_action_fun_t)(void* arg);

typedef void  (mpw_release_fun_t)(void);

typedef mp_resume_t mpw_cont_t;

/// Effect values.
/// Operations are identified by a constant string pointer.
/// They are compared by address though so they must be declared as static constants (using #MPW_NEWOPTAG)
typedef const char* const* mpw_effect_t;

typedef long mpw_opidx_t;

/// Operation values.
/// An operation is identified by an effect and index in that effect. 
/// There are defined automatically using `MPW_DEFINE_OPn` macros and can be referred to 
/// using `MPW_OPTAG(effect,opname)`.
typedef const struct mpw_optag_s {
  mpw_effect_t effect;
  mpw_opidx_t  opidx;
} *mpw_optag_t;


/// Operation functions are called when that operation is `yield`ed to. 
typedef void* (mpw_opfun_t)(mpw_cont_t* r, void* local, void* arg);


/*-----------------------------------------------------------------
  Handlers
-----------------------------------------------------------------*/
mpw_decl_export mpw_cont_t*   mpw_new(mpw_action_fun_t fun);
mpw_decl_export void*         mpw_suspend(mpw_optag_t optag, void* arg);
mpw_decl_export mpw_opidx_t   mpw_resume(mpw_effect_t eff, mpw_cont_t** resume, void* arg, void** result);
mpw_decl_export void          mpw_resume_drop(mpw_cont_t* resume);  
mpw_decl_export mpw_cont_t*   mpw_resume_dup(mpw_cont_t* resume);  

//mpw_decl_export void*         mpw_mask(mpw_effect_t eff, size_t from, mpw_actionfun_t* fun, void* arg);
//mpw_decl_export void*         mpw_finally(void* local, mpw_releasefun_t* finally_fun, mpw_actionfun_t* fun, void* arg);


/*-----------------------------------------------------------------
  Operation tags
-----------------------------------------------------------------*/

/// Null effect.
#define mpw_effect_null ((mpw_effect_t)NULL)
#define mpw_op_null  ((mpw_optag_t)NULL)

/// Get the name of an operation tag. 
mpw_decl_export const char* mpw_optag_name(mpw_optag_t optag);

/// Get the name of an effect tag. 
mpw_decl_export const char* mpw_effect_name(mpw_effect_t effect);


/*-----------------------------------------------------------------
  Operation definition helpers
-----------------------------------------------------------------*/

/// \defgroup effect_def Effect Definition helpers
/// Macros to define effect handlers.
/// \{

#define MPW_EFFECT(effect)         mpw_names_effect_##effect
#define MPW_OPTAG_DEF(effect,op)   mpw_op_##effect##_##op
#define MPW_OPTAG(effect,op)       &MPW_OPTAG_DEF(effect,op)

#define MPW_OP(effect,op)          MPW_OPTAG_DEF(effect,op).opidx

#define MPW_DECLARE_EFFECT0(effect)  \
extern const char* MPW_EFFECT(effect)[2];

#define MPW_DECLARE_EFFECT1(effect,op1)  \
extern const char* MPW_EFFECT(effect)[3];

#define MPW_DECLARE_EFFECT2(effect,op1,op2)  \
extern const char* MPW_EFFECT(effect)[4];

#define MPW_DECLARE_OP(effect,op) \
extern const struct mpw_optag_s MPW_OPTAG_DEF(effect,op);

#define MPW_DECLARE_OP0(effect,op,restype) \
MPW_DECLARE_OP(effect,op) \
restype effect##_##op();

#define MPW_DECLARE_OP1(effect,op,restype,argtype) \
MPW_DECLARE_OP(effect,op) \
restype effect##_##op(argtype arg);

#define MPW_DECLARE_VOIDOP0(effect,op) \
MPW_DECLARE_OP(effect,op) \
void effect##_##op();

#define MPW_DECLARE_VOIDOP1(effect,op,argtype) \
MPW_DECLARE_OP(effect,op) \
void effect##_##op(argtype arg);


#define MPW_DEFINE_EFFECT0(effect) \
const char* MPW_EFFECT(effect)[2] = { #effect, NULL }; 

#define MPW_DEFINE_EFFECT1(effect,op1) \
const char* MPW_EFFECT(effect)[3] = { #effect, #effect "/" #op1, NULL }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op1) = { MPW_EFFECT(effect), 0 }; 

#define MPW_DEFINE_EFFECT2(effect,op1,op2) \
const char* MPW_EFFECT(effect)[4] = {  #effect, #effect "/" #op1, #effect "/" #op2, NULL }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op1) = { MPW_EFFECT(effect), 0 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op2) = { MPW_EFFECT(effect), 1 }; 

#define MPW_DEFINE_EFFECT3(effect,op1,op2,op3) \
const char* MPW_EFFECT(effect)[5] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, NULL }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op1) = { MPW_EFFECT(effect), 0 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op2) = { MPW_EFFECT(effect), 1 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op3) = { MPW_EFFECT(effect), 2 }; 

#define MPW_DEFINE_EFFECT4(effect,op1,op2,op3,op4) \
const char* MPW_EFFECT(effect)[6] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, NULL }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op1) = { MPW_EFFECT(effect), 0 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op2) = { MPW_EFFECT(effect), 1 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op3) = { MPW_EFFECT(effect), 2 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op4) = { MPW_EFFECT(effect), 3 }; 

#define MPW_DEFINE_EFFECT5(effect,op1,op2,op3,op4,op5) \
const char* MPW_EFFECT(effect)[7] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, #effect "/" #op5, NULL }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op1) = { MPW_EFFECT(effect), 0 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op2) = { MPW_EFFECT(effect), 1 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op3) = { MPW_EFFECT(effect), 2 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op4) = { MPW_EFFECT(effect), 3 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op5) = { MPW_EFFECT(effect), 4 }; 

#define MPW_DEFINE_EFFECT6(effect,op1,op2,op3,op4,op5,op6) \
const char* MPW_EFFECT(effect)[8] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, #effect "/" #op5, #effect "/" #op6, NULL }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op1) = { MPW_EFFECT(effect), 0 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op2) = { MPW_EFFECT(effect), 1 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op3) = { MPW_EFFECT(effect), 2 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op4) = { MPW_EFFECT(effect), 3 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op5) = { MPW_EFFECT(effect), 4 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op6) = { MPW_EFFECT(effect), 5 }; 

#define MPW_DEFINE_EFFECT7(effect,op1,op2,op3,op4,op5,op6,op7) \
const char* MPW_EFFECT(effect)[9] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, #effect "/" #op5, #effect "/" #op6, #effect "/" #op7, NULL }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op1) = { MPW_EFFECT(effect), 0 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op2) = { MPW_EFFECT(effect), 1 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op3) = { MPW_EFFECT(effect), 2 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op4) = { MPW_EFFECT(effect), 3 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op5) = { MPW_EFFECT(effect), 4 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op6) = { MPW_EFFECT(effect), 5 }; \
const struct mpw_optag_s MPW_OPTAG_DEF(effect,op7) = { MPW_EFFECT(effect), 6 }; 


#define MPW_DEFINE_OP0(effect,op,restype) \
  restype effect##_##op() { void* res = mpw_suspend(MPW_OPTAG(effect,op), NULL); return mpw_##restype##_voidp(res); }

#define MPW_DEFINE_OP1(effect,op,restype,argtype) \
  restype effect##_##op(argtype arg) { void* res = mpw_suspend(MPW_OPTAG(effect,op), mpw_voidp_##argtype(arg)); return mpw_##restype##_voidp(res); }

#define MPW_DEFINE_VOIDOP0(effect,op) \
  void effect##_##op() { mpw_suspend(MPW_OPTAG(effect,op), NULL); }

#define MPW_DEFINE_VOIDOP1(effect,op,argtype) \
  void effect##_##op(argtype arg) { mpw_suspend(MPW_OPTAG(effect,op), mpw_voidp_##argtype(arg)); }

#define MPW_WRAP_FUN0(fun,restype) \
  void* wrap_##fun(void* arg) { (void)(arg); return mpw_voidp_##restype(fun()); }

#define MPW_WRAP_FUN1(fun,argtype,restype) \
  void* wrap_##fun(void* arg) { return mpw_voidp_##restype(fun(mpw_##argtype##_voidp(arg))); }

#define MPW_WRAP_VOIDFUN0(fun) \
  void* wrap_##fun(void* arg) { (void)(arg); fun(); return NULL; }

#define MPW_WRAP_VOIDFUN1(fun,argtype) \
  void* wrap_##fun(void* arg) { fun(mpw_##argtype##_voidp(arg)); return NULL; }


/*-----------------------------------------------------------------
	 Generic values
-----------------------------------------------------------------*/
typedef void* mpw_voidp_t;

/// Null value
#define mpw_voidp_null         (NULL)

/// Convert an #mpw_voidp_t back to a pointer.
#define mpw_ptr_voidp(v)       ((void*)(v))

/// Convert any pointer into a #mpw_voidp_t.
#define mpw_voidp_ptr(p)       ((void*)(p))


/// Identity; used to aid macros.
#define mpw_voidp_mpw_voidp_t(v)     (v)

/// Convert an #mpw_voidp_t back to an `int`.
#define mpw_int_voidp(v)       ((int)((intptr_t)(v)))

/// Convert an `int` to an #mpw_voidp_t.
#define mpw_voidp_int(i)       ((mpw_voidp_t)((intptr_t)(i)))

/// Convert an #mpw_voidp_t back to a `long`.
#define mpw_long_voidp(v)      ((long)((intptr_t)(v)))

/// Convert a `long` to an #mpw_voidp_t.
#define mpw_voidp_long(i)      ((mpw_voidp_t)((intptr_t)(i)))

/// Convert an #mpw_voidp_t back to a `uint64_t`.
#define mpw_uint64_t_voidp(v)      ((uint64_t)((intptr_t)(v)))

/// Convert a `uint64_t` to an #mpw_voidp_t.
#define mpw_voidp_uint64_t(i)      ((mpw_voidp_t)((intptr_t)(i)))

/// Convert an #mpw_voidp_t back to a `bool`.
#define mpw_bool_voidp(v)      (mpw_int_voidp(v) != 0 ? (1==1) : (1==0))

/// Convert a `bool` to an #mpw_voidp_t.
#define mpw_voidp_bool(b)      (mpw_voidp_int(b ? 1 : 0))

/// Type definition for strings to aid with macros.
typedef const char* mpw_string_t;

/// Convert an #mpw_voidp_t back to a `const char*`.
#define mpw_mpw_string_t_voidp(v) ((const char*)mpw_ptr_voidp(v))

/// Convert a `const char*` to an #mpw_voidp_t.
#define mpw_voidp_mpw_string_t(v) (mpw_voidp_ptr(v))

#define mpw_optag_voidp(v)     ((mpw_optag)mpw_ptr_voidp(v))
#define mpw_voidp_optag(o)     mpw_voidp_ptr(o)


#endif