// nvml.c - NVIDIA GPU power via NVML, as an acquisition category. NVML is
// loaded with dlopen (no link-time dependency); if absent, the category
// claims zero zones and starts no thread. NVML power queries can block for
// SECONDS on some drivers - harmless here, because the stall is isolated on
// this thread; main just republishes the last atomic value until it returns.
// NVML reports milliwatts; the thread scales x1000 to µW.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "nvml.h"


#define NVML_MAX	8	// max NVIDIA GPUs we track.

// --- resolved NVML symbols (versioned names; NVML_SUCCESS == 0) ---
static void*        handle           = 0;
static unsigned int (*nvml_init)(void)                          = 0;
static unsigned int (*nvml_count_fn)(unsigned int*)             = 0;
static unsigned int (*nvml_by_index)(unsigned int, void**)      = 0;
static unsigned int (*nvml_power)(void*, unsigned int*)         = 0;
static unsigned int (*nvml_shutdown)(void)                      = 0;

// --- category-private state ---
static _Atomic uint64_t* out;
static int   base;
static int   count;
static void* devs[NVML_MAX];		// nvmlDevice_t (opaque) per GPU.

static pthread_t thread;
static int started = 0;

static void* run(void* arg)
{
	(void)arg;

	// Poll continuously. A steady stream of queries also keeps the driver's
	// client state warm, reducing the re-init stalls; and any stall that does
	// occur blocks only this thread. Short sleep between rounds so we don't
	// spin the driver harder than needed.
	while (!atomic_load_explicit(&quit, memory_order_relaxed))
	{
		for (int i = 0; i < count; ++i)
		{
			unsigned int mw = 0;
			// This call may block for seconds on some drivers. That's fine:
			// main keeps rendering the last published value meanwhile.
			const uint64_t uw =
				(nvml_power && nvml_power(devs[i], &mw) == 0)
				? (uint64_t)mw * 1000ull		// mW -> µW.
				: 0;
			atomic_store_explicit(&out[base + i], uw, memory_order_relaxed);
		}

		struct timespec s = { 0, 333 * 1000 * 1000 };	// ~333ms between rounds.
		nanosleep(&s, NULL);
	}
	return NULL;
}

int nvml_locate(_Atomic uint64_t* samples, char names[][ZONE_NAME_LEN], int* parents, int offset)
{
	out   = samples;
	base  = offset;
	count = 0;

	// Load NVML. Use the versioned soname, never the unversioned name (which
	// may resolve to a CUDA stub that returns no data). Absent -> skip.
	handle = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
	if (!handle)
		return 0;

	nvml_init     = (unsigned int(*)(void))                     dlsym(handle, "nvmlInit_v2");
	nvml_count_fn = (unsigned int(*)(unsigned int*))            dlsym(handle, "nvmlDeviceGetCount_v2");
	nvml_by_index = (unsigned int(*)(unsigned int, void**))     dlsym(handle, "nvmlDeviceGetHandleByIndex_v2");
	nvml_power    = (unsigned int(*)(void*, unsigned int*))     dlsym(handle, "nvmlDeviceGetPowerUsage");
	nvml_shutdown = (unsigned int(*)(void))                     dlsym(handle, "nvmlShutdown");

	if (!nvml_init || !nvml_count_fn || !nvml_by_index || !nvml_power || !nvml_shutdown ||
	    nvml_init() != 0)
	{
		dlclose(handle);
		handle = 0;
		return 0;
	}

	unsigned int n = 0;
	if (nvml_count_fn(&n) != 0 || n == 0)
	{
		nvml_shutdown();
		dlclose(handle);
		handle = 0;
		return 0;
	}

	for (unsigned int g = 0; g < n && count < NVML_MAX; ++g)
	{
		void* dev = 0;
		if (nvml_by_index(g, &dev) != 0 || !dev)
			continue;

		const int gi = base + count;
		devs[count]  = dev;
		parents[gi]  = -1;			// top-level, additive.
		// Label NVIDIA when single, NVIDIA0/1/... when several.
		if (n > 1)
			snprintf(names[gi], ZONE_NAME_LEN, "nvidia%u", g);
		else
			snprintf(names[gi], ZONE_NAME_LEN, "nvidia");
		count++;
	}

	if (count == 0)
	{
		nvml_shutdown();
		dlclose(handle);
		handle = 0;
		return 0;
	}

	started = 1;
	pthread_create(&thread, NULL, run, NULL);
	return count;
}

void nvml_stop(void)
{
	if (!started)
		return;
	// The thread may be inside a multi-second NVML call; join waits for it to
	// return and notice quit. Worst-case shutdown delay is one poll round.
	pthread_join(thread, NULL);
	if (nvml_shutdown)
		nvml_shutdown();
	if (handle)
		dlclose(handle);
	started = 0;
}
