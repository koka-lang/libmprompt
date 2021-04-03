/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Included from "gstack_mmap.c".

  macOS (mach kernel) only.
  In `lldb` SEGV signals cannot be continued which is trouble for our
  gstacks where the SEGV is used to commit pages on demand.
  
  We can work around this by catching SEGV at the mach kernel level
  using the mach exception messages. This needs a separate thread though
  to handle the messages which is why we only do this in debug mode and
  use a regular signal handler in release mode.
----------------------------------------------------------------------------*/
#if !defined(MP_MACH_USE_EXC_PORT)

// Don't use an exception port
static void mp_os_mach_thread_init(void) { }
static void mp_os_mach_process_init(void) { }

#else
// Use an exception port (with associated thread)
static void mp_os_mach_thread_init(void);
static void mp_os_mach_process_init(void);

#include <pthread.h>
#include <mach/mach_port.h>   // mach_port_t
#include <mach/mach_init.h>   // mach_thread_self
#include <mach/thread_act.h>  // task_swap_exception_ports


//--------------------------------------------------------------------------
// Generate the following two request- and reply structures using
// $ mig -v mach/mach_exc.defs
// and extract it from `mach_exc.h`
// (We cannot use `mach/exc.h` as that is 32-bit only).
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
  //mp_trace_message("mach exception handler started\n");
  int tries = 0;
  // Keep receiving exception messages
  while(true) {
    mp_mach_exc_request_t req;    
    if (mach_msg(&req.Head, MACH_RCV_MSG, 0, sizeof(req), mp_mach_exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) == MACH_MSG_SUCCESS) {
      tries = 0;
      //mp_trace_message("mach: received exception port message %i\n", req.code[0]);
      if (req.code[0] == KERN_PROTECTION_FAILURE) {  // == EXC_BAD_ACCESS
        void* address = (void*)req.code[1];
        //x86_thread_state_t* tstate = (x86_thread_state_t*)req.old_state;
        //mp_trace_message("  address: %p\n", address);
        // And call our commit-on-demand handler to see if we can provide access.
        if (mp_gpools_commit_on_demand(address) == 0) {
          //mp_trace_message("  resolved bad acsess at %p\n", address);
          mp_mach_reply(&req, KERN_SUCCESS);
          continue;
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
  exception_mask_t prev_em;
  mach_msg_type_number_t prev_ec;  
  mach_port_t prev_ep;
  exception_behavior_t prev_b;
  thread_state_flavor_t prev_f; 
  if (thread_swap_exception_ports(mach_thread_self(), EXC_MASK_BAD_ACCESS, mp_mach_exc_port,
                                   (EXCEPTION_STATE_IDENTITY | MACH_EXCEPTION_CODES), MACHINE_THREAD_STATE,
                                   &prev_em, &prev_ec, &prev_ep, &prev_b, &prev_f) != KERN_SUCCESS) {
    mp_error_message(EINVAL, "unable to set exception port on thread\n");
    return;
  } 
  //mp_trace_message("thread mach exception ports initialized\n"); 
}

// Initialize the process exception port and associated handler thread
static void mp_mach_create_exception_thread(void) {
  mach_port_t port = mach_task_self();
  if (mach_port_allocate(port, MACH_PORT_RIGHT_RECEIVE, &mp_mach_exc_port) != KERN_SUCCESS) {
    mp_error_message(EINVAL, "unable to set mach exception port\n");
    return;
  }  
  if (mach_port_insert_right(port, mp_mach_exc_port, mp_mach_exc_port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
    mp_error_message(EINVAL, "unable to set mach exception port send permission\n");
    return;
  }
  pthread_t exc_thread;
  if (pthread_create(&exc_thread, NULL, &mp_mach_exc_thread_start, NULL) != 0) {
    mp_error_message(EINVAL, "unable to create mach exception handler thread\n");
    return;
  }  
  // mp_trace_message("process mach exception port initialized\n");
}

// Initialize process.
static void mp_os_mach_process_init(void) {
  // create a single exception handler thread to handles EXC_BAD_ACCESS (before the debugger gets it)
  static pthread_once_t exc_thread_setup_once = PTHREAD_ONCE_INIT;
  if (pthread_once(&exc_thread_setup_once, mp_mach_create_exception_thread) != 0) {
    mp_error_message(EINVAL, "unable to setup mach exception handling thread");
  }
}

#endif // !NDEBUG && __MACH__
