/* ---------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Rehandle implements the "evil" example from Xie et al, ICFP'20.
  It shows how the stack can change after the call to `exit_capture` 
  with a different reader_handler on top.
-----------------------------------------------------------------------------*/
#include "rehandle.c"

#include <thread>

static void thread_rehandle() {
  printf("\n-----------------------------\nrunning in separate thread\n");
  reader_run();
  throw_run();
  amb_state_run();
  rehandle_run();
  printf("done separate thread\n-----------------------------\n");
}

void thread_rehandle_run(void) {
  auto t = std::thread(&thread_rehandle);
  t.join();  
}
