/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/

#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>

#include "mprompt.h"
#include "internal/util.h"


// Abstract over output and error handlers
// todo: add functions to register custom handlers.
static mp_output_fun* mp_output_handler;
static void*          mp_output_arg;
static mp_error_fun*  mp_error_handler;
static void*          mp_error_arg;


// use `write` and `vsnprintf` so the message functions  are safe to call from signal handlers
#if defined(_WIN32)
#include <io.h>
#define write(fd,buf,len) _write(fd,buf,(unsigned)len)
#else
#include <unistd.h>
#endif

// low-level output
static void mp_fputs(const char* prefix, const char* message) {
  int fd = -1;
  mp_output_fun* out = mp_output_handler;
  if (out == NULL || (intptr_t)out == 2 || (FILE*)out == stderr) fd = 2;
  else if ((intptr_t)out == 1 || (FILE*)out == stdout) fd = 1;
  if (fd >= 0) { 
    ssize_t n = 0; // supress warnings
    if (prefix != NULL) n += write(fd, prefix, strlen(prefix));
    n += write(fd, message, strlen(message));
  }
  else {
    if (prefix != NULL) out(prefix, mp_output_arg);
    out(message, mp_output_arg);
  }
}

// Formatted messages
static void mp_vfprintf( const char* prefix, const char* fmt, va_list args ) {
  char buf[256];
  if (fmt==NULL) return;
  vsnprintf(buf,sizeof(buf)-1,fmt,args);
  mp_fputs(prefix,buf);
}

#ifndef NDEBUG
static void mp_show_trace_message(const char* fmt, va_list args) {
  mp_vfprintf( "libmprompt: trace: ", fmt, args);
}
#endif 

static void mp_show_error_message(const char* fmt, va_list args) {
  mp_vfprintf( "libmprompt: error: ", fmt, args);
}

void mp_trace_message(const char* fmt, ...) {
#ifdef NDEBUG
  MP_UNUSED(fmt);
#else
  va_list args;
  va_start(args, fmt);
  mp_show_trace_message(fmt, args);
  va_end(args);
#endif
}

void mp_error_message(int err, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  //fprintf(stderr, "errno: %s\n", strerror(err));
  //mp_show_error_message(fmt, args);
  va_end(args);
  // and call the error handler which may abort (or return normally)
  if (mp_error_handler != NULL) {
    mp_error_handler(err, mp_error_arg);
  }
  // default handler
  else if (err == EFAULT) {
    abort();
  }
}

mp_decl_noreturn void mp_fatal_message(int err, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  mp_show_error_message(fmt, args);
  va_end(args);
  // call the error handler which may abort (or return normally)
  if (mp_error_handler != NULL) {
    mp_error_handler(err, mp_error_arg);
  }
  // and always abort anyways
  abort();
}

mp_decl_noreturn void mp_unreachable(const char* msg) {
  mp_assert(false);
  mp_fatal_message(EINVAL,"unreachable code reached: %s\n", msg);
}