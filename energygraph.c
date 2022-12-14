// energygraph.c
//
// Graphs the energy use of a host inside a text terminal, using intel-rapl data from the /sys filesystem.
// (c)2022 Bram Stolk (b.stolk@gmail.com)

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <math.h>
#include <ctype.h>
#include <sys/types.h>
#include <dirent.h>

#include "grapher.h"	// plots an image to the terminal.
#include "hsv.h"	// hue-saturation-value colour conversions.

enum domain
{
	DOM_PLATFORM=0,
	DOM_PACKAGE0,
	DOM_UNCORE,
	DOM_CORE,
	DOM_DRAM,
	DOM_COUNT
};


#define MAXZONES	10

// How many (sub)zones in total?
static int numzones=0;

// Directory names in /sys for all the RAPL zones.
static char dnames[MAXZONES][128];

// Zone names
static char names[MAXZONES][32];

// Zone names, but capitalized.
static char capnames[MAXZONES][32];

// File handles for energy_uj for all the RAPL zones.
static FILE* files[MAXZONES];

// Previous values.
static int64_t prev[MAXZONES];

// Latest values.
static int64_t curr[MAXZONES];

// Zone parents, -1 if top level.
static int parents[MAXZONES];

// Zone number of children.
static int numchild[MAXZONES];

// Zone colours.
static float hues[MAXZONES];
static uint32_t colours[MAXZONES];



// History of statistics.
#define MAXHIST		200
typedef int64_t measurement_t[MAXZONES];
measurement_t hist[MAXHIST];
int32_t head = 0;
int32_t tail = 0;
int32_t maxuw = 4000000;	// 1 watt


int histsz(void)
{
	int sz = tail - head;
	sz = sz < 0 ? sz + MAXHIST : sz;
	return sz;
}


// For time keeping.
int64_t elapsed_ms_since_last_call( void )
{
	static int virgin = 1;
	static struct timespec prev;
	struct timespec curr;
	if ( virgin )
	{
		clock_gettime( CLOCK_MONOTONIC, &prev );
		virgin = 0;
	}
	clock_gettime( CLOCK_MONOTONIC, &curr );
	const int64_t delta_sec  = curr.tv_sec  - prev.tv_sec;
	const int64_t delta_nsec = curr.tv_nsec - prev.tv_nsec;
	const int64_t delta = delta_sec * 1000 + delta_nsec / 1e6;
	prev = curr;
	return delta;
}


static int locate_rapl_data(const char* dirname, int parent)
{
	DIR* dir = opendir(dirname);
	if (!dir)
	{
		fprintf(stderr, "Failed to open directory: %s (%s)\n", dirname, strerror(errno));
		exit(5);
	}
	struct dirent* ent;
	int numfound=0;
	while ((ent = readdir(dir)))
	{
		if (ent->d_type == DT_DIR)
			if (strstr(ent->d_name, "intel-rapl:"))
			{
				numfound++;
				const char* dn = ent->d_name;
				assert(strlen(dn) < 32);
				assert(strlen(dirname)<96);
				// Create a new entry in our zone data.
				const int idx = numzones++;
				assert(idx < MAXZONES);
				parents[idx] = parent;
				snprintf(dnames[idx], sizeof(dnames[idx]), "%s/%s", dirname, dn);
				char namefname[128];
				assert(strlen(dnames[idx])<120);
				snprintf(namefname, sizeof(namefname), "%s/name", dnames[idx]);
				FILE* f = fopen(namefname,"r");
				if (!f)
				{
					fprintf(stderr, "Failed to read %s (%s)\n", namefname, strerror(errno));
					exit(6);
				}
				char* s = fgets(names[idx], sizeof(names[idx]), f);
				assert(s);
				fclose(f);
				if (s[strlen(s)-1] == '\n') s[strlen(s)-1]=0;
				for (int i=0; names[idx][i]; ++i)
					capnames[idx][i] = toupper(names[idx][i]);
				// See if there are subzones for this.
				numchild[idx] = locate_rapl_data(dnames[idx], idx);
			}
	}
	closedir(dir);
	return numfound;
}


// Read a values, and determine delta for each domain.
static void read_values(measurement_t deltas)
{
	for (int z=0; z<numzones; ++z)
	{
		char s[32];
		s[0]=0;
		int numrd = fread(s, 1, sizeof(s), files[z]);
		(void) numrd;
		rewind(files[z]);
		curr[z] = atol(s);
		deltas[z] = curr[z] - prev[z];
		deltas[z] = deltas[z] < 0 ? 0 : deltas[z];
		prev[z] = curr[z];
	}
}


static void set_postscript(void)
{
	postscript[0] = 0;
	// Traverse the top level zones.
	for (int z=0; z<numzones; ++z)
		if (parents[z]==-1)
		{
			char tag[64];
			snprintf
			(
				tag,
				sizeof(tag), 
				SETFG "%d;%d;%dm" "%s%c ",
				(colours[z]>> 0)&0xff,
				(colours[z]>> 8)&0xff,
				(colours[z]>>16)&0xff,
				capnames[z],
				numchild[z] ? ':' : ' '
			);
			strncat(postscript, tag, sizeof(postscript) - 1 - strlen(postscript));
			// Traverse children of this top level zone.
			for (int c=0; c<numzones; ++c)
				if (parents[c] == z)
				{
					char tag[64];
					snprintf
					(
						tag,
						sizeof(tag),
						SETFG "%d;%d;%dm" "%s ",
						(colours[c]>> 0)&0xff,
						(colours[c]>> 8)&0xff,
						(colours[c]>>16)&0xff,
						names[c]
					);
					strncat(postscript, tag, sizeof(postscript) - 1 - strlen(postscript));
				}
			strncat(postscript, "  ", sizeof(postscript) - 1 - strlen(postscript));
		}
}


static int update_image(void)
{
	if ( grapher_resized )
	{
		grapher_adapt_to_new_size();
	}

	grapher_update();
	return 0;
}


static void choose_colours(void)
{
	int numtoplvl = 0;
	int h=0;
	for (int i=0; i<numzones; ++i)
		if (parents[i]==-1)
			numtoplvl++;
	for (int i=0; i<numzones; ++i)
		if (parents[i]==-1)
			hues[i] = (0.15f + h++) / numtoplvl;	// our own hue.
	for (int i=0; i<numzones; ++i)
		if (parents[i] > -1)
			hues[i] = hues[ parents[i] ];		// inherit parent's hue.
	const float saturations[4] = { 0.75f, 0.60f, 0.90f, 0.60f };
	const float values     [4] = { 0.75f, 0.90f, 0.60f, 0.60f };
	int subidx=0;
	for (int i=0; i<numzones; ++i)
	{
		float hue = hues[i];
		float sat = 1;
		float val = 1;
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
	}
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
	const int x  = imw-2-histidx;
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
	// Clear the background with black and grey.
	const uint8_t blck = 0x12;
	const uint8_t grey = 0x1f;
	for (int y=0; y<imh; ++y)
	{
		const uint8_t v = ( ((y*4) / imh) & 1 ) ? grey : blck;
		memset(im + y*imw, v, sizeof(uint32_t)*imw);
	}
	// Iterates over the columns (1 sample per column.)
	for (int j=0; j<imw-2; ++j)
	{
		if (j<hsz)	// Enough history to draw this column?
		{
			int h = tail-1-j;
			h = h < 0 ? h + MAXHIST : h;
			int64_t off=0;
			int64_t offsets[MAXZONES];
			// Set offset for top level zones.
			for (int z=0; z<numzones; ++z)
				if (parents[z] == -1)
				{
					offsets[z] = off;
					off += hist[h][z];
				}
			// Set offset for child zones.
			int suboff=0;
			for (int z=0; z<numzones; ++z)
				if (parents[z] == -1)
					suboff = 0;
				else
				{
					offsets[z] = offsets[ parents[z] ] + suboff;
					suboff += hist[h][z];
				}
			// draw parent bars.
			for (int z=0; z<numzones; ++z)
				if (parents[z] == -1)
					overflow += draw_range(j, colours[z], offsets[z], offsets[z] + hist[h][z]);
			// draw child bars.
			for (int z=0; z<numzones; ++z)
				if (parents[z] > -1)
					overflow += draw_range(j, colours[z], offsets[z], offsets[z] + hist[h][z]);
		}
	}
	if (overflow)
		maxuw *= 2;
}


int main(int argc, char* argv[])
{
	const char* rapl_dir = "/sys/devices/virtual/powercap/intel-rapl";
	const int numfound = locate_rapl_data(rapl_dir, -1);
	if (!numfound)
	{
		fprintf(stderr,"Found zero RAPL entries in your sysfs.\n");
		exit(3);
	}

	for (int z=0; z<numzones; ++z)
	{
		char fname[140];
		snprintf(fname, sizeof(fname)-1, "%s/energy_uj", dnames[z]);
		files[z] = fopen(fname,"r");
		if (!files[z])
		{
			fprintf(stderr,"Failed to open %s (%s)\n",  fname, strerror(errno));
			exit(2);
		}
	}
	measurement_t deltas;
	read_values(deltas);
	(void)elapsed_ms_since_last_call();

	choose_colours();
	set_postscript();

	int result = grapher_init();
	if ( result < 0 )
	{
		fprintf( stderr, "Failed to intialize grapher(), maybe we are not running in a terminal?\n" );
		exit(2);
	}
	enableRawMode();
	printf(SETBG "0;0;0m");
	printf(CLEARSCREEN);
	update_image();

	int done=0;
	do
	{
		sleep(1);
		// Record a sample
		const uint32_t idx = tail;
		read_values(hist[idx]);
		tail = (tail + 1) % MAXHIST;
		if ( tail == head )
			head = (head + 1) % MAXHIST;

		// Compensate for time interval
		int64_t ms = elapsed_ms_since_last_call();
		for (int z=0; z<numzones; ++z)
			hist[idx][z] = hist[idx][z] * 1000 / ms;

		// Draw the samples
		draw_overlay();
		draw_samples();
		update_image();

		// See if user pressed ESC.
		char c=0;
		const int numr = read( STDIN_FILENO, &c, 1 );
		if ( numr == 1 && ( c == 27 || c == 'q' || c == 'Q' ) )
			done=1;
	} while(!done);

	grapher_exit();

	return 0;
}

