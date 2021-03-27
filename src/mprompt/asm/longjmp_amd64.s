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
    void* mp_stack_enter(void* stack_base, void* stack_commit_limit, void* stack_limit, mp_jmpbuf_t** return_jmp, 
                         void (*fun)(void* arg, void* trapframe), void* arg);

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



/* enter stack 
   rdi: gstack pointer, 
   rsi: stack commit limit    (ignored on unix)
   rdx: stack limit           (ignored on unix)
   rcx: jmpbuf_t**            return jmpbuf indirect pointer (used for backtraces only)
   r8:  function to run
   r9:  argument to pass to the function 
*/

/* DWARF unwind info instructions: <http://dwarfstd.org/doc/DWARF5.pdf> 
   Register mapping: <https://raw.githubusercontent.com/wiki/hjl-tools/x86-psABI/x86-64-psABI-1.0.pdf> (page 61)
*/
#define DW_expression             0x10        
#define DW_val_expression         0x16        
#define DW_OP_deref               0x06        
#define DW_OP_breg(r)             (0x70+r)    /* push `register + ofs` */
#define DW_OP_plus_uconst         0x23
#define DW_OP_lit(n)              (0x30+n)
#define DW_OP_minus               0x1C
#define DW_REG_rip                16
#define DW_REG_rax                0
#define DW_REG_rdx                1
#define DW_REG_rcx                2
#define DW_REG_rbx                3
#define DW_REG_rbp                6
#define DW_REG_rsp                7

_mp_stack_enter:
mp_stack_enter:
  .cfi_startproc
  movq    (%rsp), %r11        /* rip */
  
  /* switch stack */
  andq    $~0x0F, %rdi        /* align down to 16 bytes */
  movq    %rdi, %rsp          /* and switch stack */  
  pushq   %r11                

  /* unwind info: set rbx, rbp, rsp, and rip from the current mp_jmpbuf_t return point 
     todo: can we do this more efficiently using more regular cfi directives? Perhaps we just need to load the jmpbuf ptr?
     todo: on macOS, this seems to trigger a bug when running in lldb on a throw
     note: the expression calculates the _address_ that contains the new value of the target register.
           the return jmpbuf_t* is: (%rbx) == DW_OP_breg(DW_REG_rbx), 0, DW_OP_DEREF
  */
  .cfi_escape DW_expression, DW_REG_rbx, 5, DW_OP_breg(DW_REG_rbx), 0, DW_OP_deref, DW_OP_plus_uconst, 8
  .cfi_escape DW_expression, DW_REG_rbp, 5, DW_OP_breg(DW_REG_rbx), 0, DW_OP_deref, DW_OP_plus_uconst, 24 
  .cfi_escape DW_expression, DW_REG_rsp, 5, DW_OP_breg(DW_REG_rbx), 0, DW_OP_deref, DW_OP_plus_uconst, 16
  .cfi_escape DW_expression, DW_REG_rip, 3, DW_OP_breg(DW_REG_rbx), 0, DW_OP_deref

  /* use rbp to be more compatible with unwinding */
  pushq   %rbp
  .cfi_adjust_cfa_offset 8 
  movq    %rsp, %rbp
  .cfi_def_cfa_register %rbp 

  pushq   %rbx                /* save non-volatile rbx */
  .cfi_adjust_cfa_offset 8
  /* .cfi_rel_offset %rbx, 0 */  /* hide to ensure unwinding uses the current rbx */
  movq    %rcx, %rbx          /* and put the jmpbuf_t** in rbx so it can be used for a backtrace */
  .cfi_register %rcx, %rbx 
  subq    $8, %rsp            /* align stack */
  .cfi_adjust_cfa_offset 8
  
  movq    %r9, %rdi           /* pass the function argument */
  xorq    %rsi, %rsi          /* no trap frame */
  callq   *%r8                /* and call the function */
  
  /* we should never get here (but the called function should longjmp, see `mprompt.c:mp_mprompt_stack_entry`) */
  #ifdef __MACH__
  callq   _abort
  #else
  callq   abort
  #endif

  movq    (%rbx), %rdi        /* load jmpbuf_t* and longjmp */
  jmp     mp_longjmp        

  mov     %rbp, %rsp
  pop     %rsp
  ret
  .cfi_endproc
