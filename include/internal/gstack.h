/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once
#ifndef MP_GSTACK_H
#define MP_GSTACK_H

/*------------------------------------------------------------------------------
   Internal API for in-place growable gstacks
------------------------------------------------------------------------------*/
typedef struct mp_gstack_s mp_gstack_t;
typedef struct mp_gsave_s  mp_gsave_t;

bool         mp_gstack_init(const mp_config_t* config); // normally called automatically
void         mp_gstack_clear_cache(void);               // clear thread-local cache of gstacks (called automatically on thread termination)

mp_gstack_t* mp_gstack_alloc(ssize_t extra_size, void** extra); 
void         mp_gstack_free(mp_gstack_t* gstack, bool delay);
void         mp_gstack_enter(mp_gstack_t* g, mp_jmpbuf_t** return_jmp, mp_stack_start_fun_t* fun, void* arg);

mp_gsave_t*  mp_gstack_save(mp_gstack_t* gstack, uint8_t* sp);    // save up to the given stack pointer (that should be in `gstack`)
void         mp_gsave_restore(mp_gsave_t* gsave);
void         mp_gsave_free(mp_gsave_t* gsave);

mp_gstack_t* mp_gstack_current(void);             // implemented in <mprompt.c>



/*------------------------------------------------------------------------------
  Support address sanitizer
------------------------------------------------------------------------------*/

#if defined(__has_feature) 
#if __has_feature(address_sanitizer)
#define MP_USE_ASAN 1
#endif
#endif

#if !defined(MP_USE_ASAN)
#define MP_USE_ASAN 0
#endif

#if MP_USE_ASAN
void mp_debug_asan_start_switch(const mp_gstack_t* g);
void mp_debug_asan_end_switch(bool from_system);
#else
static inline void mp_debug_asan_start_switch(const mp_gstack_t* g) { MP_UNUSED(g); };
static inline void mp_debug_asan_end_switch(bool from_system)       { MP_UNUSED(from_system); };
#endif


#endif