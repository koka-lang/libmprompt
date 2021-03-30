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

// We need atomic operations for the `gpool` on systems that do not have overcommit.
#include "internal/atomic.h"

// use pthread local storage keys to detect thread ending
#include <pthread.h>



//----------------------------------------------------------------------------------
// The OS primitive `gstack` interface based on `mmap`.
//----------------------------------------------------------------------------------

static uint8_t* mp_os_mmap_reserve(size_t size, int prot, bool* zeroed) {
  #if !defined(MAP_ANONYMOUS)
  # define MAP_ANONYMOUS  MAP_ANON
  #endif
  #if !defined(MAP_NORESERVE)
  # define MAP_NORESERVE  0
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
  // macOS: tracking anonymous page with a specific ID. (All up to 98 are taken officially but LLVM sanitizers had taken 99 and mimalloc 100)
  fd = VM_MAKE_TAG(101);
  #endif
  uint8_t* p = (uint8_t*)mmap(NULL, size, prot, flags, fd, 0);
  if (p == MAP_FAILED) {
    mp_system_error_message(ENOMEM, "failed to allocate mmap memory of size %zu\n", size);
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
    return false;
  }
  return true;
}


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

// Allocate a gstack
static uint8_t* mp_gstack_os_alloc(uint8_t** stk, ssize_t* stk_size) {
  if (!os_use_gpools) {
    // use NORESERVE to let the OS commit on demand
    bool zeroed = false; // don't require zeros
    uint8_t* full = mp_os_mmap_reserve(os_gstack_size, PROT_NONE, &zeroed);
    if (full == NULL) {
      mp_linux_check_vma_limit();
      return NULL;
    }

    *stk = full + os_gstack_gap;
    *stk_size = os_gstack_size - 2 * os_gstack_gap;    
    if (!mp_os_mem_commit(*stk, *stk_size)) {   // and make the stack area read/write.       
      mp_linux_check_vma_limit();
      munmap(full, os_gstack_size);
      return NULL;
    }    
    return full;
  }
  else {
    // use the gpool allocator to commit-on-demand even on over-commit systems (using a signal handler)
    uint8_t* full = mp_gpool_alloc(stk,stk_size);
    if (full == NULL) return NULL;      
    // (try to) commit the initial page to reduce page faults
    uint8_t* init = (os_stack_grows_down ? *stk + (*stk_size - os_gstack_initial_commit) : *stk);
    mp_os_mem_commit(init, os_gstack_initial_commit);
    return full;
  }
}

// Free the memory of a gstack
static void mp_gstack_os_free(uint8_t* full) {
  if (!os_use_gpools) {
    mp_os_mem_free(full,os_gstack_size);
  }
  else {
    mp_gpool_free(full);
  }
}

// Reset the memory of a gstack
static bool mp_gstack_os_reset(uint8_t* full, uint8_t* stk, ssize_t stk_size) {
  MP_UNUSED(full);
  // reset memory pages up to the initial commit
  ssize_t  reset_size = stk_size - os_gstack_initial_commit;
  if (!os_gstack_reset_decommits) {
    // todo: use mmap(PROT_NONE) and back to PROT_READ|PROT_WRITE to force decommit?
    #if defined(MADV_FREE_REUSABLE)
    static int advice = MADV_FREE_REUSABLE;
    #elif defined(MADV_FREE)
    static int advice = MADV_FREE;
    #else
    static int advice = MADV_DONTNEED;
    #endif  
    int err = madvise(stk, reset_size, advice);
    if (err != 0 && errno == EINVAL && advice != MADV_DONTNEED) {
      // if MADV_FREE/MADV_FREE_REUSABLE is not supported, fall back to MADV_DONTNEED from now on
      advice = MADV_DONTNEED;
      err = madvise(stk, reset_size, MADV_DONTNEED);
    }
    if (err != 0) {
      mp_system_error_message(EINVAL, "failed to reset memory at %p of size %zd\n", stk, reset_size);
    }
    return (err == 0);
  } 
  else {
    int err = 0;
    #if defined(MAP_FIXED)
      // mmap with PROT_NONE to reduce commit charge 
      void* p = mmap(stk, reset_size, PROT_NONE, (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), -1, 0);
      if (p == MAP_FAILED) {
        err = errno;
      }
    #else
      // mprotect with PROT_NONE
      int err = mprotect(stk, reset_size, PROT_NONE);
    #endif
    if (err != 0) {
      mp_system_error_message(EINVAL, "failed to decommit memory at %p of size %zd\n", stk, reset_size);      
    }
    return (err == 0);
  }
}



//--------------------------------------------------
// Init/Done
//--------------------------------------------------

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
}

static void  mp_gpools_process_init(void);
static void  mp_gpools_process_done(void);

static bool mp_gstack_os_init(void) {
  // get the page size
  #if defined(_SC_PAGESIZE)
  long result = sysconf(_SC_PAGESIZE);
  if (result > 0) {
    os_page_size = (size_t)result;
  }
  #endif
  // do we have overcommit?
  #if defined(__linux__)
  bool os_has_overcommit = mp_linux_use_overcommit();
  #else
  bool os_has_overcommit = false;
  #endif
  if (!os_has_overcommit) {
    os_use_gpools = true;     // we must use gpools if overcommit is not supported
  }

  // register pthread key to detect thread termination
  pthread_key_create(&mp_pthread_key, &mp_pthread_done);
  // set process cleanup of the gpools
  atexit(&mp_gpools_process_done);
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

static void mp_sig_handler_commit_on_demand(int signum, siginfo_t* info, void* arg) {
  // demand allocate?
  //mp_trace_message("sig: entered\n");
  uint8_t* p = mp_align_down_ptr((uint8_t*)info->si_addr, os_page_size);
  //mp_trace_message("  sig: signum: %i, addr: %p, aligned: %p\n", signum, info->si_addr, p);
  ssize_t available;
  const mp_gpool_t* gp;
  int res = mp_gpools_is_accessible(p,&available,&gp); 
  if (res == 1) {
    // a pointer to a valid gstack in our gpool, make the page read-write
    // mp_trace_message("  segv: unprotect page\n");
    // use quadratic growth; quite important for performance
    ssize_t extra = 0;
    ssize_t used = os_gstack_size - available;
    if (used > 0) { extra = 2*used; }                // doubling..
    if (extra > 1 * MP_MIB) { extra = 1 * MP_MIB; }  // up to 1MiB growh
    if (extra > available) { extra = available; }    // but not more than available
    extra = mp_align_down(extra,os_page_size);
    //mp_trace_message("expand stack: extra: %zd, avail: %zd, used: %d\n", extra, available, used);
    p = p - extra;
    mprotect(p, extra + os_page_size, PROT_READ | PROT_WRITE);
    return; // we are ok!
  }
  else if (res == 2) {  
    // demand page the mp_gpool_t.free array with zeros
    if (mprotect(p, os_page_size, PROT_READ | PROT_WRITE) == 0) {
      if (gp!=NULL && !gp->zeroed) {
        memset(p, 0, os_page_size);  // zero out as well (can be needed with MAP_UNINITIALIZED)
      }
    }    
  }
  else if (res == 0) {
    mp_fatal_message(EFAULT,"stack overflow at %p\n", info->si_addr); // abort
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
#define MP_SIG_STACK_SIZE   (MINSIGSTKSZ < 4*MP_KIB ? 4*MP_KIB : MINSIGSTKSZ)

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
  if (!os_use_gpools) return; // no need for stack for an on-demand commit handler if the OS has overcommit enabled

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
  if (!os_use_gpools) return; // no need for an on-demand commit handler if the OS has overcommit enabledv

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


