// main.c - energygraph: renders per-zone power (Watts) in a text terminal.
//
// Data acquisition is delegated to category threads (rapl/hwmo/nvml), each
// publishing microwatts into its own slice of the shared atomic samples[].
// This thread is a pure ~1Hz renderer: snapshot samples[] into a history
// column, draw. It times nothing critical - a slow paint or a blocked
// category thread just means a value is repeated, never a stall.
//
// (c)2022-2026 Bram Stolk (b.stolk@gmail.com)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <ctype.h>
#include <stdatomic.h>
#include <stdint.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <assert.h>


#include "grapher.h"	// plots an image to the terminal.
#include "hsv.h"	// hue-saturation-value colour conversions.

#include "zones.h"
#include "rapl.h"	// DRAM, intel iGPU/CPU, amd CPU reports here.
#include "hwmo.h"	// AMD iGPU/dGPU, intel dGPU reports here.
#include "nvml.h"	// NVIDIA dPGU reports here.

// --- shared with the category threads (defined here) ---
_Atomic int quit = 0;				// set on exit; threads poll and stop.
static _Atomic uint64_t samples[MAXZONES];	// µW, written by category threads.
static char  names[MAXZONES][ZONE_NAME_LEN];	// zone labels, filled at locate.
static int   parents[MAXZONES];			// -1 top-level, else parent index.

// --- main-owned (derived after locates) ---
static int      numzones = 0;
static int	num_xe = 0;
static int	num_ag = 0;
static int	num_nv = 0;
static int	num_ot = 0;
static int	bins[MAXZONES];			// in xe, ag, nv or other bin?
static int	binidx[MAXZONES];		// offset index into assigned bin.
static char     capnames[MAXZONES][ZONE_NAME_LEN];
static int      numchild[MAXZONES];
static float    hues[MAXZONES];
static uint32_t colours[MAXZONES];

#define NUMBINS 4	// Bin on: XE, AMDGPU, NVIDIA, OTHER.
static float	hue_lo[NUMBINS];
static float	hue_hi[NUMBINS];

// --- history ---
#define MAXHIST 200
typedef int64_t measurement_t[MAXZONES];
static measurement_t hist[MAXHIST];
static int32_t head = 0;
static int32_t tail = 0;
static int32_t maxuw = 4000000;			// initial y-scale: ~4W.

static int histsz(void)
{
	int sz = tail - head;
	return sz < 0 ? sz + MAXHIST : sz;
}

// Capitalize labels and count children, once, after locates.
static void derive_meta(void)
{
	// assign it to a bin, so it can be properly coloured.
	for (int z=0; z<numzones; ++z)
	{
		if (!strcmp(names[z], "xe"))
		{
			binidx[z] = num_xe++;
			bins[z] = 0;
		}
		else if (!strcmp(names[z], "amdgpu"))
		{
			binidx[z] = num_ag++;
			bins[z] = 1;
		}
		else if (!strcmp(names[z], "nvidia"))
		{
			binidx[z] = num_nv++;
			bins[z] = 2;
		}
		else
		{
			binidx[z] = num_ot++;
			bins[z] = 3;
		}
		capnames[z][0] = 0;
		for (int i=0; names[z][i]; ++i)
			capnames[z][i] = toupper((unsigned char)names[z][i]);
		capnames[z][ (int)strlen(names[z]) ] = 0;
	}
	for (int z=0; z<numzones; ++z)
		numchild[z] = 0;
	for (int z=0; z<numzones; ++z)
		if (parents[z] >= 0)
			numchild[ parents[z] ]++;
}

static void choose_colours(void)
{
	// xe : blues.
	hue_lo[0] = 210.0f / 360.0f;
	hue_hi[0] = 270.0f / 360.0f;
	// ag : reds
	hue_lo[1] = 335.0f / 360.0f;
	hue_hi[1] = 380.0f / 360.0f;
	// nv : greens
	hue_lo[2] = 100.0f / 360.0f;
	hue_hi[2] = 130.0f / 360.0f;
	// other: yellows
	hue_lo[3] =  45.0f / 360.0f;
	hue_hi[3] =  75.0f / 360.0f;
	int bin_sizes[4] = { num_xe, num_ag, num_nv, num_ot, };
	//fprintf(stderr, "bin_sizes %d %d %d %d\n", num_xe, num_ag, num_nv, num_ot);

	// Top level zones get a hue based on their position in their xe/ag/nv/ot bin.
	for (int i=0; i<numzones; ++i)
	{
		if (parents[i]==-1)
		{
			const int b = bins[i];
			const int bsz = bin_sizes[b];
			float t = (binidx[i] + 0.5f) / bsz;
			float h = hue_hi[b] * t + hue_lo[b] * (1.0f - t);
			hues[i] = h >= 1.0f ? h - 1.0f : h;
		}
	}
	// Parented zones inherit the hue from their parent.
	for (int i=0; i<numzones; ++i)
		if (parents[i] > -1)
			hues[i] = hues[ parents[i] ];

	const float saturations[4] = { 0.75f, 0.60f, 0.90f, 0.60f };
	const float values     [4] = { 0.75f, 0.90f, 0.60f, 0.60f };
	int subidx=0;
	for (int i=0; i<numzones; ++i)
	{
		float hue = hues[i];
		float sat = 1, val = 1;
		if (parents[i] == -1)
		{
			subidx=0;
			sat = saturations[0];
			val = values     [0];
		}
		else
		{
			const int j = subidx++ % 3;
			sat = saturations[1+j];
			val = values     [1+j];
		}
		colours[i] = hsv_to_rgb24(hue, sat, val);
		//fprintf(stderr, "hsv %f %f %f : %08x\n", hue, sat, val, colours[i]);
	}
}

static void set_postscript(void)
{
	postscript[0] = 0;
	for (int z=0; z<numzones; ++z)
		if (parents[z]==-1)
		{
			char tag[64];
			snprintf(tag, sizeof(tag), SETFG "%d;%d;%dm" "%s%c ",
				(colours[z]>> 0)&0xff, (colours[z]>> 8)&0xff, (colours[z]>>16)&0xff,
				capnames[z], numchild[z] ? ':' : ' ');
			strncat(postscript, tag, sizeof(postscript) - 1 - strlen(postscript));
			for (int c=0; c<numzones; ++c)
				if (parents[c] == z)
				{
					char t2[64];
					snprintf(t2, sizeof(t2), SETFG "%d;%d;%dm" "%s ",
						(colours[c]>> 0)&0xff, (colours[c]>> 8)&0xff, (colours[c]>>16)&0xff,
						names[c]);
					strncat(postscript, t2, sizeof(postscript) - 1 - strlen(postscript));
				}
			strncat(postscript, " ", sizeof(postscript) - 1 - strlen(postscript));
		}
}

static int update_image(void)
{
	if (grapher_resized)
		grapher_adapt_to_new_size();
	grapher_update();
	return grapher_resized;
}

static void draw_overlay(void)
{
	uint32_t quartermw = maxuw / 1000 / 4;
	for (int i=0; i<4; ++i)
	{
		const int mw = (4-i) * quartermw;
		const int val = mw >= 10000 ? mw/1000 : mw;
		const char* units = mw >= 10000 ? "W" : "mW";
		memset(overlay + imw * (imh/8 * i) + 1, 0, imw < 8 ? imw : 8);
		snprintf(overlay + imw * (imh/8 * i) + 1, 80, "%d %s", val, units);
	}
}

static int draw_range(int histidx, uint32_t colour, int64_t fr, int64_t to)
{
	assert(fr>=0 && to>=0);
	assert(to>=fr);
	const int x = imw-2-histidx;
	const int l0 = fr * imh / maxuw;
	const int l1 = to * imh / maxuw;
	const int y_hi = imh-1-l0;
	const int y_lo = imh-1-l1;
	for (int y=y_lo; y<=y_hi; ++y)
		if (y>=0 && y<imh)
			im[ y * imw + x ] = colour;
	return l1 >= imh-1;
}

static void draw_samples(void)
{
	const int hsz = histsz();
	int overflow=0;
	const uint8_t blck = 0x12;
	const uint8_t grey = 0x1f;
	for (int y=0; y<imh; ++y)
	{
		const uint8_t v = ( ((y*4) / imh) & 1 ) ? grey : blck;
		memset(im + y*imw, v, sizeof(uint32_t)*imw);
	}
	for (int j=0; j<imw-2; ++j)
	{
		if (j<hsz)
		{
			int h = tail-1-j;
			h = h < 0 ? h + MAXHIST : h;
			int64_t off=0;
			int64_t offsets[MAXZONES];
			for (int z=0; z<numzones; ++z)
				if (parents[z] == -1)
				{
					offsets[z] = off;
					off += hist[h][z];
				}
			int suboff=0;
			for (int z=0; z<numzones; ++z)
				if (parents[z] == -1)
					suboff = 0;
				else
				{
					offsets[z] = offsets[ parents[z] ] + suboff;
					suboff += hist[h][z];
				}
			for (int z=0; z<numzones; ++z)
				if (parents[z] == -1)
					overflow += draw_range(j, colours[z], offsets[z], offsets[z] + hist[h][z]);
			for (int z=0; z<numzones; ++z)
				if (parents[z] > -1)
					overflow += draw_range(j, colours[z], offsets[z], offsets[z] + hist[h][z]);
		}
	}
	if (overflow)
		maxuw *= 2;
}

// Terminal handling
static struct termios orig_termios;
static void disableRawMode()
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void enableRawMode()
{
	tcgetattr(STDIN_FILENO, &orig_termios);
	atexit(disableRawMode);
	struct termios raw = orig_termios;
	raw.c_lflag &= ~(ECHO);			// Don't echo key presses.
	raw.c_lflag &= ~(ICANON);		// Read by char, not by line.
	raw.c_cc[VMIN] = 0;			// No minimum nr of chars.
	raw.c_cc[VTIME] = 0;			// No waiting time.
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}


int main(int argc, char* argv[])
{
	(void)argc; (void)argv;

	// Enumerate all categories; each fills its slice of names[]/parents[] and
	// spawns its thread (only if it claimed zones).
	int n = 0;
	n += rapl_locate(samples, names, parents, n);
	n += nvml_locate(samples, names, parents, n);
	n += hwmo_locate(samples, names, parents, n);
	numzones = n;

	for (int i=0; i<numzones; ++i)
		assert(parents[i] == -1 || parents[i] < i);

	if (numzones == 0)
	{
		fprintf(stderr, "Found zero power zones (no RAPL, GPU hwmon, or NVML).\n");
		exit(3);
	}

	derive_meta();
	choose_colours();
	set_postscript();

	if ( grapher_init() < 0 )
	{
		fprintf(stderr, "Failed to initialize grapher(); not running in a terminal?\n");
		quit = 1;
		rapl_stop(); hwmo_stop(); nvml_stop();
		exit(2);
	}

	enableRawMode();
	printf(SETBG "0;0;0m");
	printf(CLEARSCREEN);
	update_image();

	int done=0;
	do
	{
		struct timespec s = { 0, 999999999 };	// ~1Hz render cadence, unless resizing.
		nanosleep(&s, NULL);

		// Snapshot the atomics into a new history column. Each zone holds its
		// latest µW; a blocked category thread just repeats its last value.
		const uint32_t idx = tail;
		for (int z=0; z<numzones; ++z)
			hist[idx][z] = (int64_t) atomic_load_explicit(&samples[z], memory_order_relaxed);
		tail = (tail + 1) % MAXHIST;
		if ( tail == head )
			head = (head + 1) % MAXHIST;

		draw_overlay();
		draw_samples();
		update_image();

		// ESC / q to quit (non-blocking; VMIN/VTIME=0).
		char c=0;
		if ( read(STDIN_FILENO, &c, 1) == 1 && (c==27 || c=='q' || c=='Q') )
			done = 1;
	} while(!done);

	// Stop acquisition threads, restore terminal, exit.
	quit = 1;
	rapl_stop();
	hwmo_stop();
	nvml_stop();
	grapher_exit();
	return 0;
}
