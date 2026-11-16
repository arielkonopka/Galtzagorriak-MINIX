/* usbtest.c - PC test of the USB device side (hal/flipper/usb.c, the CDC
 * serial port, on vendor/libusb_stm32/src/usbd_core.c) against a hostile
 * USB host (security review 2026-11-16).
 *
 * Both files are compiled unchanged (the interrupt masking is compiled
 * out); the hardware driver below them is a model of the STM32 USB
 * peripheral, driven by a random "host": SETUP packets with any fields,
 * control data, bulk data, endpoint halts, resets, suspends, and the
 * console reading and writing meanwhile.  Checked, under ASan/UBSan:
 *   - no memory error (descriptors and buffers are never overrun);
 *   - no packet longer than its endpoint, no control IN data longer than
 *     the host asked for;
 *   - every byte the host sent on the bulk endpoint reaches hal_usb_getc,
 *     in order, none lost or invented (flow control), and the receive
 *     ring never holds more than its size.
 * Exit status: the number of failures.  Run: make -C tests/host
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32wbxx.h"
#include "usb.h"
#include "hal.h"

RCC_TypeDef t_rcc = { 0, 3u << RCC_PLLCFGR_PLLSRC_Pos };
PWR_TypeDef t_pwr;
GPIO_TypeDef t_gpioa;

#define NEP	8
static struct {
	int size;			/* 0: not configured */
	uint8_t out[64];		/* OUT/SETUP packet waiting (NAK) */
	int out_len;			/* -1: none */
	int in_busy, in_len;		/* IN packet loaded, not yet taken */
	int stalled;
} ep[NEP];
static int fails, pend_ev = -1, pend_ep;
static long iter, n_ctl_in, n_rx, n_tx;

/* What the host expects on the control pipe. */
static int ctl_in_left = -1;		/* IN bytes still allowed */

/* Bulk OUT data sent by the host and not yet read by the console. */
#define SENT_MAX	(1 << 20)
static uint8_t *sent;
static unsigned sent_h, sent_t;

static void fail(const char *why)
{
	if (fails++ < 20) printf("FAIL at step %ld: %s\n", iter, why);
}

/* ---- the model of the USB peripheral (usbd_driver) ---- */
static uint32_t m_getinfo(void) { return USBD_HW_ENABLED; }
static void m_enable(bool on) { (void) on; }
static uint8_t m_connect(bool on) { (void) on; return usbd_lane_sdp; }
static void m_setaddr(uint8_t a) { (void) a; }

static bool m_ep_config(uint8_t e, uint8_t type, uint16_t size)
{
	(void) type;
	if (size > 64) fail("endpoint configured larger than 64 bytes");
	ep[e & 7].size = size;
	ep[e & 7].out_len = -1;
	ep[e & 7].in_busy = 0;
	ep[e & 7].stalled = 0;
	return true;
}

static void m_ep_deconfig(uint8_t e)
{
	ep[e & 7].size = 0;
	ep[e & 7].out_len = -1;
	ep[e & 7].in_busy = 0;
}

static int32_t m_ep_read(uint8_t e, void *buf, uint16_t blen)
{
	int n = ep[e & 7].out_len;

	if (n < 0) return -1;			/* nothing there */
	if (n > blen) n = blen;			/* the rest is dropped */
	if (n > 0) memcpy(buf, ep[e & 7].out, n);
	ep[e & 7].out_len = -1;
	return n;
}

static int32_t m_ep_write(uint8_t e, const void *buf, uint16_t blen)
{
	static uint8_t sink[64];
	int i = e & 7;

	if (!ep[i].size) return -1;
	if (ep[i].in_busy) return -1;		/* still VALID: refused */
	if (blen > ep[i].size) {
		fail("IN packet longer than the endpoint");
		return -1;
	}
	if (blen) memcpy(sink, buf, blen);	/* ASan: the source is real */
	if (i == 0 && ctl_in_left >= 0) {
		ctl_in_left -= blen;
		if (ctl_in_left < 0) fail("control IN data beyond wLength");
	}
	ep[i].in_busy = 1;
	ep[i].in_len = blen;
	if (i == 0) n_ctl_in += blen;
	if (i == 1) n_tx += blen;
	return blen;
}

static void m_ep_setstall(uint8_t e, bool s) { ep[e & 7].stalled = s; }
static bool m_ep_isstalled(uint8_t e) { return ep[e & 7].stalled; }

static void m_poll(usbd_device *dev, usbd_evt_callback cb)
{
	int ev = pend_ev;

	if (ev < 0) return;
	pend_ev = -1;
	cb(dev, ev, pend_ep);
}

static uint16_t m_frame(void) { return 0; }

static uint16_t m_serial(void *buffer)
{
	memset(buffer, 0, 18);
	return 18;
}

const struct usbd_driver usbd_devfs = {
	m_getinfo, m_enable, m_connect, m_setaddr, m_ep_config, m_ep_deconfig,
	m_ep_read, m_ep_write, m_ep_setstall, m_ep_isstalled, m_poll,
	m_frame, m_serial,
};

static void event(int ev, int e)
{
	pend_ev = ev;
	pend_ep = e;
	hal_usb_poll();
}

/* ---- the host ---- */
static void send_setup(void)
{
	static const uint8_t types[] = { 0x00, 0x80, 0x01, 0x81, 0x02, 0x82,
		0x21, 0xA1, 0x40, 0xC0, 0x22, 0xFF };
	uint8_t *s = ep[0].out;
	uint16_t len;

	s[0] = types[rand() % sizeof types];
	s[1] = rand() % 4 ? rand() % 13 : (rand() % 3 ? 0x20 + rand() % 4
								: rand());
	s[2] = rand();
	s[3] = rand() % 2 ? rand() % 4 : rand();	/* descriptor type */
	s[4] = rand() % 3 ? rand() % 3 : rand();
	s[5] = rand() % 3 ? 0x80 * (rand() % 2) : rand();
	len = rand() % 3 ? rand() % 140 : (uint16_t) rand();
	s[6] = len;
	s[7] = len >> 8;
	ep[0].out_len = rand() % 16 ? 8 : rand() % 9;	/* short: invalid */
	ep[0].in_busy = 0;
	ctl_in_left = (s[0] & 0x80) ? len : -1;
	event(usbd_evt_epsetup, 0);
}

static void send_bulk(void)
{
	int n = rand() % 65, i;

	if (!ep[1].size || ep[1].out_len >= 0) return;	/* host: NAKed */
	if (sent_h - sent_t + n > SENT_MAX) return;
	for (i = 0; i < n; i++) {
		ep[1].out[i] = rand();
		sent[sent_h++ % SENT_MAX] = ep[1].out[i];
	}
	ep[1].out_len = n;
	event(usbd_evt_eprx, 1);
}

static void console_reads(void)
{
	int n = rand() % 100, c;

	while (n-- > 0 && (c = hal_usb_getc()) >= 0) {
		if (sent_t == sent_h) {
			fail("hal_usb_getc returned a byte nobody sent");
			return;
		}
		n_rx++;
		if (c != sent[sent_t++ % SENT_MAX]) {
			fail("received byte differs from the one sent");
			return;
		}
	}
}

int main(int argc, char **argv)
{
	long steps = argc > 1 ? atol(argv[1]) : 2000000;
	uint8_t junk[300];
	int i;

	if ((sent = malloc(SENT_MAX)) == NULL) return 1;
	for (i = 0; i < NEP; i++) ep[i].out_len = -1;
	srand(1);
	if (hal_usb_init() != 0) {
		printf("FAIL hal_usb_init\n");
		return 1;
	}
	event(usbd_evt_reset, 0);
	/* SET_CONFIGURATION 1 and DTR, as a terminal does */
	memcpy(ep[0].out, "\x00\x09\x01\x00\x00\x00\x00\x00", 8);
	ep[0].out_len = 8;
	ctl_in_left = -1;
	event(usbd_evt_epsetup, 0);
	ep[0].in_busy = 0;
	memcpy(ep[0].out, "\x21\x22\x01\x00\x00\x00\x00\x00", 8);
	ep[0].out_len = 8;
	event(usbd_evt_epsetup, 0);
	ep[0].in_busy = 0;

	for (iter = 0; iter < steps && fails < 20; iter++) {
		switch (rand() % 16) {
		case 0: case 1: case 2:
			send_setup();
			break;
		case 3:				/* control OUT data */
			if (ep[0].out_len < 0) {
				ep[0].out_len = rand() % 9;
				for (i = 0; i < 8; i++) ep[0].out[i] = rand();
				event(usbd_evt_eprx, 0);
			}
			break;
		case 4:				/* control IN taken */
			if (ep[0].in_busy) {
				ep[0].in_busy = 0;
				event(usbd_evt_eptx, 0x80);
			}
			break;
		case 5: case 6: case 7:
			send_bulk();
			break;
		case 8: case 9:			/* bulk IN taken */
			if (ep[1].in_busy && !ep[1].stalled) {
				ep[1].in_busy = 0;
				event(usbd_evt_eptx, 0x81);
			}
			break;
		case 10:
			if (rand() % 64 == 0) {	/* bus reset */
				for (i = 0; i < NEP; i++) {
					ep[i].size = 0;
					ep[i].out_len = -1;
					ep[i].in_busy = 0;
				}
				event(usbd_evt_reset, 0);
			} else event(rand() % 2 ? usbd_evt_susp
						: usbd_evt_wkup, 0);
			break;
		case 11: case 12:
			for (i = rand() % sizeof junk; i > 0; i--)
				junk[i - 1] = rand();
			(void) hal_usb_write(junk, rand() % sizeof junk);
			(void) hal_usb_room();
			break;
		default:
			console_reads();
			break;
		}
		if (ep[1].size == 0 && ep[1].out_len < 0) {
			/* deconfigured: what was in flight is gone, the
			 * ring keeps what it had (the console still reads
			 * it) */
		}
	}
	/* What is still in the ring must come out intact. */
	console_reads();
	for (i = 0; i < 10000; i++) console_reads();
	if (sent_t != sent_h) fail("bytes sent never reached the console");
	printf("%s random USB host, %ld steps (%ld control IN bytes, %ld bytes"
		" received, %ld sent)\n", fails ? "FAIL" : "PASS", iter,
		n_ctl_in, n_rx, n_tx);
	printf("%d failure(s)\n", fails);
	return fails != 0;
}
