; ============================================================================
; Assembly glue: kernel entry, GDT load, and the unified interrupt path.
;
; Every interrupt vector we install (the 32 CPU exceptions, IRQ0 timer, IRQ1
; keyboard, and the int 0x80 syscall) funnels through isr_common, which saves a
; complete register frame (struct regs in cpu.h) and calls isr_dispatch(regs*).
; The dispatcher returns the frame to resume -- usually the same one, but for a
; context switch it returns *another* task's saved frame, so the restore tail
; below transparently lands us in a different task. That is the whole basis of
; preemptive multitasking here.
; ============================================================================
[bits 64]

extern isr_dispatch
extern kmain
extern __bss_start
extern __bss_end

; ---------------------------------------------------------------------------
; Kernel entry. Both boot paths (BIOS bootloader / UEFI loader) jump to the
; higher-half virtual address of _start. We install our own stack (mapped in
; every address space via the higher half), zero .bss, and call kmain.
; ---------------------------------------------------------------------------
section .text.start
global _start
_start:
    lea     rsp, [rel boot_stack_top]
    mov     r15, rdi                 ; save boot_info pointer (boot passes it in rdi)
    lea     rdi, [rel __bss_start]
    lea     rcx, [rel __bss_end]
    sub     rcx, rdi
    xor     eax, eax
    rep     stosb
    mov     rdi, r15                 ; -> kmain's first argument
    call    kmain
.hang:
    cli
    hlt
    jmp     .hang

section .text

; ---------------------------------------------------------------------------
; void gdt_flush(void *gdtr)   (rdi = &gdtr)
; Loads the GDT, reloads the data segment registers, and far-returns to reload
; CS to 0x08. The CS reload matters for the UEFI path, where the firmware left
; its own code selector loaded.
; ---------------------------------------------------------------------------
global gdt_flush
gdt_flush:
    lgdt    [rdi]
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    pop     rax                 ; return address
    push    qword 0x08          ; CS selector
    push    rax                 ; return RIP
    retfq                       ; far return -> reloads CS

; ---------------------------------------------------------------------------
; void sched_run_first(uint64_t krsp)   (rdi = saved kernel rsp of a task)
; Kicks off the very first task: point rsp at its prepared frame and fall into
; the normal restore tail, which iretq's into the task. Does not return.
; ---------------------------------------------------------------------------
global sched_run_first
sched_run_first:
    mov     rsp, rdi
    jmp     isr_restore

; ---------------------------------------------------------------------------
; Unified interrupt entry. Each stub pushes an error code (0 if the CPU did not
; provide one) and the vector number, then jumps here. isr_common saves the
; rest of the register file, calls the C dispatcher with a pointer to the frame,
; switches rsp to whatever frame the dispatcher returns, and restores + iretq.
; ---------------------------------------------------------------------------
isr_common:
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15
    mov     rdi, rsp            ; struct regs *
    cld
    call    isr_dispatch        ; -> frame to resume (in rax)
    mov     rsp, rax
isr_restore:
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 16             ; discard vec + err
    iretq

; ---------------------------------------------------------------------------
; Stubs. ISR_ERR is for vectors where the CPU pushes an error code; ISR_NOERR
; pushes a dummy 0 so every frame has the same shape.
; ---------------------------------------------------------------------------
%macro ISR_NOERR 1
global isr%1
isr%1:
    push    qword 0
    push    qword %1
    jmp     isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    push    qword %1            ; CPU already pushed the error code below this
    jmp     isr_common
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

; Hardware IRQs and the syscall gate carry no CPU error code -> push a 0.
global isr_irq0
isr_irq0:
    push    qword 0
    push    qword 0x20
    jmp     isr_common

global isr_irq1
isr_irq1:
    push    qword 0
    push    qword 0x21
    jmp     isr_common

global isr_irq4
isr_irq4:
    push    qword 0
    push    qword 0x24
    jmp     isr_common

global isr_irq12
isr_irq12:
    push    qword 0
    push    qword 0x2C
    jmp     isr_common

global isr_lapic_timer
isr_lapic_timer:
    push    qword 0
    push    qword 0x22
    jmp     isr_common

global isr_syscall
isr_syscall:
    push    qword 0
    push    qword 0x80
    jmp     isr_common

; ---------------------------------------------------------------------------
; Table of the 32 exception stub addresses, consumed by idt_init().
; ---------------------------------------------------------------------------
section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dq isr %+ i
%assign i i + 1
%endrep

; ---------------------------------------------------------------------------
; Kernel boot stack (in .bss, so it lives in the higher half and is mapped by
; every address space). _start switches to its top before calling kmain.
; ---------------------------------------------------------------------------
section .bss
    align 16
    resb 0x4000
boot_stack_top:
