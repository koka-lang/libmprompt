/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MP_UTIL_H
#define MP_UTIL_H

/*------------------------------------------------------------------------------
  Includes and compiler specifics
------------------------------------------------------------------------------*/

#include <stddef.h>     // size_t
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdlib.h>     // malloc
#include <sys/types.h>  // ssize_t  

#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(__ssize_t_defined)
#define __ssize_t_defined
typedef ptrdiff_t  ssize_t;
#endif


#if defined(_MSC_VER)
#define mp_decl_noinline        __declspec(noinline)
#define mp_decl_thread          __declspec(thread)
#define mp_decl_noreturn        __declspec(noreturn)
#define mp_decl_returns_twice
#elif (defined(__GNUC__) && (__GNUC__>=3))  // includes clang and icc
#define mp_decl_noinline        __attribute__((noinline))
#define mp_decl_thread          __thread
#define mp_decl_noreturn        __attribute__((noreturn))
#define mp_decl_returns_twice   __attribute__((returns_twice))
#else
#define mp_decl_noinline
#define mp_decl_thread          __thread    // hope for the best :-)
#define mp_decl_noreturn        
#define mp_decl_returns_twice   
#endif


/*------------------------------------------------------------------------------
  Defines
------------------------------------------------------------------------------*/

#define MP_UNUSED(x)            ((void)(x))
#define MP_KIB                  (1024)
#define MP_MIB                  (MP_KIB*MP_KIB)
#define MP_GIB                  (1024LL*MP_MIB)

#define mp_assert(x)            assert(x)
#define mp_assert_internal(x)   mp_assert(x)


/*------------------------------------------------------------------------------
  Util
------------------------------------------------------------------------------*/

typedef void (mp_output_fun)(const char* msg, void* arg);
typedef void (mp_error_fun)(int err, void* arg);

void mp_trace_message(const char* fmt, ...);
void mp_system_error_message(int err, const char* fmt, ...);  // if err == EFAULT, abort
void mp_error_message(int err, const char* fmt, ...);         // if err == EFAULT, abort

mp_decl_noreturn void  mp_fatal_message(int err, const char* fmt, ...);  // prints message and aborts
mp_decl_noreturn void  mp_unreachable(const char* msg);

static inline ssize_t mp_align_up(ssize_t x, ssize_t d) {
  return (d == 0 ? x : ((x + d - 1) / d) * d);
}

static inline uint8_t* mp_align_up_ptr(uint8_t* p, ssize_t d) {
  return (uint8_t*)mp_align_up((ssize_t)p, d);
}

static inline uintptr_t mp_align_down(uintptr_t x, size_t d) {
  return (d == 0 ? x : (x / d) * d);
}

static inline uint8_t* mp_align_down_ptr(uint8_t* p, size_t d) {
  return (uint8_t*)mp_align_down((uintptr_t)p, d);
}


/*------------------------------------------------------------------------------
  Malloc interface (to facilitate replacing malloc)
------------------------------------------------------------------------------*/

#define mp_malloc_safe_tp(tp)  (tp*)mp_malloc_safe(sizeof(tp))
#define mp_zalloc_safe_tp(tp)  (tp*)mp_zalloc_safe(sizeof(tp))
#define mp_malloc_tp(tp)       (tp*)mp_malloc(sizeof(tp))
#define mp_zalloc_tp(tp)       (tp*)mp_zalloc(sizeof(tp))

// allocate zero initialized
static inline void* mp_zalloc(size_t size) {
  return calloc(1,size);
}

static inline void* mp_malloc(size_t size) {
  return malloc(size);
}

static inline void mp_free(void* p) {
  free(p);
}

static inline void* mp_malloc_safe(size_t size) {
  void* p = mp_malloc(size);
  if (p == NULL) {
    mp_fatal_message(ENOMEM, "out of memory\n");
  }
  return p;
}

static inline void* mp_zalloc_safe(size_t size) {
  void* p = mp_zalloc(size);
  if (p == NULL) {
    mp_fatal_message(ENOMEM, "out of memory\n");
  }
  return p;
}



#endif