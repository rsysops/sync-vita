#ifndef UAC_H
#define UAC_H

/* USB Audio Class 1.0 values used by the capture function (the Vita's sound
 * to the PC). See the USB Device Class Definition for Audio Devices 1.0. */

#define UAC_IAD_DESCRIPTOR_TYPE		0x0B

#define UAC_SUBCLASS_AUDIOCONTROL	0x01
#define UAC_SUBCLASS_AUDIOSTREAMING	0x02

#define UAC_CS_INTERFACE		0x24
#define UAC_CS_ENDPOINT			0x25

#define UAC_AC_HEADER			0x01
#define UAC_AC_INPUT_TERMINAL		0x02
#define UAC_AC_OUTPUT_TERMINAL		0x03

#define UAC_AS_GENERAL			0x01
#define UAC_AS_FORMAT_TYPE		0x02
#define UAC_FORMAT_TYPE_I		0x01
#define UAC_FORMAT_PCM			0x0001

#define UAC_TERMINAL_USB_STREAMING	0x0101
#define UAC_TERMINAL_LINE_CONNECTOR	0x0603

#define UAC_EP_GENERAL			0x01
/* Isochronous, asynchronous: the Vita's audio clock is its own. */
#define UAC_EP_ISO_ASYNC		0x05

/* The stream: 48 kHz, stereo, signed 16-bit little endian, one packet of
 * 48 frames (192 bytes) every millisecond. */
#define UAC_RATE			48000
#define UAC_CHANNELS			2
#define UAC_SAMPLE_BYTES		2
#define UAC_FRAME_BYTES			(UAC_CHANNELS * UAC_SAMPLE_BYTES)
#define UAC_PACKET_FRAMES		(UAC_RATE / 1000)
#define UAC_PACKET_BYTES		(UAC_PACKET_FRAMES * UAC_FRAME_BYTES)

#endif
