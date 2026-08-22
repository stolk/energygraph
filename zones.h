// zones.h - shared contract for the acquisition categories (rapl/hwmo/nvml).
//
// Each category claims a contiguous slice of the shared arrays at a base
// offset and, if it claimed >0 zones, spawns one thread that publishes µW into
// its slice of samples[]. Slices never overlap, so no locking: categories
// write their own range, main only reads. Main is a ~1Hz renderer; a category
// whose read blocks just republishes its last value.

#pragma once

#define MAXZONES	32	// max zones across all categories.
#define ZONE_NAME_LEN	32	// per-zone label length; matches names[][].

// samples[] entries are instantaneous power in MICROWATTS (µW). Categories
// convert internally (energy counters differenced over their own dt; mW x1000).
// Each entry: one writer thread, one reader (main) -> relaxed atomics suffice.

extern _Atomic int quit;	// main sets on quit; threads poll and exit. In main.c.

