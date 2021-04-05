/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MPE_EFFECTH_H
#define MPE_EFFECTH_H

#include <stdbool.h>
#include <stdint.h>

//------------------------------------------------------
// Compiler specific attributes
//------------------------------------------------------

#if defined(_MSC_VER) || defined(__MINGW32__)
#if !defined(MP_SHARED_LIB)
#define mpe_decl_export     
#elif defined(MP_SHARED_LIB_EXPORT)
#define mpe_decl_export      __declspec(dllexport)
#else
#define mpe_decl_export      __declspec(dllimport)
#endif
#elif defined(__GNUC__) // includes clang and icc      
#define mpe_decl_export      __attribute__((visibility("default")))
#else
#define mpe_decl_export      
#endif


//------------------------------------------------------
// Interface
//------------------------------------------------------

/// \defgroup effect_handlers Handlers
/// These are the primitives to define effect handlers and yield operations
/// \{

/// A generic action 
typedef void* (mpe_actionfun_t)(void* arg);

/// A `lh_resultfun` is called when a handled action is done.
typedef void* (mpe_resultfun_t)(void* local, void* arg);

/// An acquire function copies the local state in a handler when required.
typedef void (mpe_acquirefun_t)(void* local);

/// A release function releases the local state in a handler when required.
typedef void (mpe_releasefun_t)(void* local);


/// A "resume" continuation.
/// This is first-class, and can be stored in data structures etc, and can survive
/// the scope of an operation function. It can be resumed through mpe_resume() or mpe_release_resume().
typedef struct mpe_resume_s mpe_resume_t;

/// Effect values.
/// Operations are identified by a constant string pointer.
/// They are compared by address though so they must be declared as static constants (using #MPE_NEWOPTAG)
typedef const char* const* mpe_effect_t;

/// Operation values.
/// An operation is identified by an effect and index in that effect. 
/// There are defined automatically using `MPE_DEFINE_OPn` macros and can be referred to 
/// using `MPE_OPTAG(effect,opname)`.
typedef const struct mpe_optag_s {
  mpe_effect_t effect;
  long         opidx;
} *mpe_optag_t;


/// Operation functions are called when that operation is `yield`ed to. 
typedef void* (mpe_opfun_t)(mpe_resume_t* r, void* local, void* arg);


/// Operation kinds. 
/// When defining the operations that a handler can handle, 
/// these are specified to make the handling of operations more efficient. 
typedef enum mpe_opkind_e {
  MPE_OP_NULL,        ///< Invalid operation (used in static declarations to signal end of the operation array)
  MPE_OP_FORWARD,     ///< forwarding the operation, the `opfun` should be `NULL` in this case. 
  MPE_OP_ABORT,       ///< never resume -- and do not even run finalizers or destructors
  MPE_OP_NEVER,       ///< never resume -- and run finalizers and destructors before running the operation function
  MPE_OP_TAIL_NOOP,   ///< resume at most once without performing operations; and if resumed, it is the last action performed by the operation function.
  MPE_OP_TAIL,        ///< resume at most once; and if resumed, it is the last action performed by the operation function.
  MPE_OP_SCOPED_ONCE, ///< resume at most once within the scope of an operation function.
  MPE_OP_SCOPED,      ///< resume never or multiple times within the scope of an operation function.
  MPE_OP_ONCE,        ///< resume at most once.
  MPE_OP_MULTI        ///< resume never or multiple times.
} mpe_opkind_t;

/// Operation defintion.
/// An `operation` has a kind, an identifying tag, and an associated operation function.
typedef struct mpe_operation_s {
  mpe_opkind_t opkind;  ///< Kind of the operation
  mpe_optag_t  optag;   ///< The identifying tag
  mpe_opfun_t* opfun;   ///< The operation function; use `NULL` (with #MPE_OP_FORWARD) to have the operation forwarded to the next enclosing effect (i.e. a direct tail-resume with the same arguments).
} mpe_operation_t; 

/// Handler definition.
typedef struct mpe_handlerdef_s {
  mpe_effect_t      effect;         ///< The Effect being handled.
  mpe_acquirefun_t* local_acquire;  ///< Called when the local state needs to be acquired. Can be NULL.
  mpe_releasefun_t* local_release;  ///< Called when the local state is released. Can be NULL.
  mpe_resultfun_t*  resultfun;      ///< Invoked when the handled action is done; can be NULL in which case the action result is passed unchanged.
  mpe_operation_t   operations[8];  ///< Definitions of all handled operations ending with an operation with `lh_opkind` `LH_OP_NULL`. Can be NULL to handle no operations;
                                    ///< Note: all operations must be in the same order here as in the effect definition! (since each operation has a fixed index).
} mpe_handlerdef_t;



/*-----------------------------------------------------------------
  Handlers
-----------------------------------------------------------------*/

mpe_decl_export void* mpe_handle(const mpe_handlerdef_t* hdef, void* local, mpe_actionfun_t* body, void* arg);
mpe_decl_export void* mpe_perform(mpe_optag_t optag, void* arg);

mpe_decl_export void* mpe_resume(mpe_resume_t* resume, void* local, void* arg);
mpe_decl_export void* mpe_resume_final(mpe_resume_t* resume, void* local, void* arg);  // final resumption
mpe_decl_export void* mpe_resume_tail(mpe_resume_t* resume, void* local, void* arg);   // final resumption in tail position
mpe_decl_export void  mpe_resume_release(mpe_resume_t* resume);                        // final resumption causing unwinding (raise unwind exception on resume)


mpe_decl_export void* mpe_mask(mpe_effect_t eff, size_t from, mpe_actionfun_t* fun, void* arg);
mpe_decl_export void* mpe_finally(void* local, mpe_releasefun_t* finally_fun, mpe_actionfun_t* fun, void* arg);


/*-----------------------------------------------------------------
  Operation tags
-----------------------------------------------------------------*/

/// Null effect.
#define mpe_effect_null ((mpe_effect_t)NULL)

/// The _null_ operation tag is used for the final operation struct in a list of operations.
#define mpe_op_null  ((mpe_optag_t)NULL)

/// Get the name of an operation tag. 
mpe_decl_export const char* mpe_optag_name(mpe_optag_t optag);

/// Get the name of an effect tag. 
mpe_decl_export const char* mpe_effect_name(mpe_effect_t effect);

/// \}

/*-----------------------------------------------------------------
  Operation definition helpers
-----------------------------------------------------------------*/

/// \defgroup effect_def Effect Definition helpers
/// Macros to define effect handlers.
/// \{

#define MPE_EFFECT(effect)         mpe_names_effect_##effect
#define MPE_OPTAG_DEF(effect,op)   mpe_op_##effect##_##op
#define MPE_OPTAG(effect,op)       &MPE_OPTAG_DEF(effect,op)

#define MPE_DECLARE_EFFECT0(effect)  \
extern const char* MPE_EFFECT(effect)[2];

#define MPE_DECLARE_EFFECT1(effect,op1)  \
extern const char* MPE_EFFECT(effect)[3];

#define MPE_DECLARE_EFFECT2(effect,op1,op2)  \
extern const char* MPE_EFFECT(effect)[4];

#define MPE_DECLARE_OP(effect,op) \
extern const struct mpe_optag_s MPE_OPTAG_DEF(effect,op);

#define MPE_DECLARE_OP0(effect,op,restype) \
MPE_DECLARE_OP(effect,op) \
restype effect##_##op();

#define MPE_DECLARE_OP1(effect,op,restype,argtype) \
MPE_DECLARE_OP(effect,op) \
restype effect##_##op(argtype arg);

#define MPE_DECLARE_VOIDOP0(effect,op) \
MPE_DECLARE_OP(effect,op) \
void effect##_##op();

#define MPE_DECLARE_VOIDOP1(effect,op,argtype) \
MPE_DECLARE_OP(effect,op) \
void effect##_##op(argtype arg);


#define MPE_DEFINE_EFFECT0(effect) \
const char* MPE_EFFECT(effect)[2] = { #effect, NULL }; 

#define MPE_DEFINE_EFFECT1(effect,op1) \
const char* MPE_EFFECT(effect)[3] = { #effect, #effect "/" #op1, NULL }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op1) = { MPE_EFFECT(effect), 0 }; 

#define MPE_DEFINE_EFFECT2(effect,op1,op2) \
const char* MPE_EFFECT(effect)[4] = {  #effect, #effect "/" #op1, #effect "/" #op2, NULL }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op1) = { MPE_EFFECT(effect), 0 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op2) = { MPE_EFFECT(effect), 1 }; 

#define MPE_DEFINE_EFFECT3(effect,op1,op2,op3) \
const char* MPE_EFFECT(effect)[5] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, NULL }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op1) = { MPE_EFFECT(effect), 0 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op2) = { MPE_EFFECT(effect), 1 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op3) = { MPE_EFFECT(effect), 2 }; 

#define MPE_DEFINE_EFFECT4(effect,op1,op2,op3,op4) \
const char* MPE_EFFECT(effect)[6] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, NULL }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op1) = { MPE_EFFECT(effect), 0 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op2) = { MPE_EFFECT(effect), 1 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op3) = { MPE_EFFECT(effect), 2 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op4) = { MPE_EFFECT(effect), 3 }; 

#define MPE_DEFINE_EFFECT5(effect,op1,op2,op3,op4,op5) \
const char* MPE_EFFECT(effect)[7] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, #effect "/" #op5, NULL }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op1) = { MPE_EFFECT(effect), 0 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op2) = { MPE_EFFECT(effect), 1 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op3) = { MPE_EFFECT(effect), 2 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op4) = { MPE_EFFECT(effect), 3 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op5) = { MPE_EFFECT(effect), 4 }; 

#define MPE_DEFINE_EFFECT6(effect,op1,op2,op3,op4,op5,op6) \
const char* MPE_EFFECT(effect)[8] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, #effect "/" #op5, #effect "/" #op6, NULL }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op1) = { MPE_EFFECT(effect), 0 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op2) = { MPE_EFFECT(effect), 1 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op3) = { MPE_EFFECT(effect), 2 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op4) = { MPE_EFFECT(effect), 3 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op5) = { MPE_EFFECT(effect), 4 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op6) = { MPE_EFFECT(effect), 5 }; 

#define MPE_DEFINE_EFFECT7(effect,op1,op2,op3,op4,op5,op6,op7) \
const char* MPE_EFFECT(effect)[9] = {  #effect, #effect "/" #op1, #effect "/" #op2, #effect "/" #op3, #effect "/" #op4, #effect "/" #op5, #effect "/" #op6, #effect "/" #op7, NULL }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op1) = { MPE_EFFECT(effect), 0 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op2) = { MPE_EFFECT(effect), 1 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op3) = { MPE_EFFECT(effect), 2 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op4) = { MPE_EFFECT(effect), 3 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op5) = { MPE_EFFECT(effect), 4 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op6) = { MPE_EFFECT(effect), 5 }; \
const struct mpe_optag_s MPE_OPTAG_DEF(effect,op7) = { MPE_EFFECT(effect), 6 }; 


#define MPE_DEFINE_OP0(effect,op,restype) \
  restype effect##_##op() { void* res = mpe_perform(MPE_OPTAG(effect,op), NULL); return mpe_##restype##_voidp(res); }

#define MPE_DEFINE_OP1(effect,op,restype,argtype) \
  restype effect##_##op(argtype arg) { void* res = mpe_perform(MPE_OPTAG(effect,op), mpe_voidp_##argtype(arg)); return mpe_##restype##_voidp(res); }

#define MPE_DEFINE_VOIDOP0(effect,op) \
  void effect##_##op() { mpe_perform(MPE_OPTAG(effect,op), NULL); }

#define MPE_DEFINE_VOIDOP1(effect,op,argtype) \
  void effect##_##op(argtype arg) { mpe_perform(MPE_OPTAG(effect,op), mpe_voidp_##argtype(arg)); }

#define MPE_WRAP_FUN0(fun,restype) \
  void* wrap_##fun(void* arg) { (void)(arg); return mpe_voidp_##restype(fun()); }

#define MPE_WRAP_FUN1(fun,argtype,restype) \
  void* wrap_##fun(void* arg) { return mpe_voidp_##restype(fun(mpe_##argtype##_voidp(arg))); }

#define MPE_WRAP_VOIDFUN0(fun) \
  void* wrap_##fun(void* arg) { (void)(arg); fun(); return NULL; }

#define MPE_WRAP_VOIDFUN1(fun,argtype) \
  void* wrap_##fun(void* arg) { fun(mpe_##argtype##_voidp(arg)); return NULL; }


/*-----------------------------------------------------------------
	 Generic values
-----------------------------------------------------------------*/
typedef void* mpe_voidp_t;

/// Null value
#define mpe_voidp_null         (NULL)

/// Convert an #mpe_voidp_t back to a pointer.
#define mpe_ptr_voidp(v)       ((void*)(v))

/// Convert any pointer into a #mpe_voidp_t.
#define mpe_voidp_ptr(p)       ((void*)(p))


/// Identity; used to aid macros.
#define mpe_voidp_mpe_voidp_t(v)     (v)

/// Convert an #mpe_voidp_t back to an `int`.
#define mpe_int_voidp(v)       ((int)((intptr_t)(v)))

/// Convert an `int` to an #mpe_voidp_t.
#define mpe_voidp_int(i)       ((mpe_voidp_t)((intptr_t)(i)))

/// Convert an #mpe_voidp_t back to a `long`.
#define mpe_long_voidp(v)      ((long)((intptr_t)(v)))

/// Convert a `long` to an #mpe_voidp_t.
#define mpe_voidp_long(i)      ((mpe_voidp_t)((intptr_t)(i)))

/// Convert an #mpe_voidp_t back to a `uint64_t`.
#define mpe_uint64_t_voidp(v)      ((uint64_t)((intptr_t)(v)))

/// Convert a `uint64_t` to an #mpe_voidp_t.
#define mpe_voidp_uint64_t(i)      ((mpe_voidp_t)((intptr_t)(i)))

/// Convert an #mpe_voidp_t back to a `bool`.
#define mpe_bool_voidp(v)      (mpe_int_voidp(v) != 0 ? (1==1) : (1==0))

/// Convert a `bool` to an #mpe_voidp_t.
#define mpe_voidp_bool(b)      (mpe_voidp_int(b ? 1 : 0))

/// Type definition for strings to aid with macros.
typedef const char* mpe_string_t;

/// Convert an #mpe_voidp_t back to a `const char*`.
#define mpe_mpe_string_t_voidp(v) ((const char*)mpe_ptr_voidp(v))

/// Convert a `const char*` to an #mpe_voidp_t.
#define mpe_voidp_mpe_string_t(v) (mpe_voidp_ptr(v))

#define mpe_optag_voidp(v)     ((mpe_optag)mpe_ptr_voidp(v))
#define mpe_voidp_optag(o)     mpe_voidp_ptr(o)


#endif