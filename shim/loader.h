#pragma once
#include <stdint.h>

/* Boot the SWEETXNU payload found in the ABL ramdisk; returns only on error. */
const char *loader_boot(uintptr_t dtb);

/* PSCI call through the board's conduit (shim.c). */
uint64_t psci(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3);
