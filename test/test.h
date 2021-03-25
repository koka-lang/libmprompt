/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it
  under the terms of the MIT License. A copy of the license can be
  found in the "LICENSE" file at the root of this distribution.
-----------------------------------------------------------------------------*/
#pragma once

#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <mphandler.h>


#ifdef __cplusplus
extern "C" {
#endif

#define UNUSED(x)  (void)(x)
#define mpt_assert(cond,msg)  mpt_assert_at(cond,msg,__FILE__,__LINE__)

void mpt_assert_at(bool condition, const char* msg, const char* fname, int line);

/*-----------------------------------------------------------------
  Tests
-----------------------------------------------------------------*/

void reader_run(void);
void state_run(void);
void counter_run(void);
void countern_run(void);
void mstate_run(void);
void nqueens_run(void);
void amb_run(void);
void amb_state_run(void);


#ifdef __cplusplus
void throw_run(void);
#else
// dummies in C
static inline void throw_run(void) { }
#endif

/*-----------------------------------------------------------------
  Standard effects
-----------------------------------------------------------------*/

// Reader
MPE_DECLARE_EFFECT1(reader, ask)
MPE_DECLARE_OP0(reader, ask, long)

void* reader_handle(mpe_actionfun_t action, long init, void* arg);   // tail_noop
void* greader_handle(mpe_actionfun_t action, long init, void* arg);  // general 

// State
MPE_DECLARE_EFFECT2(state, get, set)
MPE_DECLARE_OP0(state, get, long)
MPE_DECLARE_VOIDOP1(state, set, long)

void* state_handle(mpe_actionfun_t action, long init, void* arg);    // tail_noop
void* ustate_handle(mpe_actionfun_t action, long init, void* arg);   // tail
void* gstate_handle(mpe_actionfun_t action, long init, void* arg);   // general
void* ostate_handle(mpe_actionfun_t action, long init, void* arg);   // scoped_once

// Ambiguity
MPE_DECLARE_EFFECT1(amb, flip)
MPE_DECLARE_OP0(amb, flip, bool)

typedef struct _bnode* blist;  // void* lists
blist amb_handle(mpe_actionfun_t* action, void* arg);

// Choice
MPE_DECLARE_EFFECT2(choice, choose, fail)
MPE_DECLARE_OP1(choice, choose, long, long)
MPE_DECLARE_VOIDOP0(choice, fail)

blist choice_handle(mpe_actionfun_t* action, void* arg);


/*-----------------------------------------------------------------
  Statistics
-----------------------------------------------------------------*/
typedef int64_t mpt_msecs_t;
typedef int64_t mpt_usecs_t;
typedef mpt_usecs_t mpt_timer_t;

mpt_timer_t mpt_timer_start(void);
mpt_usecs_t mpt_timer_end(mpt_timer_t start);

void mpt_timer_print(mpt_timer_t start);

#define mpt_bench  for(mpt_timer_t t = mpt_timer_start(); t != 0; mpt_timer_print(t), t = 0)


void mpt_process_info(mpt_msecs_t* utime, mpt_msecs_t* stime, size_t* peak_rss,
  size_t* page_faults, size_t* page_reclaim, size_t* peak_commit);

mpt_timer_t mpt_show_process_info_start(size_t* process_start_rss);
void mpt_show_process_info(FILE* f, mpt_timer_t process_start, size_t process_start_rss);



/*-----------------------------------------------------------------
  Mini implementation of list of void*'s (for amb and choice)
-----------------------------------------------------------------*/

struct _bnode {
  struct _bnode* next;
  void* value;
};


#define mpe_voidp_blist(l)  mpe_voidp_ptr(l)
#define mpe_blist_voidp(v)  ((blist)(mpe_ptr_voidp(v)))


#define blist_nil  ((blist)(NULL))

static inline blist blist_cons(void* val, blist tail) {
  blist res = (blist)malloc(sizeof(struct _bnode));
  res->next = tail;
  res->value = val;
  return res;
}

static inline blist blist_single(void* val) {
  return blist_cons(val, NULL);
}

static inline blist blist_copy(blist xs) {
  if (xs == NULL) return xs;
  return blist_cons(xs->value, blist_copy(xs->next));
}

static inline blist blist_appendto(blist xs, blist ys)
{
  if (xs == NULL) return ys;
  blist tl = xs;
  while (tl->next != NULL) { tl = tl->next; }
  tl->next = ys;
  return xs;
}


static inline void blist_free(blist xs) {
  while (xs != NULL) {
    blist next = xs->next;
    free(xs);
    xs = next;
  }
}

static inline long blist_length(blist xs) {
  long count = 0;
  while (xs != NULL) {
    count++;
    xs = xs->next;
  }
  return count;
}

static inline void blist_println(blist xs, void (*print_elem)(void*)) {
  printf("[");
  while (xs != NULL) {
    print_elem(xs->value);
    xs = xs->next;
    if (xs != NULL) printf(",");
  }
  printf("]\n");
}


#ifdef __cplusplus
}
#endif