; ============================================================================
; tOS - BIOS bootloader (stage 1 + stage 2)
;
; Stage 1 (boot sector) uses BIOS LBA reads (int 13h / AH=42h) to pull stage 2
; and the kernel off the disk. Stage 2 checks for long-mode support (CPUID),
; walks real -> protected -> long mode, and jumps to the kernel at 0x10000.
;
; The first 2 MiB are identity-mapped as a single user-accessible page so a
; ring-3 program (linked into the kernel) can later run from it.
;
; Disk layout (built by the Makefile):
;   LBA 0              : this boot sector (stage 1) + MBR partition table
;   LBA 1..7           : rest of this binary (stage 2)            -> 0x7e00
;   LBA 8..(8+N-1)     : kernel.bin (N = KERNEL_SECTORS, -D'd in) -> 0x10000
;   LBA 2048..6143     : the tosfs partition (the FS the kernel mounts)
;
; The boot sector carries an MBR partition table whose one entry marks the
; tosfs partition; the kernel finds it by reading the table (see fs.c). The user
; programs (init/shell/ticker) are files in that partition, loaded by the kernel.
; ============================================================================

[org 0x7c00]
[bits 16]

BOOT_SECTORS    equ 8
STAGE2_SECTORS  equ BOOT_SECTORS - 1
KERNEL_ADDR     equ 0x10000              ; temp load buffer (BIOS DMAs below 1 MiB)
KERNEL_PHYS     equ 0x200000             ; final physical address (2 MiB-aligned for the 2 MiB page)
KERNEL_VMA      equ 0xffffffff80000000   ; higher-half virtual base

; --- VBE / boot_info handoff (real-mode scratch + the struct passed in RDI) ---
BOOT_INFO       equ 0x7000               ; struct boot_info (survives into the kernel)
VBE_CTRL        equ 0x2000               ; VBE controller info (transient; page tables reuse it)
VBE_MODE        equ 0x2400               ; VBE mode info (transient)

%ifndef KERNEL_SECTORS
%define KERNEL_SECTORS 64
%endif

FS_PART_LBA   equ 2048           ; tosfs partition start (1 MiB in, past the kernel)
FS_PART_CNT   equ 4096           ; tosfs partition size in sectors (== TOSFS_DISK_SECTORS)
FS_PART_TYPE  equ 0x7f           ; MBR type byte for the tosfs partition (== TOSFS_PART_TYPE)

; ---------------------------------------------------------------------------
; Stage 1: 16-bit real mode
; ---------------------------------------------------------------------------
start:
    cli
    cld
    xor     ax, ax
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     sp, 0x7c00
    mov     [boot_drive], dl

    ; --- LBA-read stage 2 (to 0x7e00) and the kernel (to 0x10000) ---
    mov     si, dap_stage2
    mov     ah, 0x42
    mov     dl, [boot_drive]
    int     0x13
    jc      disk_error

    ; The kernel can exceed the BIOS's ~127-sector-per-call DAP limit, so read it
    ; in <=64-sector chunks (32 KiB each, 64 KiB-aligned every step so no transfer
    ; crosses a 64 KiB boundary), advancing the LBA and the load segment each time.
    ; AX holds the sectors still to read. NOTE: the int 13h call needs AH=0x42,
    ; which clobbers the high byte of our counter -- so we PUSH AX *before* loading
    ; AH (an earlier version pushed after, baking 0x42 into AX; the loop counter
    ; then never reached zero and walked the load segment off into garbage).
    mov     word [dap_kernel.count], 64
    mov     ax, KERNEL_SECTORS
.kload:
    test    ax, ax
    jz      .kdone
    cmp     ax, 64
    jae     .kread
    mov     [dap_kernel.count], ax        ; final partial chunk
.kread:
    mov     si, dap_kernel
    mov     dl, [boot_drive]
    push    ax                            ; save the counter before AH is clobbered
    mov     ah, 0x42
    int     0x13
    jc      disk_error
    pop     ax
    mov     cx, [dap_kernel.count]
    sub     ax, cx
    add     word [dap_kernel.lba], cx     ; next LBA
    mov     bx, cx
    shl     bx, 5                          ; sectors -> paragraphs (*512/16 = *32)
    add     word [dap_kernel.seg], bx     ; next load segment
    jmp     .kload
.kdone:

    jmp     stage2_rm                ; continue in stage 2 (still real mode: do VBE there)

disk_error:
    mov     si, msg_disk_err
.print:
    lodsb
    test    al, al
    jz      .hang
    mov     ah, 0x0e
    int     0x10
    jmp     .print
.hang:
    hlt
    jmp     .hang

boot_drive:   db 0
msg_disk_err: db "disk read error", 0

; --- Disk Address Packets for the BIOS LBA read service ---
align 4
dap_stage2:
    db 0x10                       ; packet size
    db 0
    dw STAGE2_SECTORS             ; sectors to read
    dw 0x7e00                     ; buffer offset
    dw 0x0000                     ; buffer segment
    dq 1                          ; starting LBA

dap_kernel:
    db 0x10
    db 0
.count: dw KERNEL_SECTORS         ; sectors this call (rewritten per chunk)
.off:   dw 0x0000                 ; offset 0 ...
.seg:   dw KERNEL_ADDR >> 4       ; ... within segment 0x1000  => 0x10000 (advances per chunk)
.lba:   dq BOOT_SECTORS           ; kernel starts right after this binary (advances per chunk)

; ---------------------------------------------------------------------------
; 32-bit GDT (flat code + data)
; ---------------------------------------------------------------------------
gdt32_start:
    dq 0x0000000000000000
gdt32_code:
    dw 0xffff
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
gdt32_data:
    dw 0xffff
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
gdt32_end:

gdt32_descriptor:
    dw gdt32_end - gdt32_start - 1
    dd gdt32_start

CODE_SEG32 equ gdt32_code - gdt32_start
DATA_SEG32 equ gdt32_data - gdt32_start

; --- MBR partition table at offset 446: one entry for the tosfs partition ---
times 446 - ($ - $$) db 0        ; pad the boot code out to the partition table
    db 0x00                      ; status: not bootable
    db 0xFE, 0xFF, 0xFF          ; CHS of first sector (LBA-only marker)
    db FS_PART_TYPE              ; partition type
    db 0xFE, 0xFF, 0xFF          ; CHS of last sector
    dd FS_PART_LBA               ; first LBA (little-endian)
    dd FS_PART_CNT               ; sector count
    times 16 * 3 db 0            ; partition entries 2-4: unused
dw 0xaa55

; ===========================================================================
; Stage 2 real-mode entry: query VBE for a linear framebuffer (so the GUI works
; on BIOS too), then enable A20 and switch to protected mode. If no suitable VBE
; mode is found, boot_info.console stays 0 (VGA text) and the OS falls back.
; ===========================================================================
[bits 16]
stage2_rm:
    ; zero the boot_info handoff -> console defaults to 0 (VGA text)
    mov     di, BOOT_INFO
    xor     ax, ax
    mov     cx, 16                  ; 32 bytes
    rep     stosw

    call    setup_vbe

    ; --- enable the A20 line (fast A20) ---
    in      al, 0x92
    or      al, 2
    out     0x92, al

    ; --- enter protected mode ---
    lgdt    [gdt32_descriptor]
    mov     eax, cr0
    or      eax, 1
    mov     cr0, eax
    jmp     CODE_SEG32:protected_mode

; Set a 32bpp linear-framebuffer VBE mode and record it in boot_info. On any
; failure it just returns, leaving boot_info.console = 0 (VGA text fallback).
setup_vbe:
    pusha
    push    es
    push    fs
    xor     ax, ax
    mov     es, ax

    mov     di, VBE_CTRL                  ; VBE controller info (request VBE2 list)
    mov     dword [es:di], 0x32454256     ; 'VBE2'
    mov     ax, 0x4F00
    int     0x10
    cmp     ax, 0x004F
    jne     .out

    mov     si, [es:VBE_CTRL + 14]        ; VideoModePtr: offset @14, segment @16
    mov     ax, [es:VBE_CTRL + 16]
    mov     fs, ax
.next:
    mov     cx, [fs:si]                   ; next mode number
    add     si, 2
    cmp     cx, 0xFFFF
    je      .out                          ; end of list, none matched

    push    si
    mov     ax, 0x4F01                    ; get mode info -> VBE_MODE
    mov     di, VBE_MODE
    int     0x10
    pop     si
    cmp     ax, 0x004F
    jne     .next

    mov     ax, [es:VBE_MODE + 0]         ; ModeAttributes
    test    ax, 0x80                      ; linear framebuffer available?
    jz      .next
    test    ax, 0x10                      ; graphics (not text) mode?
    jz      .next
    cmp     byte [es:VBE_MODE + 25], 32   ; 32 bits per pixel?
    jne     .next
    mov     ax, [es:VBE_MODE + 18]        ; XResolution
    cmp     ax, 800
    jb      .next
    cmp     ax, 1280
    ja      .next

    mov     bx, cx                        ; set this mode, linear-framebuffer bit
    or      bx, 0x4000
    mov     ax, 0x4F02
    int     0x10
    cmp     ax, 0x004F
    jne     .next

    mov     word [es:BOOT_INFO + 0], 1            ; console = framebuffer
    mov     ax, [es:VBE_MODE + 18]
    mov     [es:BOOT_INFO + 4], ax               ; width
    mov     ax, [es:VBE_MODE + 20]
    mov     [es:BOOT_INFO + 8], ax               ; height
    mov     ax, [es:VBE_MODE + 16]               ; BytesPerScanLine
    shr     ax, 2                                ; -> pixels (32bpp)
    mov     [es:BOOT_INFO + 12], ax              ; pitch (pixels per scanline)
    mov     eax, [es:VBE_MODE + 40]              ; PhysBasePtr (LFB)
    mov     [es:BOOT_INFO + 24], eax             ; fb_phys (low dword)
.out:
    pop     fs
    pop     es
    popa
    ret

; ===========================================================================
; Stage 2: protected mode, long-mode checks, paging, long mode
; ===========================================================================
[bits 32]
protected_mode:
    mov     ax, DATA_SEG32
    mov     ds, ax
    mov     es, ax
    mov     ss, ax
    mov     fs, ax
    mov     gs, ax
    mov     esp, 0x90000

    call    check_cpuid
    call    check_long_mode

    ; --- copy the kernel up to its physical load address (1 MiB) ---
    ;     BIOS could only DMA below 1 MiB, so it landed in the temp buffer.
    cld
    mov     esi, KERNEL_ADDR
    mov     edi, KERNEL_PHYS
    mov     ecx, (KERNEL_SECTORS * 512) / 4
    rep     movsd

    ; --- page tables: low 2 MiB identity + higher-half kernel ---
    ;     low identity keeps stage 2 running through the switch and gives the
    ;     kernel direct access to physical low memory; the higher-half entry
    ;     maps 0xFFFFFFFF80000000 -> phys 1 MiB so the kernel runs high.
    mov     edi, 0x1000
    mov     cr3, edi
    xor     eax, eax
    mov     ecx, 4096 * 6 / 4                      ; zero PML4..PD_high + PD_fb (0x6000)
    rep     stosd

    mov     dword [0x1000], 0x2000 | 0b11          ; PML4[0]      -> PDPT_low
    mov     dword [0x2000], 0x3000 | 0b11          ; PDPT_low[0]  -> PD_low
    mov     dword [0x3000], 0x0000 | 0b10000011    ; PD_low[0]    = 0..2 MiB (PS)

    mov     dword [0x1000 + 511*8], 0x4000 | 0b11  ; PML4[511]    -> PDPT_high
    mov     dword [0x4000 + 510*8], 0x5000 | 0b11  ; PDPT_high[510]-> PD_high
    mov     dword [0x5000], KERNEL_PHYS | 0b10000011 ; PD_high[0] = phys 1 MiB (PS)

    ; --- if VBE gave us a framebuffer, map it at FB_VBASE (PDPT_high[511]) so the
    ;     kernel's console can touch it before vmm_init rebuilds the tables; the
    ;     UEFI loader maps this too. 16 MiB via 8 huge pages in PD_fb (0x6000). ---
    cmp     dword [BOOT_INFO], 1                   ; console == framebuffer?
    jne     .no_fb
    mov     ebx, [BOOT_INFO + 24]                  ; fb_phys (low 32 bits)
    and     ebx, 0xFFE00000                        ; 2 MiB-align
    mov     edi, 0x6000                            ; PD_fb
    mov     ecx, 8
.fb_loop:
    mov     eax, ebx
    or      eax, 0b10000011                        ; present | write | 2 MiB page
    mov     [edi], eax                             ; (high dword already zeroed)
    add     ebx, 0x200000
    add     edi, 8
    loop    .fb_loop
    mov     dword [0x4000 + 511*8], 0x6000 | 0b11  ; PDPT_high[511] -> PD_fb
.no_fb:

    mov     eax, cr4
    or      eax, 1 << 5                            ; CR4.PAE
    mov     cr4, eax

    mov     ecx, 0xc0000080                        ; IA32_EFER
    rdmsr
    or      eax, 1 << 8                            ; EFER.LME
    wrmsr

    mov     eax, cr0
    or      eax, 1 << 31                           ; CR0.PG
    mov     cr0, eax

    lgdt    [gdt64_descriptor]
    jmp     CODE_SEG64:long_mode

; --- is CPUID available? (toggle EFLAGS.ID, bit 21) ---
check_cpuid:
    pushfd
    pop     eax
    mov     ecx, eax
    xor     eax, 1 << 21
    push    eax
    popfd
    pushfd
    pop     eax
    push    ecx
    popfd
    cmp     eax, ecx
    je      .fail
    ret
.fail:
    mov     esi, msg_no_cpuid
    jmp     pm_error

; --- does this CPU support long mode? (ext leaf 0x80000001, EDX bit 29) ---
check_long_mode:
    mov     eax, 0x80000000
    cpuid
    cmp     eax, 0x80000001
    jb      .fail
    mov     eax, 0x80000001
    cpuid
    test    edx, 1 << 29
    jz      .fail
    ret
.fail:
    mov     esi, msg_no_lm
    jmp     pm_error

pm_error:
    mov     edi, 0xb8000
    mov     ah, 0x4f
.loop:
    lodsb
    test    al, al
    jz      .hang
    mov     [edi], al
    mov     [edi + 1], ah
    add     edi, 2
    jmp     .loop
.hang:
    cli
    hlt
    jmp     .hang

msg_no_cpuid: db "CPUID not supported", 0
msg_no_lm:    db "long mode not supported", 0

; ---------------------------------------------------------------------------
; 64-bit GDT
; ---------------------------------------------------------------------------
gdt64_start:
    dq 0x0000000000000000
gdt64_code:
    dq 0x00209a0000000000
gdt64_data:
    dq 0x0000920000000000
gdt64_end:

gdt64_descriptor:
    dw gdt64_end - gdt64_start - 1
    dq gdt64_start

CODE_SEG64 equ gdt64_code - gdt64_start
DATA_SEG64 equ gdt64_data - gdt64_start

; ===========================================================================
; 64-bit long mode: hand off to the C kernel
; ===========================================================================
[bits 64]
long_mode:
    mov     ax, DATA_SEG64
    mov     ds, ax
    mov     es, ax
    mov     fs, ax
    mov     gs, ax
    mov     ss, ax
    mov     rsp, 0x90000

    mov     edi, BOOT_INFO               ; boot_info (console=FB if VBE set a mode, else VGA)
    mov     rax, KERNEL_VMA
    jmp     rax                          ; enter the kernel in the higher half

times (512 * BOOT_SECTORS) - ($ - $$) db 0
