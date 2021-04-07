/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Included from `gstack.c`

  Posix (`mmap`) specific allocation of gstack's
-----------------------------------------------------------------------------*/

#include <sys/mman.h>  // mmap
#include <unistd.h>    // sysconf
#include <signal.h>    // sigaction
#include <fcntl.h>     // file read
#include <pthread.h>   // use pthread local storage keys to detect thread ending

// We need atomic operations for the `gpool` on systems that do not have overcommit.
#include "internal/atomic.h"

// forward declaration 
static bool mp_mmap_commit_on_demand(void* addr, bool addr_in_other_thread);

// macOS in debug mode needs an exception port handler 
#include "gstack_mmap_mach.c"


//----------------------------------------------------------------------------------
// The OS memory low-level allocation primitives
//----------------------------------------------------------------------------------

// Extra error message on Linux since often ENOMEM means the VMA limit is too low
static void mp_linux_check_vma_limit(void) {
  #if defined(__linux__)
  if (errno == ENOMEM) {
    mp_error_message(ENOMEM, "the previous error may have been caused by a low memory map limit.\n"
                              "  On Linux this can be controlled by increasing the vm.max_map_count. For example:\n"
                              "  > sudo sysctl -w vm.max_map_count=1000000\n");
  }
  #endif
}


static uint8_t* mp_os_mmap_reserve(size_t size, int prot, bool* zeroed) {
  #if !defined(MAP_ANONYMOUS)
  # define MAP_ANONYMOUS  MAP_ANON
  #endif
  #if !defined(MAP_NORESERVE)
  # if defined(MAP_LAZY)  
  #  define MAP_NORESERVE  MAP_LAZY
  # else
  #  define MAP_NORESERVE  0
  # endif
  #endif
  #if !defined(MAP_STACK)
  # define MAP_STACK  0
  #endif
  int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK;
  #if !defined(MAP_UNINITIALIZED)
  if (zeroed != NULL) {
    *zeroed = true;
  }
  #else
  if (zeroed != NULL) {
    if (*zeroed == false) {  // allow non-zero'd memory?
      flags |= MAP_UNINITIALIZED;
    }
  }
  #endif
  #if defined(PROT_MAX)
  prot |= PROT_MAX(PROT_READ | PROT_WRITE); // BSD
  #endif
  int fd = -1;
  #if defined(VM_MAKE_TAG)
  // macOS: tracking anonymous page with a specific ID. (All up to 98 are taken officially but LLVM sanitizers have taken 99 and mimalloc 100)
  fd = VM_MAKE_TAG(101);
  #endif
  uint8_t* p = (uint8_t*)mmap(NULL, size, prot, flags, fd, 0);
  if (p == MAP_FAILED) {
    mp_system_error_message(ENOMEM, "failed to allocate mmap memory of size %zu\n", size);
    mp_linux_check_vma_limit();      
    return NULL;
  }
  return p;
}

// Reserve virtual memory range
static uint8_t* mp_os_mem_reserve(ssize_t size) {
  return mp_os_mmap_reserve(size, PROT_NONE, NULL);
}

// Free reserved memory
static void  mp_os_mem_free(uint8_t* p, ssize_t size) {
  MP_UNUSED(size);
  if (p == NULL) return;
  if (munmap(p,size) != 0) {
    mp_system_error_message(ENOMEM, "failed to free memory at %p of size %zd\n", p, size);
  }
}

// Commit a range of pages
static bool mp_os_mem_commit(uint8_t* start, ssize_t size) {
  if (mprotect(start, size, PROT_READ | PROT_WRITE) != 0) {   
    mp_system_error_message(ENOMEM, "failed to commit memory at %p of size %zd\n", start, size);
    mp_linux_check_vma_limit();      
    return false;
  }
  return true;
}

// Reset the memory of a gstack
static bool mp_os_mem_reset(uint8_t* p, ssize_t size) {
  // we can only decommit if MAP_FIXED is defined
  #if defined(MAP_FIXED)  
  if (os_gstack_reset_decommits) {
    // mmap with PROT_NONE to reduce commit charge
    if (mmap(p, size, PROT_NONE, (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), -1, 0) == MAP_FAILED) {
      mp_system_error_message(EINVAL, "failed to decommit memory at %p of size %zd\n", p, size);
      return false;      
    }
    return true;
  }
  #endif

  // reset using madvise
  #if defined(MADV_FREE_REUSABLE)           
  static int advice = MADV_FREE_REUSABLE;
  #elif defined(MADV_FREE)                 
  static int advice = MADV_FREE;
  #else
  static int advice = MADV_DONTNEED;
  #endif  
  int err = madvise(p, size, advice);
  if (err != 0 && errno == EINVAL && advice != MADV_DONTNEED) {
    // if MADV_FREE/MADV_FREE_REUSABLE is not supported, fall back to MADV_DONTNEED from now on
    advice = MADV_DONTNEED;
    err = madvise(p, size, MADV_DONTNEED);
  }
  if (err != 0) {
    mp_system_error_message(EINVAL, "failed to reset memory at %p of size %zd\n", p, size);
  }
  return (err == 0);  
}


//----------------------------------------------------------------------------------
// The OS primitive `gstack` interface based on `mmap`.
//----------------------------------------------------------------------------------


// Set initial committed page in a gstack and a guard page to grow on-demand
static bool mp_mmap_initial_commit(uint8_t* stk, ssize_t stk_size, ssize_t* initial_commit) {
  if (initial_commit != NULL) *initial_commit = 0;
  if (os_use_overcommit) {
    // and make the stack area read/write.       
    if (!mp_os_mem_commit(stk, stk_size)) {
      return false;
    }
    if (initial_commit != NULL) *initial_commit = stk_size;
  }
  else {
    // only commit the initial pages and demand-page the rest
    uint8_t* base = mp_base(stk, stk_size);
    uint8_t* commit_start;
    mp_push(base, os_gstack_initial_commit, &commit_start);
    if (!mp_os_mem_commit(commit_start, os_gstack_initial_commit)) {
      return false;
    }
    if (initial_commit != NULL) *initial_commit = os_gstack_initial_commit;
  }
  return true;
}

// Allocate a gstack
static uint8_t* mp_gstack_os_alloc(uint8_t** stk, ssize_t* stk_size, ssize_t* initial_commit) {
  if (initial_commit != NULL) { *initial_commit = 0; }
  if (!os_use_gpools) {
    // use NORESERVE to let the OS commit on demand
    bool zeroed = false; // don't require zeros
    uint8_t* full = mp_os_mmap_reserve(os_gstack_size, PROT_NONE, &zeroed);
    if (full == NULL) {
      return NULL;
    }

    *stk = full + os_gstack_gap;
    *stk_size = os_gstack_size - 2 * os_gstack_gap;    
    if (!mp_mmap_initial_commit(*stk, *stk_size, initial_commit)) {
      munmap(full, os_gstack_size);
      return NULL;
    }
    return full;
  }
  else {
    // use the gpool allocator to commit-on-demand even on over-commit systems (using a signal handler)
    uint8_t* full = mp_gpool_alloc(stk,stk_size);
    if (full == NULL) return NULL;      
    if (!mp_mmap_initial_commit(*stk, *stk_size, initial_commit)) {
      mp_gpool_free(full);
      return NULL;
    }
    return full;
  }  
}

// Free the memory of a gstack
static void mp_gstack_os_free(uint8_t* full, uint8_t* stk, ssize_t stk_size, ssize_t stk_commit) {
  MP_UNUSED(stk_commit);
  if (!os_use_gpools) {
    mp_os_mem_free(full,os_gstack_size);
  }
  else {
    // reset the full range. todo: just reset the actual committed range?
    mp_os_mem_reset(stk,stk_size);
    mp_gpool_free(full);
  }
}



//--------------------------------------------------
// Init/Done
//--------------------------------------------------

static void  mp_gpools_process_init(void);
static void  mp_gpools_process_done(void);
static void  mp_gpools_thread_init(void);


#if defined(__linux__)
static bool mp_linux_use_overcommit(void) {
  int open_flags = O_RDONLY;
  #if defined(O_CLOEXEC) // BSD
  open_flags |= O_CLOEXEC;
  #endif
  int fd = open("/proc/sys/vm/overcommit_memory", open_flags);
	if (fd < 0) return false;
  char buf[128];
  ssize_t nread = read(fd, &buf, sizeof(buf));
	close(fd);
  // <https://www.kernel.org/doc/Documentation/vm/overcommit-accounting>
  // 0: heuristic overcommit, 1: always overcommit, 2: never overcommit (ignore NORESERVE)
  return (nread > 1 && (buf[0] == '0' || buf[0] == '1'));  
}
#endif

pthread_key_t mp_pthread_key = 0;

static void mp_pthread_done(void* value) {
  if (value!=NULL) mp_gstack_thread_done();
}

static void mp_gstack_os_thread_init() {
  pthread_setspecific(mp_pthread_key, (void*)(1));  // set to non-zero value
  mp_gpools_thread_init();
  mp_os_mach_thread_init();  
}

static bool mp_gstack_os_init(void) {
  // get the page size
  #if defined(_SC_PAGESIZE)
  long result = sysconf(_SC_PAGESIZE);
  if (result > 0) {
    os_page_size = (size_t)result;
  }
  #endif
  // can we support overcommit?
  os_use_overcommit = false;
  #if defined(__linux__)
  if (!(os_use_gpools || os_gstack_grow_fast) && mp_linux_use_overcommit()) {
    os_use_overcommit = true;
  }
  #endif
  
  // register pthread key to detect thread termination
  pthread_key_create(&mp_pthread_key, &mp_pthread_done);

  // set process cleanup of the gpools
  atexit(&mp_gpools_process_done);

  mp_os_mach_process_init();  // macOS; note: must come before gpools_process_init as it may enable gpools.
  mp_gpools_process_init();  
  return true;
}


// ----------------------------------------------------
// Signal handler for gpools to commit-on-demand
// ----------------------------------------------------

// Install a signal handler that handles page faults in gpool allocated stacks
// by making them accessible (PROT_READ|PROT_WRITE).
static struct sigaction mp_sig_segv_prev_act;
static struct sigaction mp_sig_bus_prev_act;
static mp_decl_thread stack_t* mp_sig_stack;  // every thread needs a signal stack in order do demand commit stack pages

static bool mp_mmap_commit_on_demand(void* addr, bool addr_in_other_thread) {
  // demand allocate?
  uint8_t* page = mp_align_down_ptr((uint8_t*)addr, os_page_size);
  ssize_t available = 0;
  ssize_t stack_size = 0;
  mp_access_t access = MP_NOACCESS;
  mp_gstack_t* g = mp_gstack_current();  
  if (g != NULL) {
    // normally we only handle accesses in our current gstack
    access = mp_gstack_check_access(g, page, &stack_size, &available, NULL);
  }
  else if (addr_in_other_thread && os_use_gpools) {
     // on mach (macOS) while debugging we use a separate mach exception thread handler
     // in that case we can use gpools to determine if the access is in one of our gstacks.
     access = mp_gpools_check_access( page, &stack_size, &available, NULL );
  }
  
  if (access == MP_ACCESS) {
    // a pointer to a valid gstack in our gpool, make the page read-write
    // mp_trace_message("  segv: unprotect page\n");
    // use quadratic growth; quite important for performance
    ssize_t extra = 0;
    ssize_t used = stack_size - available;
    if (os_gstack_grow_fast && used > 0) { extra = 2*used; }   // doubling..
    if (extra > 1 * MP_MIB) { extra = 1 * MP_MIB; }            // up to 1MiB growh
    if (extra > available) { extra = available; }              // but not more than available
    extra = mp_align_down(extra,os_page_size);
    //mp_trace_message("expand stack: extra: %zd, avail: %zd, used: %d\n", extra, available, used);
    uint8_t* commit_start;
    mp_push(page, extra, &commit_start);
    if (mprotect(commit_start, extra + os_page_size, PROT_READ | PROT_WRITE) == 0) {
      if (g != NULL) { g->committed = mp_unpush(commit_start, g->stack, g->stack_size ); }
    };
    return true; 
  }
  else if (access == MP_NOACCESS_STACK_OVERFLOW) {
    // stack overflow
    mp_error_message(EINVAL,"stack overflow at %p\n", addr);  // abort?
  }
  
  // not in one of our pools or error
  return false;
}

static void mp_sig_handler_commit_on_demand(int signum, siginfo_t* info, void* arg) {
  // demand allocate?
  //mp_trace_message("sig: signum: %i, addr: %p\n", signum, info->si_addr);
  if (mp_mmap_commit_on_demand(info->si_addr, false)) {
    return; // ok!
  }
  
  // otherwise call the previous handler
  //mp_trace_message("  sig: forward signal\n");
  struct sigaction* parent = (signum == SIGBUS ? &mp_sig_bus_prev_act : &mp_sig_segv_prev_act);
  if ((parent->sa_flags & SA_SIGINFO) != 0 && parent->sa_sigaction != NULL) {
    (parent->sa_sigaction)(signum, info, arg);
  }
  else if (parent->sa_handler != NULL) {
    (parent->sa_handler)(signum);
  }
}



// ----------------------------------------------------
// Thread init/done: set alternate signal handler stack
// ----------------------------------------------------

// or SIGSTKSIZE but we require just a small stack
#define MP_SIG_STACK_SIZE   (MINSIGSTKSZ < 8*MP_KIB ? 8*MP_KIB : MINSIGSTKSZ)

static void mp_gpools_thread_done(void) {
  // free signal handler stack
  if (mp_sig_stack != NULL) {
    // disable signal stack
    stack_t ss;
    ss.ss_flags = SS_DISABLE;
    ss.ss_sp = NULL;
    ss.ss_size = MP_SIG_STACK_SIZE;
    sigaltstack(&ss,NULL);  
    // and free it
    mp_free(mp_sig_stack->ss_sp);
    mp_free(mp_sig_stack);
    mp_sig_stack = NULL;
  }
}

// Each thread needs to register an alternative stack for the signal handler to run in.
static void mp_gpools_thread_init(void) {
  if (!os_use_gpools && os_use_overcommit) return; // no need for stack for an on-demand commit handler if the OS has overcommit enabled

  // use an alternate signal stack (since we handle stack overflows)
  if (mp_sig_stack == NULL) {    
    stack_t old_ss;
    memset(&old_ss, 0, sizeof(old_ss));
    int err = sigaltstack(NULL, &old_ss);
    if (err==0 && old_ss.ss_sp == NULL) {  
      // alternate signal stack not yet installed; create one
      mp_sig_stack = mp_malloc_tp(stack_t);
      if (mp_sig_stack != NULL) {
        mp_sig_stack->ss_flags = 0;
        mp_sig_stack->ss_size = MP_SIG_STACK_SIZE;  
        mp_sig_stack->ss_sp = mp_malloc(mp_sig_stack->ss_size);
        if (mp_sig_stack->ss_sp != NULL) {
          if (sigaltstack(mp_sig_stack, NULL) != 0) {
            mp_free(mp_sig_stack->ss_sp);
            mp_sig_stack->ss_sp = NULL;
          }
        }
        if (mp_sig_stack->ss_sp == NULL) {
          mp_free(mp_sig_stack);
          mp_sig_stack = NULL;
        }
      }
      if (mp_sig_stack == NULL) {
        mp_system_error_message(EINVAL, "unable to set alternate signal stack\n");
      }     
    }
  }
}


static void mp_gpools_process_done(void) {
  // reset previous signal handlers
  if (mp_sig_segv_prev_act.sa_sigaction != NULL) {
    sigaction(SIGSEGV, &mp_sig_segv_prev_act, NULL);  
    memset(&mp_sig_segv_prev_act,0,sizeof(mp_sig_segv_prev_act));
  }
  if (mp_sig_bus_prev_act.sa_sigaction != NULL) {
    sigaction(SIGBUS, &mp_sig_bus_prev_act, NULL);  
    memset(&mp_sig_bus_prev_act,0,sizeof(mp_sig_bus_prev_act));
  }
  mp_gpools_thread_done();
}


// At process initialization we register our page fault handler for gpool on-demand paging.
static void mp_gpools_process_init(void) {
  mp_gpools_thread_init();
  if (!os_use_gpools && os_use_overcommit) return; // no need for an on-demand commit handler if the OS has overcommit enabledv

  // install signal handler
  if (mp_sig_segv_prev_act.sa_sigaction == NULL && mp_sig_segv_prev_act.sa_handler == NULL) {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_sigaction = &mp_sig_handler_commit_on_demand;
    act.sa_flags = SA_SIGINFO | SA_ONSTACK;    
    sigemptyset(&act.sa_mask);
    int err = sigaction(SIGSEGV, &act, &mp_sig_segv_prev_act);
    #if !defined(__linux__)
    // install bus signal too on BSD's (definitely needed on macOS)
    if (err==0) {
      err = sigaction(SIGBUS,  &act, &mp_sig_bus_prev_act); 
    }
    #endif
    if (err != 0) {
      mp_system_error_message(EINVAL, "unable to install signal handler\n");
    }
    // mp_trace_message("signal handler installed\n");
  }
}


