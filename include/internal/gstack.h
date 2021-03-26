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
   In-place growable gstacks
------------------------------------------------------------------------------*/
typedef struct mp_gstack_s mp_gstack_t;
typedef struct mp_gsave_s  mp_gsave_t;

bool         mp_gstack_init(ssize_t gstack_size, ssize_t max_gpool_size); // normally called automatically
void         mp_gstack_clear_cache(void);   // clear thread-local cache of gstacks (called automatically on thread termination)

mp_gstack_t* mp_gstack_alloc(void); 
void         mp_gstack_free(mp_gstack_t* gstack);
void*        mp_gstack_reserve(mp_gstack_t* gstack, size_t size);
void         mp_gstack_enter(mp_gstack_t* g, mp_jmpbuf_t** return_jmp, mp_stack_start_fun_t* fun, void* arg);

mp_gsave_t*  mp_gstack_save(mp_gstack_t* gstack, uint8_t* sp);    // save up to the given stack pointer (that should be in `gstack`)
void         mp_gsave_restore(mp_gsave_t* gsave);
void         mp_gsave_free(mp_gsave_t* gsave);



#endif