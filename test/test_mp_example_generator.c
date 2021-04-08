/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.

  Example of using low-level prompts for a generator
-----------------------------------------------------------------------------*/
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <mprompt.h>

typedef void* (iterator_fun)(intptr_t arg);

typedef struct iter_env_s {
  iterator_fun* iter;
  intptr_t      arg;
} iter_env_t;

// Executed for each yielded value (as the body of a foreach)
static void* gen_yield(mp_resume_t* r, void* arg) {
  iter_env_t* env = (iter_env_t*)arg;
  mp_resume_tail( r, (env->iter)( env->arg) );
  return NULL;
}

// Generator
static void* gen_action(mp_prompt_t* p, void* arg) {
  iter_env_t* env = (iter_env_t*)arg;
  intptr_t n = env->arg;
  for(intptr_t i = 0; i < n; i++) {
    env->arg = i;
    mp_yield(p, &gen_yield, env);
  };
  return NULL;
}

// Foreach
static void gen_foreach( iterator_fun* iter, intptr_t n ) {
  iter_env_t env = { iter, n };
  mp_prompt( &gen_action, &env );
}

// Our foreach body
static void* my_foreach_body(intptr_t i) {
  printf("%zd.", i);
  return NULL;
}

int main() {
  gen_foreach( my_foreach_body, 10);
  printf("\ndone\n");
}
