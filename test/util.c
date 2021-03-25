/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it 
  under the terms of the MIT License. A copy of the License can be 
  found in the file "license.txt" at the root of this distribution.
-----------------------------------------------------------------------------*/
#include "test.h"

#define MP_USEC_PER_SEC  1000000

void mpt_assert_at(bool condition, const char* msg, const char* fname, int line) {
  if (condition) return;
  fprintf(stderr,"test failed: %s:%d: %s\n", fname, line, msg );
}

void mpt_timer_print(mpt_timer_t start) {
  mpt_usecs_t t = mpt_timer_end(start);
  printf("%2ld.%03lds: ", (long)(t / 1000000), (long)((t % 1000000) / 1000));
}

// ----------------------------------------------------------------
// Basic timer for convenience; use micro-seconds to avoid doubles
// (2^63-1) us ~= 292471 years
// ----------------------------------------------------------------
#ifdef _WIN32
#include <windows.h>
static mpt_usecs_t mpt_to_usecs(LARGE_INTEGER t) {
  static LARGE_INTEGER mfreq; // = 0
  if (mfreq.QuadPart == 0) {
    QueryPerformanceFrequency(&mfreq);
    //mfreq.QuadPart = f.QuadPart/I64(1000000);
    if (mfreq.QuadPart == 0) mfreq.QuadPart = 1000;
  }
  // calculate in parts to avoid overflow
  int64_t secs = t.QuadPart / mfreq.QuadPart;
  int64_t frac = t.QuadPart % mfreq.QuadPart;
  mpt_usecs_t u = secs*MP_USEC_PER_SEC + ((frac*MP_USEC_PER_SEC)/mfreq.QuadPart);
  return u;
}

static mpt_usecs_t mpt_timer_now(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return mpt_to_usecs(t);
}
#else
#include <time.h>
#ifdef CLOCK_REALTIME
static mpt_usecs_t mpt_timer_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return ((mpt_usecs_t)t.tv_sec * MP_USEC_PER_SEC) + ((mpt_usecs_t)t.tv_nsec/1000);
}
#else
// low resolution timer
static mpt_usecs_t mpt_timer_now(void) {
  int64_t t = (int64_t)clock();
  // calculate in parts to avoid overflow
  int64_t secs = t / (int64_t)CLOCKS_PER_SEC;
  int64_t frac = t % (int64_t)CLOCKS_PER_SEC;
  return (secs*MP_USEC_PER_SEC + ((frac*MP_USEC_PER_SEC)/CLOCKS_PER_SEC);
}
#endif
#endif

static mpt_usecs_t mpt_timer_diff;

mpt_timer_t mpt_timer_start(void) {
  if (mpt_timer_diff == 0) {
    mpt_timer_t t0 = mpt_timer_now();
    mpt_timer_diff = mpt_timer_now() - t0;
    if (mpt_timer_diff==0) mpt_timer_diff = 1;
  }
  return mpt_timer_now();
}

mpt_usecs_t mpt_timer_end(mpt_timer_t start) {
  mpt_usecs_t end = mpt_timer_now();
  return (end - start - mpt_timer_diff);
}


// --------------------------------------------------------
// Basic process statistics
// --------------------------------------------------------

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")

static mpt_msecs_t mpt_filetime_msecs(const FILETIME* ftime) {
  ULARGE_INTEGER i;
  i.LowPart = ftime->dwLowDateTime;
  i.HighPart = ftime->dwHighDateTime;
  mpt_msecs_t msecs = (i.QuadPart / 10000); // FILETIME is in 100 nano seconds
  return msecs;
}

void mpt_process_info(mpt_msecs_t* utime, mpt_msecs_t* stime, size_t* peak_rss, size_t* page_faults, size_t* page_reclaim, size_t* peak_commit) {
  FILETIME ct;
  FILETIME ut;
  FILETIME st;
  FILETIME et;
  GetProcessTimes(GetCurrentProcess(), &ct, &et, &st, &ut);
  *utime = mpt_filetime_msecs(&ut);
  *stime = mpt_filetime_msecs(&st);

  PROCESS_MEMORY_COUNTERS info;
  GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
  *peak_rss = (size_t)info.PeakWorkingSetSize;
  *page_faults = (size_t)info.PageFaultCount;
  *peak_commit = (size_t)info.PeakPagefileUsage;
  *page_reclaim = 0;
}

#elif defined(__unix__) || defined(__unix) || defined(unix) || (defined(__APPLE__) && defined(__MACH__)) || defined(__HAIKU__)
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>

#if defined(__APPLE__) && defined(__MACH__)
#include <mach/mach.h>
#endif

#if defined(__HAIKU__)
#include <kernel/OS.h>
#endif

static mpt_msecs_t timeval_secs(const struct timeval* tv) {
  return ((mpt_msecs_t)tv->tv_sec * 1000L) + ((mpt_msecs_t)tv->tv_usec / 1000L);
}

void mpt_process_info(mpt_msecs_t* utime, mpt_msecs_t* stime, size_t* peak_rss, size_t* page_faults, size_t* page_reclaim, size_t* peak_commit) {
  struct rusage rusage;
  getrusage(RUSAGE_SELF, &rusage);
#if !defined(__HAIKU__)
#if defined(__APPLE__) && defined(__MACH__)
  *peak_rss = rusage.ru_maxrss;  // apple reports in bytes
#else
  *peak_rss = rusage.ru_maxrss * 1024;
#endif
  *page_faults = rusage.ru_majflt;
  *page_reclaim = rusage.ru_minflt;
  *peak_commit = 0;
#else
  // Haiku does not have (yet?) a way to
  // get these stats per process
  thread_info tid;
  area_info mem;
  ssize_t c;
  *peak_rss = 0;
  *page_faults = 0;
  *page_reclaim = 0;
  *peak_commit = 0;
  get_thread_info(find_thread(0), &tid);
  while (get_next_area_info(tid.team, &c, &mem) == B_OK) {
      *peak_rss += mem.ram_size;
  }
#endif
  *utime = timeval_secs(&rusage.ru_utime);
  *stime = timeval_secs(&rusage.ru_stime);
}

#else
#ifndef __wasi__
// WebAssembly instances are not processes
#pragma message("define a way to get process info")
#endif

void mpt_process_info(mpt_msecs_t* utime, mpt_msecs_t* stime, size_t* peak_rss, size_t* page_faults, size_t* page_reclaim, size_t* peak_commit) {
  *peak_rss = 0;
  *page_faults = 0;
  *page_reclaim = 0;
  *peak_commit = 0;
  *utime = 0;
  *stime = 0;
}
#endif

mpt_timer_t mpt_show_process_info_start(size_t* process_start_rss) {
  if (process_start_rss != NULL) {
    mpt_msecs_t user_time;
    mpt_msecs_t sys_time;
    size_t peak_rss;
    size_t page_faults;
    size_t page_reclaim;
    size_t peak_commit;
    mpt_process_info(&user_time, &sys_time, &peak_rss, &page_faults, &page_reclaim, &peak_commit);
    *process_start_rss = peak_rss;
  }
  return mpt_timer_start();
}

void mpt_show_process_info(FILE* f, mpt_timer_t process_start, size_t process_start_rss) {
  mpt_usecs_t wall_time = mpt_timer_end(process_start);
  mpt_msecs_t user_time;
  mpt_msecs_t sys_time;
  size_t peak_rss;
  size_t page_faults;
  size_t page_reclaim;
  size_t peak_commit;
  mpt_process_info(&user_time, &sys_time, &peak_rss, &page_faults, &page_reclaim, &peak_commit);
  size_t main_rss = peak_rss - process_start_rss;
  fprintf(f, "elapsed: %ld.%03lds, user: %ld.%03lds, sys: %ld.%03lds, rss: %zu%s, main rss: %zu%s\n", 
                  (long)(wall_time/1000000), (long)((wall_time%1000000)/1000), 
                  (long)(user_time/1000), (long)(user_time%1000), (long)(sys_time/1000), (long)(sys_time%1000), 
                  (peak_rss > 10*1024*1024 ? peak_rss/(1024*1024) : (peak_rss+1023)/1024),
                  (peak_rss > 10*1024*1024 ? "mb" : "kb"),
                  (main_rss > 10*1024*1024 ? main_rss/(1024*1024) : (main_rss+1023)/1024),
                  (main_rss > 10*1024*1024 ? "mb" : "kb") );
}