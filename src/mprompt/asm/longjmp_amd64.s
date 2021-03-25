/* ----------------------------------------------------------------------------
  Copyright (c) 2021, Microsoft Research, Daan Leijen
  This is free software; you can redistribute it and/or modify it 
  under the terms of the MIT License. A copy of the License can be 
  found in the LICENSE file at the root of this distribution.

  AMD64 (x86_64) System V calling convention as used on Solaris, Linux, FreeBSD, macOS, etc.
  - <https://en.wikipedia.org/wiki/X86_calling_conventions>
  - <http://chamilo2.grenet.fr/inp/courses/ENSIMAG3MM1LDB/document/doc_abi_ia64.pdf>, page 21
  - <http://www.agner.org/optimize/calling_conventions.pdf>, page 10

  Primitives to switch stacks:
 
    typedef uint8_t mp_jmp_buf_t[MP_JMPBUF_SIZE];  // machine word aligned
  
    bool     mp_setjmp ( mp_jmp_buf_t jmpbuf );
    void     mp_longjmp( mp_jmp_buf_t jmpbuf );
    void*    mp_stack_enter(void* stack_base, void* stack_commit_limit, void* stack_limit, void (*fun)(void*), void* arg);
 
  `mp_stack_enter` enters a fresh stack and runs `fun(arg)`; it also receives 
  a (pointer to a pointer to a) return jmpbuf to which it longjmp's on return.
-----------------------------------------------------------------------------*/

/*
jmpbuf layout 
   0: rip
   8: rbx
  16: rsp
  24: rbp
  32: r12
  40: r13
  48: r14
  56: r15
  64: mxcsr, sse status register (32 bits)
  68: fpcr, fpu control word (16 bits)  
  70: unused  
  72: sizeof jmpbuf
*/

#ifdef __MACH__  
/* on macOS the compiler adds underscores to cdecl functions */
.global _mp_setjmp
.global _mp_longjmp
.global _mp_stack_enter
#else
.global mp_setjmp
.global mp_longjmp
.global mp_stack_enter
.type mp_setjmp,%function
.type mp_longjmp,%function
.type mp_stack_enter,%function
#endif

_mp_setjmp:
mp_setjmp:                   /* rdi: jmpbuf */
  movq    (%rsp), %rax       /* rip: return address is on the stack */
  leaq    8 (%rsp), %rcx     /* rsp - return address */

  movq    %rax,  0 (%rdi)    /* save registers */
  movq    %rbx,  8 (%rdi)    
  movq    %rcx, 16 (%rdi)
  movq    %rbp, 24 (%rdi)
  movq    %r12, 32 (%rdi)
  movq    %r13, 40 (%rdi)
  movq    %r14, 48 (%rdi)
  movq    %r15, 56 (%rdi)

  stmxcsr 64 (%rdi)          /* save sse control word */
  fnstcw  68 (%rdi)          /* save fpu control word */
  
  xor     %rax, %rax         /* return 0 */
  ret


_mp_longjmp:
mp_longjmp:                  /* rdi: jmp_buf */ 

  movq   8 (%rdi), %rbx       /* restore registers */
  movq  16 (%rdi), %rsp       /* switch stack */
  movq  24 (%rdi), %rbp
  movq  32 (%rdi), %r12
  movq  40 (%rdi), %r13
  movq  48 (%rdi), %r14
  movq  56 (%rdi), %r15

  /*fnclex*/                  /* clear fpu exception flags */
  ldmxcsr 64 (%rdi)           /* restore sse control word */
  fldcw   68 (%rdi)           /* restore fpu control word */
    
  movq  $1, %rax            
  jmpq  *(%rdi)               /* and jump to rip */



/* switch stack 
   rdi: gstack pointer, 
   rsi: stack commit limit    (ignored on unix)
   rdx: stack limit           (ignored on unix)
   rcx: function to run
   r8:  argument to pass to the function 
   todo: provide dwarf backtrace
*/
_mp_stack_enter:
mp_stack_enter:
  andq    $~0x0F, %rdi     /* align down to 16 bytes */
  movq    %rdi, %rsp       /* and switch stack */  
  
  movq    %r8, %rdi        /* pass the function argument */
  callq   *%rcx            /* and call the function */
  
  /* we should never get here... */
  #ifdef __MACH__
  callq   _abort
  #else
  callq   abort
  #endif


