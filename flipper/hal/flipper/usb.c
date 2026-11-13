/* USB CDC-ACM serial port ("virtual COM port") for the Flipper Zero, on top
 * of libusb_stm32 (flipper/vendor/libusb_stm32, Apache-2.0).
 *
 * The device appears on the host as /dev/ttyACM*.  Output is buffered in a
 * ring and sent once the host has opened the port (DTR set); what is
 * written before that - the boot messages - waits in the ring until it is
 * full.  Input goes into a second ring, read with hal_usb_getc().
 *
 * hal_usb_poll() must be called from the USB interrupt (or a loop).
 * hal_usb_write()/hal_usb_getc() may be called from thread code; they mask
 * interrupts briefly.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "stm32wbxx.h"
#include "usb.h"
#include "usb_cdc.h"
#include "hal.h"

#define EP0_SIZE	0x08
#define RXD_EP		0x01
#define TXD_EP		0x81
#define DATA_SZ		0x40
#define NTF_EP		0x82
#define NTF_SZ		0x08

#define TX_RING		2048	/* power of 2 */
#define RX_RING		256	/* power of 2 */

struct cdc_config {
	struct usb_config_descriptor		config;
	struct usb_iad_descriptor		comm_iad;
	struct usb_interface_descriptor		comm;
	struct usb_cdc_header_desc		cdc_hdr;
	struct usb_cdc_call_mgmt_desc		cdc_mgmt;
	struct usb_cdc_acm_desc			cdc_acm;
	struct usb_cdc_union_desc		cdc_union;
	struct usb_endpoint_descriptor		comm_ep;
	struct usb_interface_descriptor		data;
	struct usb_endpoint_descriptor		data_eprx;
	struct usb_endpoint_descriptor		data_eptx;
} __attribute__((packed));

static const struct usb_device_descriptor device_desc = {
	.bLength		= sizeof(struct usb_device_descriptor),
	.bDescriptorType	= USB_DTYPE_DEVICE,
	.bcdUSB			= VERSION_BCD(2, 0, 0),
	.bDeviceClass		= USB_CLASS_IAD,
	.bDeviceSubClass	= USB_SUBCLASS_IAD,
	.bDeviceProtocol	= USB_PROTO_IAD,
	.bMaxPacketSize0	= EP0_SIZE,
	.idVendor		= 0x0483,	/* ST "Virtual COM Port", like */
	.idProduct		= 0x5740,	/* the stock firmware */
	.bcdDevice		= VERSION_BCD(2, 0, 0),
	.iManufacturer		= 1,
	.iProduct		= 2,
	.iSerialNumber		= INTSERIALNO_DESCRIPTOR,
	.bNumConfigurations	= 1,
};

static const struct cdc_config config_desc = {
	.config = {
		.bLength		= sizeof(struct usb_config_descriptor),
		.bDescriptorType	= USB_DTYPE_CONFIGURATION,
		.wTotalLength		= sizeof(struct cdc_config),
		.bNumInterfaces		= 2,
		.bConfigurationValue	= 1,
		.iConfiguration		= NO_DESCRIPTOR,
		.bmAttributes		= USB_CFG_ATTR_RESERVED
					| USB_CFG_ATTR_SELFPOWERED,
		.bMaxPower		= USB_CFG_POWER_MA(100),
	},
	.comm_iad = {
		.bLength		= sizeof(struct usb_iad_descriptor),
		.bDescriptorType	= USB_DTYPE_INTERFASEASSOC,
		.bFirstInterface	= 0,
		.bInterfaceCount	= 2,
		.bFunctionClass		= USB_CLASS_CDC,
		.bFunctionSubClass	= USB_CDC_SUBCLASS_ACM,
		.bFunctionProtocol	= USB_PROTO_NONE,
		.iFunction		= NO_DESCRIPTOR,
	},
	.comm = {
		.bLength		= sizeof(struct usb_interface_descriptor),
		.bDescriptorType	= USB_DTYPE_INTERFACE,
		.bInterfaceNumber	= 0,
		.bAlternateSetting	= 0,
		.bNumEndpoints		= 1,
		.bInterfaceClass	= USB_CLASS_CDC,
		.bInterfaceSubClass	= USB_CDC_SUBCLASS_ACM,
		.bInterfaceProtocol	= USB_PROTO_NONE,
		.iInterface		= NO_DESCRIPTOR,
	},
	.cdc_hdr = {
		.bFunctionLength	= sizeof(struct usb_cdc_header_desc),
		.bDescriptorType	= USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType	= USB_DTYPE_CDC_HEADER,
		.bcdCDC			= VERSION_BCD(1, 1, 0),
	},
	.cdc_mgmt = {
		.bFunctionLength	= sizeof(struct usb_cdc_call_mgmt_desc),
		.bDescriptorType	= USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType	= USB_DTYPE_CDC_CALL_MANAGEMENT,
		.bmCapabilities		= 0,
		.bDataInterface		= 1,
	},
	.cdc_acm = {
		.bFunctionLength	= sizeof(struct usb_cdc_acm_desc),
		.bDescriptorType	= USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType	= USB_DTYPE_CDC_ACM,
		.bmCapabilities		= USB_CDC_CAP_LINE,
	},
	.cdc_union = {
		.bFunctionLength	= sizeof(struct usb_cdc_union_desc),
		.bDescriptorType	= USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType	= USB_DTYPE_CDC_UNION,
		.bMasterInterface0	= 0,
		.bSlaveInterface0	= 1,
	},
	.comm_ep = {
		.bLength		= sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType	= USB_DTYPE_ENDPOINT,
		.bEndpointAddress	= NTF_EP,
		.bmAttributes		= USB_EPTYPE_INTERRUPT,
		.wMaxPacketSize		= NTF_SZ,
		.bInterval		= 0xFF,
	},
	.data = {
		.bLength		= sizeof(struct usb_interface_descriptor),
		.bDescriptorType	= USB_DTYPE_INTERFACE,
		.bInterfaceNumber	= 1,
		.bAlternateSetting	= 0,
		.bNumEndpoints		= 2,
		.bInterfaceClass	= USB_CLASS_CDC_DATA,
		.bInterfaceSubClass	= USB_SUBCLASS_NONE,
		.bInterfaceProtocol	= USB_PROTO_NONE,
		.iInterface		= NO_DESCRIPTOR,
	},
	.data_eprx = {
		.bLength		= sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType	= USB_DTYPE_ENDPOINT,
		.bEndpointAddress	= RXD_EP,
		.bmAttributes		= USB_EPTYPE_BULK,
		.wMaxPacketSize		= DATA_SZ,
		.bInterval		= 0x01,
	},
	.data_eptx = {
		.bLength		= sizeof(struct usb_endpoint_descriptor),
		.bDescriptorType	= USB_DTYPE_ENDPOINT,
		.bEndpointAddress	= TXD_EP,
		.bmAttributes		= USB_EPTYPE_BULK,
		.wMaxPacketSize		= DATA_SZ,
		.bInterval		= 0x01,
	},
};

static const struct usb_string_descriptor lang_desc =
	USB_ARRAY_DESC(USB_LANGID_ENG_US);
static const struct usb_string_descriptor manuf_desc =
	USB_STRING_DESC("MINIX");
static const struct usb_string_descriptor prod_desc =
	USB_STRING_DESC("MINIX 2.0 on Flipper Zero");
static const struct usb_string_descriptor *const dtable[] = {
	&lang_desc, &manuf_desc, &prod_desc,
};

static usbd_device udev;
static uint32_t ubuf[0x20];		/* control transfer buffer */
static struct usb_cdc_line_coding line = {
	.dwDTERate = 115200, .bCharFormat = USB_CDC_1_STOP_BITS,
	.bParityType = USB_CDC_NO_PARITY, .bDataBits = 8,
};

static volatile int configured, dtr, tx_busy, last_full;
static uint8_t tx_ring[TX_RING], rx_ring[RX_RING];
static volatile unsigned tx_head, tx_tail, rx_head, rx_tail;
static int started;

static uint32_t irq_save(void)
{
	uint32_t primask;

	__asm volatile ("mrs %0, primask; cpsid i" : "=r" (primask) : : "memory");
	return primask;
}

static void irq_restore(uint32_t primask)
{
	__asm volatile ("msr primask, %0" : : "r" (primask) : "memory");
}

/* Send the next packet if the host is listening and the IN endpoint is
 * free.  Interrupts must be masked (or we are in the USB interrupt). */
static void tx_kick(void)
{
	uint8_t pkt[DATA_SZ];
	unsigned n = 0;

	if (!configured || !dtr || tx_busy) return;
	while (n < DATA_SZ && tx_tail != tx_head) {
		pkt[n++] = tx_ring[tx_tail % TX_RING];
		tx_tail++;
	}
	if (n == 0) {
		if (!last_full) return;
		last_full = 0;		/* end the transfer: zero length packet */
	} else {
		last_full = (n == DATA_SZ);
	}
	tx_busy = 1;
	usbd_ep_write(&udev, TXD_EP, pkt, n);
}

static void cdc_rxtx(usbd_device *dev, uint8_t event, uint8_t ep)
{
	uint8_t pkt[DATA_SZ];
	int n, i;

	if (event == usbd_evt_eptx) {
		tx_busy = 0;
		tx_kick();
	} else {
		n = usbd_ep_read(dev, RXD_EP, pkt, DATA_SZ);
		for (i = 0; i < n; i++) {
			if (rx_head - rx_tail < RX_RING) {
				rx_ring[rx_head % RX_RING] = pkt[i];
				rx_head++;
			}
		}
	}
}

static usbd_respond cdc_getdesc(usbd_ctlreq *req, void **address,
							uint16_t *length)
{
	const uint8_t dtype = req->wValue >> 8;
	const uint8_t dnumber = req->wValue & 0xFF;
	const void *desc;
	uint16_t len = 0;

	switch (dtype) {
	case USB_DTYPE_DEVICE:
		desc = &device_desc;
		break;
	case USB_DTYPE_CONFIGURATION:
		desc = &config_desc;
		len = sizeof(config_desc);
		break;
	case USB_DTYPE_STRING:
		if (dnumber >= sizeof dtable / sizeof dtable[0]) return usbd_fail;
		desc = dtable[dnumber];
		break;
	default:
		return usbd_fail;
	}
	if (len == 0) len = ((const struct usb_header_descriptor *) desc)->bLength;
	*address = (void *) desc;
	*length = len;
	return usbd_ack;
}

static usbd_respond cdc_control(usbd_device *dev, usbd_ctlreq *req,
					usbd_rqc_callback *callback)
{
	(void) callback;
	if (((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType)
				!= (USB_REQ_INTERFACE | USB_REQ_CLASS)
	    || req->wIndex != 0)
		return usbd_fail;

	switch (req->bRequest) {
	case USB_CDC_SET_CONTROL_LINE_STATE:
		dtr = (req->wValue & 0x01) != 0;	/* terminal opened */
		tx_kick();
		return usbd_ack;
	case USB_CDC_SET_LINE_CODING:
		memcpy(&line, req->data, sizeof line);	/* ignored */
		return usbd_ack;
	case USB_CDC_GET_LINE_CODING:
		dev->status.data_ptr = &line;
		dev->status.data_count = sizeof line;
		return usbd_ack;
	default:
		return usbd_fail;
	}
}

static usbd_respond cdc_setconf(usbd_device *dev, uint8_t cfg)
{
	switch (cfg) {
	case 0:
		configured = dtr = tx_busy = 0;
		usbd_ep_deconfig(dev, NTF_EP);
		usbd_ep_deconfig(dev, TXD_EP);
		usbd_ep_deconfig(dev, RXD_EP);
		usbd_reg_endpoint(dev, RXD_EP, 0);
		usbd_reg_endpoint(dev, TXD_EP, 0);
		return usbd_ack;
	case 1:
		usbd_ep_config(dev, RXD_EP, USB_EPTYPE_BULK, DATA_SZ);
		usbd_ep_config(dev, TXD_EP, USB_EPTYPE_BULK, DATA_SZ);
		usbd_ep_config(dev, NTF_EP, USB_EPTYPE_INTERRUPT, NTF_SZ);
		usbd_reg_endpoint(dev, RXD_EP, cdc_rxtx);
		usbd_reg_endpoint(dev, TXD_EP, cdc_rxtx);
		configured = 1;
		tx_busy = last_full = 0;
		tx_kick();
		return usbd_ack;
	default:
		return usbd_fail;
	}
}

/* 48 MHz for USB from PLLSAI1, as the stock firmware does: HSE 32 MHz /
 * M 2 (shared with the main PLL) * N 6 = 96 MHz / Q 2. */
static int usb_clock(void)
{
	uint32_t timeout;

	if ((RCC->PLLCFGR & RCC_PLLCFGR_PLLSRC) != (3u << RCC_PLLCFGR_PLLSRC_Pos))
		return -1;			/* no crystal: no USB */
	RCC->CR &= ~RCC_CR_PLLSAI1ON;
	while (RCC->CR & RCC_CR_PLLSAI1RDY) ;
	RCC->PLLSAI1CFGR = (6u << RCC_PLLSAI1CFGR_PLLN_Pos)
			 | (1u << RCC_PLLSAI1CFGR_PLLQ_Pos)	/* /2 */
			 | RCC_PLLSAI1CFGR_PLLQEN;
	RCC->CR |= RCC_CR_PLLSAI1ON;
	for (timeout = 200000; !(RCC->CR & RCC_CR_PLLSAI1RDY); )
		if (--timeout == 0) return -1;
	RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_CLK48SEL) | RCC_CCIPR_CLK48SEL_0;
	PWR->CR2 |= PWR_CR2_USV;		/* VDDUSB is valid */
	return 0;
}

static void usb_pins(void)
{
	unsigned pin;

	RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
	for (pin = 11; pin <= 12; pin++) {	/* DM, DP: AF10 */
		GPIOA->OSPEEDR |= 3u << (pin * 2);
		GPIOA->PUPDR &= ~(3u << (pin * 2));
		GPIOA->AFR[1] = (GPIOA->AFR[1] & ~(0xFu << ((pin - 8) * 4)))
			      | (10u << ((pin - 8) * 4));
		GPIOA->MODER = (GPIOA->MODER & ~(3u << (pin * 2)))
			     | (2u << (pin * 2));
	}
}

int hal_usb_init(void)
{
	if (started) return 0;
	if (usb_clock() < 0) return -1;
	usb_pins();
	usbd_init(&udev, &usbd_hw, EP0_SIZE, ubuf, sizeof ubuf);
	usbd_reg_config(&udev, cdc_setconf);
	usbd_reg_control(&udev, cdc_control);
	usbd_reg_descr(&udev, cdc_getdesc);
	usbd_enable(&udev, true);
	usbd_connect(&udev, true);
	started = 1;
	return 0;
}

int hal_usb_irq(void)
{
	return USB_LP_IRQn;
}

void hal_usb_poll(void)
{
	if (started) usbd_poll(&udev);
}

int hal_usb_connected(void)
{
	return configured && dtr;
}

unsigned hal_usb_write(const void *buf, unsigned n)
{
	const uint8_t *p = buf;
	unsigned done = 0;
	uint32_t s = irq_save();

	while (done < n && tx_head - tx_tail < TX_RING) {
		tx_ring[tx_head % TX_RING] = p[done++];
		tx_head++;
	}
	if (started) tx_kick();
	irq_restore(s);
	return done;
}

int hal_usb_getc(void)
{
	int c = -1;
	uint32_t s = irq_save();

	if (rx_tail != rx_head) {
		c = rx_ring[rx_tail % RX_RING];
		rx_tail++;
	}
	irq_restore(s);
	return c;
}
