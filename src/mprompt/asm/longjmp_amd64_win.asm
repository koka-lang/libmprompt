; -----------------------------------------------------------------------------------------------
; Copyright (c) 2021, Microsoft Research, Daan Leijen
; This is free software; you can redistribute it and/or modify it 
; under the terms of the MIT License. A copy of the License can be 
; found in the LICENSE at the root of this distribution.
;
; AMD64 (x86_64) calling convention as used on Windows 
; see: 
; - https://en.wikipedia.org/wiki/X86_calling_conventions
; - https://msdn.microsoft.com/en-us/library/ms235286.aspx
; - http://www.agner.org/optimize/calling_conventions.pdf
;
; Primitives to switch stacks:
;
; typedef uint8_t mp_jmp_buf_t[MP_JMPBUF_SIZE];  // machine word aligned
;
; bool  mp_setjmp(mp_jmpbuf_t* jmp);
; void  mp_longjmp(mp_jmpbuf_t* jmp);
; void* mp_stack_enter(void* stack_base, void* stack_commit_limit, void* stack_limit, mp_jmpbuf_t** return_jmp, 
;                      void (*fun)(void* arg,void* trapframe), void* arg);
;
; `mp_stack_enter` enters a fresh stack and runs `fun(arg)`
; ----------------------------------------------------------------------------------------------

; -------------------------------------------------------
; note: we use 'movdqu' instead of 'movdqa' since we cannot
;       guarantee proper alignment.
;
; jmpbuf layout
;   0: rip
;   8; rsp
;  16: rbx
;  24: rbp
;  32: rsi
;  40: rdi
;  48: r12
;  56: r13
;  64: r14
;  72: r15
;  80: xmm6
;      ... (128-bit sse registers)
; 224: xmm15
; 240: gs:0x08,   stack base  (highest address)
; 248: gs:0x10,   stack limit (lowest address)
; 256: gs:0x1478, stack guarantee (deallocation stack)  
; 264: gs:0x20,   fiber data                            
; 272: mxcrs, sse control word (32 bit)
; 276: fpcr, fpu control word (16 bit)
; 278: unused (16 bit)
; 280: sizeof jmpbuf

MP_JMPBUF_SIZE equ 280


; note: we may skip saving the stack guarantee and fiber data?
; -------------------------------------------------------

.CODE 

EXTRN abort : PROC

; bool mp_setjmp(mp_jmpbuf_t* savebuf);
; rcx: savebuf
mp_setjmp PROC
  mov     r8, [rsp]        ; rip (save the return address)
  lea     r9, [rsp+8]      ; rsp (minus return address)
  
  mov     [rcx+0],  r8     ; save registers 
  mov     [rcx+8],  r9
  mov     [rcx+16], rbx    
  mov     [rcx+24], rbp
  mov     [rcx+32], rsi
  mov     [rcx+40], rdi
  mov     [rcx+48], r12
  mov     [rcx+56], r13
  mov     [rcx+64], r14
  mov     [rcx+72], r15
    
  mov     r8,  gs:[8]      ; (pre)load stack limits from the TIB
  mov     r9,  gs:[16]
  mov     r10, gs:[32]
  mov     r11, gs:[5240]
  
  movdqu  [rcx+80],  xmm6  ; save sse registers
  movdqu  [rcx+96],  xmm7
  movdqu  [rcx+112], xmm8
  movdqu  [rcx+128], xmm9
  movdqu  [rcx+144], xmm10 
  movdqu  [rcx+160], xmm11
  movdqu  [rcx+176], xmm12
  movdqu  [rcx+192], xmm13
  movdqu  [rcx+208], xmm14
  movdqu  [rcx+224], xmm15

  mov     [rcx+240], r8    ; save stack limits and fiber data
  mov     [rcx+248], r9
  mov     [rcx+256], r11 
  mov     [rcx+264], r10
  
  stmxcsr [rcx+272]        ; save sse control word
  fnstcw  [rcx+276]        ; save fpu control word
  
  xor     rax, rax         ; return 0 at first
  ret

mp_setjmp ENDP


; void  mp_longjmp(mp_jmpbuf_t* jmpbuf);
; rcx: jmpbuf 
mp_longjmp PROC

  mov     r11,   [rcx]         ; load rip in r11
  mov     rsp,   [rcx+8]       ; restore rsp
    
  mov     rbx,   [rcx+16]      ; restore registers 
  mov     rbp,   [rcx+24]
  mov     rsi,   [rcx+32]  
  mov     rdi,   [rcx+40]
  mov     r12,   [rcx+48]
  mov     r13,   [rcx+56]
  mov     r14,   [rcx+64]
  mov     r15,   [rcx+72]
  
  movdqu  xmm6,  [rcx+80]      ; restore sse registers
  movdqu  xmm7,  [rcx+96]
  movdqu  xmm8,  [rcx+112]
  movdqu  xmm9,  [rcx+128]
  movdqu  xmm10, [rcx+144]
  movdqu  xmm11, [rcx+160]
  movdqu  xmm12, [rcx+176]
  movdqu  xmm13, [rcx+192]
  movdqu  xmm14, [rcx+208]
  movdqu  xmm15, [rcx+224]
  
  mov     rax, [rcx+240]       ; load stack limits and fiber data
  mov     r8,  [rcx+248]
  mov     r9,  [rcx+256]
  mov     r10, [rcx+264]
  
  ldmxcsr [rcx+272]            ; restore sse control word
  ; fnclex                     ; clear fpu exception flags
  fldcw   [rcx+276]            ; restore fpu control word
    
  mov     gs:[8],    rax       ; restore stack limits and fiber data  
  mov     gs:[16],   r8
  mov     gs:[32],   r10
  mov     gs:[5240], r9  

  mov     rax, 1               ; return 1 to setjmp
  jmp     r11                  ; and jump to the rip

mp_longjmp ENDP



; void* mp_stack_enter(void* stack_base, void* stack_commit_limit, void* stack_limit, mp_jmpbuf_t** return_jmp, 
;                       (*fun)(void* arg,void* trapframe), void* arg);
;
; rcx: stack_base
; rdx: stack_commit_limit   (only used on windows for stack probes)
; r8 : stack_limit          (only used on windows for stack probes)
; r9 : jmpbuf_t**           pointer to return jmpbuf pointer; for now unused but may be used for unwinding
; [rsp+40] : fun            (just above shadow space + return address)
; [rsp+48] : arg              
;
; On windows, gs points to the TIB: 
; - gs:8 contains the stack base (highest address), 
; - gs:16 the limit (lowest address)
; - gs:5240 the stack guarantee; set to limit on entry
; Before a call, we need to reserve 32 bytes of shadow space for the callee to spill registers in.
mp_stack_enter_plain PROC FRAME 
  mov     r10, rsp         ; old rsp
  mov     r11, [rsp]       ; rip
  sub     rsp, 40          ; home area + align
.ALLOCSTACK 40
.ENDPROLOG

  mov     gs:[8], rcx      ; set new stack base
  mov     gs:[16], rdx     ; commit limit for stack probes (i.e. __chkstk)
  mov     gs:[5240], r8    ; (virtual) stack limit   

  and     rcx, NOT 15      ; align new stack base
  mov     rsp, rcx         ; switch the stack
  push    r11              ; help unwinding code by putting in return rip (remove?)
  push    r9               ; save return jmpbuf_t**
  sub     rsp, 32          ; home space 

  mov     rax, [r10+40]    ; fun
  mov     rcx, [r10+48]    ; set arg from old stack
  xor     rdx, rdx         ; no trap frame
  call    rax              ; and call the function (it should never return but use longjmp)
  
  ; we should never reach this...
  call    abort        

  mov     rcx, [rsp+40]    ; load return jmpbuf_t* in rcx
  mov     rcx, [rcx]       
  jmp     mp_longjmp

mp_stack_enter_plain ENDP


; Push a trap frame so it can be unwound
; This work currently best with unwinding, but it is still not quite enough since the stack 
; limits in the thread local TIB are not updated; 
; For exceptions we will need to catch and propagate manually through prompt points anyways.
; But perhaps this can become useful in the future?
mp_stack_enter PROC FRAME
  mov     r10, rsp          ; save rsp in r10
  mov     r11, [r10]        ; rip     
  
  ; switch stack
  and     rcx, NOT 15       ; align
  mov     rsp, rcx
  
  push    r11               ; simulate call for unwinding

  ; for the trap frame, we only need to set rsp and rip:
  ; <https://docs.microsoft.com/en-us/cpp/build/exception-handling-x64>
  ; RSP+40  SS              ; we use stack base
  ; RSP+32  Old RSP 
  ; RSP+24  EFLAGS          ; we use stack commit limit
  ; RSP+16  CS              ; we use stack reserved size (deallocation limit)
  ; RSP+8   RIP  
  ; RSP+0   error code      ; we use the return jmpbuf_t**
  push    rcx
  lea     rax, [r10+8]      ; return rsp (minus return address)
  push    rax               
  push    rdx
  push    r8
  push    r11               ; return rip
  push    r9                ; return point
  ; and fall through (with un-aligned stack)

.PUSHFRAME code            ; unwind rsp - 48
  sub     rsp, 40          ; reserve home area for calls + align
.ALLOCSTACK 40    
.ENDPROLOG

  mov     gs:[8], rcx      ; set new stack base
  mov     gs:[16], rdx     ; commit limit for stack probes (i.e. __chkstk)
  mov     gs:[5240], r8    ; (virtual) stack limit   

  mov     rax, [r10+40]    ; set fun from old stack
  mov     rcx, [r10+48]    ; set arg from old stack
  lea     rdx, [rsp+40]    ; set rdx to the trap frame address (so we can update it for unwinding/backtraces)
  call    rax              ; and call the function (which should never return but use longjmp)
  
  ; we should never reach this...
  call    abort           

  mov     rcx, [rsp+40]    ; load jmpbuf_t*
  mov     rcx, [rcx]
  jmp     mp_longjmp       ; and longjmp

mp_stack_enter ENDP

END
