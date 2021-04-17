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

// Stack info. 
// For security we allocate this separately from the actual stack.
// To save an allocation, we reserve `extra_size` space where the 
// `mp_prompt_t` information will be.
// All sizes (except for `extra_size`) are `os_page_size` aligned.
struct mp_gstack_s {
  mp_gstack_t*  next;               // used for the cache and delay list
  uint8_t*      full;               // stack reserved memory (including noaccess gaps)
  ssize_t       full_size;          // (for now always fixed to be `os_gstack_size`)
  uint8_t*      stack;              // stack inside the full area (without gaps)
  ssize_t       stack_size;         // actual available total stack size (includes reserved space) (depends on platform, but usually `os_gstack_size - 2*mp_gstack_gap`)
  ssize_t       initial_commit;     // initial committed memory (usually `os_page_size`)  
  ssize_t       committed;          // current committed estimate
  ssize_t       extra_size;         // size of extra allocated bytes.         
  uint8_t       extra[1];           // extra allocated (holds the mp_prompt_t structure)
};



//----------------------------------------------------------------------------------
// Configuration
//----------------------------------------------------------------------------------

// Static configuration; should be set once at startup.
// Todo: make this easier to change by reading environment variables?
static bool    os_use_gpools              = true;          // reuse gstacks in-process
static bool    os_use_overcommit          = false;         // commit on demand by relying on overcommit? (only if available)
static bool    os_stack_grows_down        = true;          // on almost all systems
static ssize_t os_page_size               = 0;             // initialized at startup

static ssize_t os_gstack_initial_commit   = 0;             // initial commit size (initialized to be at least `os_page_size`)
static ssize_t os_gstack_size             = 8 * MP_MIB;    // reserved memory for a stack (including the gaps)
static ssize_t os_gstack_gap              = 64 * MP_KIB;   // noaccess gap between stacks; `os_gstack_gap > min(64*1024, os_page_size, os_gstack_size/2`.
static bool    os_gstack_reset_decommits  = false;         // force full decommit when resetting a stack?
static bool    os_gstack_grow_fast        = true;          // use doubling to grow gstacks (up to 1MiB)
static ssize_t os_gstack_cache_max_count  = 4;             // number of prompts to keep in the thread local cache
static ssize_t os_gstack_exn_guaranteed   = 32 * MP_KIB;   // guaranteed stack size available during an exception unwind (only used on Windows)

#if defined(_MSC_VER) && !defined(NDEBUG)  // gpool a tad smaller in msvc so debug traces work (as the gpool can be placed lower than the system stack)
static ssize_t os_gpool_max_size          = 16 * MP_GIB;   // virtual size of one gstack pooled area (holds about 2^15 gstacks)
#else
static ssize_t os_gpool_max_size          = 256 * MP_GIB;  // virtual size of one gstack pooled area (holds about 2^15 gstacks)
#endif

// Find base of an area in the stack (we use "base" as the logical bottom of the stack).
static uint8_t* mp_base(uint8_t* sp, ssize_t size) {
  return (os_stack_grows_down ? sp + size : sp);
}

// Adjust pointer taking stack direction into account
static uint8_t* mp_push(uint8_t* sp, ssize_t size, uint8_t** start) {
  uint8_t* p = (os_stack_grows_down ? sp - size : sp + size);
  if (start != NULL) *start = (os_stack_grows_down ? p : sp);
  return p;
}

// Return how far a pointer is into a stack taking stack direction into account
static ssize_t mp_unpush(const uint8_t* sp, const uint8_t* stk, ssize_t stk_size) {
  return (os_stack_grows_down ? ((stk + stk_size) - sp) : (sp - stk));
}


//----------------------------------------------------------------------------------
// Platform specific, low-level OS interface.
//
// By design always reserve `os_gstack_size` memory with `os_gstack_initial_commit`
// initially committed. By making this constant, we can implement efficient caching,
// "gpools", commit-on-demand handlers etc.
//----------------------------------------------------------------------------------
static uint8_t* mp_gstack_os_alloc(uint8_t** stack, ssize_t* stack_size, ssize_t* initial_commit);
static void     mp_gstack_os_free(uint8_t* full, uint8_t* stack, ssize_t stack_size, ssize_t stk_commit);
static bool     mp_gstack_os_init(void);
static void     mp_gstack_os_thread_init(void);
static void     mp_gstack_thread_done(void);  // called by hook installed in os specific include

// Used by the gpool implementation
static uint8_t* mp_os_mem_reserve(ssize_t size);
static void     mp_os_mem_free(uint8_t* p, ssize_t size);
static bool     mp_os_mem_commit(uint8_t* start, ssize_t size);

// Used by signal handler to check access
typedef enum mp_access_e {
  MP_NOACCESS,                    // no access (outside pool)
  MP_NOACCESS_STACK_OVERFLOW,     // no access due to stack overflow (in gap)
  MP_ACCESS,                      // access in a gstack
  MP_ACCESS_META                  // access in initial meta data (the `free` stack)
} mp_access_t;

static mp_access_t  mp_gstack_check_access(mp_gstack_t* g, void* address, ssize_t* stack_size, ssize_t* available, ssize_t* commit_available);

// The gpool interface
typedef struct mp_gpool_s mp_gpool_t;
static uint8_t*     mp_gpool_alloc(uint8_t** stk, ssize_t* stk_size);
static void         mp_gpool_free(uint8_t* stk);
static mp_access_t  mp_gpools_check_access(void* address, ssize_t* available, ssize_t* stack_size, const mp_gpool_t** gp);


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


// Is a pointer in the gstack?
static bool mp_gstack_contains(const mp_gstack_t* g, const uint8_t* p) {
  return (p >= g->stack && p < (g->stack + g->stack_size));
}

// Offset from base of the stack
static uint8_t* mp_gstack_base_at(const mp_gstack_t* g, ssize_t ofs) {
  return (os_stack_grows_down ? g->stack + g->stack_size - ofs : g->stack + ofs);
}

// Base of the stack
static uint8_t* mp_gstack_base(const mp_gstack_t* g) {
  return mp_gstack_base_at(g, 0);
}



//----------------------------------------------------------------------------------
// Interface
//----------------------------------------------------------------------------------


// We have a small cache per thread of stacks to avoid going to the OS too often.
static mp_decl_thread mp_gstack_t* _mp_gstack_cache;
static mp_decl_thread ssize_t      _mp_gstack_cache_count;


// We also have a delayed free list to keep gstacks alive during exception unwinding
// (since some exception implementations allocate exception information in stack areas that are already unwound)
// it is cleared when either: 1. another gstack is allocated, 2. clear_cache is called, 3. the thread terminates
static mp_decl_thread mp_gstack_t* _mp_gstack_delayed_free;

static void mp_gstack_clear_delayed(void) {
  if (_mp_gstack_delayed_free == NULL) return;
  #ifdef __cplusplus
  if (std::uncaught_exception()) {
    return; // don't clear while exception unwinding is active
  }
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
mp_gstack_t* mp_gstack_alloc(ssize_t extra_size, void** extra)
{
  if (extra != NULL) { *extra = NULL;  }
  mp_gstack_init(NULL);  // always check initialization
  mp_assert(os_page_size != 0);
  mp_gstack_clear_delayed();  // this might free some gstacks to our local cache
  
  // first look in our thread local cache..
  #if !defined(NDEBUG)
  void* sp = (void*)&sp;
  #endif
  mp_gstack_t* g = _mp_gstack_cache;  
  mp_gstack_t* prev = NULL;
  while (g != NULL) {
    bool good = (g->extra_size >= extra_size);
    #if !defined(NDEBUG)
    // only use a cached stack if it is under the parent stack (to help unwinding during debugging)
    void* stack = g->stack;
    good = good && (os_stack_grows_down ? stack < sp : sp < stack);
    #endif  
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

  // otherwise allocate fresh
  if (g == NULL) {
    // allocate separately for security
    extra_size = mp_align_up(extra_size, sizeof(void*));    
    g = (mp_gstack_t*)mp_malloc(sizeof(mp_gstack_t) - 1 + extra_size); 
    if (g == NULL) {
      return NULL;
    }

    // allocate the actual stack
    uint8_t* stk;
    ssize_t  stk_size;
    ssize_t  initial_commit;
    uint8_t* full = mp_gstack_os_alloc(&stk, &stk_size, &initial_commit);
    if (full == NULL) { 
      mp_free(g);
      errno = ENOMEM;
      return NULL;
    }    
    
    uint8_t* base = mp_base(stk, stk_size);
    mp_assert_internal((intptr_t)base % 32 == 0);

    // initialize with debug 0xFD
    #ifndef NDEBUG
    uint8_t* commit_start;
    mp_push(base, initial_commit, &commit_start);
    memset(commit_start, 0xFD, initial_commit);
    #endif
    
    //mp_trace_message("alloc gstack: full: %p, base: %p, base_limit: %p\n", full, base, mp_push(base, stk_size,NULL));
    g->next = NULL;
    g->full = full;
    g->full_size = os_gstack_size;
    g->stack = stk;
    g->stack_size = stk_size;
    g->initial_commit = g->committed = initial_commit;
    g->extra_size = extra_size;
  }

  if (extra != NULL && extra_size > 0) {
    *extra = &g->extra[0];
  }
  return g;
}


// Enter a gstack
void mp_gstack_enter(mp_gstack_t* g, mp_jmpbuf_t** return_jmp, mp_stack_start_fun_t* fun, void* arg) {
  uint8_t* base = mp_gstack_base(g);
  uint8_t* base_commit_limit = mp_push(base, g->committed, NULL);
  uint8_t* base_limit = mp_push(base, g->stack_size, NULL);
  uint8_t* base_entry_sp = base;
#if _WIN32
  if (os_use_gpools || os_gstack_grow_fast) {
    // set an artificially low stack limit so our page fault handler gets called and we can:
    // - prevent guard pages from growing into a gap in gpools
    // - keep track of the committed area and grow by doubling to improve performance.
    ULONG guaranteed = 0;
    SetThreadStackGuarantee(&guaranteed);
    ssize_t guard_size = os_page_size + mp_align_up(guaranteed, os_page_size);
    base_limit = mp_push(base_commit_limit, guard_size, NULL);
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
  if (_mp_gstack_cache_count < os_gstack_cache_max_count) {
    // allowed to cache.
    // we keep it as-is    
    g->next = _mp_gstack_cache;
    _mp_gstack_cache = g;
    _mp_gstack_cache_count++;
    return;
  }

  // otherwise free it to the OS
  mp_gstack_os_free(g->full, g->stack, g->stack_size, g->committed);
  mp_free(g);
}


// Clear all (thread local) cached gstacks.
void mp_gstack_clear_cache(void) {
  mp_gstack_clear_delayed();
  mp_gstack_t* g = _mp_gstack_cache;
  while( g != NULL) {
    mp_gstack_t* next = _mp_gstack_cache = g->next;
    _mp_gstack_cache_count--;
    mp_gstack_os_free(g->full, g->stack, g->stack_size, g->committed);
    mp_free(g);
    g = next;
  }
  mp_assert_internal(_mp_gstack_cache == NULL);
  mp_assert_internal(_mp_gstack_cache_count == 0);
}


//----------------------------------------------------------------------------------
// Saving / Restoring
//----------------------------------------------------------------------------------

struct mp_gsave_s {
  void*   stack;
  ssize_t stack_size;
  void*   extra;        // mp_prompt_t structure
  ssize_t extra_size;
  uint8_t data[1];      // combined data; starts with extra
};

// save a gstack
#if MP_USE_ASAN
__attribute__((no_sanitize("address")))
#endif
mp_gsave_t* mp_gstack_save(mp_gstack_t* g, uint8_t* sp) {
  mp_assert_internal(mp_gstack_contains(g, sp));
  ssize_t stack_size = mp_unpush(sp, g->stack, g->stack_size);
  mp_assert_internal(stack_size >= 0 && stack_size <= g->stack_size);
  mp_gsave_t* gs = (mp_gsave_t*)mp_malloc_safe(sizeof(mp_gsave_t) - 1 + stack_size + g->extra_size);
  gs->stack = (os_stack_grows_down ? sp : g->stack);
  gs->stack_size = stack_size;
  gs->extra = &g->extra[0];
  gs->extra_size = g->extra_size;
  #if MP_USE_ASAN
    for(ssize_t i = 0; i < gs->extra_size; i++) { gs->data[i] = ((uint8_t*)gs->extra)[i]; }
    for(ssize_t i = 0; i < gs->stack_size; i++) { gs->data[i + gs->extra_size] = ((uint8_t*)gs->stack)[i]; }
  #else
    memcpy(gs->data, gs->extra, gs->extra_size);
    memcpy(gs->data + gs->extra_size, gs->stack, gs->stack_size);
  #endif
  return gs;
}

void mp_gsave_restore(mp_gsave_t* gs) {
  memcpy(gs->extra, gs->data, gs->extra_size);
  memcpy(gs->stack, gs->data + gs->extra_size, gs->stack_size);
}

void mp_gsave_free(mp_gsave_t* gs) {
  mp_free(gs);
}


//----------------------------------------------------------------------------------
// Is an address located in a gstack?
//----------------------------------------------------------------------------------

static mp_access_t mp_gstack_check_access(mp_gstack_t* g, void* address, ssize_t* stack_size, ssize_t* available, ssize_t* commit_available) {
  // todo: guard against buffer overflow changing the gstack fields
  // (using a canary or by allocating the gstack meta data separately)
  if (stack_size != NULL) { *stack_size = 0; }
  if (available != NULL)  { *available = 0; }
  if (commit_available != NULL) { *commit_available = 0; }
  if (g == NULL) return MP_NOACCESS;
  if (stack_size != NULL) { *stack_size = g->stack_size; }

  const uint8_t* p = (uint8_t*)address;
  if (mp_gstack_contains(g, p)) {
    const ssize_t used = mp_unpush(p, g->stack, g->stack_size);
    mp_assert_internal(used <= g->stack_size);
    if (available != NULL) { *available = g->stack_size - used; }
    if (commit_available != NULL) { *commit_available = mp_max(0, g->committed - used); }
    return MP_ACCESS;
  }
  else if (p >= g->full && p < g->stack) {
    // in the gap
    return MP_NOACCESS_STACK_OVERFLOW;
  }
  else {
    return MP_NOACCESS;
  }  
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
bool mp_gstack_init(const mp_config_t* config) {
  if (os_page_size == 0) 
  {
    // user settings
    if (config != NULL) {      
      os_gstack_reset_decommits = config->stack_reset_decommits;
      os_use_overcommit = config->stack_use_overcommit;      
      if (os_use_overcommit) {
        os_use_gpools = false;
        os_gstack_grow_fast = false;
      }
      else {
        os_use_gpools = config->gpool_enable;
        os_gstack_grow_fast = config->stack_grow_fast;
      }
      if (config->gpool_max_size > 0) {
        os_gpool_max_size = mp_align_up(config->gpool_max_size, 64 * MP_KIB);
      }
      if (config->stack_max_size > 0) {
        os_gstack_size = mp_align_up(config->stack_max_size, 4 * MP_KIB);
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
      if (config->stack_cache_count >= 0) {
        os_gstack_cache_max_count = config->stack_cache_count;
      }
      else if (config->stack_cache_count < 0) {
        os_gstack_cache_max_count = 0;
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
 
  return true;
}

mp_config_t mp_config_default(void) {
  mp_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  #ifdef _WIN32
  cfg.gpool_enable = false;
  cfg.stack_grow_fast = false;
  #else
  cfg.gpool_enable = true;
  cfg.stack_grow_fast = true;
  #endif
  cfg.stack_use_overcommit = false;
  cfg.stack_reset_decommits = false;
  cfg.gpool_max_size = os_gpool_max_size;
  cfg.stack_max_size = os_gstack_size;
  cfg.stack_initial_commit = os_gstack_initial_commit;
  cfg.stack_exn_guaranteed = os_gstack_exn_guaranteed;
  cfg.stack_cache_count = os_gstack_cache_max_count;
  cfg.stack_gap_size = os_gstack_gap;
  return cfg;
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


//----------------------------------------------------------------------------------
// Support address sanitizer
//----------------------------------------------------------------------------------

#if MP_USE_ASAN
// sanitize hooks, see: <https://github.com/llvm-mirror/compiler-rt/blob/master/include/sanitizer/common_interface_defs.h>
mp_decl_externc  void __sanitizer_start_switch_fiber(void** fake_stack_save, const void* bottom, size_t size);
mp_decl_externc  void __sanitizer_finish_switch_fiber(void* fake_stack_save, const void** bottom_old, size_t* size_old);

static mp_decl_thread const void* system_stack;
static mp_decl_thread size_t system_stack_size;

void mp_debug_asan_start_switch(const mp_gstack_t* g) {
  if (g == NULL) {
    // system stack
    __sanitizer_start_switch_fiber(NULL, system_stack, system_stack_size);
  }
  else {
    __sanitizer_start_switch_fiber(NULL, g->stack, g->stack_size);
  }
}

void mp_debug_asan_end_switch(bool from_system) {
  const void* old;
  size_t old_size;
  __sanitizer_finish_switch_fiber(NULL, &old, &old_size);
  if (from_system) {
    system_stack = old;
    system_stack_size = old_size;
  }
}

#endif