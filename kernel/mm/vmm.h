#pragma once
#include <stdint.h>
#include "bootinfo.h"

/* Per-task user virtual layout (all under the per-process PML4[0] subtree). */
#define USER_VBASE     0x400000      /* code entry                            */
#define USER_STACK_TOP 0x5FF000      /* top of the 64 KiB user stack (grows down) */

void vmm_init(struct boot_info *bi); /* build the shared higher-half tables   */
/* Load an ELF program into a fresh address space. Returns the PML4 phys addr
 * (0 on failure) and writes the program's entry point to *entry. */
uint64_t vmm_create_user(const char *prog, uint64_t *entry);
uint64_t vmm_fork(uint64_t parent_pml4);     /* copy an address space -> new PML4 */
void     vmm_destroy_user(uint64_t pml4_phys);   /* reclaim a task's frames    */
struct fbinfo;
int      vmm_map_user_fb(struct fbinfo *out);    /* map the framebuffer into the caller */

/* Window surfaces: shared pixel buffers mapped into both an app and the
 * compositor (see kernel/ipc.c). */
/* Map device MMIO (a PCI BAR in the PCI hole, outside the RAM identity map) into
 * the shared higher half; returns a kernel pointer, or 0 if the window is full. */
void    *vmm_map_mmio(uint64_t phys, uint64_t bytes);

uint64_t vmm_alloc_surface(int nframes);                 /* contiguous frames -> phys base, 0 = OOM */
void     vmm_free_surface(uint64_t base, int nframes);
uint64_t vmm_map_surface(uint64_t pml4_phys, int id, uint64_t phys_base, int nframes);
void     vmm_unmap_surface(uint64_t pml4_phys, int id, int nframes);
uint64_t vmm_surface_vaddr(int id);
uint64_t vmm_current_pml4(void);
void     vmm_flush_self(void);

/* Validate a user pointer in the current address space before the kernel touches
 * it, so a bad pointer fails a syscall instead of faulting the kernel and halting
 * the machine. vmm_user_ok: a byte range is mapped+user; vmm_user_str_ok: a
 * NUL-terminated string of at most `max` bytes is fully mapped+user. */
int      vmm_user_ok(uint64_t vaddr, uint64_t len);
int      vmm_user_str_ok(uint64_t vaddr, int max);
uint64_t vmm_ram_bytes(void);
void     vmm_fb_size(uint32_t *w, uint32_t *h);

/* Map `nframes` of private anonymous RAM into the live address space at *brk
 * (grows the region upward across page tables on demand); returns the base
 * vaddr and bumps *brk, 0 on failure. Backs SYS_MMAP (back buffers, heaps). */
uint64_t vmm_mmap(uint64_t *brk, int nframes);

#define KSTACK_SZ 16384                  /* per-task kernel stack (4 pages)         */
uint64_t vmm_alloc_kstack(void);         /* contiguous kernel stack -> base, 0 = OOM */
void     vmm_free_kstack(uint64_t base); /* reclaim one allocated by vmm_alloc_kstack */
uint64_t vmm_kernel_pml4(void);      /* kernel-only address space (idle task)  */
