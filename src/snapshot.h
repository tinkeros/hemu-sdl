#pragma once
#include "cpu.h"

// Per-core state arrays (defined in snapshot.cpp, declared extern in cpu.h)
// g_cpu_st[NCORES], g_ncore, g_ipi_pending[], g_icr_high[], g_dc2

void emu_main();  // call once per frame (was Main() in HolyC)

// SMP: call after first emu_main() to launch AP threads
void smp_start();
void smp_stop();
