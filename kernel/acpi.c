/* Minimal ACPI table parsing (no AML interpreter). Finds the RSDP, walks the
 * RSDT/XSDT, and pulls out:
 *   - MADT  -> the enabled CPUs' Local-APIC ids (real-hardware CPU discovery,
 *              replacing the QEMU-only fw_cfg count).
 *   - FADT  -> PM1a/PM1b control ports + the _S5 sleep type (ACPI poweroff), and
 *              the reset register (ACPI reset). These generalise the old hardcoded
 *              QEMU magic ports (0x604 poweroff / 8042 reset): on QEMU they resolve
 *              to exactly those, but they also work on real hardware / other VMs.
 *
 * Memory: ACPI tables can sit anywhere in physical RAM (often in ACPI-reclaim
 * regions the frame pool excludes, or the legacy BIOS area), so we reach them via
 * vmm_map_mmio() -- which maps ANY physical page regardless of the e820 type --
 * rather than relying on the RAM identity map. Runs during kmain (vmm_map_mmio is
 * wired into the live bootstrap tables, proven by the AHCI/NVMe probes). */
#include "acpi.h"
#include "cpu.h"
#include "console.h"
#include "vmm.h"
#include "smp.h"      /* MAX_CPUS */

struct rsdp {
    char     sig[8];          /* "RSD PTR " */
    uint8_t  checksum;        /* sum of bytes 0..19 == 0 */
    char     oemid[6];
    uint8_t  revision;        /* 0 = ACPI 1.0 (RSDT), >=2 = ACPI 2.0+ (XSDT) */
    uint32_t rsdt_addr;
    uint32_t length;          /* (rev >= 2) total length */
    uint64_t xsdt_addr;       /* (rev >= 2) */
    uint8_t  ext_checksum;
    uint8_t  rsv[3];
} __attribute__((packed));

struct sdt_header {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

/* Generic Address Structure (FADT reset register etc.). */
struct gas {
    uint8_t  space_id;        /* 0 = system memory, 1 = system I/O */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} __attribute__((packed));

/* --- parsed results -------------------------------------------------------- */
static int      have_pm1 = 0;
static uint16_t pm1a_cnt = 0, pm1b_cnt = 0;
static uint16_t slp_typa = 0, slp_typb = 0;   /* already shifted into PM1_CNT bit position */
static int      have_reset = 0;
static uint16_t reset_port = 0;
static uint8_t  reset_value = 0;
static uint8_t  cpu_ids[MAX_CPUS];
static int      cpu_n = 0;

#define SLP_EN (1u << 13)

static uint8_t sum_bytes(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p; uint8_t s = 0;
    for (uint32_t i = 0; i < n; i++) s = (uint8_t)(s + b[i]);
    return s;
}
static int sig_is(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Map a physical structure for reading (via the MMIO window, so any e820 type). */
static void *map_phys(uint64_t phys, uint32_t len) { return vmm_map_mmio(phys, len); }

/* Scan a physical byte range for the "RSD PTR " signature on a 16-byte boundary. */
static struct rsdp *scan_rsdp(uint8_t *base, uint32_t len) {
    for (uint32_t off = 0; off + 20 <= len; off += 16) {
        struct rsdp *r = (struct rsdp *)(base + off);
        if (sig_is(r->sig, "RSD PTR ", 8) && sum_bytes(r, 20) == 0) return r;
    }
    return 0;
}

static struct rsdp *find_rsdp(void) {
    uint8_t *low = (uint8_t *)map_phys(0, 0x200000);   /* whole first 2 MiB (covers EBDA + BIOS area) */
    if (!low) return 0;
    uint32_t ebda = (uint32_t)(*(uint16_t *)(low + 0x40E)) << 4;   /* EBDA segment -> linear */
    if (ebda >= 0x400 && ebda < 0x100000) {
        struct rsdp *r = scan_rsdp(low + ebda, 1024);
        if (r) return r;
    }
    return scan_rsdp(low + 0xE0000, 0x20000);          /* 0xE0000 .. 0xFFFFF */
}

/* --- FADT --------------------------------------------------------------------
 * Pull the PM1 control ports + the reset register, then scan the DSDT for _S5 to
 * get the sleep type. Field offsets are from the ACPI spec FADT layout. */
static void parse_fadt(struct sdt_header *fadt) {
    uint8_t *f = (uint8_t *)fadt;
    uint32_t len = fadt->length;
    pm1a_cnt = (uint16_t)(*(uint32_t *)(f + 64));      /* PM1a_CNT_BLK */
    pm1b_cnt = (uint16_t)(*(uint32_t *)(f + 68));      /* PM1b_CNT_BLK */
    if (pm1a_cnt) have_pm1 = 1;

    /* RESET_REG (offset 116, 12-byte GAS) + RESET_VALUE (offset 128), ACPI 2.0+ */
    if (len >= 129) {
        struct gas *rr = (struct gas *)(f + 116);
        uint8_t rv = f[128];
        if (rr->space_id == 1 && rr->address) {        /* system I/O space */
            reset_port = (uint16_t)rr->address; reset_value = rv; have_reset = 1;
        }
    }

    /* _S5 sleep type from the DSDT (the minimal, no-AML byte scan). */
    uint64_t dsdt_phys = *(uint32_t *)(f + 40);        /* DSDT (32-bit) */
    if (len >= 148) { uint64_t x = *(uint64_t *)(f + 140); if (x) dsdt_phys = x; }  /* X_DSDT */
    if (!dsdt_phys) return;
    struct sdt_header *dsdt = (struct sdt_header *)map_phys(dsdt_phys, 64);
    if (!dsdt || !sig_is(dsdt->sig, "DSDT", 4)) return;
    uint32_t dlen = dsdt->length;
    uint8_t *d = (uint8_t *)map_phys(dsdt_phys, dlen);
    if (!d) return;
    for (uint32_t i = 0; i + 8 < dlen; i++) {
        if (d[i] == '_' && d[i+1] == 'S' && d[i+2] == '5' && d[i+3] == '_') {
            uint8_t *p = d + i + 4;
            if (*p == 0x12) { p += 2; p++; }           /* PackageOp, skip pkglen byte + numelements */
            if (*p == 0x0A) p++;                        /* BytePrefix */
            slp_typa = (uint16_t)((*p & 7) << 10);
            p++;
            if (*p == 0x0A) p++;
            slp_typb = (uint16_t)((*p & 7) << 10);
            break;
        }
    }
}

/* --- MADT: collect the enabled CPUs' Local-APIC ids. ----------------------- */
static void parse_madt(struct sdt_header *madt) {
    uint8_t *m = (uint8_t *)madt;
    uint32_t len = madt->length;
    uint32_t off = 44;                                 /* past the header + LAPIC addr + flags */
    while (off + 2 <= len) {
        uint8_t type = m[off], elen = m[off + 1];
        if (elen < 2) break;
        if (type == 0 && off + 8 <= len) {             /* Processor Local APIC */
            uint8_t apic_id = m[off + 3];
            uint32_t flags  = *(uint32_t *)(m + off + 4);
            if ((flags & 1) && cpu_n < MAX_CPUS) cpu_ids[cpu_n++] = apic_id;  /* enabled */
        }
        off += elen;
    }
}

void acpi_init(void) {
    struct rsdp *r = find_rsdp();
    if (!r) { console_puts("[acpi] no RSDP\r\n"); return; }

    /* choose RSDT (rev 0) or XSDT (rev >= 2); entries are 4- or 8-byte phys ptrs */
    int use_xsdt = (r->revision >= 2 && r->xsdt_addr);
    uint64_t root_phys = use_xsdt ? r->xsdt_addr : (uint64_t)r->rsdt_addr;
    struct sdt_header *root = (struct sdt_header *)map_phys(root_phys, sizeof(struct sdt_header));
    if (!root) { console_puts("[acpi] no RSDT/XSDT\r\n"); return; }
    root = (struct sdt_header *)map_phys(root_phys, root->length);
    uint32_t entries = (root->length - sizeof(struct sdt_header)) / (use_xsdt ? 8 : 4);
    uint8_t *etbl = (uint8_t *)root + sizeof(struct sdt_header);

    int found_fadt = 0, found_madt = 0;
    for (uint32_t i = 0; i < entries; i++) {
        uint64_t tphys = use_xsdt ? *(uint64_t *)(etbl + i * 8) : (uint64_t)*(uint32_t *)(etbl + i * 4);
        struct sdt_header *h = (struct sdt_header *)map_phys(tphys, sizeof(struct sdt_header));
        if (!h) continue;
        h = (struct sdt_header *)map_phys(tphys, h->length);
        if (sig_is(h->sig, "FACP", 4)) { parse_fadt(h); found_fadt = 1; }
        else if (sig_is(h->sig, "APIC", 4)) { parse_madt(h); found_madt = 1; }
    }

    console_puts("[acpi] rev ");
    console_putdec(r->revision);
    console_puts(use_xsdt ? " (XSDT), " : " (RSDT), ");
    if (found_madt) { console_putdec((uint64_t)cpu_n); console_puts(" CPU(s) via MADT"); }
    else console_puts("no MADT");
    if (found_fadt) {
        console_puts(", poweroff PM1a="); console_puthex(pm1a_cnt);
        if (have_reset) { console_puts(" reset="); console_puthex(reset_port); }
    }
    console_puts("\r\n");
}

int acpi_cpu_apic_ids(uint8_t *ids, int max) {
    int n = cpu_n < max ? cpu_n : max;
    for (int i = 0; i < n; i++) ids[i] = cpu_ids[i];
    return n;
}

int acpi_poweroff(void) {
    if (!have_pm1) return -1;
    outw(pm1a_cnt, (uint16_t)(slp_typa | SLP_EN));
    if (pm1b_cnt) outw(pm1b_cnt, (uint16_t)(slp_typb | SLP_EN));
    return 0;
}

int acpi_reset(void) {
    if (!have_reset) return -1;
    outb(reset_port, reset_value);
    return 0;
}
