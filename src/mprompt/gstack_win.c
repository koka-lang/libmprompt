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


// -----------------------------------------------------
// Interface
// -----------------------------------------------------

static bool  mp_win_initial_commit(uint8_t* stk, ssize_t stk_size, bool commit_initial);
static void  mp_win_trace_stack_layout(uint8_t* base, uint8_t* xbase_limit);
static DWORD mp_win_get_error(void);

// Reserve memory
static uint8_t* mp_os_mem_reserve(ssize_t size) {
  uint8_t* p = (uint8_t*)VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS);
  if (p == NULL) {
    mp_error_message(ENOMEM, "failed to reserve memory of size %uz (code 0x%xu)\n", size, mp_win_get_error());
  }
  return p;
}

// Free reserved memory
static void  mp_os_mem_free(uint8_t* p, ssize_t size) {
  MP_UNUSED(size);
  if (p == NULL) return;
  if (!VirtualFree(p, 0, MEM_RELEASE)) {
    mp_error_message(ENOMEM, "failed to free memory as %p of size %uz (code 0x%xu)\n", p, size, mp_win_get_error());
  }
}

// Commit a range of pages
static bool mp_os_mem_commit(uint8_t* start, ssize_t size) {
  if (VirtualAlloc(start, size, MEM_COMMIT, PAGE_READWRITE) == NULL) {   
    mp_error_message(ENOMEM, "failed to commit memory at %p of size %uz (error %d)\n", start, size, errno);
    return false;
  }
  return true;
}


// Allocate a gstack
static uint8_t* mp_gstack_os_alloc(uint8_t** stk, ssize_t* stk_size) {
  if (!os_use_gpools) {
    // reserve virtual full stack
    uint8_t* full = mp_os_mem_reserve(os_gstack_size);
    if (full == NULL) return NULL;

    *stk = full + os_gstack_gap;
    *stk_size = os_gstack_size - 2 * os_gstack_gap;
    // and initialize the guard page and initial commit
    if (!mp_win_initial_commit(*stk, *stk_size, true)) {
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
    if (!mp_win_initial_commit(*stk, *stk_size, true)) {
      mp_gpool_free(full);
      return NULL;
    }
    return full;
  }
}

// Set initial committed page in a gstack and a guard page to grow on-demand
static bool mp_win_initial_commit(uint8_t* stk, ssize_t stk_size, bool commit_initial) {
  if (stk == NULL) return false;
  uint8_t* base = mp_base(stk, stk_size);
  uint8_t* commit_start;
  uint8_t* commit_base = mp_push(base, os_gstack_initial_commit, &commit_start);
  if (commit_initial && os_gstack_initial_commit > 0) {
    // commit initial pages    
    if (!mp_os_mem_commit(commit_start, os_gstack_initial_commit)) {
      return false;
    }
  }  
  // set a guard page to grow on demand; this is handled by the OS since it cannot call a user fault handler as
  // the stack just ran out. It will raise a stack-overflow once the end of the reserved space is reached.
  // the actual guard pages is determined by the Windows thread stack guarantee.
  ULONG guaranteed = 0;
  SetThreadStackGuarantee(&guaranteed);
  size_t guard_size = os_page_size + mp_align_up(guaranteed, os_page_size);
  uint8_t* guard_start;
  mp_push(commit_base, guard_size, &guard_start);
  if (VirtualAlloc(guard_start, guard_size, MEM_COMMIT, PAGE_GUARD | PAGE_READWRITE) == NULL) {
    mp_error_message(ENOMEM, "failed to set guard page at %p of size %zu (code 0x%xu)\n", guard_start, guard_size, mp_win_get_error());
    return false;
  }       
  return true;
}

// Free the memory of a gstack
static void mp_gstack_os_free(uint8_t* full) {
  if (full == NULL) return;
  if (!os_use_gpools) {
    mp_os_mem_free(full, os_gstack_size);
  }
  else {
    mp_gpool_free(full);
  }
}

// Reset the memory in a gstack
static bool mp_gstack_os_reset(uint8_t* full, uint8_t* stk, ssize_t stk_size) {
  ssize_t reset_size = stk_size - os_gstack_initial_commit;
  if (reset_size <= 0) return true;
  if (!os_gstack_reset_decommits) {
    // reset memory pages up to the initial commit
    // note: this makes the physical memory available again but may still show
    //       up in the working set until it is actually reused :-( The advantage is that
    //       these pages no longer need to be zero'd, nor do we need to set the guard page again,
    //       and thus this can be more efficient.
    // todo: is this the current call ok since it includes a mix of committed and decommitted pages?
    //       we should perhaps only reset the committed range.
    if (VirtualAlloc(stk, reset_size, MEM_RESET, PAGE_NOACCESS /* ignored */) == NULL) {
      mp_error_message(EINVAL, "failed to reset memory at %p of size %uz (code 0x%xu)\n", stk, reset_size, mp_win_get_error());
      return false;
    }
    return true;
  }
  else {
    // decommit any committed pages up to the initial commit and reset the guard page
    // note: this will recommit on demand and gives zero'd pages which may be expensive.
    #pragma warning(suppress:6250) // warning: MEM_DECOMMIT does not free the memory
    if (!VirtualFree(stk, reset_size, MEM_DECOMMIT)) {
      mp_error_message(EINVAL, "failed to decommit memory at %p of size %uz (code 0x%xu)\n", stk, reset_size, mp_win_get_error());
      return false;
    }
    // .. and reinitialize guard
    return mp_win_initial_commit(stk, stk_size, false);
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

static bool mp_gstack_os_init(void) {
  // for now we always enable gpools or otherwise exceptions don't work
  os_use_gpools = true;

  // page size
  SYSTEM_INFO sys_info;
  GetSystemInfo(&sys_info);
  os_page_size = sys_info.dwPageSize;

  // set up thread termination routine
  mp_win_fls_key = FlsAlloc(&mp_win_thread_done);
  atexit(&mp_win_process_done);

  if (os_use_gpools) {
    // install a page fault handler to grow stackson demand 
    PVOID handler = AddVectoredExceptionHandler(1, &mp_gstack_win_page_fault);
    if (handler == NULL) {
      mp_error_message(EINVAL, "unable to install page fault handler (code %xu) -- fall back to guarded demand paging\n", mp_win_get_error());
      os_use_gpools = false; // fall back to regular demand paging 
    }
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


static MP_TIB* mp_win_tib(void) {
  return (MP_TIB*)NtCurrentTeb();
}

static mp_decl_noinline uint8_t* mp_win_addr(volatile uint8_t* p) {
  return (uint8_t*)p;
}

static mp_decl_noinline uint8_t* mp_win_current_sp(void) {
  volatile uint8_t b;
  return mp_win_addr(&b);
}

#define MP_CPP_EXN 0xE06D7363  // "msc"

// Guard page fault handler: not required as Windows already grows stacks with a guard
// page automatically; we use it to grow the stack exponentially for performance when using
// gpool's (`os_use_gpools`).
//
// To avoid errors during RtlUnwindEx we also commit an extra stack page if a C++ exception is 
// thrown and little stack space is available.
// Todo: should we also do this for other (system) exceptions?
static LONG WINAPI mp_gstack_win_page_fault(PEXCEPTION_POINTERS ep) {
  const DWORD exncode = ep->ExceptionRecord->ExceptionCode;
  if (!os_use_gpools ||
      (exncode != STATUS_STACK_OVERFLOW && exncode != STATUS_ACCESS_VIOLATION && exncode != MP_CPP_EXN)) {  // STATUS_GUARD_PAGE_VIOLATION
    return EXCEPTION_CONTINUE_SEARCH;
  }

  // find the page start
  MP_TIB* tib = mp_win_tib();
  uint8_t* const addr = (exncode!=MP_CPP_EXN ? (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1] : tib->StackLimit - 8); 
  uint8_t* const page = mp_align_down_ptr(addr, os_page_size); 
  ssize_t commit_available = mp_win_current_sp() - tib->StackLimit;
  ssize_t available;
  int res = mp_gpools_is_accessible(page, &available, NULL);

  if ((res == 1 && exncode == STATUS_STACK_OVERFLOW) || 
      (res == 1 && exncode == MP_CPP_EXN && commit_available < os_page_size)) 
  {
    // pointer in our gpool stack, make the page read-write
    // mp_trace_message("  segv: unprotect page\n");
    // use exponential growth; important for performance, see `main.cpp:test1M`.
    ULONG    guaranteed = 0;
    SetThreadStackGuarantee(&guaranteed);
    const ssize_t  guard_size = os_page_size + mp_align_up(guaranteed, os_page_size);

    ssize_t extra = 0;
    ssize_t used = os_gstack_size - os_gstack_gap - available;
    if (used > 0) {
      extra = (exncode != MP_CPP_EXN ? 2 * used : os_page_size);  // doubling.. (or just one extra page for exceptions)
    }
    if (extra > 1 * MP_MIB) {
      extra = 1 * MP_MIB;   // up to 1MiB growh
    }
    if (extra > available - guard_size) {
      extra = available - guard_size;     // up to stack limit
    }
    extra = mp_align_down(extra, os_page_size);
    if (extra >= 0) {
      uint8_t* const extend = (uint8_t*)page - extra;
      if (VirtualAlloc(extend, extra + os_page_size, MEM_COMMIT, PAGE_READWRITE) != NULL) {
        uint8_t* const gpage = extend - guard_size;
        if (VirtualAlloc(gpage, guard_size, MEM_COMMIT, PAGE_GUARD | PAGE_READWRITE) != NULL) {
          tib->StackLimit = extend;
          tib->StackRealLimit = gpage; 
          //mp_trace_message("expanded stack: extra: %zdk, available: %zdk, used: %zdk\n", extra/1024, available/1024, used/1024);
          //mp_trace_stack_layout(NULL, NULL);
          return (exncode!=MP_CPP_EXN ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH);
        }
      }
    }
  }
  else if (res == 2 && exncode == STATUS_ACCESS_VIOLATION) {
    // at first page for zero'ing
    if (VirtualAlloc(page, os_page_size, MEM_COMMIT, PAGE_READWRITE) != NULL) {
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  

  return EXCEPTION_CONTINUE_SEARCH;  
}


// -----------------------------------------------------
// Util
// -----------------------------------------------------

static DWORD mp_win_get_error(void) {
  DWORD err = GetLastError();
  #ifndef NDEBUG
  if (err != ERROR_SUCCESS) {
    char buf[256];
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY, NULL, err, 0, buf, 255, NULL);
    mp_trace_message("windows error %u: %s", err, buf);
  }
  #endif
  return err;
}


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
  if (base_glimit != NULL) { mp_trace_message("guard: %p, guaranteed: %zi\n", base_glimit, guaranteed); }
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
