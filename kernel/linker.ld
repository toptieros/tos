/* Higher-half kernel: linked to run at virtual 0xFFFFFFFF80000000 but loaded
 * (LMA) at physical 0x100000. AT() sets each section's load address; the flat
 * binary objcopy produces is contiguous from 0x100000, which the bootloader
 * copies there before mapping VMA -> LMA and jumping high. */
ENTRY(_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x200000;

SECTIONS
{
    . = KERNEL_VMA;

    .text : AT(ADDR(.text) - KERNEL_VMA + KERNEL_LMA) {
        *(.text.start)
        *(.text*)
    }
    .rodata : AT(ADDR(.rodata) - KERNEL_VMA + KERNEL_LMA) { *(.rodata*) }
    .data   : AT(ADDR(.data)   - KERNEL_VMA + KERNEL_LMA) { *(.data*) }

    . = ALIGN(8);
    __bss_start = .;
    .bss : AT(ADDR(.bss) - KERNEL_VMA + KERNEL_LMA) { *(.bss*) *(COMMON) }
    __bss_end = .;

    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame*) }
}
