/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Included from "gstack_mmap.c".

  macOS (mach kernel) only:
  In `lldb` SEGV signals cannot be continued which is trouble for our
  gstacks where the SEGV is used to commit pages on demand.
  <https://bugs.llvm.org//show_bug.cgi?id=22868>
  
  We can work around this by catching SEGV at the mach kernel level
  using the mach exception messages. We could always do this on macOS
  (instead of using a regular signal handler) but since it requires an 
  extra thread we prefer currently to only use this when running under
  a debugger.
----------------------------------------------------------------------------*/
#if !defined(__MACH__)

// Never use an exception port
static void mp_os_mach_thread_init(void) { }
static void mp_os_mach_process_init(void) { }

#else
// Use a Mach exception port if running under a debugger
static void mp_os_mach_thread_init(void);
static void mp_os_mach_process_init(void);

#include <pthread.h>
#include <mach/mach_port.h>   // mach_port_t
#include <mach/mach_init.h>   // mach_thread_self
#include <mach/thread_act.h>  // task_swap_exception_ports
#include <sys/sysctl.h>

//--------------------------------------------------------------------------
// Generate the following two request- and reply structures using
// $ mig -v mach/mach_exc.defs
// and extract it from `mach_exc.h`
// (We cannot use `mach/exc.h` as that is 32-bit only).
// todo: can we simplify/avoid this code ?
//--------------------------------------------------------------------------
#ifdef  __MigPackStructs
#pragma pack(push, 4)
#endif
typedef struct {
  mach_msg_header_t Head;
  /* start of the kernel processed data */
  mach_msg_body_t msgh_body;
  mach_msg_port_descriptor_t thread;
  mach_msg_port_descriptor_t task;
  /* end of the kernel processed data */
  NDR_record_t NDR;
  exception_type_t exception;
  mach_msg_type_number_t codeCnt;
  int64_t code[2];
  int flavor;
  mach_msg_type_number_t old_stateCnt;
  natural_t old_state[614];
} __Request__mach_exception_raise_state_identity_t __attribute__((unused));

typedef struct {
  mach_msg_header_t Head;
  NDR_record_t NDR;
  kern_return_t RetCode;
  int flavor;
  mach_msg_type_number_t new_stateCnt;
  natural_t new_state[614];
} __Reply__mach_exception_raise_state_identity_t __attribute__((unused));
#ifdef  __MigPackStructs
#pragma pack(pop)
#endif

typedef __Request__mach_exception_raise_state_identity_t mp_mach_exc_request_t;
typedef __Reply__mach_exception_raise_state_identity_t   mp_mach_exc_reply_t;

// mach reply message (using the original request)
static void mp_mach_reply( const mp_mach_exc_request_t* req, kern_return_t ret) {
  mp_mach_exc_reply_t reply;
  reply.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->Head.msgh_bits), 0);
  reply.Head.msgh_remote_port = req->Head.msgh_remote_port;
  reply.Head.msgh_local_port = MACH_PORT_NULL;
  reply.Head.msgh_reserved = 0;
  reply.Head.msgh_id = req->Head.msgh_id + 100;
  reply.NDR = req->NDR;
  reply.RetCode = ret;
  reply.flavor = req->flavor;
  const size_t state_size  = sizeof(reply.new_state);
  const size_t state_count = state_size/sizeof(reply.new_state[0]);
  reply.new_stateCnt = ((req->old_stateCnt <= state_count) ? req->old_stateCnt : state_count);
  const size_t state_used = reply.new_stateCnt * sizeof(reply.new_state[0]);
  reply.Head.msgh_size = offsetof(mp_mach_exc_reply_t,new_state) + state_used;  // size must match used state
  memcpy(reply.new_state, req->old_state, state_used);
  mach_msg(&reply.Head, MACH_SEND_MSG, reply.Head.msgh_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}


//--------------------------------------------------------------------------
// Mach exception handler
//--------------------------------------------------------------------------

// process wide exception port
static mach_port_name_t mp_mach_exc_port = MACH_PORT_NULL;
  
// the main exception handling thread
static void* mp_mach_exc_thread_start(void* arg) {
  MP_UNUSED(arg);
  int tries = 0;
  // Keep receiving exception messages
  while(true) {
    mp_mach_exc_request_t req;    
    if (mach_msg(&req.Head, MACH_RCV_MSG, 0, sizeof(req), mp_mach_exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) == MACH_MSG_SUCCESS) {
      tries = 0;
      //mp_trace_message("mach: received exception port message %i\n", req.code[0]);
      if (req.code[0] == KERN_PROTECTION_FAILURE) {  // == EXC_BAD_ACCESS
        void* address = (void*)req.code[1];
        // And call our commit-on-demand handler to reliably provide access if the address is in one of our gstacks
        if (mp_mmap_commit_on_demand(address,true)) {
          //mp_trace_message("  resolved bad acsess at %p\n", address);
          mp_mach_reply(&req, KERN_SUCCESS);  // resolved!
          continue;                           // wait for next request
        }
      }
      //mp_trace_message("  unable to handle exception message\n");
      mp_mach_reply(&req, KERN_FAILURE); // forward to task handler
    }
    else {
      //mp_trace_message("mach: unable to receive exception message\n");      
      tries++;
      if (tries > 3) break;  // stop the thread if we cannot receive messages at all
    }
  }
  return NULL;
}


//--------------------------------------------------------------------------
// Mach exception handler initialization
//--------------------------------------------------------------------------

// Set the thread-local exception port to use our (process wide) exception thread
static void mp_os_mach_thread_init(void) {
  if (mp_mach_exc_port != MACH_PORT_NULL) {
    if (thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_ACCESS, mp_mach_exc_port,
                                    (EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES), MACHINE_THREAD_STATE) != KERN_SUCCESS) {
      mp_error_message(EINVAL, "unable to set exception port on thread\n");
    } 
  }
}

// Check if we are running under a debugger
// <https://stackoverflow.com/questions/2200277/detecting-debugger-on-mac-os-x>
static bool mp_os_mach_in_debugger(void) {
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
  struct kinfo_proc info = { };
  size_t size = sizeof(info);
	if (sysctl(mib, 4, &info, &size, NULL, 0) == 0) {
    return ((info.kp_proc.p_flag & P_TRACED) != 0);
  }
  return false;
}

// Initialize process. (should be called at most once at process start)
static void mp_os_mach_process_init(void) {
  // Only set up a mach exception handler if we are running under a debugger
  // Design note: in principle we could always enable this and not use a signal handler at all.
  //              the only drawback would be the creation of an extra thread.
  if (!mp_os_mach_in_debugger()) return;
  os_use_gpools = true;  // we must use gpools in this situation or otherwise we cannot
                         // determine access in the commit_on_demand handler (as the handler
                         // runs now in a separate thread)

  // Create a single exception handler thread to handles EXC_BAD_ACCESS (before the debugger gets it)
  mach_port_t port = mach_task_self();
  if (mach_port_allocate(port, MACH_PORT_RIGHT_RECEIVE, &mp_mach_exc_port) != KERN_SUCCESS) {
    mp_error_message(EINVAL, "unable to set mach exception port\n");
    return;
  }  
  if (mach_port_insert_right(port, mp_mach_exc_port, mp_mach_exc_port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
    mp_error_message(EINVAL, "unable to set mach exception port send permission\n");
    goto err;
  }
  static pthread_t mp_mach_exc_thread;
  if (pthread_create(&mp_mach_exc_thread, NULL, &mp_mach_exc_thread_start, NULL) != 0) {
    mp_error_message(EINVAL, "unable to create mach exception handler thread\n");
    goto err;
  }  
  return; 

err:
  if (mp_mach_exc_port != MACH_PORT_NULL) {
    mach_port_deallocate(port, mp_mach_exc_port);
    mp_mach_exc_port = MACH_PORT_NULL;
  }
}

#endif // __MACH__
