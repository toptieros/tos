/* fastfetch -- a small system-information banner, in the spirit of neofetch /
 * fastfetch. An ordinary piped program: it queries the kernel (SYS_SYSINFO,
 * SYS_TIME) and prints a colour ASCII logo beside the facts, using ANSI escapes
 * that the terminal emulator renders. The shell runs it at startup. */
#include "ulib.h"

#define RESET "\x1b[0m"
#define LOGO  "\x1b[1;36m"      /* bold cyan  */
#define KEY   "\x1b[1;34m"      /* bold blue  */
#define ACC   "\x1b[1;92m"      /* bright green (title) */

static const char *logo[] = {
    "    _      ___  ____  ",
    "   | |_   / _ \\/ ___| ",
    "   | __| | | | \\___ \\ ",
    "   | |_  | |_| |___) |",
    "    \\__|  \\___/|____/ ",
    "                      ",
};
#define LOGO_LINES (int)(sizeof(logo) / sizeof(logo[0]))

static void emitu(unsigned v) { printu(v); }

/* print "KEY label RESET value" as one info line */
static void field(const char *label, const char *val) {
    print(KEY); print(label); print(RESET); print(val);
}

int main(void) {
    struct sysinfo si; sysinfo(&si);
    struct rtctime t; rtc_time(&t);
    unsigned up = (unsigned)(si.uptime_ticks / (si.timer_hz ? si.timer_hz : 100));

    /* Build the right-hand info column as a sequence of closures-by-hand: we
     * print the logo line, two spaces, then the matching info line. */
    for (int i = 0; i < LOGO_LINES; i++) {
        print(LOGO); print(logo[i]); print(RESET); print("   ");
        switch (i) {
        case 0: print(ACC); print("root@tos"); print(RESET); break;
        case 1: print("---------------"); break;
        case 2: field("OS:       ", "tOS x86_64"); break;
        case 3: field("Kernel:   ", "tOS 1.0"); break;
        case 4: print(KEY); print("Uptime:   "); print(RESET);
                emitu(up / 60); print("m "); emitu(up % 60); print("s"); break;
        case 5: print(KEY); print("Shell:    "); print(RESET); print("tsh"); break;
        }
        print("\r\n");
    }
    /* remaining facts under the logo */
    print(KEY); print("Terminal: "); print(RESET); print("term\r\n");
    print(KEY); print("CPU:      "); print(RESET); print("x86_64 ("); emitu(si.ncpu); print(" cores)\r\n");
    print(KEY); print("Memory:   "); print(RESET); emitu((unsigned)(si.ram_bytes / (1024 * 1024))); print(" MiB\r\n");
    print(KEY); print("Display:  "); print(RESET); emitu(si.fb_w); print("x"); emitu(si.fb_h); print("\r\n");
    print(KEY); print("Files:    "); print(RESET); emitu(si.nfiles); print("\r\n");
    print(KEY); print("Tasks:    "); print(RESET); emitu(si.ntasks); print("\r\n");

    /* a row of colour blocks, like the fetch tools */
    print("\r\n");
    for (int c = 0; c < 8; c++) { print("\x1b[4"); printc((char)('0' + c)); print("m  "); }
    print(RESET); print("\r\n");
    return 0;
}

__attribute__((section(".text.start"), used, noreturn))
void _ustart(void) { main(); proc_exit(); }
