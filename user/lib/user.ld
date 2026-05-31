/* Each user program is linked as a static ELF64 executable. The kernel's ELF
 * loader (vmm.c) reads the program headers and maps each PT_LOAD segment at its
 * p_vaddr, then iretq's to e_entry -- so the entry point no longer has to sit at
 * a fixed address. We force a single RWX load segment that also carries the ELF
 * header + program headers (FILEHDR PHDRS), so the file maps linearly from
 * offset 0 onto USER_VBASE and the loader's job is a straight copy. Everything
 * still has to fit in the 64 KiB code window below the stack (see vmm.c). */
ENTRY(_ustart)

PHDRS { all PT_LOAD FILEHDR PHDRS FLAGS(7); }   /* one segment, R+W+X */

SECTIONS
{
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text.start)
        *(.text*)
    } :all
    .rodata : { *(.rodata*) *(.data.rel.ro*) *(.got) *(.got.plt) } :all

    /* C++ global constructors: g++ registers each in .init_array; the crt walks
     * this table (bracketed by these symbols) before calling the app. Empty for
     * C programs, so adding it here is harmless to them. */
    .init_array : {
        . = ALIGN(8);              /* the table is an array of 8-byte fn pointers; the
                                    * start symbol must be 8-aligned or the crt walks
                                    * it off by the padding and calls a garbage ptr */
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.ctors))
        __init_array_end = .;
    } :all

    .data   : { *(.data*) }   :all
    .bss    : { *(.bss*) *(COMMON) } :all

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) *(.eh_frame_hdr) }
}
