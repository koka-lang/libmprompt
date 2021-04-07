/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Included from `gstack.c`

  Windows specific allocation of gstacks. 
-----------------------------------------------------------------------------*/
#include <windows.h>
#include <fibersapi.h>           // needed for thread termination <https://devblogs.microsoft.com/oldnewthing/20191011-00/?p=102989>
#include "internal/atomic.h"

// -----------------------------------------------------
// Interface
// -----------------------------------------------------

static uint8_t* mp_win_get_stack_extent(ssize_t* commit_available, ssize_t* available, ssize_t* stack_size, uint8_t** base);
static bool     mp_win_initial_commit(uint8_t* stk, ssize_t stk_size, ssize_t* initial_commit, bool commit_initial);
static void     mp_win_trace_stack_layout(uint8_t* base, uint8_t* xbase_limit);

static const char* mp_system_error_message(int errno, const char* fmt, ...);

// Reserve memory
// we use a hint address in windows to try to stay under the system stack for better backtraces
static _Atomic(ssize_t) mp_os_reserve_hint;

static uint8_t* mp_os_mem_reserve(ssize_t size) {
  uint8_t* p = NULL;
  ssize_t rsize = mp_align_up(size, 64 * MP_KIB);  
  ssize_t hint  = mp_atomic_load(&mp_os_reserve_hint);
  // initialize
  if (hint == 0) {
    hint = (ssize_t)&hint - 64 * MP_MIB;  // place lower as the system stack (and use ASLR from the stack)
    hint = mp_align_down(hint, 64 * MP_KIB);
    ssize_t expected = 0;
    if (!mp_atomic_cas(&mp_os_reserve_hint, &expected, hint)) { 
      hint = expected;
    }
  }
  // try placing under the system stack (for better backtraces)
  if (hint > rsize && hint > MP_GIB) {  
    ssize_t hint = mp_atomic_add(&mp_os_reserve_hint, -rsize) - rsize;      // race is ok, it is just a hint
    if (hint > 0) {
      p = (uint8_t*)VirtualAlloc((void*)hint, size, MEM_RESERVE, PAGE_NOACCESS);
    }
  }
  // otherwise allocation determined by OS
  if (p == NULL) {
    p = (uint8_t*)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
  }
  if (p == NULL) {
    mp_system_error_message(ENOMEM, "failed to reserve memory of size %zd\n", size);
  }
  return p;
}

// Free reserved memory
static void  mp_os_mem_free(uint8_t* p, ssize_t size) {
  MP_UNUSED(size);
  if (p == NULL) return;
  if (!VirtualFree(p, 0, MEM_RELEASE)) {
    mp_system_error_message(ENOMEM, "failed to free memory as %p of size %zd\n", p, size);
  }
}

// Commit a range of pages
static bool mp_os_mem_commit(uint8_t* start, ssize_t size) {
  if (VirtualAlloc(start, size, MEM_COMMIT, PAGE_READWRITE) == NULL) {   
    mp_system_error_message(ENOMEM, "failed to commit memory at %p of size %zd\n", start, size);
    return false;
  }
  return true;
}


// Allocate a gstack
static uint8_t* mp_gstack_os_alloc(uint8_t** stk, ssize_t* stk_size, ssize_t* initial_commit) {
  if (!os_use_gpools) {
    // reserve virtual full stack
    uint8_t* full = mp_os_mem_reserve(os_gstack_size);
    if (full == NULL) return NULL;

    *stk = full + os_gstack_gap;
    *stk_size = os_gstack_size - 2 * os_gstack_gap;
    // and initialize the guard page and initial commit
    if (!mp_win_initial_commit(*stk, *stk_size, initial_commit, true)) {
      mp_os_mem_free(full, os_gstack_size);
      return NULL;
    }
    //mp_trace_stack_layout(full + os_gstack_size - os_gstack_gap, full + os_gstack_gap);
    return full;
  }
  else {
    // Use gpool allocation
    uint8_t* full = mp_gpool_alloc(stk, stk_size);
    if (full == NULL) return NULL;
    
    // and initialize the guard page and initial commit
    if (!mp_win_initial_commit(*stk, *stk_size, initial_commit, true)) {
      mp_gpool_free(full);
      return NULL;
    }
    return full;
  }
}

// Set initial committed page in a gstack and a guard page to grow on-demand
static bool mp_win_initial_commit(uint8_t* stk, ssize_t stk_size, ssize_t* initial_commit, bool commit_initial) {
  if (initial_commit != NULL) *initial_commit = 0;
  if (stk == NULL) return false;
  uint8_t* base = mp_base(stk, stk_size);
  uint8_t* commit_start;
  uint8_t* commit_base = mp_push(base, os_gstack_initial_commit, &commit_start);
  if (commit_initial && os_gstack_initial_commit > 0) {
    // commit initial pages    
    if (!mp_os_mem_commit(commit_start, os_gstack_initial_commit)) {
      return false;
    }
    if (initial_commit != NULL) *initial_commit = os_gstack_initial_commit;
  }  
  // Set a guard page to grow on demand; this is handled by the OS since it cannot call a user fault handler as
  // the stack just ran out. It will raise a stack-overflow once the end of the reserved space is reached.
  // the actual number of guard pages is determined by the Windows thread stack guarantee.
  ULONG guaranteed = 0;
  SetThreadStackGuarantee(&guaranteed);
  ssize_t guard_size = os_page_size + mp_align_up(guaranteed, os_page_size);
  uint8_t* guard_start;
  mp_push(commit_base, guard_size, &guard_start);
  if (VirtualAlloc(guard_start, guard_size, MEM_COMMIT, PAGE_GUARD | PAGE_READWRITE) == NULL) {
    mp_system_error_message(ENOMEM, "failed to set guard page at %p of size %zd\n", guard_start, guard_size);
    return false;
  }       
  //mp_trace_message("initial commit at %p\n", commit_start);
  //mp_win_trace_stack_layout(base, stk);
  return true;
}

// Free the memory of a gstack
static void mp_gstack_os_free(uint8_t* full, uint8_t* stk, ssize_t stk_size, ssize_t stk_commit) {
  if (full == NULL) return;
  if (!os_use_gpools) {
    mp_os_mem_free(full, os_gstack_size);
  }
  else {
    stk_size   = mp_align_up(stk_size, os_page_size);
    stk_commit = mp_align_down(stk_commit, os_page_size);
    // decommit entire range      
    // Note: we cannot reset partly as a new allocation sets up an initial guard page
    //  and inside C++ exception handling routines the `__chkstk` may fail if these are not in a contiguous virtual area.
    //  For now, this means there is not much advantage to using gpools on Windows. A way to improve this 
    //  would be to also track if an block in a gpool is being reused without needing to set up a fresh guard page.
    #pragma warning(suppress:6250) // warning: MEM_DECOMMIT does not free the memory
    if (VirtualFree(stk, stk_size, MEM_DECOMMIT) == NULL) {
      mp_system_error_message(EINVAL, "failed to decommit memory at %p of size %zd\n", stk, stk_size);
    };    
    //mp_trace_message("deallocated gstack:\n");
    //mp_win_trace_stack_layout(mp_base(stk, stk_size), stk);
    mp_gpool_free(full);
  }
}


// -----------------------------------------------------
// Initialization
// -----------------------------------------------------

// use thread local storage keys to detect thread ending
static DWORD mp_win_fls_key;

static void NTAPI mp_win_thread_done(PVOID value) {
  mp_gstack_thread_done();
}

static void mp_win_process_done(void) {
  if (mp_win_fls_key != 0) {
    FlsFree(mp_win_fls_key);
  }
}

static void mp_gstack_os_thread_init(void) {
  FlsSetValue(mp_win_fls_key, (PVOID)(1)); // any value other than 0 triggers cleanup call
}


static LONG WINAPI mp_gstack_win_page_fault(PEXCEPTION_POINTERS ep);

static uint8_t* mp_win_main_stack_base;

static bool mp_gstack_os_init(void) {
  // page size
  SYSTEM_INFO sys_info;
  GetSystemInfo(&sys_info);
  os_page_size = sys_info.dwPageSize;

  // remember the system stack
  mp_win_get_stack_extent(NULL, NULL, NULL, &mp_win_main_stack_base);

  // set up thread termination routine
  mp_win_fls_key = FlsAlloc(&mp_win_thread_done);
  atexit(&mp_win_process_done);

  // install a page fault handler to grow gstack's on demand and prevent
  // guard pages to grow into the gaps in a gpool (as that is one contiguous reserved space)  
  // Moreover, we always need this handler to guarantee enough stack space 
  // during C++ exception handling: it seems the system will only grow the system
  // stack automatically during an exception and now our gstacks.
  PVOID handler = AddVectoredExceptionHandler(1, &mp_gstack_win_page_fault);
  if (handler == NULL) {
    mp_system_error_message(EINVAL, "unable to install page fault handler -- fall back to guarded demand paging\n");
    os_use_gpools = false;        // fall back to regular demand paging 
    os_gstack_grow_fast = false;
  }
  
  return true;
}



// -----------------------------------------------------
// Page fault handler
// Only used if gpool's are used 
// -----------------------------------------------------

#include <memoryapi.h>
#include <winternl.h>

// Extended TIB stucture from _NT_TIB
typedef struct MP_TIB_S {
  struct _EXCEPTION_REGISTRATION_RECORD* ExceptionList;
  uint8_t* StackBase;                // bottom of the stack (highest address)
  uint8_t* StackLimit;               // commit limit (points to the top of the guard page)
  PVOID    SubSystemTib;
  PVOID    FiberData;
  PVOID    ArbitraryUserPointer;
  struct _NT_TIB* Self;
  PVOID    padding1[(0x1478 - 7*sizeof(PVOID))/sizeof(PVOID)];
  uint8_t* StackRealLimit;           // "Deallocation limit", the actual reserved size
  PVOID    padding2[(0x1748 - sizeof(PVOID) - 0x1478 - 7*sizeof(PVOID))/sizeof(PVOID)];
  size_t   StackGuaranteed;          // Guaranteed available stack during an exception
} MP_TIB;


// Current TIB
static MP_TIB* mp_win_tib(void) {
  return (MP_TIB*)NtCurrentTeb();
}

// Get the current stack pointer
static mp_decl_noinline uint8_t* mp_win_addr(volatile uint8_t* p) {
  return (uint8_t*)p;
}

static mp_decl_noinline uint8_t* mp_win_current_sp(void) {
  volatile uint8_t b;
  return mp_win_addr(&b);
}

// Get the limits of the current stack of this thread
static uint8_t* mp_win_tib_get_stack_extent(const MP_TIB* tib, ssize_t* commit_available, ssize_t* available, ssize_t* stack_size, uint8_t** base) {
  uint8_t* sp = mp_win_current_sp();
  mp_assert_internal(os_stack_grows_down);
  bool instack = (sp > tib->StackRealLimit && sp <= tib->StackBase);
  if (commit_available != NULL) *commit_available = (instack ? sp - tib->StackLimit : 0);
  if (available != NULL) *available = (instack ? sp - tib->StackRealLimit : 0);
  if (stack_size != NULL) *stack_size = tib->StackBase - tib->StackRealLimit;
  if (base != NULL) *base = tib->StackBase;
  return (instack ? sp : NULL);
}

static uint8_t* mp_win_get_stack_extent(ssize_t* commit_available, ssize_t* available, ssize_t* stack_size, uint8_t** base) {
  MP_TIB* tib = mp_win_tib();
  return mp_win_tib_get_stack_extent(tib, commit_available, available, stack_size, base);
}


// C++ exceptions are identified by this exception code on MSVC
#define MP_CPP_EXN 0xE06D7363  // "msc"

// Guard page fault handler: generally not required as Windows already grows stacks with a guard
// page automatically; we use it to: 
// 1. grow the stack quadratically for performance 
// 2. prevent gpool gstack from growing into gaps (since a gpool is a contiguous reserved area)
// We do this by artificially limiting the `StackRealLimit` in the TIB which triggers the fault handler. 
//
// But we also always need to avoid errors during RtlUnwindEx. It seems that during a C++ exeption
// only guard pages in the system stack are grown automatically but not guards in gstacks.
// (This might be because we are already handling exceptions so the OS cannot fault again).
// We work around this by guaranteeing `os_gstack_exn_guaranteed` stack upfront as soon as an 
// exception is being thrown and thus avoiding faults during the actual unwinding.
static LONG WINAPI mp_gstack_win_page_fault(PEXCEPTION_POINTERS ep) 
{
  const DWORD exncode = ep->ExceptionRecord->ExceptionCode;
  if (exncode != MP_CPP_EXN && exncode != STATUS_STACK_OVERFLOW && exncode != STATUS_ACCESS_VIOLATION) { // && exncode != STATUS_GUARD_PAGE_VIOLATION
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // find the page start
  MP_TIB* tib = mp_win_tib();
  uint8_t* const addr = (exncode != MP_CPP_EXN ? (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1] : tib->StackLimit - 8);
  uint8_t* const page = mp_align_down_ptr(addr, os_page_size);
    
  // determine precisely if the address is in an accessible part of the current gstack
  ssize_t available = 0;
  ssize_t commit_available = 0;
  ssize_t stack_size = 0;
  mp_gstack_t* g = mp_gstack_current();
  mp_access_t res = mp_gstack_check_access(g, page, &stack_size, &available, &commit_available);

  // for C++ exceptions we only need to grow if there is actually not enough committed stack left
  if (exncode == MP_CPP_EXN && commit_available >= os_gstack_exn_guaranteed) {
    //mp_win_trace_stack_layout(NULL, NULL);
    return EXCEPTION_CONTINUE_SEARCH;
  }

  if (res == MP_ACCESS) {
    // pointer in our gstack, make the page read-write
    // (or, for c++ exception we need to guarantee stack size before the exception is being unwound).    
    ULONG    guaranteed = 0;
    SetThreadStackGuarantee(&guaranteed);
    const ssize_t  guard_size = os_page_size + mp_align_up(guaranteed, os_page_size);

    // reserve always one page + `extra`.
    ssize_t extra = (exncode != MP_CPP_EXN ? 0 : os_gstack_exn_guaranteed - os_page_size);
    ssize_t used = stack_size - available;
    mp_assert_internal(used >= 0);
    if (os_gstack_grow_fast && exncode != MP_CPP_EXN && used > 0) {
      extra = 2 * used;                   // doubling.. 
    }
    if (extra > 1 * MP_MIB) {
      extra = 1 * MP_MIB;                 // up to 1MiB growh
    }
    if (extra > available - guard_size) {
      extra = available - guard_size;     // up to stack limit 
    }
    extra = mp_align_down(extra, os_page_size);
    if (extra >= 0) {
      uint8_t* extend; 
      mp_push(page, extra, &extend);
      ssize_t commit_size = extra + os_page_size;
      if (VirtualAlloc(extend, commit_size, MEM_COMMIT, PAGE_READWRITE) != NULL) {
        uint8_t* gpage;
        mp_push(extend, guard_size, &gpage);
        if (VirtualAlloc(gpage, guard_size, MEM_COMMIT, PAGE_GUARD | PAGE_READWRITE) != NULL) {
          tib->StackLimit = extend;
          tib->StackRealLimit = gpage; 
          if (g != NULL) { g->committed = mp_unpush(extend, g->stack, g->stack_size); }
          //mp_trace_message("expanded stack: extra: %zdk, available: %zdk, stack_size: %zdk, used: %zdk\n", extra/1024, available/1024, g->stack_size/1024, used/1024);
          //mp_win_trace_stack_layout(tib->StackBase, tib->StackBase - g->stack_size);
          return (exncode!=MP_CPP_EXN ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH);
        }
      }
    }
  }
  
  return EXCEPTION_CONTINUE_SEARCH;  
}


// -----------------------------------------------------
// Debug tests
// -----------------------------------------------------

static void mp_win_trace_stack_layout(uint8_t* base, uint8_t* xbase_limit) {
  uint8_t* base_glimit = NULL;
  ssize_t  guaranteed = 0;
  if (base == NULL) {
    MP_TIB* tib = mp_win_tib();
    base = tib->StackBase;
    xbase_limit = tib->StackRealLimit;
    base_glimit = tib->StackLimit;
    guaranteed = tib->StackGuaranteed;
  }
  else {
    base = (uint8_t*)mp_align_up((intptr_t)base, os_page_size);
  }
  mp_trace_message("-- stack, rsp: %p ---------------------------------\n", mp_win_current_sp());
  uint8_t* base_limit = (uint8_t*)mp_align_up((intptr_t)xbase_limit, os_page_size);
  uint8_t* full = base_limit - os_gstack_gap;
  uint8_t* end = base + os_gstack_gap;
  mp_trace_message("full : %p, end : %p\n", full, end);
  mp_trace_message("limit: %p, base: %p\n", xbase_limit, base);
  if (base_glimit != NULL) { mp_trace_message("guard: %p, guaranteed: %zd\n", base_glimit, guaranteed); }
  for (uint8_t* p = full; p < end; ) {
    MEMORY_BASIC_INFORMATION info;
    VirtualQuery(p, &info, sizeof(info));
    mp_trace_message("%p, size: %4zik, protect: 0x%04X, state: 0x%04X\n",
      p, info.RegionSize / 1024,
      info.Protect,
      info.State);
    p += info.RegionSize;
  };
  mp_trace_message("---------------------------------------------------\n");
}

/*

void win_probe(uint8_t* base, uint8_t* base_limit, int count) {
  for (int i = 0; i < count; i++) {
    uint8_t* p = base - ((i + 1) * os_page_size);
    *p = (uint8_t)i;
  }
  mp_win_trace_stack_layout(base, base_limit);
}

void mp_gstack_win_test(mp_gstack_t* g) {
  uint8_t* stk = g->stack;
  ssize_t  stk_size = g->stack_size;
  uint8_t * base = mp_base(stk, stk_size);
  uint8_t* base_limit = stk;
  mp_win_trace_stack_layout(base, base_limit);

  win_probe(base, base_limit, 8);

  ssize_t reset_size = stk_size - os_page_size;
  MP_TIB* tib = mp_win_tib();

  //if (!VirtualProtect(tib->StackRealLimit, os_page_size, PAGE_READWRITE, NULL)) {
  //  mp_system_error_message(EINVAL, "mem_protect\n");
  //}
  ssize_t commit_size = mp_align_down(tib->StackBase - tib->StackLimit, os_page_size);
  DWORD err = DiscardVirtualMemory(tib->StackLimit, commit_size);
  //if (err != ERROR_SUCCESS) {
  //  mp_system_error_message(EINVAL, "mem_discard: %x\n", err);
  //}
  //if (VirtualAlloc(stk, reset_size, MEM_RESET, PAGE_NOACCESS) == NULL) {
  //  mp_system_error_message(EINVAL, "mem_reset\n");
  //}
  mp_win_trace_stack_layout(base, base_limit);

  mp_win_initial_commit(stk, reset_size, true);
  mp_win_trace_stack_layout(base, base_limit);
  
  tib->StackLimit = mp_push(base, 2*os_page_size, NULL);
  tib->StackRealLimit = mp_push(base, 3*os_page_size, NULL);
  mp_win_trace_stack_layout(base, base_limit);

  win_probe(base, base_limit, 10);
}
*/