#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <libusb-1.0/libusb.h>

#include "os_generic.h"
#include "color_utilities.h"

#define USB_VENDOR_ID  0x1209
#define USB_PRODUCT_ID 0x2305

#define NR_LEDS_PER_STRAND     301
#define block_size             512
#define STRANDS                32

#define USB_TIMEOUT 1024
#define TRANSFERS 8

uint8_t buffers[TRANSFERS][block_size];
static libusb_context *ctx = NULL;
static libusb_device_handle *handle;
int abortLoop = 0;
int xfertotal = 0;
int triggers = 0;
int done_frame = 0;
int frame_num;

uint32_t LEDs[NR_LEDS_PER_STRAND][STRANDS];

static void sighandler(int signum)
{
	printf( "\nInterrupt signal received\n" );
	abortLoop = 1;
}

////////////////////////////////////////////////////////////////////////
// LED Drawing function
////////////////////////////////////////////////////////////////////////
static inline void UpdateLEDs()
{
	static uint32_t phases[NR_LEDS_PER_STRAND][STRANDS];

	int l;
	int s;

	srand( 0 );
	for( s = 0; s < 32; s++ )
	{
		for( l = 0; l < NR_LEDS_PER_STRAND; l++ )
		{
			int ph = phases[l][s] += (rand()%1024)+512;

			ph = (ph>>10) & 0xff;

			LEDs[l][s] = EHSVtoHEX( ph, 255, 255 );
		}
	}
}

////////////////////////////////////////////////////////////////////////
// USB Callback
////////////////////////////////////////////////////////////////////////
int USBCallbackFill( uint8_t * data )
{
	static int ledstate;
	const int leds_per_transfer = 5;
	const int num_leds_per_string = NR_LEDS_PER_STRAND;

	uint32_t * lb = (uint32_t*)data;

	int terminal = 0;
	int ledno = ledstate * leds_per_transfer;
	int ledremain = num_leds_per_string - ledno;
	if( ledremain <= leds_per_transfer )
	{
		terminal = 1;
	}
	else
	{
		ledremain = leds_per_transfer;
	}

	lb[0] = ((terminal?0x8000:0x0000) | ledstate*leds_per_transfer*24) | // Offset
			( ledremain * 24 ) << 16;

	uint32_t * lbo = lb+1;

	int l;
	for( l = 0; l < leds_per_transfer; l++ )
	{
		int bit;
		int ledid = leds_per_transfer * ledstate + l;

		uint32_t * ledrow = LEDs[ledid];
		for( bit = 0; bit < 24; bit++ )
		{
			uint32_t bitmask = 1<<(23-bit);
			uint32_t outmask = 0;
			int strand = 0;
			for( strand = 0; strand < STRANDS; strand++ )
				outmask |= (!!(ledrow[strand]&bitmask))<<strand;

			lbo[bit+l*24] = outmask;
		}
	}


	if( terminal )
	{
		frame_num++;
		ledstate = 0;
		triggers++;
		done_frame = 1;
	}
	else
	{
		ledstate++;
	}

	return 512;
}

void xcallback (struct libusb_transfer *transfer)
{
	xfertotal += transfer->actual_length;
	transfer->length = USBCallbackFill( transfer->buffer );
	libusb_submit_transfer( transfer );
}


int main(int argc, char **argv)
{
	//Pass Interrupt Signal to our handler
	signal(SIGINT, sighandler);

	libusb_init(&ctx);
	libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, 3);

	handle = libusb_open_device_with_vid_pid( ctx, USB_VENDOR_ID, USB_PRODUCT_ID );
	if ( !handle )
	{
		fprintf( stderr, "Error: couldn't find handle\n" );
		return 1;
	}

	libusb_detach_kernel_driver(handle, 3);

	int r = 1;
	r = libusb_claim_interface(handle, 3 );
	if( r < 0 )
	{
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		return 2;
	}

	double dRecvTotalTime = 0;
	double dSendTotalTime = 0;
	double dLastPrint = OGGetAbsoluteTime();
	int rtotal = 0, stotal = 0;

	// Async (Fast, bulk)
	// About 260-320 Mbit/s

	struct libusb_transfer * transfers[TRANSFERS];
	int n;
	for( n = 0; n < TRANSFERS; n++ )
	{
		int k;
		for( k = 0; k < block_size; k++ )
		{
			buffers[n][k] = k;
		}
		struct libusb_transfer * t = transfers[n] = libusb_alloc_transfer( 0 );
		libusb_fill_bulk_transfer( t, handle, 0x05 /*Endpoint for send */, buffers[n], block_size, xcallback, (void*)(intptr_t)n, 1000 );
		t->length = USBCallbackFill( t->buffer );
		libusb_submit_transfer( t );
	}


	while(!abortLoop)
	{
		double dNow = OGGetAbsoluteTime();

		libusb_handle_events(ctx);

		if( dNow - dLastPrint > 1 )
		{
			dSendTotalTime = dNow - dLastPrint;
			printf( "%f MB/s %cX (%d FPS)\n", xfertotal / (dSendTotalTime * 1024 * 1024), 'T', triggers );
			triggers = 0;
			xfertotal = 0;
			dSendTotalTime = 0;
			dLastPrint = dNow;
		}

		if( done_frame )
		{
			UpdateLEDs();

			done_frame = 0;
		}
	}

	for( n = 0; n < TRANSFERS; n++ )
	{
		libusb_cancel_transfer( transfers[n] );
	 	libusb_free_transfer( transfers[n] );
	}

	libusb_release_interface (handle, 0);
	libusb_close(handle);
	libusb_exit(NULL);

	return 0;
}

