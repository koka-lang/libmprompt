/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MP_LONGJMP_H
#define MP_LONGJMP_H

#include <stdint.h>

#if defined(__cplusplus)
#define mp_decl_externc     extern "C"
#else
#define mp_decl_externc
#endif


/*------------------------------------------------------------------------------
   mp_setjmp, mp_longjmp, mp_stack_enter

   We need a _fast_ and _plain_ version where `setjmp` just saves the register
   context, and `longjmp` just restores the register context and jumps to the saved
   location. Some platforms or libraries try to do more though, like trying
   to unwind the stack on a longjmp to invoke finalizers or saving and restoring signal
   masks. In those cases we try to substitute our own definitions.
------------------------------------------------------------------------------*/

typedef void (mp_stack_start_fun_t)(void* arg, void* trap_frame);

// A register context. Always has `reg_ip` and `reg_sp` members.
typedef struct mp_jmpbuf_s mp_jmpbuf_t;

mp_decl_externc mp_decl_returns_twice  void* mp_setjmp(mp_jmpbuf_t* save_jmp);
mp_decl_externc mp_decl_noreturn       void  mp_longjmp(mp_jmpbuf_t* jmp);
mp_decl_externc                        void* mp_stack_enter(void* stack_base, void* stack_commit_limit, void* stack_limit, 
                                                            mp_jmpbuf_t** return_jmp,
                                                            mp_stack_start_fun_t* fun, void* arg);


#if defined(_WIN32) && defined(_M_X64)  // windows 64-bit

typedef struct mp_xmm_s {
  uint64_t lo;
  uint64_t hi;
} mp_xmm_t;

#define MP_JMPBUF_SIZE 280
struct mp_jmpbuf_s {
  void*     reg_ip;            
  void*     reg_sp;
  int64_t   reg_rbx;
  int64_t   reg_rbp;
  int64_t   reg_rsi;
  int64_t   reg_rdi;
  int64_t   reg_r12;
  int64_t   reg_r13;  
  int64_t   reg_r14;
  int64_t   reg_r15;
  mp_xmm_t  reg_xmm6;
  mp_xmm_t  reg_xmm7;
  mp_xmm_t  reg_xmm8;
  mp_xmm_t  reg_xmm9;
  mp_xmm_t  reg_xmm10;
  mp_xmm_t  reg_xmm11;
  mp_xmm_t  reg_xmm12;
  mp_xmm_t  reg_xmm13;
  mp_xmm_t  reg_xmm14;
  mp_xmm_t  reg_xmm15;
  void*     tib_stack_base;
  void*     tib_stack_limit;
  void*     tib_stack_dealloc;
  void*     tib_fiber_data;
  uint32_t  reg_mxcrs;
  uint16_t  reg_fpcr;
  uint16_t  context_padding;
};

#elif defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64)

# define MP_JMPBUF_SIZE 72
struct mp_jmpbuf_s {
  void*     reg_ip;            
  int64_t   reg_rbx;
  void*     reg_sp;
  int64_t   reg_rbp;
  int64_t   reg_r12;
  int64_t   reg_r13;  
  int64_t   reg_r14;
  int64_t   reg_r15;
  uint32_t  reg_mxcrs;
  uint16_t  reg_fpcr;
  uint16_t  context_padding;
};

#elif defined(_M_ARM64) || defined(__aarch64__)
# define MP_JMPBUF_SIZE 192

struct mp_jmpbuf_s {
  int64_t   reg_x18;
  int64_t   reg_x19;
  int64_t   reg_x20;
  int64_t   reg_x21;
  int64_t   reg_x22;
  int64_t   reg_x23;
  int64_t   reg_x24;
  int64_t   reg_x25;
  int64_t   reg_x26;
  int64_t   reg_x27;
  int64_t   reg_x28;
  void*     reg_fp;
  void*     reg_ip;            
  void*     reg_sp;
  int64_t   reg_fpcr;
  int64_t   reg_fpsr;
  int64_t   reg_d8;  
  int64_t   reg_d9;
  int64_t   reg_d10; 
  int64_t   reg_d11;
  int64_t   reg_d12;
  int64_t   reg_d13;
  int64_t   reg_d14; 
  int64_t   reg_d15;
};

#else
#error "unsupported platform"
#endif

#endif