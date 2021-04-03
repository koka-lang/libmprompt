/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Implementation of "growable" stacklets.
  Each `gstack` allocates `os_gstack_size` (8MiB) virtual memory
  but allocates on-demand while the stack grows. Uses an OS page 
  committed memory at minimum (and 2 on Windows)
-----------------------------------------------------------------------------*/

#include <string.h>
#include <errno.h>
#include "mprompt.h"
#include "internal/util.h"
#include "internal/longjmp.h"       // mp_stack_enter
#include "internal/gstack.h"

#ifdef __cplusplus
#include <exception>
#endif

/*------------------------------------------------------------------------------
   Growable stacklets
------------------------------------------------------------------------------*/

// stack info; located just before the base of the stack
struct mp_gstack_s {
  mp_gstack_t* next;                // used for the cache and delay list
  uint8_t*    full;                 // stack reserved memory (including noaccess gaps)
  ssize_t     full_size;            // (for now always fixed to be `os_gstack_size`)
  uint8_t*    stack;                // stack inside the full area (without gaps)
  ssize_t     stack_size;           // actual available total stack size (depends on platform, but usually `os_gstack_size - 2*mp_gstack_gap`)
  ssize_t     initial_commit;       // initial committed memory (usually `os_page_size`)
  ssize_t     initial_reserved;     // initial reserved memory on the stack (for the `gstack_t` and `prompt_t` structures)  
};



//----------------------------------------------------------------------------------
// Configuration
//----------------------------------------------------------------------------------

// Static configuration; should be set once at startup.
// Todo: make this easier to change by reading environment variables?
static bool    os_use_gpools              = true;          // seems always faster
static bool    os_stack_grows_down        = true;          // on almost all systems
static ssize_t os_page_size               = 0;             // initialized at startup

static ssize_t os_gstack_initial_commit   = 0;             // initial commit size (initialized to be at least `os_page_size`)
static ssize_t os_gstack_size             = 8 * MP_MIB;    // reserved memory for a stack (including the gaps)
static ssize_t os_gstack_gap              = 64 * MP_KIB;   // noaccess gap between stacks; `os_gstack_gap > min(64*1024, os_page_size, os_gstack_size/2`.
static bool    os_gstack_reset_decommits  = false;         // force full decommit when resetting a stack?
static ssize_t os_gstack_cache_count      = 4;             // number of prompts to keep in the thread local cache
static ssize_t os_gstack_exn_guaranteed   = 32 * MP_KIB;   // guaranteed stack size available during an exception unwind (only used on Windows)

#if defined(_MSC_VER) && !defined(NDEBUG)  // gpool a tad smaller in msvc so debug traces work (as the gpool can be lower than the system stack)
static ssize_t os_gpool_max_size          = 16 * MP_GIB;   // virtual size of one gstack pooled area (holds about 2^15 gstacks)
#else
static ssize_t os_gpool_max_size          = 256 * MP_GIB;  // virtual size of one gstack pooled area (holds about 2^15 gstacks)
#endif

// Find base of an area in the stack
static uint8_t* mp_base(uint8_t* sp, ssize_t size) {
  return (os_stack_grows_down ? sp + size : sp);
}

// Adjust pointer taking stack direction into account
static uint8_t* mp_push(uint8_t* sp, ssize_t size, uint8_t** start) {
  uint8_t* p = (os_stack_grows_down ? sp - size : sp + size);
  if (start != NULL) *start = (os_stack_grows_down ? p : sp);
  return p;
}


//----------------------------------------------------------------------------------
// Platform specific, low-level OS interface.
//
// By design always reserve `os_gstack_size` memory with `os_gstack_initial_commit`
// initially committed. By making this constant, we can implement efficient caching,
// "gpools", commit-on-demand handlers etc.
//----------------------------------------------------------------------------------
static uint8_t* mp_gstack_os_alloc(uint8_t** stack, ssize_t* stack_size);
static void     mp_gstack_os_free(uint8_t* full);
static bool     mp_gstack_os_reset(uint8_t* full, uint8_t* stack, ssize_t stack_size);  // keep stack but mark memory as uncommitted to the OS
static bool     mp_gstack_os_init(void);
static void     mp_gstack_os_thread_init(void);

// Used by the gpool implementation
static uint8_t* mp_os_mem_reserve(ssize_t size);
static void     mp_os_mem_free(uint8_t* p, ssize_t size);
static bool     mp_os_mem_commit(uint8_t* start, ssize_t size);

// gpool interface
typedef struct  mp_gpool_s mp_gpool_t;
static int      mp_gpools_is_accessible(void* p, ssize_t* available, const mp_gpool_t** gp);
static uint8_t* mp_gpool_alloc(uint8_t** stk, ssize_t* stk_size);
static void     mp_gpool_free(uint8_t* stk);

// called by hook installed in os specific include
static void     mp_gstack_thread_done(void);  


// platform specific definitions are in included files
#if defined(_WIN32)
#include "gstack_gpool.c"
#include "gstack_win.c"
#elif defined(__unix__) || defined(__unix) || defined(unix) || (defined(__APPLE__) && defined(__MACH__)) || defined(__HAIKU__)
#include "gstack_gpool.c"
#include "gstack_mmap.c"
#else
#error "unsupported platform: add specific definitions"
#endif



//----------------------------------------------------------------------------------
// Util
//----------------------------------------------------------------------------------

#ifndef NDEBUG
// Is a pointer in the gstack?
static bool mp_gstack_contains(mp_gstack_t* g, uint8_t* p) {
  return (p >= g->stack && p < (g->stack + g->stack_size));
}
#endif

// Offset from base of the stack
static uint8_t* mp_gstack_base_at(mp_gstack_t* g, ssize_t ofs) {
  return (os_stack_grows_down ? g->stack + g->stack_size - ofs : g->stack + ofs);
}

// Base of the stack
static uint8_t* mp_gstack_base(mp_gstack_t* g) {
  return mp_gstack_base_at(g, 0);
}


// Magic value to avoid decommitting memory in most cases
#define MP_GSTACK_CANARY        (0x67706C6F746B696E)

// The canary is at a fixed distance from the stack bottom to quickly
// detect if the stack may have grown larger than that. This can avoid doing an explicit memory reset.
static uint64_t* mp_gstack_get_canary_ptr(mp_gstack_t* g) {
  return (uint64_t*)mp_gstack_base_at(g, g->initial_commit - sizeof(uint64_t));
}

static void mp_gstack_set_canary(mp_gstack_t* g) {
  uint64_t* p = mp_gstack_get_canary_ptr(g);
  // mp_trace_message("set canary: gs: %p, canary: %p\n", gs, p);
  *p = MP_GSTACK_CANARY;
}


static bool mp_gstack_has_valid_canary(mp_gstack_t* g) {
  return (*mp_gstack_get_canary_ptr(g) == MP_GSTACK_CANARY);
}



//----------------------------------------------------------------------------------
// Interface
//----------------------------------------------------------------------------------


// We have a small cache per thread of stacks to avoid going to the OS too often.
static mp_decl_thread mp_gstack_t* _mp_gstack_cache;
static mp_decl_thread ssize_t      _mp_gstack_cache_count;


static ssize_t mp_gstack_initial_reserved(void) {
  return mp_align_up(sizeof(mp_gstack_t), 16);
}


// We have a delayed free list to keep gstacks alive during exception unwinding
// it is cleared when: 1. another gstack is allocated, 2. clear_cache is called, 3. the thread terminates
static mp_decl_thread mp_gstack_t* _mp_gstack_delayed_free;

static void mp_gstack_clear_delayed(void) {
  if (_mp_gstack_delayed_free == NULL) return;
  #ifdef __cplusplus
  if (std::uncaught_exception()) return; // don't clear while exceptions are active
  #endif
  mp_gstack_t* g = _mp_gstack_delayed_free;
  while (g != NULL) {
    mp_gstack_t* next = _mp_gstack_delayed_free = g->next;
    mp_gstack_free(g, false);  // maybe move to cache
    g = next;
  }
  mp_assert_internal(_mp_gstack_delayed_free == NULL);
}

// Allocate a growable stacklet.
mp_gstack_t* mp_gstack_alloc(void)
{
  mp_gstack_init(NULL);  // always check initialization
  mp_assert(os_page_size != 0);
  mp_gstack_clear_delayed();  // this might free some gstacks to our local cache
  
  // first look in our thread local cache..
  mp_gstack_t* g = _mp_gstack_cache;
  #if defined(NDEBUG)
  // pick the head if available
  if (g != NULL) {
    _mp_gstack_cache = g->next;
    _mp_gstack_cache_count--;
    g->next = NULL;
  }
  #else
  // only use a cached stack if it is under the parent stack (to help unwinding during debugging)
  void* sp = (void*)&g;
  mp_gstack_t* prev = NULL;
  while (g != NULL) {
    bool good = (os_stack_grows_down ? (void*)g < sp : sp < (void*)g);
    if (good) {
      if (prev == NULL) { _mp_gstack_cache = g->next; }
                   else { prev->next = g->next; }
      _mp_gstack_cache_count--;
      g->next = NULL;
      break;
    }
    else {
      prev = g;
      g = g->next;
    }
  }
  #endif

  // otherwise allocate fresh
  if (g == NULL) {
    uint8_t* stk;
    ssize_t  stk_size;
    uint8_t* full = mp_gstack_os_alloc(&stk, &stk_size);
    if (full == NULL) { 
      errno = ENOMEM;
      return NULL;
    }
    
    // reserve space at the base for the gstack structure itself.
    uint8_t* base = mp_base(stk, stk_size);
    ssize_t initial_reserved = mp_gstack_initial_reserved();
    mp_push(base, initial_reserved, (uint8_t**)&g); //

    // initialize with debug 0xFD
    #ifndef NDEBUG
    uint8_t* commit_start;
    mp_push(base, os_gstack_initial_commit, &commit_start);
    memset(commit_start, 0xFD, os_gstack_initial_commit);
    #endif
    //mp_trace_message("alloc gstack: full: %p, base: %p, base_limit: %p\n", full, base, mp_push(base, stk_size,NULL));
    g->next = NULL;
    g->full = full;
    g->full_size = os_gstack_size;
    g->stack = stk;
    g->stack_size = stk_size;
    g->initial_commit = os_gstack_initial_commit;
    g->initial_reserved = initial_reserved;
    mp_gstack_set_canary(g);    
  }

  return g;
}


// Pre-reserve space on the stack before entry
void* mp_gstack_reserve(mp_gstack_t* g, size_t sz) {
  uint8_t* p;
  sz = mp_align_up(sz, 16);
  mp_push(mp_gstack_base_at(g, g->initial_reserved), sz, &p);
  g->initial_reserved += sz;
  mp_assert(g->initial_reserved < g->initial_commit);
  return p;
}


// Enter a gstack
void mp_gstack_enter(mp_gstack_t* g, mp_jmpbuf_t** return_jmp, mp_stack_start_fun_t* fun, void* arg) {
  uint8_t* base = mp_gstack_base(g);
  uint8_t* base_commit_limit = mp_push(base, g->initial_commit, NULL);
  uint8_t* base_limit = mp_push(base, g->stack_size, NULL);
  uint8_t* base_entry_sp = mp_push(base, g->initial_reserved + 16 /* a bit extra */, NULL);
#if _WIN32
  if (os_use_gpools) {
    // set an artificially low stack limit so our page fault handler gets called and we 
    // can commit quadratically to improve performance.
    ULONG guaranteed = 0;
    SetThreadStackGuarantee(&guaranteed);
    base_limit = base_commit_limit - os_page_size - mp_align_up(guaranteed, os_page_size);
  }
#endif
  mp_stack_enter(base_entry_sp, base_commit_limit, base_limit, return_jmp, fun, arg);  
}


// Free a gstack
void mp_gstack_free(mp_gstack_t* g, bool delay) {
  if (g == NULL) return;
  mp_assert(os_page_size != 0);
  //mp_trace_message("free gstack: %p\n", p);  

  // if delayed, always push it on the delayed list
  if (delay) {
    g->next = _mp_gstack_delayed_free;
    _mp_gstack_delayed_free = g;
    return;
  }

  // otherwise try to put it in our thread local cache...
  if (_mp_gstack_cache_count < os_gstack_cache_count) {
    // allowed to cache.
    // If the cookie is valid, we assume the guard page was never hit and we don't need to do anything.
    // Otherwise we decommit potentially committed memory of a deep stack to reduce memory pressure.
    g->initial_reserved = mp_gstack_initial_reserved();  // reset reserved area
    if (!mp_gstack_has_valid_canary(g)) {
      // decommit
      if (!mp_gstack_os_reset(g->full, g->stack, g->stack_size)) {
        goto mp_free;
      }
    }
    g->next = _mp_gstack_cache;
    _mp_gstack_cache = g;
    _mp_gstack_cache_count++;
    return;
  }

mp_free:
  // otherwise free it to the OS
  mp_gstack_os_free(g->full);
}


// Clear all (thread local) cached gstacks.
void mp_gstack_clear_cache(void) {
  mp_gstack_clear_delayed();
  mp_gstack_t* g = _mp_gstack_cache;
  while( g != NULL) {
    mp_gstack_t* next = _mp_gstack_cache = g->next;
    _mp_gstack_cache_count--;
    mp_gstack_os_free(g->full);
    g = next;
  }
  mp_assert_internal(_mp_gstack_cache == NULL);
  mp_assert_internal(_mp_gstack_cache_count == 0);
}


//----------------------------------------------------------------------------------
// Saving / Restoring
//----------------------------------------------------------------------------------

struct mp_gsave_s {
  void* stack;
  size_t  size;
  uint8_t data[1];
};

// save a gstack
mp_gsave_t* mp_gstack_save(mp_gstack_t* g, uint8_t* sp) {
  mp_assert_internal(mp_gstack_contains(g, sp));
  uint8_t* base = mp_gstack_base(g);
  ssize_t size = (os_stack_grows_down ? mp_gstack_base(g) - sp : sp - g->stack);
  uint8_t* stack = (os_stack_grows_down ? sp : base);
  mp_gsave_t* gs = (mp_gsave_t*)mp_malloc_safe(sizeof(mp_gsave_t) - 1 + size);
  gs->stack = stack;
  gs->size = size;
  memcpy(gs->data, stack, size);
  return gs;
}

void mp_gsave_restore(mp_gsave_t* gs) {
  memcpy(gs->stack, gs->data, gs->size);
}

void mp_gsave_free(mp_gsave_t* gs) {
  mp_free(gs);
}


//----------------------------------------------------------------------------------
// Initialization
//----------------------------------------------------------------------------------



// Done (called automatically)
static void mp_gstack_done(void) {  
  mp_gstack_thread_done();
}

static void mp_gstack_thread_init(void);  // called from `mp_gstack_init`


// Init (called by mp_prompt_init and gstack_alloc)
bool mp_gstack_init(mp_config_t* config) {
  if (os_page_size == 0) 
  {
    // user settings
    if (config != NULL) {
      if (config->stack_max_size > 0) {
        os_gstack_size = mp_align_up(config->stack_max_size, 4 * MP_KIB);
      }
      os_use_gpools = config->gpool_enable;
      if (config->gpool_max_size > 0) {
        os_gpool_max_size = mp_align_up(config->gpool_max_size, 64 * MP_KIB);
      }
      if (config->stack_exn_guaranteed > 0) {
        os_gstack_exn_guaranteed = mp_align_up(config->stack_exn_guaranteed, 4 * MP_KIB);
      }
      if (config->stack_initial_commit > 0) {
        os_gstack_initial_commit = mp_align_up(config->stack_initial_commit, 4 * MP_KIB);
      }
      if (config->stack_gap_size > 0) {
        os_gstack_gap = mp_align_up(config->stack_gap_size, 4 * MP_KIB);
      }
      if (config->stack_cache_count > 0) {
        os_gstack_cache_count = config->stack_cache_count;
      }
      else if (config->stack_cache_count < 0) {
        os_gstack_cache_count = 0;
      }
    }

    // os specific initialization
    if (!mp_gstack_os_init()) return false;
    if (os_page_size == 0) os_page_size = 4 * MP_KIB;

    // ensure stack sizes are page aligned
    os_gstack_size = mp_align_up(os_gstack_size, os_page_size);
    os_gstack_exn_guaranteed = mp_align_up(os_gstack_exn_guaranteed, os_page_size);
    os_gstack_gap = mp_align_up(os_gstack_gap, os_page_size);
    os_gpool_max_size = mp_align_up(os_gpool_max_size, os_page_size);
    os_gstack_initial_commit = (os_gstack_initial_commit == 0 ? os_page_size : mp_align_up(os_gstack_initial_commit, os_page_size));
    if (os_gstack_initial_commit > os_gstack_size) os_gstack_initial_commit = os_gstack_size;

    // register exit routine
    atexit(&mp_gstack_done);
  }
  
  // Thread specific initialization
  mp_gstack_thread_init();

  // Return actual settings
  if (config != NULL) {
    config->gpool_enable = os_use_gpools;
    config->gpool_max_size = os_gpool_max_size;
    config->stack_max_size = os_gstack_size;
    config->stack_gap_size = os_gstack_gap;
    config->stack_exn_guaranteed = os_gstack_exn_guaranteed;
    config->stack_initial_commit = os_gstack_initial_commit;
    config->stack_cache_count = os_gstack_cache_count;
  }
  return true;
}



static void mp_gstack_thread_done(void) {
  mp_gstack_clear_cache();  // also does mp_gstack_clear_delayed
}

static mp_decl_thread bool _mp_gstack_init;

static void mp_gstack_thread_init(void) {
  if (_mp_gstack_init) return;  // already initialized?
  _mp_gstack_init = true;
  mp_gstack_os_thread_init();  
}

