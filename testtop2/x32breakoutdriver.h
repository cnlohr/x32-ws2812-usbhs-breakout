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

// Define some basics.
//#define NR_LEDS_PER_STRAND	 (215)
//#define block_size			 512
//#define STRANDS				32
//#define USB_TIMEOUT 1024
//#define TRANSFERS 8
//#define DEVICES 2
// Define serials
// const char * serials[DEVICES] = { "f38dabcd5b6ebc14", "d3f9abcd3bdabc14" };

// You must provide this.
static inline void UpdateLEDs();

// Call these.
int SetupBreakoutDriver();
void TickBreakoutDriver();
void CleanupBreakoutDriver();

static uint8_t buffers[DEVICES][TRANSFERS][block_size];
static libusb_context *ctx = NULL;
libusb_hotplug_callback_handle callback_handle;
static libusb_device_handle * handles[DEVICES];
static libusb_device * devList[DEVICES];
static struct libusb_transfer * transfers[DEVICES][TRANSFERS];
static int xfertotal = 0;
static int done_frame = 0;
static int done_mask = 0;
static int frame_num;

uint32_t LEDs[DEVICES][NR_LEDS_PER_STRAND][STRANDS];
int configured[DEVICES];


////////////////////////////////////////////////////////////////////////
// USB Callback
////////////////////////////////////////////////////////////////////////
int USBCallbackFill( uint8_t * data, int device )
{
	if( !configured[device] )
	{
		configured[device] = 1;
		uint16_t * d = (uint16_t*)data;
		d[0] = 0xffff; // We are going to configure mode.
		d[1] = 0xffff;
		d[2] = 37*4;  // Period
		d[3] = 0;     // One timing
		d[4] = 13*4;  // Data timing
		d[5] = 26*4;  // Zero timing
		return 6*2;
	}

	static int ledstate[DEVICES];

	const int leds_per_transfer = 5;
	const int num_leds_per_string = NR_LEDS_PER_STRAND;

	uint32_t * lb = (uint32_t*)data;

	int terminal = 0;
	int ledno = ledstate[device] * leds_per_transfer;
	int ledremain = num_leds_per_string - ledno;
	if( ledremain <= leds_per_transfer )
	{
		terminal = 1;
	}
	else
	{
		ledremain = leds_per_transfer;
	}

	lb[0] = ((terminal?0x8000:0x0000) | ledstate[device]*leds_per_transfer*24) | // Offset
			( ledremain * 24 ) << 16;

	uint32_t * lbo = lb+1;

	int l;
	for( l = 0; l < leds_per_transfer; l++ )
	{
		int bit;
		int ledid = leds_per_transfer * ledstate[device] + l;

		uint32_t * ledrow = LEDs[device][ledid];
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
		ledstate[device] = 0;
		done_frame |= 1<<device;
	}
	else
	{
		ledstate[device]++;
	}

	return 512;
}

void xcallback (struct libusb_transfer *transfer)
{
	xfertotal += transfer->actual_length;
	transfer->length = USBCallbackFill( transfer->buffer, (intptr_t)transfer->user_data );
	libusb_submit_transfer( transfer );
}

void DeviceArrive( struct libusb_device *dev )
{
	struct libusb_device_descriptor desc;
	libusb_device_handle *thandle;
	int r = libusb_get_device_descriptor(dev, &desc);
	uint8_t sserial[64];

	if( desc.idVendor != USB_VENDOR_ID || desc.idProduct != USB_PRODUCT_ID )
	{
		return;
	}

	int err = libusb_open(dev, &thandle);
	if( err )
	{
		fprintf( stderr, "Error opening device.  Did you forget to `make install_udev_rules`?\n" );
		return;
	}

	int captured = 0;
	int device = 0;
	fprintf( stderr, "Adding\n" );
	if( thandle )
	{
		int retry = 100;
		fprintf( stderr, "Getting serial from ID %d\n", desc.iSerialNumber );
do_try_again:
		int serv = libusb_get_string_descriptor_ascii( thandle, desc.iSerialNumber, sserial, 63 );
		if( serv >= 0 )
		{
			sserial[63] = '\0';
			fprintf( stderr, "Found serial: %s ", sserial );
			int n;
			for( n = 0; n < DEVICES; n++ )
			{
				if( strcmp( serials[n], (const char*)sserial ) == 0 )
				{
					fprintf( stderr, "captured as %d\n", n );
					captured = 1;
					handles[n] = thandle;
					devList[n] = dev;
					device = n;
				}
			}
			if( !captured)
			{
				fprintf( stderr, "skipped\n" );
			}
		}
		else
		{
			if( retry-- )
			{
				usleep(5000);
				goto do_try_again;
			}
			fprintf( stderr, "no serial ID found (serv = %d)\n", serv );
		}
	}
	if( !captured )
	{
		fprintf( stderr, "No device match\n" );
		libusb_close(thandle);
		return;
	}

	libusb_device_handle * handle = handles[device];
	if( !handle )
	{
		fprintf( stderr, "Error: device with serial %s not found\n", serials[device] );
		return;
	}

	libusb_detach_kernel_driver(handle, 3);

	r = libusb_claim_interface(handle, 3 );
	if( r < 0 )
	{
		fprintf(stderr, "usb_claim_interface error %d\n", r);
		return;
	}

	int n;
	for( n = 0; n < TRANSFERS; n++ )
	{
		struct libusb_transfer * t = transfers[device][n] = libusb_alloc_transfer( 0 );
		libusb_fill_bulk_transfer( t, handle, 0x05 /*Endpoint for send */, buffers[device][n], block_size, xcallback, (void*)(intptr_t)n, 1000 );
		t->user_data = (void*)(uintptr_t)device;
		t->length = USBCallbackFill( t->buffer, device );
		libusb_submit_transfer( t );
	}
	fprintf( stderr, "Adding Device %d\n", device );
	done_mask |= 1<<device;
	done_frame |= 1<<device;
	configured[device] = 0;
}

// Pass 0 for dev to destroy all devices and transfers.
void DeviceDepart( struct libusb_device *dev )
{
	int device;
	for( device = 0; device < DEVICES; device++ )
	{
		if( devList[device] == dev || dev == 0 )
		{
			fprintf( stderr, "Removing %d\n", device );
			int n;
			for( n = 0; n < TRANSFERS; n++ )
			{
				if( transfers[device][n] )
				{
					libusb_cancel_transfer( transfers[device][n] );
				 	libusb_free_transfer( transfers[device][n] );
				}
			}
			if( handles[device] )
			{
				libusb_release_interface (handles[device], 0);
				libusb_close(handles[device]);
			}
		}
	}
}
 
int hotplug_callback(struct libusb_context *ctx, struct libusb_device *dev, libusb_hotplug_event event, void *user_data)
{
	struct libusb_device_descriptor desc;

	(void)libusb_get_device_descriptor(dev, &desc);

	if (LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED == event)
	{
		DeviceArrive( dev );
	}
	else if (LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT == event)
	{
		DeviceDepart( dev );
	}
	else
	{
		fprintf(stderr, "Unhandled event %d\n", event);
	}

	return 0;
}


void TickBreakoutDriver()
{
	libusb_handle_events(ctx);
}

int SetupBreakoutDriver()
{
	libusb_init(&ctx);
	libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, 0);

	int rc = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
		LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT, 0, USB_VENDOR_ID, USB_PRODUCT_ID,
		LIBUSB_HOTPLUG_MATCH_ANY, hotplug_callback, NULL,
		&callback_handle);
	if (LIBUSB_SUCCESS != rc)
	{
		fprintf(stderr, "Error creating a hotplug callback\n");
		return -1;
	}

	struct libusb_device **devs;
	int cnt = libusb_get_device_list(ctx, &devs);
	if (cnt < 0)
	{
		fprintf( stderr, "Device enuemration issue.\n" );
		return -1;
	}
	for ( int c = 0; c < cnt; c++ )
	{
		DeviceArrive( devs[c] );
	}
	libusb_free_device_list(devs, 1);
	return 0;
}

void CleanupBreakoutDriver()
{
	libusb_hotplug_deregister_callback(NULL, callback_handle);


	libusb_exit(NULL);
}

