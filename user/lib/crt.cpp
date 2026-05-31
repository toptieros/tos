/* tOS C++ runtime: the freestanding entry + minimal C++ ABI support linked into
 * every C++ app (C apps don't link this, so there's no duplicate _ustart). It
 *   - is the ELF entry (_ustart, placed first by user.ld): it runs the global
 *     constructors the compiler registered in .init_array, then calls app_main();
 *   - provides operator new/delete backed by the libc heap (SYS_MMAP);
 *   - stubs the ABI symbols g++ references (__cxa_pure_virtual, __cxa_atexit,
 *     __dso_handle). We never run global destructors -- apps proc_exit(). */
#include <stddef.h>

extern "C" {
    void  proc_exit(void) __attribute__((noreturn));
    void *malloc(size_t);
    void  free(void *);
}

/* bracketed by user.ld: the table of constructor function pointers */
extern "C" void (*__init_array_start[])();
extern "C" void (*__init_array_end[])();

extern "C" int app_main();      /* every C++ tOS app provides this */

extern "C" __attribute__((section(".text.start"), used, noreturn))
void _ustart() {
    for (void (**fn)() = __init_array_start; fn != __init_array_end; ++fn) (*fn)();
    app_main();
    proc_exit();
}

void *operator new(size_t n)              { return malloc(n); }
void *operator new[](size_t n)            { return malloc(n); }
void  operator delete(void *p)   noexcept { free(p); }
void  operator delete[](void *p) noexcept { free(p); }
void  operator delete(void *p, size_t)   noexcept { free(p); }
void  operator delete[](void *p, size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual()                          { proc_exit(); }
extern "C" int  __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
extern "C" { void *__dso_handle = nullptr; }
