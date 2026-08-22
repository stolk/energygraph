// hwmo.c - hwmon GPU power as an acquisition category. Handles two kinds:
//   xe (Arc/Battlemage): energy1_input "card" = µJ counter (parent), with
//     energy2_input "pkg" = µJ counter (child, subset of card). Differenced.
//   amdgpu: power1_average / power1_input = instantaneous µW rate, published
//     directly (no differencing).
// The thread publishes µW for every zone; only allowlisted DISCRETE-GPU drivers
// are admitted (disjoint from RAPL - see README on double-counting).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "hwmo.h"


#define HWMO_ROOT	"/sys/class/hwmon"
#define HWMO_MAX	16	// max hwmon GPU zones we track.

// Driver names whose hwmon power is disjoint from the package RAPL domain.
static const char* allow_drivers[] = { "amdgpu", "xe", 0 };

// A hwmon power/energy source: filename, whether it is an energy counter
// (µJ, differenced) or an instantaneous rate (µW, direct), and an optional
// child filename (a subset of the parent, nested as a child zone).
struct src { const char* fname; const char* child; int is_rate; };
static const struct src srcs[] =
{
	{ "energy1_input",  "energy2_input", 0 },	// xe: card (parent) + pkg (child).
	{ "power1_average", NULL,            1 },	// amdgpu: instantaneous µW.
	{ "power1_input",   NULL,            1 },	// amdgpu fallback.
	{ 0, 0, 0 }
};

// --- category-private state ---
static _Atomic uint64_t* out;
static int  base;
static int  count;
static FILE* files[HWMO_MAX];		// open data handle per zone.
static int   zone_is_rate[HWMO_MAX];	// 1 = rate (direct), 0 = counter (diff).
static uint64_t prev_uj[HWMO_MAX];	// last counter reading (counters only).

static pthread_t thread;
static int started = 0;

// Read a zone's raw value (µJ for counters, µW for rates). 0 on failure.
static uint64_t read_raw(int i)
{
	char s[32];
	s[0] = 0;
	int numrd = fread(s, 1, sizeof(s), files[i]);
	(void) numrd;
	rewind(files[i]);
	return (uint64_t) atol(s);
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Admit one zone: open its data file, record kind, set label + parent.
// Returns 1 if admitted, 0 otherwise. label may be NULL (then derive none).
static int admit(
	const char* node, const char* fname, int is_rate, int parent,
	const char* label,
	char names[][ZONE_NAME_LEN], int* parents)
{
	if (count >= HWMO_MAX)
		return 0;
	char fn[300];
	snprintf(fn, sizeof(fn), "%s/%s/%s", HWMO_ROOT, node, fname);
	FILE* f = fopen(fn, "r");
	if (!f)
		return 0;

	const int gi = base + count;
	files[count]      = f;
	zone_is_rate[count] = is_rate;
	prev_uj[count]    = 0;
	parents[gi]       = parent;
	snprintf(names[gi], ZONE_NAME_LEN, "%s", label);
	count++;
	return 1;
}

// Read a hwmon label file (e.g. energy2_label) into buf; empty on failure.
static void read_label(const char* node, const char* labelfile, char* buf, size_t n)
{
	buf[0] = 0;
	char fn[300];
	snprintf(fn, sizeof(fn), "%s/%s/%s", HWMO_ROOT, node, labelfile);
	FILE* f = fopen(fn, "r");
	if (f && fgets(buf, n, f))
		if (buf[strlen(buf)-1] == '\n')
			buf[strlen(buf)-1] = 0;
	if (f)
		fclose(f);
}

static void locate(char names[][ZONE_NAME_LEN], int* parents)
{
	DIR* d = opendir(HWMO_ROOT);
	if (!d)
		return;
	struct dirent* e;
	while ((e = readdir(d)))
	{
		if (e->d_type != DT_LNK && e->d_type != DT_DIR)
			continue;
		if (strncmp(e->d_name, "hwmon", 5))
			continue;

		// Driver name.
		char nf[300];
		snprintf(nf, sizeof(nf), "%s/%s/name", HWMO_ROOT, e->d_name);
		FILE* f = fopen(nf, "r");
		if (!f)
			continue;
		char drv[ZONE_NAME_LEN];
		drv[0] = 0;
		char* s = fgets(drv, sizeof(drv), f);
		fclose(f);
		if (!s)
			continue;
		if (drv[strlen(drv)-1] == '\n')
			drv[strlen(drv)-1] = 0;

		int ok = 0;
		for (int i = 0; allow_drivers[i]; ++i)
			if (!strcmp(drv, allow_drivers[i]))
				ok = 1;
		if (!ok)
			continue;

		// Probe the source table; first that opens wins.
		const struct src* src = 0;
		for (int i = 0; srcs[i].fname && !src; ++i)
		{
			char fn[300];
			snprintf(fn, sizeof(fn), "%s/%s/%s", HWMO_ROOT, e->d_name, srcs[i].fname);
			FILE* t = fopen(fn, "r");
			if (t)
			{
				fclose(t);
				src = &srcs[i];
			}
		}
		if (!src)
			continue;

		// Parent zone: labelled by the driver name (e.g. XE, AMDGPU).
		const int parent_gi = base + count;
		if (!admit(e->d_name, src->fname, src->is_rate, -1, drv, names, parents))
			continue;

		// Optional child sub-domain (e.g. xe pkg inside card).
		if (src->child)
		{
			// Derive the child's label file: "energy2_input" -> "energy2_label".
			char labelfile[32];
			const char* us = strchr(src->child, '_');
			int pre = us ? (int)(us - src->child) : (int)strlen(src->child);
			snprintf(labelfile, sizeof(labelfile), "%.*s_label", pre, src->child);
			char lbl[ZONE_NAME_LEN];
			read_label(e->d_name, labelfile, lbl, sizeof(lbl));
			admit(e->d_name, src->child, src->is_rate,
			      parent_gi, lbl[0] ? lbl : "pkg", names, parents);
		}
	}
	closedir(d);
}

static void* run(void* arg)
{
	(void)arg;

	// Prime counters and the clock.
	for (int i = 0; i < count; ++i)
		if (!zone_is_rate[i])
			prev_uj[i] = read_raw(i);
	int64_t t_prev = now_ms();

	while (!atomic_load_explicit(&quit, memory_order_relaxed))
	{
		struct timespec s = { 1, 0 };
		nanosleep(&s, NULL);

		const int64_t t_now = now_ms();
		int64_t dt = t_now - t_prev;
		if (dt < 1)
			dt = 1;
		t_prev = t_now;

		for (int i = 0; i < count; ++i)
		{
			const uint64_t raw = read_raw(i);
			uint64_t uw;
			if (zone_is_rate[i])
			{
				uw = raw;			// already µW.
			}
			else
			{
				uint64_t d = (raw >= prev_uj[i]) ? raw - prev_uj[i] : 0;
				prev_uj[i] = raw;
				uw = d * 1000ull / (uint64_t)dt;	// µJ/ms -> µW.
			}
			atomic_store_explicit(&out[base + i], uw, memory_order_relaxed);
		}
	}
	return NULL;
}

int hwmo_locate(_Atomic uint64_t* samples, char names[][ZONE_NAME_LEN], int* parents, int offset)
{
	out   = samples;
	base  = offset;
	count = 0;

	locate(names, parents);

	if (count > 0)
	{
		started = 1;
		pthread_create(&thread, NULL, run, NULL);
	}
	return count;
}

void hwmo_stop(void)
{
	if (!started)
		return;
	pthread_join(thread, NULL);
	for (int i = 0; i < count; ++i)
		if (files[i])
			fclose(files[i]);
	started = 0;
}
