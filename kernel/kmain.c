/* tOS kernel entry. The boot front-end jumps here in 64-bit long mode. We bring
 * up the console, GDT/TSS, IDT, keyboard and timer, then start the preemptive
 * scheduler with one user task (the shell) plus a ring-0 idle task. */
#include "console.h"
#include "bootinfo.h"
#include "gdt.h"
#include "idt.h"
#include "vmm.h"
#include "sched.h"
#include "keyboard.h"
#include "timer.h"
#include "fs.h"
#include "smp.h"
#include "mouse.h"
#include "virtio_blk.h"
#include "blockdev.h"
#include "ata.h"
#include <stdint.h>

/* Copied out of the boot-provided struct (which lives in low memory that later
 * address spaces no longer map) into the kernel's own higher-half storage. */
static struct boot_info bootinfo;

void kmain(struct boot_info *bi) {
    if (bi) bootinfo = *bi;
    else    bootinfo.console = BOOT_CONSOLE_VGA;

    console_init(&bootinfo);
    console_puts("[kernel] tOS is alive in 64-bit long mode\r\n");
    console_puts("[kernel] running in the higher half at ");
    console_puthex((uint64_t)&kmain);
    console_puts("\r\n");

    gdt_init();
    console_puts("[kernel] GDT + TSS installed\r\n");

    idt_init();
    console_puts("[kernel] IDT installed; syscalls on int 0x80\r\n");

    kbd_init();
    timer_init();
    console_puts("[kernel] PIC remapped; timer on IRQ0, keyboard on IRQ1\r\n");

    vmm_init(&bootinfo);

    if (bootinfo.console == BOOT_CONSOLE_FB) {
        mouse_init((int)bootinfo.width, (int)bootinfo.height);
        console_puts("[kernel] PS/2 mouse enabled on IRQ12\r\n");
    }

    /* Block-device layer: the boot disk (ATA) plus any virtio-blk install target,
     * registered by name so the installer can target a disk by index. */
    bdev_register("ata0", ata_sectors(), ata_bdev_read, ata_bdev_write);
    virtio_blk_init();                    /* probes + registers "virtio0" if present (no-op if absent) */
    bdev_dump();

    if (fs_mount() < 0) {
        console_puts("[kernel] PANIC: no tosfs disk found -- cannot load init\r\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    console_puts("[kernel] mounted tosfs from disk (partition LBA ");
    console_putdec(fs_base_lba());
    console_puts(")\r\n");

    sched_init();
    if (sched_spawn("init") < 0) {        /* PID 1: it brings up the shell */
        console_puts("[kernel] PANIC: could not load 'init' from disk\r\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
    smp_init();                           /* bring up the other CPUs (if any) */

    console_puts("[kernel] starting init (pid 1) in ring 3 ...\r\n");
    sched_start();
}
