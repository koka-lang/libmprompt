/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Included from "gstack.c".

  On systems without overcommit (like BSD's or Linux with overcommit disabled)
  or on Windows using exponential commit for pages on the stack,
  we need to use our own page fault handler to commit stack pages on-demand.
  To detect reliably whether a page fault occurred in one of our stacks and 
  also to limit expansion of stacks beyond their maximum size, we reserve
  large virtual memory areas, called a `gpool`, where the  `gstack`s are located.

  These are linked with each gpool containing about 32000 8MiB gstacks.
  This allows the page fault handler to quickly determine if a fault is in
  one our stacks. In between each stack is a gap and the first stack
  is used for the gpool info:

  |----------------------------------------------------------------------------------------|
  | mp_gpool_t .... |xxxx| stack 1  .... |xxxx| stack 2 .... |xxx| ...   | stack N ... |xxx|
  |----------------------------------------------------------------------------------------|

  The mp_gpool_t has a "free stack" itself (`free`) consisting of N (~32000) `int16_t`
  indices which are demand initialized to zero. The top of the stack starts
  at 0. Each entry at index `i` represents an available gstack at index `N - free[i] - i`:
  so the initial on-demand zero'd `free` stack makes all gstacks available in the pool :-)
  (The top index 0 is not used (reserved for the first block) so the for next index
   `i==1` we get the gstack index `N-1` and going down from there. We try to allocate
    top-down to improve back traces in debuggers that often stop if the parent frame
    has a lower address)
  From this free stack we can pop gstacks to use, or push back ones that are freed
  in a very efficient way. Moreover, reused gstacks do not need to be re-committed
  (and re-zero initialized by the OS).

  Since the gpool list is global we use a small spinlock for thread-safe
  allocation and free.
-----------------------------------------------------------------------------*/

// We need atomic operations for the `gpool` on systems that do not have overcommit.
#include "internal/atomic.h"

//----------------------------------------------------------------------------------
// gpool
//----------------------------------------------------------------------------------
#define MP_GPOOL_MAX_COUNT  (32000)         // at most INT16_MAX

typedef struct mp_gpool_s {
  struct mp_gpool_s* next;
  ssize_t  full_size;       // full mmap'd reserved size
  ssize_t  size;            // always: block_count * block_size
  ssize_t  block_count;
  ssize_t  block_size;
  ssize_t  gap_size;
  bool     zeroed;          // is the free area surely zero'd?
  // protected by a lock:
  mp_spin_lock_t free_lock;
  ssize_t  free_sp;
  int16_t  free[MP_GPOOL_MAX_COUNT];
} mp_gpool_t;


// Global list of gpools
static _Atomic(mp_gpool_t*)mp_gpools;

// Walk the gpools
static mp_gpool_t* mp_gpool_first(void) {
  return mp_atomic_load_ptr(mp_gpool_t, &mp_gpools);
}

static mp_gpool_t* mp_gpool_next(const mp_gpool_t* gp) {
  return (gp == NULL ? mp_gpool_first() : gp->next);
}

// Is a pointer located in a stack page and thus can be made accessible?
// This routine is called from the pagefault signal handler to verify if 
// the address is in one of our stacks and is allowed to be committed.
static mp_access_t mp_gpools_check_access(void* p, ssize_t* available, const mp_gpool_t** gpool) {
  // for all pools
  if (available != NULL) *available = 0;
  if (gpool != NULL) *gpool = NULL;
  for (const mp_gpool_t* gp = mp_gpool_first(); gp != NULL; gp = mp_gpool_next(gp)) {    
    ptrdiff_t ofs = (uint8_t*)p - (uint8_t*)gp;
    if (ofs >= 0 && ofs < gp->size) {   // in the pool?
      if (ofs <= (ptrdiff_t)sizeof(mp_gpool_t)) {
        // the start page
        if (available != NULL) *available = (sizeof(mp_gpool_t) - ofs);
        if (gpool != NULL) *gpool = gp;
        return MP_ACCESS_META;
      }
      else {
        ptrdiff_t block_ofs = ofs % gp->block_size;
        //mp_trace_message("  gp: %p, ofs: %zd, idx: %zd, bofs: %zd, b/g: %zd / %zd\n", gp, ofs, ofs / gp->block_size, block_ofs, gp->block_size, gp->gap_size);
        if (block_ofs < (gp->block_size - gp->gap_size)) {  // not in a gap?
          ssize_t avail = (os_stack_grows_down ? block_ofs : gp->block_size - gp->gap_size - block_ofs);
          if (available != NULL) *available = avail;
          if (gpool != NULL) *gpool = gp;
          return (avail == 0 ? MP_NOACCESS_STACK_OVERFLOW : MP_ACCESS);
        }
        else {
          // stack overflow
          return MP_NOACCESS_STACK_OVERFLOW;
        }
      }
    }
  }
  return MP_NOACCESS;
}

// Create a new pool in a given reserved virtual memory area.
static mp_gpool_t* mp_gpool_create(void* p, ssize_t size, ssize_t stack_size, ssize_t gap_size, bool zeroed) {
  // check parameters  
  mp_assert_internal(size >= stack_size + gap_size && p != NULL && (size_t)stack_size > sizeof(mp_gpool_t));
  stack_size = mp_align_up(stack_size, os_page_size);
  gap_size = mp_align_up(gap_size, os_page_size);
  ssize_t block_size = stack_size + gap_size;
  ssize_t count = size / block_size;
  mp_assert_internal(count > 1);
  if (count <= 0) return NULL;
  if (count > (os_gpool_max_size / block_size)) {
    count = (os_gpool_max_size / block_size);
  }
  if (count > MP_GPOOL_MAX_COUNT) {
    count = MP_GPOOL_MAX_COUNT;
  }
  // init
  if (!zeroed) {
    memset(p, 0, os_page_size); // zero out the initial page; the rest is done on-demand
  }
  mp_gpool_t* gp = (mp_gpool_t*)p;
  gp->zeroed = zeroed;
  gp->full_size = size;
  gp->size = count * block_size;
  gp->block_count = count;
  gp->block_size = block_size;
  gp->gap_size = gap_size;
  gp->free_sp = 1;  // first block is allocated to the gpool_t itself
  gp->free_lock = mp_spin_lock_create();
  // push atomically at the head of the pools
  gp->next = mp_atomic_load_ptr(mp_gpool_t, &mp_gpools);
  while (!mp_atomic_cas_ptr(mp_gpool_t, &mp_gpools, &gp->next, gp)) {};
  //mp_trace_message("gpool_create: %p, b1: %p, b2: %p\n", gp, (uint8_t*)gp + gp->block_size, (uint8_t*)gp + 2*gp->block_size);
  return gp;
}

// Allocate a fresh growable stack area from the pools
static uint8_t* mp_gpool_allocx(uint8_t** stk, ssize_t* stk_size) {
  // for all pools
  for (mp_gpool_t* gp = mp_gpool_first(); gp != NULL; gp = mp_gpool_next(gp)) {
    ssize_t block_idx = 0;
    ssize_t sp;
    volatile int16_t _access = 0;
    _access += gp->free[gp->free_sp + 64]; // ensure no page fault happens inside the spin lock
    mp_spin_lock(&gp->free_lock) {
      // pop from free stack
      sp = gp->free_sp;
      if (sp < gp->block_count) {
        gp->free_sp = sp + 1;
        block_idx = gp->free[sp] + sp;
      }
    }
    if (os_stack_grows_down) {
      block_idx = gp->block_count - block_idx; // grow from top
    }
    mp_assert_internal(block_idx >= 0 && block_idx < gp->block_count);
    if (block_idx > 0) {
      if (block_idx >= gp->block_count) return NULL; // paranoia
      uint8_t* p = ((uint8_t*)gp + (block_idx * gp->block_size));
      //mp_trace_message("gpool_alloc: gp: %p, p: %p, block_idx: %zd, sp: %zd\n", gp, p, block_idx, sp);
      //try to make the initial stack commit accessible (to avoid handling as grow signal)
      //uint8_t* top = (os_stack_grows_down ? p + (gp->block_size - gp->gap_size - os_gstack_initial_commit) : p); 
      //mp_mem_os_commit(top, os_gstack_initial_commit);  // ok if it fails
      *stk = p;
      *stk_size = gp->block_size - gp->gap_size;
      return p;
    }
  }
  return NULL;
}

// Allocate a fresh growable stack area from the pools
static uint8_t* mp_gpool_alloc(uint8_t** stk, ssize_t* stk_size) {
  uint8_t* p = mp_gpool_allocx(stk, stk_size);
  if (p != NULL) return p;

  // allocate a fresh gpool
  size_t poolsize = os_gpool_max_size;
  uint8_t* pool = mp_os_mem_reserve(poolsize);
  if (pool == NULL) return NULL;

  // commit on demand in the regular fault handler
  ssize_t init_size = mp_align_up(sizeof(mp_gpool_t), os_page_size);
  
  if (!mp_os_mem_commit(pool, init_size)) {   // make initial part read/write. 
    mp_os_mem_free(pool, poolsize);
    return NULL;
  }
    
  // make it available 
  mp_gpool_create(pool, poolsize, os_gstack_size - os_gstack_gap, os_gstack_gap, true);

  // and try to allocate again 
  return mp_gpool_allocx(stk, stk_size);
}


// Free a growable stack area back to the pools
static void mp_gpool_free(uint8_t* stk) {
  // for all pools
  for (mp_gpool_t* gp = mp_gpool_first(); gp != NULL; gp = mp_gpool_next(gp)) {
    ptrdiff_t ofs = (uint8_t*)stk - (uint8_t*)gp;
    if (ofs >= 0 && ofs < gp->size) {
      mp_assert(ofs % gp->block_size == 0);
      ptrdiff_t sp;
      ptrdiff_t block_idx = (ofs / gp->block_size);
      mp_assert(block_idx > 0); if (block_idx == 0) return;
      mp_assert(block_idx < gp->block_count); if (block_idx >= gp->block_count) return;
      ptrdiff_t idx;
      if (os_stack_grows_down) {
        idx = gp->block_count - block_idx; // reverse if growing down
      }
      else {
        idx = block_idx;
      }
      mp_spin_lock(&gp->free_lock) {
        // push on free stack
        gp->free_sp--;
        sp = gp->free_sp;
        //idx = gp->block_count - block_idx - sp;
        idx = idx - sp;        
        gp->free[sp] = (uint16_t)idx;
      }
      mp_assert(idx >= INT16_MIN && idx <= INT16_MAX);
      mp_assert(sp > 0);
      return; // done
    }
  }
}
