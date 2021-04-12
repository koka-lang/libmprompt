#include "test.h"

MPW_DEFINE_EFFECT1(generate,yield);


void* generate( void* stop ) {
  long n = 0;
  while (!mpw_bool_voidp(stop)) {
    stop = mpw_suspend( MPW_OPTAG(generate, yield), mpw_voidp_long(n) );
    n++;
  }
  return mpw_voidp_long(n);
}

long consume(long max) {
  mpw_cont_t* cont = mpw_new( &generate );
  long n = 0;
  void* res;    
  
  do { 
    switch( mpw_resume( MPW_EFFECT(generate), &cont, mpw_voidp_bool(n > max), &res) ) 
    {
      case 0: // yield 
        mpt_printf("yielded: %ld\n", mpw_long_voidp(res));
        break;
    }
    n++;
  } 
  while(cont != NULL);

  mpt_printf("returned from consumer\n");
  return mpw_long_voidp(res);
}


void wasm_generator_run(void) {
  long N = 9;
  long res = 0;
  mpt_bench{ res = consume(N); }
  mpt_printf("wgenerator  : %ld\n", res);
  mpt_assert(res == N+1, "wgenerator");
}