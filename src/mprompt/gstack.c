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

/*------------------------------------------------------------------------------
   Growable stacklets
------------------------------------------------------------------------------*/

// stack info; located just before the base of the stack
struct mp_gstack_s {
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
static ssize_t os_gstack_cache_size       = 4;             // thread local cache size
static ssize_t os_gpool_max_size          = 256 * MP_GIB;  // virtual size of one gstack pooled area (holds about 2^15 gstacks)


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
static mp_decl_thread mp_gstack_t** _mp_gstack_cache;

static ssize_t mp_gstack_initial_reserved(void) {
  return mp_align_up(sizeof(mp_gstack_t), 16);
}

// Allocate a growable stacklet.
mp_gstack_t* mp_gstack_alloc(void)
{
  mp_gstack_init(0,-1);  // always check initialization
  mp_assert(os_page_size != 0);
  mp_gstack_t* g = NULL;

  // first look in our thread local cache..
  mp_gstack_t** cache = _mp_gstack_cache;
  if (cache != NULL) {
    for (int i = 0; i < os_gstack_cache_size; i++) {
      g = cache[i];
      if (g != NULL) {
        cache[i] = NULL;
        //mp_trace_message("found in cache: %p\n", g);
        break;
      }
    }
  }

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
void mp_gstack_enter(mp_gstack_t* g, void (*fun)(void* arg), void* arg) {
  uint8_t* base = mp_gstack_base(g);
  uint8_t* base_commit_limit = mp_push(base, g->initial_commit, NULL);
  uint8_t* base_limit = mp_push(base, g->stack_size, NULL);
  uint8_t* base_entry_sp = mp_push(base, g->initial_reserved + 16 /* a bit extra */, NULL);
#if _WIN32
  if (os_use_gpools) {
    // set an artificially low stack limit so our page fault handler gets called and we 
    // can commit exponentially to improve performance.
    ULONG guaranteed = 0;
    SetThreadStackGuarantee(&guaranteed);
    base_limit = base_commit_limit - os_page_size - mp_align_up(guaranteed, os_page_size);
  }
#endif
  mp_stack_enter(base_entry_sp, base_commit_limit, base_limit, fun, arg);
}


// Free a gstack
void mp_gstack_free(mp_gstack_t* g) {
  if (g == NULL) return;
  mp_assert(os_page_size != 0);
  //mp_trace_message("free gstack: %p\n", p);  

  // first try to put it in our thread local cache...
  mp_gstack_t** cache = _mp_gstack_cache;
  if (cache != NULL) {
    for (int i = 0; i < os_gstack_cache_size; i++) {
      if (cache[i] == NULL) {
        // free slot, use it
        // If the cookie is valid, we assume the guard page was never hit and we don't need to do anything.
        // Otherwise we decommit potentially committed memory of a deep stack to reduce memory pressure.
        g->initial_reserved = mp_gstack_initial_reserved();  // reset reserved area
        if (!mp_gstack_has_valid_canary(g)) {
          // decommit
          if (!mp_gstack_os_reset(g->full, g->stack, g->stack_size)) continue;
        }
        cache[i] = g;
        return;
      }
    }
  }

  // otherwise free it to the OS
  mp_gstack_os_free(g->full);
}


// Clear all (thread local) cached gstacks.
void mp_gstack_clear_cache(void) {
  mp_gstack_t** cache = _mp_gstack_cache;
  if (cache != NULL) {
    for (int i = 0; i < os_gstack_cache_size; i++) {
      mp_gstack_t* gs = cache[i];
      cache[i] = NULL;
      if (gs != NULL) {
        mp_gstack_os_free(gs->full);
      }
    }
  }
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
bool mp_gstack_init(ssize_t gstack_size, ssize_t gpool_max_size) {
  if (os_page_size == 0) {
    // user settings
    if (gstack_size > 1) {
      os_gstack_size = mp_align_up(gstack_size, 4 * MP_KIB);
    }
    if (gpool_max_size == 0) {
      os_use_gpools = false;
    }
    else if (gpool_max_size >= 1) {
      os_use_gpools = true;
      if (gpool_max_size > 1) { os_gpool_max_size = mp_align_up(gpool_max_size, 4 * MP_KIB); }
    }
    // os specific initialization
    if (!mp_gstack_os_init()) return false;
    if (os_page_size == 0) os_page_size = 4 * MP_KIB;

    // ensure stack sizes are page aligned
    os_gstack_size = mp_align_up(os_gstack_size, os_page_size);
    os_gstack_initial_commit = (os_gstack_initial_commit == 0 ? os_page_size : mp_align_up(os_gstack_initial_commit, os_page_size));
    if (os_gstack_initial_commit > os_gstack_size) os_gstack_initial_commit = os_gstack_size;
    os_gpool_max_size = mp_align_up(os_gpool_max_size, os_page_size);

    // register exit routine
    atexit(&mp_gstack_done);
  }
  mp_gstack_thread_init();
  return true;
}



static void mp_gstack_thread_done(void) {
  mp_gstack_clear_cache();
  mp_free(_mp_gstack_cache);
  _mp_gstack_cache = NULL;  
}

static mp_decl_thread bool _mp_gstack_init;

static void mp_gstack_thread_init(void) {
  if (_mp_gstack_init) return;  // already initialized?
  _mp_gstack_init = true;
  mp_gstack_os_thread_init();
  
  // create cache
  if (os_gstack_cache_size > 0) {
    _mp_gstack_cache = (mp_gstack_t**)mp_zalloc((os_gstack_cache_size + 1) * sizeof(void*));
  }
}

