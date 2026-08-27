#include "uvc.h"

#ifndef HID_DESCRIPTOR_HID
#define HID_DESCRIPTOR_HID 0x21
#endif
#ifndef HID_DESCRIPTOR_REPORT
#define HID_DESCRIPTOR_REPORT 0x22
#endif

/*
 * USB definitions
 */

#define USB_DT_CS_DEVICE		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_DEVICE)
#define USB_DT_CS_CONFIG		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_CONFIG)
#define USB_DT_CS_STRING		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_STRING)
#define USB_DT_CS_INTERFACE		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_INTERFACE)
#define USB_DT_CS_ENDPOINT		(USB_CTRLTYPE_TYPE_CLASS | USB_DT_ENDPOINT)

/*
 * UVC Configurable options
 */

#define CONTROL_INTERFACE 		0
#define STREAM_INTERFACE		1

#define INTERFACE_CTRL_ID		0
#define INPUT_TERMINAL_ID		1
#define OUTPUT_TERMINAL_ID		2

#define FORMAT_INDEX_UNCOMPRESSED_NV12	1

/*
 * Helper macros
 */

#define VIDEO_FRAME_SIZE_NV12(w, h)		(((w) * (h) * 3) / 2)

#define FRAME_BITRATE(w, h, bpp, interval)	(((w) * (h) * (bpp)) / ((interval) * 100 * 1E-9))
#define FPS_TO_INTERVAL(fps)			((1E9 / 100) / (fps))

/* Interface Association Descriptor */
static
unsigned char interface_association_descriptor[] = {
	UVC_INTERFACE_ASSOCIATION_DESC_SIZE,		/* Descriptor Size: 8 */
	UVC_INTERFACE_ASSOCIATION_DESCRIPTOR_TYPE,	/* Interface Association Descr Type: 11 */
	0x00,						/* I/f number of first VideoControl i/f */
	0x02,						/* Number of Video i/f */
	USB_CLASS_VIDEO,				/* CC_VIDEO : Video i/f class code */
	UVC_SC_VIDEO_INTERFACE_COLLECTION,		/* SC_VIDEO_INTERFACE_COLLECTION : Subclass code */
	UVC_PC_PROTOCOL_UNDEFINED,			/* Protocol : Not used */
	0x00,						/* String desc index for interface */
};

DECLARE_UVC_HEADER_DESCRIPTOR(1);

static struct __attribute__((packed)) {
	struct UVC_HEADER_DESCRIPTOR(1) header_descriptor;
	struct uvc_input_terminal_descriptor input_terminal_descriptor;
	struct uvc_output_terminal_descriptor output_terminal_descriptor;
} video_control_descriptors = {
	.header_descriptor = {
		.bLength			= sizeof(video_control_descriptors.header_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VC_HEADER,
		.bcdUVC				= 0x0110,
		.wTotalLength			= sizeof(video_control_descriptors),
		.dwClockFrequency		= 48000000,
		.bInCollection			= 1,
		.baInterfaceNr			= {STREAM_INTERFACE},
	},
	.input_terminal_descriptor = {
		.bLength			= sizeof(video_control_descriptors.input_terminal_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VC_INPUT_TERMINAL,
		.bTerminalID			= INPUT_TERMINAL_ID,
		.wTerminalType			= UVC_ITT_VENDOR_SPECIFIC,
		.bAssocTerminal			= 0,
		.iTerminal			= 0,
	},
	.output_terminal_descriptor = {
		.bLength			= sizeof(video_control_descriptors.output_terminal_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VC_OUTPUT_TERMINAL,
		.bTerminalID			= OUTPUT_TERMINAL_ID,
		.wTerminalType			= UVC_TT_STREAMING,
		.bAssocTerminal			= 0,
		.bSourceID			= INPUT_TERMINAL_ID,
		.iTerminal			= 0,
	},
};

DECLARE_UVC_INPUT_HEADER_DESCRIPTOR(1, 1);
DECLARE_UVC_FRAME_UNCOMPRESSED(2);

static struct __attribute__((packed)) {
	struct UVC_INPUT_HEADER_DESCRIPTOR(1, 1) input_header_descriptor;
	struct uvc_format_uncompressed format_uncompressed_nv12;
	struct UVC_FRAME_UNCOMPRESSED(2) frames_uncompressed_nv12[5];
	struct uvc_color_matching_descriptor format_uncompressed_nv12_color_matching;
} video_streaming_descriptors = {
	.input_header_descriptor = {
		.bLength			= sizeof(video_streaming_descriptors.input_header_descriptor),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VS_INPUT_HEADER,
		.bNumFormats			= 1,
		.wTotalLength			= sizeof(video_streaming_descriptors),
		.bEndpointAddress		= USB_ENDPOINT_IN | 0x01,
		.bmInfo				= 0,
		.bTerminalLink			= OUTPUT_TERMINAL_ID,
		.bStillCaptureMethod		= 0,
		.bTriggerSupport		= 0,
		.bTriggerUsage			= 0,
		.bControlSize			= 1,
		.bmaControls			= {{0}, },
	},
	.format_uncompressed_nv12 = {
		.bLength			= sizeof(video_streaming_descriptors.format_uncompressed_nv12),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VS_FORMAT_UNCOMPRESSED,
		.bFormatIndex			= FORMAT_INDEX_UNCOMPRESSED_NV12,
		.bNumFrameDescriptors		= 5,
		.guidFormat			= UVC_GUID_FORMAT_NV12,
		.bBitsPerPixel			= 12,
		.bDefaultFrameIndex		= 1,
		.bAspectRatioX			= 0,
		.bAspectRatioY			= 0,
		.bmInterfaceFlags		= 0,
		.bCopyProtect			= 0,
	},
	.frames_uncompressed_nv12 = {
		(struct UVC_FRAME_UNCOMPRESSED(2)){
			.bLength			= UVC_DT_FRAME_UNCOMPRESSED_SIZE(2),
			.bDescriptorType		= USB_DT_CS_INTERFACE,
			.bDescriptorSubType		= UVC_VS_FRAME_UNCOMPRESSED,
			.bFrameIndex			= 1,
			.bmCapabilities			= 0,
			.wWidth				= 960,
			.wHeight			= 544,
			.dwMinBitRate			= FRAME_BITRATE(960, 544, 12, FPS_TO_INTERVAL(30)),
			.dwMaxBitRate			= FRAME_BITRATE(960, 544, 12, FPS_TO_INTERVAL(60)),
			.dwMaxVideoFrameBufferSize	= VIDEO_FRAME_SIZE_NV12(960, 544),
			.dwDefaultFrameInterval		= FPS_TO_INTERVAL(60),
			.bFrameIntervalType		= 2,
			.dwFrameInterval		= {FPS_TO_INTERVAL(60), FPS_TO_INTERVAL(30)},
		},
		(struct UVC_FRAME_UNCOMPRESSED(2)){
			.bLength			= UVC_DT_FRAME_UNCOMPRESSED_SIZE(2),
			.bDescriptorType		= USB_DT_CS_INTERFACE,
			.bDescriptorSubType		= UVC_VS_FRAME_UNCOMPRESSED,
			.bFrameIndex			= 2,
			.bmCapabilities			= 0,
			.wWidth				= 896,
			.wHeight			= 504,
			.dwMinBitRate			= FRAME_BITRATE(896, 504, 12, FPS_TO_INTERVAL(30)),
			.dwMaxBitRate			= FRAME_BITRATE(896, 504, 12, FPS_TO_INTERVAL(60)),
			.dwMaxVideoFrameBufferSize	= VIDEO_FRAME_SIZE_NV12(896, 504),
			.dwDefaultFrameInterval		= FPS_TO_INTERVAL(60),
			.bFrameIntervalType		= 2,
			.dwFrameInterval		= {FPS_TO_INTERVAL(60), FPS_TO_INTERVAL(30)},
		},
		(struct UVC_FRAME_UNCOMPRESSED(2)){
			.bLength			= UVC_DT_FRAME_UNCOMPRESSED_SIZE(2),
			.bDescriptorType		= USB_DT_CS_INTERFACE,
			.bDescriptorSubType		= UVC_VS_FRAME_UNCOMPRESSED,
			.bFrameIndex			= 3,
			.bmCapabilities			= 0,
			.wWidth				= 864,
			.wHeight			= 488,
			.dwMinBitRate			= FRAME_BITRATE(864, 488, 12, FPS_TO_INTERVAL(30)),
			.dwMaxBitRate			= FRAME_BITRATE(864, 488, 12, FPS_TO_INTERVAL(60)),
			.dwMaxVideoFrameBufferSize	= VIDEO_FRAME_SIZE_NV12(864, 488),
			.dwDefaultFrameInterval		= FPS_TO_INTERVAL(60),
			.bFrameIntervalType		= 2,
			.dwFrameInterval		= {FPS_TO_INTERVAL(60), FPS_TO_INTERVAL(30)},
		},
		(struct UVC_FRAME_UNCOMPRESSED(2)){
			.bLength			= UVC_DT_FRAME_UNCOMPRESSED_SIZE(2),
			.bDescriptorType		= USB_DT_CS_INTERFACE,
			.bDescriptorSubType		= UVC_VS_FRAME_UNCOMPRESSED,
			.bFrameIndex			= 4,
			.bmCapabilities			= 0,
			.wWidth				= 480,
			.wHeight			= 272,
			.dwMinBitRate			= FRAME_BITRATE(480, 272, 12, FPS_TO_INTERVAL(30)),
			.dwMaxBitRate			= FRAME_BITRATE(480, 272, 12, FPS_TO_INTERVAL(60)),
			.dwMaxVideoFrameBufferSize	= VIDEO_FRAME_SIZE_NV12(480, 272),
			.dwDefaultFrameInterval		= FPS_TO_INTERVAL(60),
			.bFrameIntervalType		= 2,
			.dwFrameInterval		= {FPS_TO_INTERVAL(60), FPS_TO_INTERVAL(30)},
		},
		(struct UVC_FRAME_UNCOMPRESSED(2)){
			.bLength			= UVC_DT_FRAME_UNCOMPRESSED_SIZE(2),
			.bDescriptorType		= USB_DT_CS_INTERFACE,
			.bDescriptorSubType		= UVC_VS_FRAME_UNCOMPRESSED,
			.bFrameIndex			= 5,
			.bmCapabilities			= 0,
			.wWidth				= 1280,
			.wHeight			= 720,
			.dwMinBitRate			= FRAME_BITRATE(1280, 720, 12, FPS_TO_INTERVAL(20)),
			.dwMaxBitRate			= FRAME_BITRATE(1280, 720, 12, FPS_TO_INTERVAL(30)),
			.dwMaxVideoFrameBufferSize	= VIDEO_FRAME_SIZE_NV12(1280, 720),
			.dwDefaultFrameInterval		= FPS_TO_INTERVAL(30),
			.bFrameIntervalType		= 2,
			.dwFrameInterval		= {FPS_TO_INTERVAL(30), FPS_TO_INTERVAL(20)},
		},
	},
	.format_uncompressed_nv12_color_matching = {
		.bLength			= sizeof(video_streaming_descriptors.format_uncompressed_nv12_color_matching),
		.bDescriptorType		= USB_DT_CS_INTERFACE,
		.bDescriptorSubType		= UVC_VS_COLORFORMAT,
		.bColorPrimaries		= 0,
		.bTransferCharacteristics	= 0,
		.bMatrixCoefficients		= 0,
	},
};


/*
 * PC control HID interface.
 * Wire format for both reports: 1 byte Report ID + 15 data bytes
 * = 16 bytes total, matching the 16-byte buffers used on both sides.
 * Report ID 2 = output report (PC -> Vita).
 * Report ID 1 = input/heartbeat report (Vita -> PC).
 */
#define CONTROL_HID_INTERFACE 2
#define HID_EP_IN  2
#define HID_EP_OUT 3
#define HID_REPORT_ID_INPUT  1
#define HID_REPORT_ID_OUTPUT 2

static unsigned char hid_report_descriptor[] __attribute__((aligned(64))) = {
	0x06, 0x00, 0xFF,       /* Usage Page (Vendor 0xFF00) */
	0x09, 0x01,             /* Usage */
	0xA1, 0x01,             /* Collection (Application) */

	0x85, HID_REPORT_ID_INPUT,
	0x09, 0x02,
	0x15, 0x00,
	0x26, 0xFF, 0x00,
	0x75, 0x08,
	0x95, 0x3F,
	0x81, 0x02,             /* Input: 63 data bytes */

	0x85, HID_REPORT_ID_OUTPUT,
	0x09, 0x03,
	0x15, 0x00,
	0x26, 0xFF, 0x00,
	0x75, 0x08,
	0x95, 0x3F,
	0x91, 0x02,             /* Output: 63 data bytes */

	0xC0
};

static unsigned char hiddesc[] = {
	0x09, 0x21,             /* HID descriptor */
	0x11, 0x01,             /* HID 1.11 */
	0x00,
	0x01,
	0x22,                    /* Report descriptor */
	sizeof(hid_report_descriptor) & 0xFF,
	(sizeof(hid_report_descriptor) >> 8) & 0xFF
};

/* Endpoint blocks */
static
struct SceUdcdEndpoint endpoints[4] = {
	{USB_ENDPOINT_OUT, 0, 0, 0},
	{USB_ENDPOINT_IN, 1, 0, 0},
	{USB_ENDPOINT_IN, 2, 0, 0},
	{USB_ENDPOINT_OUT, 3, 0, 0},
};

/* Interface */
static
struct SceUdcdInterface interface = {
	.expectNumber		= -1,
	.interfaceNumber	= 0,
	.numInterfaces		= 3
};

/* String descriptors */
static
struct SceUdcdStringDescriptor string_descriptor_product = {
	14,
	USB_DT_STRING,
	{'P', 'S', 'V', 'i', 't', 'a'}
};

static
struct SceUdcdStringDescriptor string_descriptor_serial = {
	18,
	USB_DT_STRING,
	{'U', 'D', 'C', 'D', ' ', 'U', 'V', 'C'}
};

/* Hi-Speed device descriptor */
static
struct SceUdcdDeviceDescriptor devdesc_hi = {
	USB_DT_DEVICE_SIZE,
	USB_DT_DEVICE,
	0x200,				/* bcdUSB */
	USB_DEVICE_CLASS_MISCELLANEOUS,	/* bDeviceClass (Miscellaneous Device Class)*/
	0x02,				/* bDeviceSubClass (Common Class) */
	0x01,				/* bDeviceProtocol (Interface Association Descriptor) */
	64,				/* bMaxPacketSize0 */
	0,				/* idProduct */
	0,				/* idVendor */
	0x100,				/* bcdDevice */
	0,				/* iManufacturer */
	2,				/* iProduct */
	3,				/* iSerialNumber */
	1				/* bNumConfigurations */
};

/* Hi-Speed endpoint descriptors */
static
struct SceUdcdEndpointDescriptor endpdesc_hi[4] = {
	/* Video Streaming endpoint */
	{
		USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
		USB_ENDPOINT_IN | 0x01, USB_ENDPOINT_TYPE_BULK, 0x200, 0x00
	},
	/* HID input endpoint */
	{
		USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
		USB_ENDPOINT_IN | HID_EP_IN, USB_ENDPOINT_TYPE_INTERRUPT, 0x40, 0x01
	},
	/* HID output endpoint */
	{
		USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
		USB_ENDPOINT_OUT | HID_EP_OUT, USB_ENDPOINT_TYPE_INTERRUPT, 0x40, 0x01
	},
	{0}
};

/* Hi-Speed interface descriptor */
static
struct SceUdcdInterfaceDescriptor interdesc_hi[4] = {
	{	/* Standard Video Control Interface Descriptor */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		CONTROL_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOCONTROL,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&video_control_descriptors,
		sizeof(video_control_descriptors)
	},
	{	/* Standard Video Streaming Interface Descriptor */
		/* Alternate setting 0 = Operational Setting */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		STREAM_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		1,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOSTREAMING,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		&endpdesc_hi[0],		/* endpoints */
		(void *)&video_streaming_descriptors,
		sizeof(video_streaming_descriptors)
	},
	{
		USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE,
		CONTROL_HID_INTERFACE, 0, 2,
		USB_CLASS_HID, 0x00, 0x00, 0,
		&endpdesc_hi[1], (void *)&hiddesc, sizeof(hiddesc)
	},
	{0}
};

/* Hi-Speed settings */
static
struct SceUdcdInterfaceSettings settings_hi[3] = {
	{&interdesc_hi[0], 0, 1},
	{&interdesc_hi[1], 0, 1},
	{&interdesc_hi[2], 0, 1},
};

/* Hi-Speed configuration descriptor */
static
struct SceUdcdConfigDescriptor confdesc_hi = {
	USB_DT_CONFIG_SIZE,
	USB_DT_CONFIG,
	    (USB_DT_CONFIG_SIZE + 3 * USB_DT_INTERFACE_SIZE + 3 * USB_DT_ENDPOINT_SIZE +
		sizeof(video_control_descriptors) +
		sizeof(video_streaming_descriptors) + sizeof(hiddesc)),	/* wTotalLength */
	3,			/* bNumInterfaces */
	1,			/* bConfigurationValue */
	0,			/* iConfiguration */
	0x80,			/* bmAttributes */
	250,			/* bMaxPower */
	&settings_hi[0],
	interface_association_descriptor,
	sizeof(interface_association_descriptor)
};

/* Hi-Speed configuration */
static
struct SceUdcdConfiguration config_hi = {
	&confdesc_hi,
	&settings_hi[0],
	&interdesc_hi[0],
	&endpdesc_hi[0]
};

/* Full-Speed device descriptor */
static
struct SceUdcdDeviceDescriptor devdesc_full = {
	USB_DT_DEVICE_SIZE,
	USB_DT_DEVICE,
	0x200,				/* bcdUSB (should be 0x110 but the PSVita freezes otherwise) */
	USB_DEVICE_CLASS_MISCELLANEOUS,	/* bDeviceClass (Miscellaneous Device Class)*/
	0x02,				/* bDeviceSubClass (Common Class) */
	0x01,				/* bDeviceProtocol (Interface Association Descriptor) */
	0x40,				/* bMaxPacketSize0 */
	0,				/* idProduct */
	0,				/* idVendor */
	0x100,				/* bcdDevice */
	0,				/* iManufacturer */
	2,				/* iProduct */
	3,				/* iSerialNumber */
	1				/* bNumConfigurations */
};

/* Full-Speed endpoint descriptors */
static
struct SceUdcdEndpointDescriptor endpdesc_full[4] = {
	{
		USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
		USB_ENDPOINT_IN | 0x01, USB_ENDPOINT_TYPE_BULK, 0x40, 0x00
	},
	{
		USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
		USB_ENDPOINT_IN | HID_EP_IN, USB_ENDPOINT_TYPE_INTERRUPT, 0x40, 0x01
	},
	{
		USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT,
		USB_ENDPOINT_OUT | HID_EP_OUT, USB_ENDPOINT_TYPE_INTERRUPT, 0x40, 0x01
	},
	{0}
};

/* Full-Speed interface descriptor */
static
struct SceUdcdInterfaceDescriptor interdesc_full[4] = {
	{	/* Standard Video Control Interface Descriptor */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		CONTROL_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		0,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOCONTROL,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		NULL,				/* endpoints */
		(void *)&video_control_descriptors,
		sizeof(video_control_descriptors)
	},
	{	/* Standard Video Streaming Interface Descriptor */
		/* Alternate setting 0 = Operational Setting */
		USB_DT_INTERFACE_SIZE,
		USB_DT_INTERFACE,
		STREAM_INTERFACE,		/* bInterfaceNumber */
		0,				/* bAlternateSetting */
		1,				/* bNumEndpoints */
		USB_CLASS_VIDEO,		/* bInterfaceClass */
		UVC_SC_VIDEOSTREAMING,		/* bInterfaceSubClass */
		UVC_PC_PROTOCOL_UNDEFINED,	/* bInterfaceProtocol */
		0,				/* iInterface */
		&endpdesc_full[0],		/* endpoints */
		(void *)&video_streaming_descriptors,
		sizeof(video_streaming_descriptors)
	},
	{
		USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE,
		CONTROL_HID_INTERFACE, 0, 2,
		USB_CLASS_HID, 0x00, 0x00, 0,
		&endpdesc_full[1], (void *)&hiddesc, sizeof(hiddesc)
	},
	{0}
};

/* Full-Speed settings */
static
struct SceUdcdInterfaceSettings settings_full[3] = {
	{&interdesc_full[0], 0, 1},
	{&interdesc_full[1], 0, 1},
	{&interdesc_full[2], 0, 1}
};

/* Full-Speed configuration descriptor */
static
struct SceUdcdConfigDescriptor confdesc_full = {
	USB_DT_CONFIG_SIZE,
	USB_DT_CONFIG,
	    (USB_DT_CONFIG_SIZE + 3 * USB_DT_INTERFACE_SIZE + 3 * USB_DT_ENDPOINT_SIZE +
		sizeof(video_control_descriptors) +
		sizeof(video_streaming_descriptors) + sizeof(hiddesc)),	/* wTotalLength */
	3,			/* bNumInterfaces */
	1,			/* bConfigurationValue */
	0,			/* iConfiguration */
	0x80,			/* bmAttributes */
	250,			/* bMaxPower */
	&settings_full[0],
	interface_association_descriptor,
	sizeof(interface_association_descriptor)
};

/* Full-Speed configuration */
static
struct SceUdcdConfiguration config_full = {
	&confdesc_full,
	&settings_full[0],
	&interdesc_full[0],
	&endpdesc_full[0]
};

