// rapl.c - RAPL energy zones (package/core/uncore/dram) as an acquisition
// category. Each zone is a µJ counter under /sys/.../powercap/intel-rapl;
// the thread differences it over its own measured dt and publishes µW.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <errno.h>

#include "rapl.h"


#define RAPL_ROOT	"/sys/devices/virtual/powercap/intel-rapl"
#define RAPL_MAX	16	// max RAPL zones we track.

// --- category-private state (this thread's exclusive slice) ---
static _Atomic uint64_t* out;			// -> samples[base .. base+count)
static int  base;				// our offset into the shared array.
static int  count;				// zones we claimed.
static FILE* files[RAPL_MAX];			// open energy_uj handle per zone.
static uint64_t prev_uj[RAPL_MAX];		// last counter reading (µJ).

static pthread_t thread;
static int started = 0;

// Recurse the powercap tree, admitting each intel-rapl:* zone. Mirrors the
// original locate: parent is the GLOBAL index of the enclosing zone, or -1.
static void locate_dir(
	const char* dir, int parent,
	char names[][ZONE_NAME_LEN], int* parents)
{
	const char* s = getenv("TOLERATE_MISSING_ENERGY_DATA");
	int tolerant = s && strcasestr(s, "RAPL") != 0;
	const char* hint =
		"Either run as root, or tolerate missing data:\n"
		"  $ sudo energygraph\n"
		"  $ TOLERATE_MISSING_ENERGY_DATA=rapl energygraph\n";
	DIR* d = opendir(dir);
	if (!d)
	{
		fprintf(stderr, "Failed to open %s - %s\n", dir, strerror(errno));
		if (!tolerant)
		{
			fprintf(stderr, "%s", hint);
			exit(1);
		}
		return;
	}
	struct dirent* e;
	while ((e = readdir(d)))
	{
		if (e->d_type != DT_DIR)
			continue;
		if (!strstr(e->d_name, "intel-rapl:"))
			continue;
		if (count >= RAPL_MAX)
			break;

		char sub[256];
		snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);

		// Open this zone's energy_uj; skip the zone if absent.
		char fn[300];
		snprintf(fn, sizeof(fn), "%s/energy_uj", sub);
		FILE* f = fopen(fn, "r");
		if (!f)
		{
			fprintf(stderr, "Failed to open %s - %s\n", fn, strerror(errno));
			if (!tolerant)
			{
				fprintf(stderr, "%s", hint);
				exit(1);
			}
			continue;
		}

		const int gi = base + count;		// global zone index.
		files[count]   = f;
		prev_uj[count] = 0;
		parents[gi]    = parent;

		// Label from the zone's name file, else the dir name.
		char nf[300];
		snprintf(nf, sizeof(nf), "%s/name", sub);
		FILE* lf = fopen(nf, "r");
		char lbl[ZONE_NAME_LEN];
		lbl[0] = 0;
		if (lf && fgets(lbl, sizeof(lbl), lf))
		{
			if (lbl[strlen(lbl)-1] == '\n')
				lbl[strlen(lbl)-1] = 0;
		}
		if (lf)
			fclose(lf);
		snprintf(names[gi], ZONE_NAME_LEN, "%s", lbl[0] ? lbl : e->d_name);

		count++;

		// Recurse: children of THIS zone point their parent at gi.
		locate_dir(sub, gi, names, parents);
	}
	closedir(d);
}

static uint64_t read_uj(int i)
{
	char s[32];
	s[0] = 0;
	int numrd = fread(s, 1, sizeof(s), files[i]);
	(void) numrd;
	rewind(files[i]);
	return (uint64_t) atol(s);
}

// Monotonic milliseconds.
static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void* run(void* arg)
{
	(void)arg;

	// Prime prev[] and the clock so the first published sample is a real
	// delta over a real interval, not a spike against zero.
	for (int i = 0; i < count; ++i)
		prev_uj[i] = read_uj(i);
	int64_t t_prev = now_ms();

	while (!atomic_load_explicit(&quit, memory_order_relaxed))
	{
		struct timespec s = { 1, 0 };		// 1s cadence.
		nanosleep(&s, NULL);

		const int64_t t_now = now_ms();
		int64_t dt = t_now - t_prev;
		if (dt < 1)
			dt = 1;
		t_prev = t_now;

		for (int i = 0; i < count; ++i)
		{
			const uint64_t cur = read_uj(i);
#if 0
			fprintf(stderr, "rapl z%d cur=%llu prev=%llu dt=%ld\n",
			        i, (unsigned long long)cur, (unsigned long long)prev_uj[i], (long)dt);
#endif
			uint64_t d = (cur >= prev_uj[i]) ? cur - prev_uj[i] : 0;
			prev_uj[i] = cur;
			// µJ over dt ms -> µW: (d µJ / dt ms) * 1000 ms/s = d*1000/dt µW.
			// Multiply before divide to keep integer precision.
			const uint64_t uw = d * 1000ull / (uint64_t)dt;
			atomic_store_explicit(&out[base + i], uw, memory_order_relaxed);
		}
	}
	return NULL;
}

int rapl_locate(_Atomic uint64_t* samples, char names[][ZONE_NAME_LEN], int* parents, int offset)
{
	out   = samples;
	base  = offset;
	count = 0;

	locate_dir(RAPL_ROOT, -1, names, parents);

	if (count > 0)
	{
		started = 1;
		pthread_create(&thread, NULL, run, NULL);
	}
	return count;
}

void rapl_stop(void)
{
	if (!started)
		return;
	pthread_join(thread, NULL);
	for (int i = 0; i < count; ++i)
		if (files[i])
			fclose(files[i]);
	started = 0;
}
