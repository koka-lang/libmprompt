/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MP_ATOMIC_H
#define MP_ATOMIC_H

// --------------------------------------------------------------------------------------------
// Atomics
// Portability layer between C, C++, and MSVC.
// -----------------------------------------------------------------------------------------------

#if defined(__cplusplus)
// Use C++ atomics
#include <atomic>
#define  _Atomic(tp)            std::atomic<tp>
#define  mp_atomic(name)        std::atomic_##name
#define  mp_memory_order(name)  std::memory_order_##name
#elif defined(_MSC_VER)
// Use MSVC C wrapper for C11 atomics
#define  _Atomic(tp)            tp
#define  ATOMIC_VAR_INIT(x)     x
#define  mp_atomic(name)        mp_msvc_atomic_##name
#define  mp_memory_order(name)  mp_memory_order_##name
#else
// Use C11 atomics
#include <stdatomic.h>
#define  mp_atomic(name)        atomic_##name
#define  mp_memory_order(name)  memory_order_##name
#endif

#define mp_atomic_cas(p,expected,desired)        mp_atomic(compare_exchange_strong)(p,expected,desired)
#define mp_atomic_load(p)                        mp_atomic(load)(p)
#define mp_atomic_store(p,x)                     mp_atomic(store)(p,x)
#define mp_atomic_add(p,x)                       mp_atomic(fetch_add)(p,x)

static inline void mp_atomic_yield(void);


#if defined(__cplusplus) || !defined(_MSC_VER)

// In C++/C11 atomics we have polymorphic atomics so can use the typed `ptr` variants (where `tp` is the type of atomic value)
// We use these macros so we can provide a typed wrapper in MSVC in C compilation mode as well
#define mp_atomic_load_ptr(tp,p)                mp_atomic_load(p)

// In C++ we need to add casts to help resolve templates if NULL is passed
#if defined(__cplusplus)
#define mp_atomic_store_ptr(tp,p,x)             mp_atomic_store(p,(tp*)x)
#define mp_atomic_cas_ptr(tp,p,exp,des)         mp_atomic_cas(p,exp,(tp*)des)
#else
#define mp_atomic_store_ptr(tp,p,x)             mp_atomic_store(p,x)
#define mp_atomic_cas_ptr(tp,p,exp,des)         mp_atomic_cas(p,exp,des)
#endif

#else // defined(_MSC_VER)

// MSVC C compilation wrapper that uses Interlocked operations to model C11 atomics.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#ifdef _WIN64
typedef LONG64   msc_intptr_t;
#define MI_64(f) f##64
#else
typedef LONG     msc_intptr_t;
#define MI_64(f) f
#endif

static inline bool mp_msvc_atomic_compare_exchange_strong(_Atomic(intptr_t)*p, intptr_t* expected, intptr_t desired) {
  intptr_t read = (intptr_t)MI_64(_InterlockedCompareExchange)((volatile msc_intptr_t*)p, (msc_intptr_t)desired, (msc_intptr_t)(*expected));
  if (read == *expected) {
    return true;
  }
  else {
    *expected = read;
    return false;
  }
}

static inline intptr_t mp_msvc_atomic_exchange(_Atomic(intptr_t)*p, intptr_t exchange) {
  return (intptr_t)MI_64(_InterlockedExchange)((volatile msc_intptr_t*)p, (msc_intptr_t)exchange);
}

static inline intptr_t mp_msvc_atomic_load(_Atomic(intptr_t) const* p) {
  #if defined(_M_IX86) || defined(_M_X64)
    return *p;
  #else
    uintptr_t x = *p;
    while (!mp_msvc_atomic_compare_exchange_strong(p, &x, x)) { /* nothing */ };
    return x;
  #endif
}

static inline void mp_msvc_atomic_store(_Atomic(intptr_t)*p, intptr_t x) {
  #if defined(_M_IX86) || defined(_M_X64)
    *p = x;
  #else
    mi_atomic_exchange(p, x);
  #endif
}

static inline intptr_t mp_msvc_atomic_add(_Atomic(intptr_t)*p, intptr_t x) {
  return (intptr_t)MI_64(InterlockedAdd)((volatile msc_intptr_t*)p, (msc_intptr_t)x);
}

// ptr variants
#define mp_atomic_load_ptr(tp,p)                (tp*)mp_atomic_load((_Atomic(uintptr_t)*)(p))
#define mp_atomic_store_ptr(tp,p,x)             mp_atomic_store((_Atomic(uintptr_t)*)(p),(uintptr_t)x)
#define mp_atomic_cas_ptr(tp,p,exp,des)         mp_atomic_cas((_Atomic(uintptr_t)*)(p),(uintptr_t*)exp,(uintptr_t)des)

#endif

// ---------------------------------------------------------------
// Yield 
// ---------------------------------------------------------------
#if defined(__cplusplus)
#include <thread>
static inline void mp_atomic_yield(void) {
  std::this_thread::yield();
}
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static inline void mp_atomic_yield(void) {
  YieldProcessor();
}
#elif defined(__SSE2__)
#include <emmintrin.h>
static inline void mp_atomic_yield(void) {
  _mm_pause();
}
#elif (defined(__GNUC__) || defined(__clang__)) && \
      (defined(__x86_64__) || defined(__i386__) || \
       defined(__aarch64__) || \
       defined(__arm__) || defined(__armel__) || defined(__ARMEL__) || \
       defined(__powerpc__) || defined(__ppc__) || defined(__PPC__))
  #if defined(__x86_64__) || defined(__i386__)
  static inline void mp_atomic_yield(void) {
    __asm__ volatile ("pause" ::: "memory");
  }
  #elif defined(__aarch64__)
  static inline void mp_atomic_yield(void) {
    __asm__ volatile("wfe");
  }
  #elif (defined(__arm__) && __ARM_ARCH__ >= 7)
  static inline void mp_atomic_yield(void) {
    __asm__ volatile("yield" ::: "memory");
  }
  #elif defined(__powerpc__) || defined(__ppc__) || defined(__PPC__)
  static inline void mp_atomic_yield(void) {
    __asm__ __volatile__ ("or 27,27,27" ::: "memory");
  }
  #elif defined(__armel__) || defined(__ARMEL__)
  static inline void mp_atomic_yield(void) {
    __asm__ volatile ("nop" ::: "memory");
  }
  #endif
#elif defined(__sun)
// Fallback for other archs
#include <synch.h>
static inline void mp_atomic_yield(void) {
  smt_pause();
}
#elif defined(__wasi__)
#include <sched.h>
static inline void mp_atomic_yield(void) {
  sched_yield();
}
#else
#include <unistd.h>
static inline void mp_atomic_yield(void) {
  sleep(0);
}
#endif


// ---------------------------------------------------------------
// Spin lock for very small critical sections
// ---------------------------------------------------------------
typedef _Atomic(intptr_t) mp_spin_lock_t;

#define mp_spin_lock_create()  (0)

static inline void mp_spin_lock_acquire(mp_spin_lock_t* l) {
  intptr_t expected = 0;
  while (!mp_atomic_cas(l, &expected, 1)) { 
    expected = 0;
    mp_atomic_yield(); 
  }
}

static inline void mp_spin_lock_release(mp_spin_lock_t* l) {
  mp_atomic_store(l, 0);
}

#define mp_spin_lock(l)  \
  for( bool _once = (mp_spin_lock_acquire(l), true); \
       _once; \
       _once = (mp_spin_lock_release(l), false))



#endif // header
