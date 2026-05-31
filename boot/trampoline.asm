; ============================================================================
; tOS - application-processor (AP) startup trampoline.
;
; The BSP copies this blob to physical 0x8000 and sends each AP an INIT-SIPI-SIPI
; with vector 0x08, so each AP begins here in 16-bit real mode at CS=0x0800,IP=0.
; It walks real -> protected -> long mode (loading the CR3 / stack / entry the BSP
; patched into the tail) and jumps to the kernel's 64-bit ap_main in the higher
; half. The same kernel page tables the BSP uses map both this page (low identity)
; and the kernel (higher half), so the jump lands cleanly.
; ============================================================================
TRAMPO equ 0x8000           ; physical load address (must match smp.c)

org 0
bits 16
_trampoline:
    cli
    cld
    mov     ax, cs                      ; cs = 0x0800 -> [label] reaches phys 0x8000+label
    mov     ds, ax
    o32 lgdt [gdtr]
    mov     eax, cr0
    or      eax, 1                      ; PE
    mov     cr0, eax
    jmp     dword 0x08:(TRAMPO + pm32)

bits 32
pm32:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax
    mov     eax, cr4
    or      eax, 1 << 5                 ; CR4.PAE
    mov     cr4, eax
    mov     eax, [TRAMPO + ap_cr3]      ; kernel PML4 (patched in)
    mov     cr3, eax
    mov     ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or      eax, 1 << 8                 ; EFER.LME
    wrmsr
    mov     eax, cr0
    or      eax, 1 << 31                ; CR0.PG
    mov     cr0, eax
    jmp     0x18:(TRAMPO + lm64)        ; far jump into the 64-bit code segment

bits 64
lm64:
    mov     ax, 0x10
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax
    mov     eax, TRAMPO + ap_stack
    mov     rsp, [rax]                  ; per-AP kernel stack (patched in)
    mov     eax, TRAMPO + ap_entry
    mov     rax, [rax]                  ; ap_main (higher half, patched in)
    jmp     rax

align 8
gdt:
    dq 0x0000000000000000               ; null
    dq 0x00CF9A000000FFFF               ; 0x08 32-bit code
    dq 0x00CF92000000FFFF               ; 0x10 data
    dq 0x00209A0000000000               ; 0x18 64-bit code (L)
gdt_end:
gdtr:
    dw gdt_end - gdt - 1
    dd TRAMPO + gdt

; --- fields the BSP patches before each SIPI (fixed offsets, see smp.c) ---
times 0xFE0 - ($ - $$) db 0
ap_cr3:   dq 0                          ; offset 0xFE0
ap_entry: dq 0                          ; offset 0xFE8
ap_stack: dq 0                          ; offset 0xFF0
times 0x1000 - ($ - $$) db 0
