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


// Find the correct entries in /sys that we need.
static int locate_rapl_data(void)
{
	const char* cmd = "find /sys/devices/virtual/powercap/intel-rapl/ -name name -print -exec cat {} \\;";
	FILE* f = popen(cmd,"r");
	char fnam[128];
	char name[128];
	numzones=0;
	while(1)
	{
		const char* s0 = fgets(fnam, sizeof(fnam), f);
		const char* s1 = fgets(name, sizeof(name), f);
		if (!s0 || !s1)
			break;
		if (name[strlen(name)-1] == '\n') name[strlen(name)-1]=0;
		int idx = numzones++;
		strncpy(names[idx], name, sizeof(names[idx]));
		char* s = strstr(fnam,"/name");
		assert(s);
		*s = 0;
		strncpy(dnames[idx], fnam, sizeof(dnames[idx])-1);
	}

	for (int i=0; i<numzones; ++i)
		numchild[i] = 0;
	for (int i=0; i<numzones; ++i)
	{
		int p=-1;
		for (int j=0; j<numzones; ++j)
			if (i!=j && strstr(dnames[i], dnames[j]))
				p=j;
		parents[i] = p;
		if (p>=0)
			numchild[p] += 1;
	}
	for (int i=0; i<numzones; ++i)
	{
		if (parents[i]==-1)
		{
			fprintf(stderr,"%s\n", names[i]);
			for (int j=0; j<numzones; ++j)
				if (parents[j] == i)
					fprintf(stderr, "  %s\n", names[j]);
		}
	}
	return numzones;
}


// Read a values, and determine delta for each domain.
static void read_values(measurement_t deltas)
{
	for (int z=0; z<numzones; ++z)
	{
		char s[32];
		s[0]=0;
		fread(s, 1, sizeof(s), files[z]);
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
				names[z],
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
	snprintf(overlay + imw * (imh/8 * 0) + 1, 80, "%d mW", 4*quartermw);
	snprintf(overlay + imw * (imh/8 * 1) + 1, 80, "%d mW", 3*quartermw);
	snprintf(overlay + imw * (imh/8 * 2) + 1, 80, "%d mW", 2*quartermw);
	snprintf(overlay + imw * (imh/8 * 3) + 1, 80, "%d mW", 1*quartermw);
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
	memset(im, 0, sizeof(uint32_t)*imw*imh);
	const int hsz = histsz();
	int overflow=0;
	for (int j=0; j<imw-2; ++j)
	{
		if (j<hsz)
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
	const int numfound = locate_rapl_data();
	(void)numfound;

	for (int z=0; z<numzones; ++z)
	{
		char fname[128];
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
#if 0
		fprintf
		(
			stderr,
			"platform %10ld | "
			"package-0 %10ld | "
			"core %10ld | "
			"uncore %10ld\n",
			hist[idx][0],
			hist[idx][3],
			hist[idx][1],
			hist[idx][2]
		);
#endif
		// See if user pressed ESC.
		char c=0;
		const int numr = read( STDIN_FILENO, &c, 1 );
		if ( numr == 1 && ( c == 27 || c == 'q' || c == 'Q' ) )
			done=1;
	} while(!done);

	grapher_exit();

	return 0;
}

