/* ===========================================================================
 * tOS UEFI loader. Places the (embedded) kernel and user images where the
 * kernel expects them, queries GOP for the framebuffer, builds a page map
 * (4 GiB identity for the transition + the higher half + the framebuffer),
 * exits boot services, and jumps into the same kmain the BIOS path uses --
 * passing a boot_info in RDI so the kernel renders to the framebuffer.
 * =========================================================================== */
#include <stdint.h>
#include "bootinfo.h"                /* struct boot_info, FB_VBASE (from kernel/) */
#include "kernel_blob.h"             /* kernel_bin[], kernel_bin_len (programs embedded) */

typedef uint64_t EFI_STATUS;
typedef void    *EFI_HANDLE;

typedef struct { uint32_t a; uint16_t b, c; uint8_t d[8]; } EFI_GUID;

typedef struct {
    char  Hdr[24];
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (*AllocatePages)(int, int, uint64_t, uint64_t *);      /* 40 */
    void *FreePages;
    EFI_STATUS (*GetMemoryMap)(uint64_t *, void *, uint64_t *, uint64_t *, uint32_t *); /* 56 */
    void *AllocatePool, *FreePool;
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    void *HandleProtocol, *Reserved, *RegisterProtocolNotify;
    void *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (*ExitBootServices)(EFI_HANDLE, uint64_t);             /* 232 */
    void *GetNextMonotonicCount, *Stall, *SetWatchdogTimer;
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);        /* 320 */
} EFI_BOOT_SERVICES;

typedef struct {
    char  Hdr[24];
    void *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle; void *ConIn;
    EFI_HANDLE ConsoleOutHandle; void *ConOut;
    EFI_HANDLE StandardErrorHandle; void *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;                                 /* 96 */
} EFI_SYSTEM_TABLE;

/* Graphics Output Protocol (only the fields we read). */
typedef struct {
    uint32_t Version, HorizontalResolution, VerticalResolution, PixelFormat;
    uint32_t PixelInformation[4];
    uint32_t PixelsPerScanLine;
} GOP_MODE_INFO;
typedef struct {
    uint32_t MaxMode, Mode;
    GOP_MODE_INFO *Info;
    uint64_t SizeOfInfo, FrameBufferBase, FrameBufferSize;
} GOP_MODE;
typedef struct {
    void *QueryMode, *SetMode, *Blt;
    GOP_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

static EFI_GUID GOP_GUID =
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a } };

#define AllocateAddress 2
#define EfiLoaderData   2

#define POOL_PHYS    0x100000
#define KERNEL_PHYS  0x200000
#define KERNEL_VMA   0xFFFFFFFF80000000ULL

#define LT_PML4    0x1F0000
#define LT_PDPT_LO 0x1F1000
#define LT_PD0     0x1F2000          /* PD0..PD3 -> 0..4 GiB identity */
#define LT_PDPT_HI 0x1F6000
#define LT_PD_HIGH 0x1F7000          /* higher-half kernel */
#define LT_PD_FB   0x1F8000          /* framebuffer window */

#define COM1 0x3f8
static inline void outb(uint16_t p, uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static inline uint8_t inb(uint16_t p){uint8_t r;__asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p));return r;}
static void serial_init(void){
    outb(COM1+1,0); outb(COM1+3,0x80); outb(COM1+0,3);
    outb(COM1+1,0); outb(COM1+3,3); outb(COM1+2,0xc7); outb(COM1+4,0x0b);
}
static void sputs(const char *s){ for(;*s;++s){ while((inb(COM1+5)&0x20)==0){} outb(COM1,(uint8_t)*s);} }

static void copy(uint64_t dst, const unsigned char *src, uint64_t n){
    volatile uint8_t *d=(volatile uint8_t*)dst;
    for(uint64_t i=0;i<n;i++) d[i]=src[i];
}
static void put(uint64_t t, int i, uint64_t v){ ((volatile uint64_t*)t)[i]=v; }
static void zero(uint64_t t){ for(int i=0;i<512;i++) ((volatile uint64_t*)t)[i]=0; }

static struct boot_info boot_info;

static void build_tables(void){
    zero(LT_PML4); zero(LT_PDPT_LO); zero(LT_PDPT_HI); zero(LT_PD_HIGH); zero(LT_PD_FB);
    for(int g=0; g<4; g++) zero(LT_PD0 + (uint64_t)g*0x1000);

    put(LT_PML4, 0,   LT_PDPT_LO | 0x3);
    put(LT_PML4, 511, LT_PDPT_HI | 0x3);

    for(int g=0; g<4; g++){                                  /* 0..4 GiB identity */
        uint64_t pd = LT_PD0 + (uint64_t)g*0x1000;
        put(LT_PDPT_LO, g, pd | 0x3);
        for(int j=0;j<512;j++)
            put(pd, j, ((uint64_t)g*0x40000000 + (uint64_t)j*0x200000) | 0x83);
    }

    put(LT_PDPT_HI, 510, LT_PD_HIGH | 0x3);                  /* 0xFF..80000000 -> kernel */
    put(LT_PD_HIGH, 0, KERNEL_PHYS | 0x83);

    if (boot_info.console == BOOT_CONSOLE_FB){               /* FB_VBASE -> framebuffer */
        uint64_t fb = boot_info.fb_phys & ~0x1fffffULL;
        put(LT_PDPT_HI, 511, LT_PD_FB | 0x3);
        for(int i=0;i<8;i++) put(LT_PD_FB, i, (fb + (uint64_t)i*0x200000) | 0x83);
    }
}

EFI_STATUS efi_main(EFI_HANDLE img, EFI_SYSTEM_TABLE *st){
    EFI_BOOT_SERVICES *bs = st->BootServices;
    uint64_t addr;

    serial_init();
    sputs("[uefi] tOS loader\r\n");

    addr = POOL_PHYS;   bs->AllocatePages(AllocateAddress, EfiLoaderData, 256, &addr);
    addr = KERNEL_PHYS; bs->AllocatePages(AllocateAddress, EfiLoaderData, 512, &addr);

    copy(KERNEL_PHYS, kernel_bin, kernel_bin_len);

    /* query GOP for the framebuffer */
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop = 0;
    boot_info.console = BOOT_CONSOLE_VGA;
    if (bs->LocateProtocol(&GOP_GUID, 0, (void **)&gop) == 0 && gop){
        boot_info.console = BOOT_CONSOLE_FB;
        boot_info.width   = gop->Mode->Info->HorizontalResolution;
        boot_info.height  = gop->Mode->Info->VerticalResolution;
        boot_info.pitch   = gop->Mode->Info->PixelsPerScanLine;
        boot_info.fb_phys = gop->Mode->FrameBufferBase;
        sputs("[uefi] GOP framebuffer found\r\n");
    }

    build_tables();
    sputs("[uefi] exiting boot services\r\n");

    static uint8_t mmap[32768];
    uint64_t msize, mkey, dsize; uint32_t dver;
    for(int t=0; t<16; t++){
        msize = sizeof(mmap);
        bs->GetMemoryMap(&msize, mmap, &mkey, &dsize, &dver);
        if (bs->ExitBootServices(img, mkey) == 0) break;
    }

    uint64_t cr3 = LT_PML4, bi = (uint64_t)&boot_info, entry = KERNEL_VMA;
    __asm__ volatile(
        "cli\n\t"
        "mov %0, %%cr3\n\t"
        "mov %1, %%rdi\n\t"
        "jmp *%2\n\t"
        : : "r"(cr3), "r"(bi), "r"(entry) : "rdi", "memory");

    for(;;) __asm__ volatile("hlt");
    return 0;
}
