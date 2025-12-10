#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define NR_LEDS_PER_STRAND	 (215)
#define block_size			 512
#define STRANDS				32
#define USB_TIMEOUT 1024
#define TRANSFERS 8
#define DEVICES 2
const char * serials[DEVICES] = { "f38dabcd5b6ebc14", "d3f9abcd3bdabc14" };

static int abortLoop = 0;
static int triggers = 0;

#include "x32breakoutdriver.h"

////////////////////////////////////////////////////////////////////////
// LED Drawing function
////////////////////////////////////////////////////////////////////////

static inline int hue2( const int n )
{
	if( n < 64 )
		return n * 4;
	else if( n < 128 )
		return 255;
	else if( n < 192 )
		return 252 - (n-128)*4;
	else return 0;
		
}

static inline void UpdateLEDs()
{
	static uint32_t phases[DEVICES][NR_LEDS_PER_STRAND][STRANDS];

	int d;
	int l;
	int s;

	srand( 0 );
	for( d = 0; d < DEVICES; d++ )
	{
		for( s = 0; s < 32; s++ )
		{
			for( l = 0; l < NR_LEDS_PER_STRAND; l++ )
			{
				int ph = phases[d][l][s] += (rand()%1024)+512;

				ph = (ph>>10) & 0xff;

				//LEDs[l][s] = EHSVtoHEX( ph, 255, 64 );

				int r = hue2((ph + 0)&0xff);
				int g = hue2((ph + 85)&0xff);
				int b = hue2((ph + 171)&0xff);

				r-=128;
				g-=128;
				b-=128;

				if( r < 0 ) r = 0;
				if( g < 0 ) g = 0;
				if( b < 0 ) b = 0;
				if( r > 255 ) r = 255;
				if( g > 255 ) g = 255;
				if( b > 255 ) b = 255;

				LEDs[d][l][s] = r | (g<<8) | (b<<16);
			}
		}
	}
}

static void sighandler(int signum)
{
	printf( "\nInterrupt signal received\n" );
	abortLoop = 1;
}

int main(int argc, char **argv)
{
	double dSendTotalTime = 0;
	double dLastPrint = OGGetAbsoluteTime();

	//Pass Interrupt Signal to our handler
	signal(SIGINT, sighandler);

	SetupBreakoutDriver();

	while(!abortLoop)
	{
		double dNow = OGGetAbsoluteTime();

		// TODO: Make this timeoutable.
		TickBreakoutDriver();

		if( dNow - dLastPrint > 1 )
		{
			dSendTotalTime = dNow - dLastPrint;
			printf( "%f MB/s %cX (%d FPS)\n", xfertotal / (dSendTotalTime * 1024 * 1024), 'T', triggers );
			triggers = 0;
			xfertotal = 0;
			dSendTotalTime = 0;
			dLastPrint = dNow;
		}
		if( done_frame == done_mask )
		{
			// TODO: Make this threadable, with a semaphore maybe?
			frame_num++;
			triggers++;
			done_frame = 0;
			UpdateLEDs();
		}
	}

	CleanupBreakoutDriver();

	return 0;
}

