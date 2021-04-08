/* ---------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Rehandle implements the "evil" example from 
  Ningning Xie and Daan Leijen, "Generalized Evidence Passing for Effect Handlers", MSR-TR-2021-5, Mar 2021.
  It shows how the stack can change after the call to `exit_capture` 
  with a different reader_handler on top.
-----------------------------------------------------------------------------*/
#include "test.h"

/* ---------------------------------------------------------------------------
  Show dynamic backtraces (just testing unwinding through prompts)
-----------------------------------------------------------------------------*/

#define SHOW_BACKTRACE  0
#define USE_LIB_UNWIND  0

#if SHOW_BACKTRACE  // show dynamic backtrace
#include <mprompt.h>

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib,"dbghelp")
static HANDLE current_process = INVALID_HANDLE_VALUE;
static void print_backtrace(const char* msg) {
  fprintf(stderr,"-----------------------------\n");
  fprintf(stderr, "backtrace at: %s\n", msg);
  void* bt[128];
  int n = mp_backtrace(bt, 128);
  if (current_process == INVALID_HANDLE_VALUE) {
    current_process = GetCurrentProcess();
    SymInitialize(current_process, NULL, TRUE);
  }
  PSYMBOL_INFO info = (PSYMBOL_INFO)calloc(1, sizeof(SYMBOL_INFO) + 256 * sizeof(TCHAR));
  info->MaxNameLen = 255;
  info->SizeOfStruct = sizeof(SYMBOL_INFO);
  for (int i = 0; i < n; i++) {
    if (SymFromAddr(current_process, (DWORD64)(bt[i]), 0, info)) {
      fprintf(stderr, "frame %2d: %8p: %s\n", i, bt[i], info->Name);
    }
    else {
      fprintf(stderr, "frame %2d: %8p: <unknown: error: 0x%04x>\n", i, bt[i], GetLastError());
    }
  }
  free(info);
  fprintf(stderr,"\n");
}

#elif USE_LIB_UNWIND 
// if not on macOS: 
// edit the cmake to link the mptest target with libunwind:
//   target_link_libraries(mptest PRIVATE mpeff )  
//    ==> target_link_libraries(mptest PUBLIC mpeff unwind)
// on ARM64 you also need to link gcc_s _before_ libunwind:
//    ==> target_link_libraries(mptest PRIVATE mpeff gcc_s unwind)
// or otherwise exceptions are not caught!
// install libunwind as: 
//   $ sudo apt-get install libunwind-dev
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#include <string.h>
static void print_backtrace(const char* msg) {
  fprintf(stderr,"-----------------------------\n");
  fprintf(stderr, "libunwind backtrace at: %s\n", msg);
  unw_cursor_t cursor; unw_context_t uc;
  
  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);
  int i = 0;
  while (unw_step(&cursor) > 0) {
    unw_word_t ip;
    unw_get_reg(&cursor, UNW_REG_IP, &ip);
    char name[128];
    unw_word_t ofs;
    unw_get_proc_name(&cursor, name, 128, &ofs);
    
    mpt_printf ("frame %2d: %8p: %s at offset %ld\n", i, (void*)ip, name, (long)ofs);
    i++;
    if (false) { //strcmp(name,"mp_stack_enter") == 0) {
      unw_proc_info_t pinfo;
      unw_get_proc_info(&cursor,&pinfo);
      bool sig = unw_is_signal_frame(&cursor);
      mpt_printf("signal: %i\n", sig);
    }
  }
  fprintf(stderr,"\n");
}

#else

// Posix simple stack trace
#include <execinfo.h>
static void print_backtrace(const char* msg) {
  fprintf(stderr,"-----------------------------\n");
  fprintf(stderr, "backtrace at: %s\n", msg);
  void* bt[128];
  int n = mp_backtrace(bt,128);
  if (n <= 0) {
    fprintf(stderr, "  unable to get a backtrace\n");
    return;
  }

  char** bts = backtrace_symbols(bt,n);
  for(int i = 0; i < n; i++) {
    void* ip = bt[i];
    char* s = bts[i];
    if (ip!=NULL && s!=NULL) {
      fprintf(stderr,"  frame %d: %8p: %s\n", i, ip, s);
    }
    else {
      break;
    }
  }
  fprintf(stderr,"\n");
  free(bts);
} 
#endif

#else  // no backtrace
static void print_backtrace(const char* msg) {
  UNUSED(msg);
}
#endif


/* ---------------------------------------------------------------------------
  Rehandling effect
-----------------------------------------------------------------------------*/

// Effect that returns its resumption
MPE_DEFINE_EFFECT1(exit, capture)
MPE_DEFINE_VOIDOP0(exit, capture)

static void* op_exit_capture(mpe_resume_t* r, void* local, void* arg) {
  UNUSED(arg); UNUSED(local);
  return r; // return the resumption as is
}
 
static void* exit_handle(mpe_actionfun_t action, void* arg) {
  static const mpe_handlerdef_t exit_hdef = { MPE_EFFECT(exit), NULL, NULL, NULL, {
    { MPE_OP_ONCE, MPE_OPTAG(exit,capture), &op_exit_capture },
    { MPE_OP_NULL, mpe_op_null, NULL }
  } };
  return mpe_handle(&exit_hdef, NULL, action, arg);
}


/* ---------------------------------------------------------------------------
  Test
-----------------------------------------------------------------------------*/


// Ask twice with an exit_capture in between
static void* rehandle_body(void* arg) {
  UNUSED(arg);
  print_backtrace("reader_ask 1");
  long x = reader_ask();   // return 1
  exit_capture();          // exit and resume under a new reader
  print_backtrace("reader_ask 2");
  long y = reader_ask();   // now it returns 2
  return mpe_voidp_long(x + y);
}

// first handler
static void* with_exit_handle(void* arg) {
  return exit_handle(&rehandle_body, arg);
}

// and second replacement handler
static void* with_resume(void* arg) {
  mpe_resume_t* r = (mpe_resume_t*)arg;
  return mpe_resume_final(r, NULL, NULL);
}

static void test(void) {
  long res;
  mpt_bench{
    void* r = reader_handle(&with_exit_handle, 1, NULL);        // reader returns 1  -- final return is a resumption from with_exit_handle
    res = mpe_long_voidp(reader_handle(&with_resume, 2, r));    // new reader returns 2 -- this resumes the resumption under a new reader
  }
  mpt_printf("rehandle  : %ld\n", res);
  mpt_assert(res == 3, "test-rehandle");
}

void rehandle_run(void) {
  test();
}
