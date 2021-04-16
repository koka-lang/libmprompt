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
static void mp_fputs(mp_output_fun* out, const char* prefix, const char* message) {
  int fd = -1;
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
static void mp_vfprintf( mp_output_fun* out, const char* prefix, const char* fmt, va_list args ) {
  char buf[256];
  if (fmt==NULL) return;
  vsnprintf(buf,sizeof(buf)-1,fmt,args);
  mp_fputs(out, prefix,buf);
}

#ifndef NDEBUG
static void mp_show_trace_message(const char* fmt, va_list args) {
  mp_vfprintf( mp_output_handler, "libmprompt: trace: ", fmt, args);
}
#endif 

static void mp_show_error_message(const char* fmt, va_list args) {
  mp_vfprintf( mp_output_handler, "libmprompt: error: ", fmt, args);
}

#if defined(_WIN32)
#include <windows.h>
static void mp_show_system_error_message(const char* fmt, va_list args) {
  DWORD err = GetLastError();
  mp_vfprintf(mp_output_handler, "libmprompt: error: ", fmt, args);
  if (err != ERROR_SUCCESS) {
    char buf[256];
    snprintf(buf, sizeof(buf) - 1, "0x%xd: ", err);
    size_t len = strlen(buf);
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY, NULL, err, 0, buf+len, (DWORD)(sizeof(buf) - len - 1), NULL);
    strcat_s(buf, "\n");
    mp_fputs(mp_output_handler,  "            code : ", buf);
  }
}
#else
#include <string.h>
static void mp_show_system_error_message(const char* fmt, va_list args) {
  int err = errno;
  mp_vfprintf(mp_output_handler, "libmprompt: error: ", fmt, args);
  if (err != 0) {
    char buf[256];
    snprintf(buf, sizeof(buf) - 1, "%d: %s\n", err, strerror(err));
    mp_fputs(mp_output_handler,  "            code : ", buf);
  }
}
#endif


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
  mp_show_error_message(fmt, args);
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

void mp_system_error_message(int err, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  mp_show_system_error_message(fmt, args);
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


/* ----------------------------------------------------------------------------
  Guard cookie
  To get an initial secure random context we rely on the OS:
  - Windows     : RtlGenRandom (or BCryptGenRandom but needs an extra DLL)
  - OSX,bsd,wasi: arc4random_buf
  - Linux       : getrandom (or /dev/urandom)
  If we cannot get good randomness, we fall back to weak randomness based on a timer and ASLR.
-----------------------------------------------------------------------------*/

#if defined(WIN32)
/*
#pragma comment (lib,"bcrypt.lib")
#include <windows.h>
#include <bcrypt.h>
static bool os_random_buf(void* buf, size_t buf_len) {
  return (BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)buf_len, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0);
}
*/
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#define RtlGenRandom  SystemFunction036
#ifdef __cplusplus
extern "C" {
#endif
  BOOLEAN NTAPI RtlGenRandom(PVOID RandomBuffer, ULONG RandomBufferLength);
#ifdef __cplusplus
}
#endif
static bool os_random_buf(void* buf, size_t buf_len) {
  mp_assert_internal(buf_len >= sizeof(uintptr_t));
  memset(buf, 0, buf_len);
  RtlGenRandom(buf, (ULONG)buf_len);
  return (((uintptr_t*)buf)[0] != 0);  // sanity check (but RtlGenRandom should never fail)
}
#elif defined(ANDROID) || defined(XP_DARWIN) || defined(__APPLE__) || defined(__DragonFly__) || \
      defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
      defined(__wasi__)
#include <stdlib.h>
static bool os_random_buf(void* buf, size_t buf_len) {
  arc4random_buf(buf, buf_len);
  return true;
}
#elif defined(__linux__)
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
static bool os_random_buf(void* buf, size_t buf_len) {
  // Modern Linux provides `getrandom` but different distributions either use `sys/random.h` or `linux/random.h`
  // and for the latter the actual `getrandom` call is not always defined.
  // (see <https://stackoverflow.com/questions/45237324/why-doesnt-getrandom-compile>)
  // We therefore use a syscall directly and fall back dynamically to /dev/urandom when needed.
#ifdef SYS_getrandom
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK (1)
#endif
  static volatile uintptr_t no_getrandom; // = 0
  if (no_getrandom == 0) {
    ssize_t ret = syscall(SYS_getrandom, buf, buf_len, GRND_NONBLOCK);
    if (ret >= 0) return (buf_len == (size_t)ret);
    if (ret != ENOSYS) return false;
    no_getrandom = 1; // don't call again, and fall back to /dev/urandom
  }
#endif
  int flags = O_RDONLY;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
  int fd = open("/dev/urandom", flags, 0);
  if (fd < 0) return false;
  size_t count = 0;
  while (count < buf_len) {
    ssize_t ret = read(fd, (char*)buf + count, buf_len - count);
    if (ret <= 0) {
      if (errno != EAGAIN && errno != EINTR) break;
    }
    else {
      count += ret;
    }
  }
  close(fd);
  return (count == buf_len);
}
#else
static bool os_random_buf(void* buf, size_t buf_len) {
  return false;
}
#endif

#if defined(WIN32)
#include <Windows.h>
#elif defined(__APPLE__)  
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

static uint64_t os_random_weak(void) {
  uint64_t x = (uint64_t)&os_random_weak ^ 0x853C49E6748FEA9B; // hopefully, ASLR makes the address random
  do {
#if defined(WIN32)
    LARGE_INTEGER pcount;
    QueryPerformanceCounter(&pcount);
    x ^= (uint64_t)(pcount.QuadPart);
#elif defined(__APPLE__)
    x ^= mach_absolute_time();
#else
    struct timespec time;
#if defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &time);
#else
    clock_gettime(CLOCK_REALTIME, &time);
#endif  
    x ^= (uint64_t)time.tv_sec << 17;
    x ^= (uint64_t)time.tv_nsec;
#endif
  } while (x == 0);
  return x;
}

#if INT64_MAX == INTPTR_MAX
uintptr_t mp_guard_cookie = 0x00002B992DDFA232;
#else
uintptr_t mp_guard_cookie = 0x0040E64E;
#endif

void mp_guard_init(void) {
  uint64_t key;
  if (!os_random_buf(&key, sizeof(key))) {   // try secure random first
    key = os_random_weak();                  // .. and otherwise fall back to weaker random
  }
  mp_guard_cookie = key;
}