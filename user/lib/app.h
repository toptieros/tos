/* Minimal C++ app prelude for tOS. Include this in a C++ app to get placement
 * new (we do not pull in the hosted <new>) and the app entry the crt calls.
 * The C system headers (ulib.h, ugfx.h) are C++-safe (extern "C"). */
#pragma once
#include <stddef.h>

inline void *operator new(size_t, void *p)   noexcept { return p; }
inline void *operator new[](size_t, void *p) noexcept { return p; }

extern "C" int app_main();   /* the crt runs global ctors, then calls this */
