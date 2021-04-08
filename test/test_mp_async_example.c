#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <mprompt.h>

#define N 1000       // max active async workers
#define M 1000000    // total number of requests

static void* await_result(mp_resume_t* r, void* arg) {
  (void)(arg);
  return r;  // instead of resuming ourselves, we return the resumption as a "suspended async computation" (A)
}

static void* async_worker(mp_prompt_t* parent, void* arg) {
  (void)(arg);
  // start a fresh worker
  // ... do some work
  intptr_t partial_result = 0;
  // and await some request; we do this by yielding up to our prompt and running `await_result` (in the parent context!)
  mp_yield( parent, &await_result, NULL );
  // when we are resumed at some point, we do some more work 
  // ... do more work
  partial_result++;
  // and return with the result (B)
  return (void*)(partial_result);
}

static void async_workers(void) {
  mp_resume_t** workers = (mp_resume_t**)calloc(N,sizeof(mp_resume_t*));  // allocate array of N resumptions
  intptr_t count = 0;
  for( int i = 0; i < M; i++) {  // perform M connections
    int j = i % N;               // pick an active worker
    // if the worker is actively waiting (suspended), resume it
    if (workers[j] != NULL) {  
      count += (intptr_t)mp_resume(workers[j], NULL);  // (B)
      workers[j] = NULL;
    }
    // and start a fresh worker and wait for its first yield (suspension). 
    // the worker returns its own resumption as a result.
    if (i < (M - N)) {
      workers[j] = (mp_resume_t*)mp_prompt( &async_worker, NULL );  // (A)
    }
  }
  printf("ran %zd workers\n", count);
}

int main() {
  async_workers();
  return 0;
}
