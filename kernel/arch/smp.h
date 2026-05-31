/* Symmetric multiprocessing: bring up the other CPUs (application processors). */
#pragma once
#include <stdint.h>

#define MAX_CPUS 8

void smp_init(void);          /* enumerate + start the APs (call before sched_start) */
int  smp_cpu_count(void);     /* number of CPUs that came online (>=1)               */
