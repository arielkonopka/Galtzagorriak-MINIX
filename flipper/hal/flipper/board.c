/* Flipper Zero (STM32WB55RG) board support.
 *
 * Pin assignments are taken from flipperzero-firmware
 * targets/f7/furi_hal/furi_hal_resources.[ch]; the display init sequence
 * from lib/u8g2/u8g2_glue.c.  CPU2 (radio core) is never started.
 */
#include <stddef.h>
#include <stdint.h>
#include "stm32wbxx.h"
#include "hal.h"

uint32_t SystemCoreClock = 4000000;	/* MSI after reset */
volatile uint32_t hal_ms;
void (*hal_tick_hook)(void);

const char *hal_board_name(void) { return "Flipper Zero (STM32WB55RG)"; }
uint32_t hal_cpu_hz(void) { return SystemCoreClock; }

/*===========================================================================*
 *				GPIO					     *
 *===========================================================================*/
enum { MODE_IN = 0, MODE_OUT = 1, MODE_AF = 2, MODE_ANALOG = 3 };
enum { PULL_NONE = 0, PULL_UP = 1, PULL_DOWN = 2 };

static void gpio_cfg(GPIO_TypeDef *port, unsigned pin, unsigned mode,
		     unsigned pull, unsigned af)
{
	unsigned s2 = pin * 2;

	port->OSPEEDR = (port->OSPEEDR & ~(3u << s2)) | (2u << s2);	/* fast */
	port->OTYPER &= ~(1u << pin);					/* push-pull */
	port->PUPDR = (port->PUPDR & ~(3u << s2)) | (pull << s2);
	if (pin < 8)
		port->AFR[0] = (port->AFR[0] & ~(0xFu << (pin * 4))) | (af << (pin * 4));
	else
		port->AFR[1] = (port->AFR[1] & ~(0xFu << ((pin - 8) * 4))) | (af << ((pin - 8) * 4));
	port->MODER = (port->MODER & ~(3u << s2)) | (mode << s2);
}

static inline void gpio_set(GPIO_TypeDef *port, unsigned pin, int on)
{
	port->BSRR = on ? (1u << pin) : (1u << (pin + 16));
}

static inline int gpio_get(GPIO_TypeDef *port, unsigned pin)
{
	return (port->IDR >> pin) & 1;
}

/*===========================================================================*
 *				iButton 1-Wire (R6.7)			     *
 *===========================================================================*/
/* PB14, open drain with the board's pull-up (stock: OutputOpenDrain, no
 * pull).  The kernel builds the slots from these and masks interrupts
 * around the timed part of each (<= 70 us); the times are stock's
 * standard-speed ones (lib/one_wire/one_wire_host.c). */
void hal_ow_line(int low)
{
	GPIOB->OTYPER |= 1u << 14;			/* open drain */
	gpio_set(GPIOB, 14, !low);
	GPIOB->MODER = (GPIOB->MODER & ~(3u << 28)) | (1u << 28);	/* out */
}

int hal_ow_read(void)
{
	return gpio_get(GPIOB, 14);
}

void hal_ow_delay(unsigned us)
{
	uint32_t s = DWT->CYCCNT, n = us * (SystemCoreClock / 1000000);

	while (DWT->CYCCNT - s < n) ;
}

/* A microsecond clock, mod 2^16, for the 1-Wire slave's slot times (the
 * longest it measures is a reset, ~0.5-1 ms).  The cycle counter shifted:
 * the core runs at 64 MHz whenever it does (2^6 cycles a microsecond),
 * and 2^32 / 2^6 is a multiple of 2^16, so differences stay right across
 * the counter's wrap. */
uint16_t hal_ow_now(void)
{
	return (uint16_t) (DWT->CYCCNT >> 6);
}

/* R6.7 part 3, a Dallas key emulated: PB14 released (open drain, high by
 * the pull-up) and EXTI line 14 on both edges.  Line 14 shares the
 * EXTI15_10 vector with the Left/Right/Up/Back buttons (10-13); its
 * handler asks hal_ib_edge() first.  Off: the line masked, the pin left
 * released. */
void hal_ib_emu(int on)
{
	if (on) {
		hal_ow_line(0);
		SYSCFG->EXTICR[3] = (SYSCFG->EXTICR[3] & ~SYSCFG_EXTICR4_EXTI14)
					| 1u << SYSCFG_EXTICR4_EXTI14_Pos;  /* B */
		EXTI->RTSR1 |= EXTI_RTSR1_RT14;
		EXTI->FTSR1 |= EXTI_FTSR1_FT14;
		EXTI->PR1 = EXTI_PR1_PIF14;
		EXTI->IMR1 |= EXTI_IMR1_IM14;
	} else {
		EXTI->IMR1 &= ~EXTI_IMR1_IM14;
		EXTI->PR1 = EXTI_PR1_PIF14;
	}
}

/* 1 if line 14 had an edge (the flag cleared), else 0. */
int hal_ib_edge(void)
{
	if (!(EXTI->PR1 & EXTI_PR1_PIF14)) return 0;
	EXTI->PR1 = EXTI_PR1_PIF14;
	return 1;
}

/* Forget an edge of our own (the presence pulse we drove). */
void hal_ib_clear(void)
{
	EXTI->PR1 = EXTI_PR1_PIF14;
}

/* R6.7 part 3b, Cyfral and Metakom: the Flipper's own way, below with the
 * LF RFID capture it shares (hal_ib_capt_arm, hal_ib_play).  The
 * synchronous capture is for boards that have no DMA to do it (QEMU). */
int hal_ib_capt(uint32_t dst, unsigned n, unsigned ms, int *level)
{
	(void) dst; (void) n; (void) ms; (void) level;
	return -1;
}

/*===========================================================================*
 *				Radios parked (T14a)			     *
 *===========================================================================*/
/* After a power-on reset the CC1101 (Sub-GHz) idles with its crystal
 * running, ~1.7 mA, and the ST25R3916 (NFC) is not in power-down: MINIX
 * does not use them yet, so at boot both are put to sleep over SPI1 and
 * SPI1 is switched off again.  Command bytes and order from the stock
 * firmware (lib/drivers/cc1101*, st25r3916*, furi_hal_nfc.c; GPL).
 * Pins: SPI1 SCK PA5, MISO PB4, MOSI PB5 (AF5); CC1101 CS PD0 (mode 0),
 * ST25R3916 CS PE4 (mode 1).  Both CS lines stay high: a low CS wakes
 * the CC1101. */
#define CC_CS(on)	gpio_set(GPIOD, 0, !(on))
#define NFC_CS(on)	gpio_set(GPIOE, 4, !(on))
#define SPI1_MISO	gpio_get(GPIOB, 4)

static void spi1_on(int cpha)
{
	RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
	(void) RCC->APB2ENR;
	gpio_cfg(GPIOA, 5, MODE_AF, PULL_NONE, 5);	/* SCK */
	gpio_cfg(GPIOB, 4, MODE_AF, PULL_NONE, 5);	/* MISO */
	gpio_cfg(GPIOB, 5, MODE_AF, PULL_NONE, 5);	/* MOSI */
	SPI1->CR1 = 0;
	SPI1->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;	/* 8-bit */
	SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI
		  | (4u << SPI_CR1_BR_Pos)		/* 64 MHz / 32 = 2 MHz */
		  | (cpha ? SPI_CR1_CPHA : 0) | SPI_CR1_SPE;
}

static void spi1_off(void)
{
	while (SPI1->SR & SPI_SR_BSY) ;
	SPI1->CR1 = 0;
	RCC->APB2ENR &= ~RCC_APB2ENR_SPI1EN;
	gpio_cfg(GPIOA, 5, MODE_ANALOG, PULL_NONE, 0);
	gpio_cfg(GPIOB, 4, MODE_ANALOG, PULL_NONE, 0);
	gpio_cfg(GPIOB, 5, MODE_ANALOG, PULL_NONE, 0);
}

static uint8_t spi1_xfer(uint8_t out)
{
	while (!(SPI1->SR & SPI_SR_TXE)) ;
	*(volatile uint8_t *) &SPI1->DR = out;
	while (!(SPI1->SR & SPI_SR_RXNE)) ;
	return *(volatile uint8_t *) &SPI1->DR;
}

static unsigned cc_ready(void)	/* CS low: us until MISO low (CHIP_RDYn) */
{
	uint32_t t0 = DWT->CYCCNT, us;

	CC_CS(1);
	while (SPI1_MISO)
		if ((us = (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000)) > 2000)
			return us;
	return (DWT->CYCCNT - t0) / (SystemCoreClock / 1000000);
}

static void nfc_cmd(uint8_t a, int n, uint8_t b, uint8_t *in)
{
	uint8_t r;

	NFC_CS(1);
	(void) spi1_xfer(a);
	r = n > 1 ? spi1_xfer(b) : 0;
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
	if (in != NULL) *in = r;
}

void hal_radios_park(void)
{
	gpio_set(GPIOD, 0, 1);				/* both deselected */
	gpio_cfg(GPIOD, 0, MODE_OUT, PULL_NONE, 0);
	gpio_set(GPIOE, 4, 1);
	gpio_cfg(GPIOE, 4, MODE_OUT, PULL_NONE, 0);

	spi1_on(0);					/* CC1101: mode 0 */
	(void) cc_ready();
	(void) spi1_xfer(0x30);				/* SRES */
	CC_CS(0);
	(void) cc_ready();				/* reset done */
	(void) spi1_xfer(0x39);				/* SPWD */
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);					/* sleeps from here */
	spi1_off();

	spi1_on(1);					/* ST25R3916: mode 1 */
	nfc_cmd(0xC1, 1, 0, NULL);			/* SET_DEFAULT */
	nfc_cmd(0x02, 2, 0x00, NULL);			/* op control: off */
	spi1_off();

	/* The debug interface's clocks in Stop/Standby only in dev mode. */
	if (!hal_dev())
		DBGMCU->CR &= ~(DBGMCU_CR_DBG_SLEEP | DBGMCU_CR_DBG_STOP
							| DBGMCU_CR_DBG_STANDBY);
}

/* For the boot report: is the CC1101 asleep (its crystal must start: a
 * long wait for CHIP_RDYn; it is put back to sleep), the ST25R3916's
 * op-control (0: off) and chip id, and whether the SMPS runs. */
void hal_radios_check(unsigned *cc_us, unsigned *nfc_op, unsigned *nfc_id,
							int *smps)
{
	uint8_t v;

	spi1_on(0);
	*cc_us = cc_ready();
	(void) spi1_xfer(0x39);				/* SPWD again */
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);
	spi1_off();
	spi1_on(1);
	nfc_cmd(0x40 | 0x02, 2, 0, &v);			/* read op control */
	*nfc_op = v;
	nfc_cmd(0x40 | 0x3F, 2, 0, &v);			/* read IC identity */
	*nfc_id = v;
	spi1_off();
	*smps = (PWR->SR2 & PWR_SR2_SMPSF) != 0;
}

/*===========================================================================*
 *			The ST25R3916 as a poller (#390, R13.1)		     *
 *===========================================================================*/
/* NFC-A, polled: no IRQ pin, no static RAM (the state is in the chip's
 * registers).  Register values and the order from the stock firmware
 * (furi_hal_nfc*.c, st25r3916*.c; GPL).  SPI1 mode 1, CS PE4.  A register
 * address with NR_B is in space B (a 0xFB prefix); a read is 0x40 | reg, a
 * write reg, a direct command 0xC0 | n, the FIFO load 0x80 and read 0x9F. */
static uint32_t ms_since(uint32_t start);
#define NR_B		0x40
#define NR_IRQ_OSC	0x00000080UL	/* main irq register, byte 0 ... */
#define NR_IRQ_RXE	0x00000010UL
#define NR_IRQ_TXE	0x00000008UL
#define NR_IRQ_DCT	0x00008000UL	/* ... byte 1 ... */
#define NR_IRQ_ERR	0x00F00000UL	/* ... byte 2: crc, par, err2, err1 */

static uint8_t nr_r(unsigned reg)
{
	uint8_t v;

	NFC_CS(1);
	if (reg & NR_B) (void) spi1_xfer(0xFB);
	(void) spi1_xfer(0x40 | (reg & 0x3F));
	v = spi1_xfer(0);
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
	return v;
}

static void nr_w(unsigned reg, uint8_t v)
{
	NFC_CS(1);
	if (reg & NR_B) (void) spi1_xfer(0xFB);
	(void) spi1_xfer(reg & 0x3F);
	(void) spi1_xfer(v);
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
}

static void nr_mod(unsigned reg, unsigned mask, unsigned val)
{
	nr_w(reg, (uint8_t) ((nr_r(reg) & ~mask) | (val & mask)));
}

static void nr_cmd(uint8_t c)
{
	NFC_CS(1);
	(void) spi1_xfer(c);
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
}

static void nr_test_mod(unsigned reg, unsigned mask, unsigned val)
{
	uint8_t v;

	NFC_CS(1);
	(void) spi1_xfer(0xFC);			/* test register access */
	(void) spi1_xfer(0x40 | reg);
	v = spi1_xfer(0);
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
	NFC_CS(1);
	(void) spi1_xfer(0xFC);
	(void) spi1_xfer(reg);
	(void) spi1_xfer((v & ~mask) | (val & mask));
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
}

/* The four interrupt registers (0x1A-0x1D) as one word; reading clears. */
static uint32_t nr_irq(void)
{
	uint32_t v = 0;
	int i;

	NFC_CS(1);
	(void) spi1_xfer(0x40 | 0x1A);
	for (i = 0; i < 4; i++) v |= (uint32_t) spi1_xfer(0) << (8 * i);
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
	return v;
}

/* Enable (not mask) these sources: a masked one is not recorded. */
static void nr_enable(uint32_t on)
{
	int i;

	NFC_CS(1);
	(void) spi1_xfer(0x16);
	for (i = 0; i < 4; i++) (void) spi1_xfer((uint8_t) (~on >> (8 * i)));
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
}

/* Wait for one of the sources in want (or ms): everything seen so far. */
static uint32_t nr_wait(uint32_t want, unsigned ms)
{
	uint32_t t0 = DWT->CYCCNT, acc = 0;

	while (!(acc & want) && ms_since(t0) < ms) acc |= nr_irq();
	return acc;
}

static void nr_fifo_w(const uint8_t *p, unsigned n)
{
	NFC_CS(1);
	(void) spi1_xfer(0x80);
	while (n-- > 0) (void) spi1_xfer(*p++);
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
}

static void nr_fifo_r(uint8_t *p, unsigned n)
{
	NFC_CS(1);
	(void) spi1_xfer(0x9F);
	while (n-- > 0) *p++ = spi1_xfer(0);
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
}

/* The chip ready to poll: 0; -1 no ST25R3916 answers; -2 the oscillator
 * does not start; -3 SPI1 is in use (Sub-GHz).  *mv: the supply it
 * measured. */
/* Is the chip up as a poller?  The state is SPI1 itself: on, in mode 1
 * (the CC1101 uses mode 0), so it costs no RAM. */
int hal_nfc_active(void)
{
	return (RCC->APB2ENR & RCC_APB2ENR_SPI1EN)
				&& (SPI1->CR1 & SPI_CR1_CPHA) != 0;
}

int hal_nfc_start(unsigned *mv)
{
	unsigned ad, v;

	if (RCC->APB2ENR & RCC_APB2ENR_SPI1EN) return -3;	/* the CC1101's */
	spi1_on(1);
	nr_cmd(0xC1);				/* SET_DEFAULT */
	hal_delay_ms(2);
	nr_w(0x01, 0x04);			/* IO_CONF2: driver strength */
	if ((nr_r(0x3F) & 0xF8) != 0x28) { spi1_off(); return -1; }
	(void) nr_irq();
	nr_enable(0);
	nr_test_mod(0x04, 0x10, 0x10);		/* no overheat protection */
	if (!(nr_r(0x02) & 0x80)) {		/* the oscillator */
		nr_enable(NR_IRQ_OSC);
		nr_mod(0x02, 0x80, 0x80);
		(void) nr_wait(NR_IRQ_OSC, 10);
	}
	nr_enable(0);
	if (!(nr_r(0x31) & 0x10)) { spi1_off(); return -2; }
	nr_mod(0x2C, 0x07, 0x00);		/* measure VDD */
	nr_enable(NR_IRQ_DCT);
	nr_cmd(0xDF);
	(void) nr_wait(NR_IRQ_DCT, 100);
	nr_enable(0);
	ad = nr_r(0x25);
	v = ad * 23 + (ad * 4 + 5) / 10;
	if (mv != NULL) *mv = v;
	nr_mod(0x01, 0x80, v < 3600 ? 0x80 : 0x00);	/* sup3V */
	nr_mod(0x00, 0x07, 0x07);		/* no MCU clock */
	nr_mod(0x01, 0x18, 0x00);		/* no MISO pull-down */
	nr_mod(0x28, 0x0F, 0x00);		/* driver resistance 1 Ohm */
	nr_mod(NR_B | 0x2A, 0x80, 0x80);	/* minimum non-overlap */
	nr_mod(0x2A, 0x7F, 0x11);		/* field detect on: 105 mV */
	nr_mod(0x2B, 0x7F, 0x00);		/* off: 75 mV */
	nr_mod(NR_B | 0x28, 0x30, 0x30);	/* load modulation */
	nr_mod(0x08, 0xF0, 0x50);		/* FDT adjust */
	nr_mod(0x29, 0xFF, 0x0F);		/* RFO resistance when modulated */
	nr_mod(NR_B | 0x05, 0x40, 0x40);	/* RX start on 4 bits */
	nr_w(0x26, 0x82);			/* antenna tuning */
	nr_w(0x27, 0x82);
	nr_mod(0x02, 0x03, 0x03);		/* automatic field detect */
	if (!(nr_r(0x2C) & 0x80)) {		/* adjust the regulators */
		nr_mod(0x2C, 0x80, 0x80);
		nr_mod(0x2C, 0x80, 0x00);
		nr_cmd(0xD6);
		hal_delay_ms(6);
	}
	return 0;
}

/* NFC-A, 106 kbit/s, as the stock poller sets it. */
void hal_nfc_mode_a(void)
{
	nr_mod(0x02, 0x04, 0x00);		/* no wake-up */
	nr_mod(0x0A, 0x04, 0x00);		/* the correlator */
	nr_w(NR_B | 0x30, 0x40);		/* over- and undershoot */
	nr_w(NR_B | 0x31, 0x03);
	nr_w(NR_B | 0x32, 0x40);
	nr_w(NR_B | 0x33, 0x03);
	nr_mod(0x03, 0x7C, 0x08);		/* ISO 14443A, OOK */
	nr_w(0x0B, 0x08);			/* RX_CONF1: 600 kHz zero */
	nr_w(0x0C, 0x2D);			/* AGC on, 3:1, squelch */
	nr_w(0x0D, 0x00);
	nr_w(0x0E, 0x00);
	nr_w(NR_B | 0x0C, 0x51);		/* correlator */
	nr_w(NR_B | 0x0D, 0x00);
}

void hal_nfc_field(int on)
{
	if (on) {
		nr_w(NR_B | 0x15, 0);		/* minimum guard time */
		nr_mod(0x02, 0x48, 0x48);	/* RX and TX on */
		hal_delay_ms(5);
	} else
		nr_mod(0x02, 0x48, 0x00);
}

/* REQA: the ATQA (b0 | b1 << 8), or -1 no answer. */
int hal_nfc_reqa(uint32_t *seen, int wupa)
{
	uint8_t b[2];
	uint32_t acc;
	unsigned n;

	nr_cmd(0xDB);				/* clear the FIFO */
	nr_mod(0x0A, 0x80, 0x80);		/* no CRC on receive */
	nr_mod(0x05, 0xC0, 0x00);		/* parity as the standard */
	nr_w(0x23, 0);
	(void) nr_irq();
	nr_enable(NR_IRQ_TXE | NR_IRQ_RXE | NR_IRQ_ERR);
	nr_cmd(wupa ? 0xC7 : 0xC6);		/* TRANSMIT_REQA / WUPA */
	acc = nr_wait(NR_IRQ_RXE, 8);
	nr_enable(0);
	if (seen != NULL) *seen = acc;
	if (!(acc & NR_IRQ_RXE) || (acc & NR_IRQ_ERR)) return -1;
	n = nr_r(0x1E) | (nr_r(0x1F) & 0xC0) << 2;
	if (n != 2) return -1;
	nr_fifo_r(b, 2);
	return b[0] | b[1] << 8;
}

static unsigned nr_crc_a(const uint8_t *p, unsigned n)	/* ISO 14443-3 A */
{
	unsigned c = 0x6363;
	uint8_t b;

	while (n-- > 0) {
		b = (uint8_t) (*p++ ^ c);
		b ^= (uint8_t) (b << 4);
		c = (c >> 8) ^ ((unsigned) b << 8) ^ ((unsigned) b << 3) ^ (b >> 4);
		c &= 0xFFFF;
	}
	return c;
}

/* One frame out and, unless HAL_NFC_NO_RX, the answer in; lengths in bits
 * (<minix/nfc.h> NFCIOCXFER, the flags are its NFC_XF_*).  The CRC is
 * made and checked here, as the stock firmware does it in software; the
 * chip keeps the parity.  fwt in 1/13.56 MHz, 0: 10 ms; polled, at most
 * 100 ms (longer waits need the chip's timer and its interrupt: later).
 * 0 and *rx_bits; -1 no answer; -2 CRC, parity or framing; -3 the answer is
 * longer than the room; -4 not supported (a CRC with NFC_XF_PARITY or
 * on a part byte). */
int hal_nfc_xfer(const uint8_t *tx, unsigned tx_bits, uint8_t *rx,
	unsigned rx_room, unsigned *rx_bits, unsigned fwt, unsigned flags)
{
	uint8_t crc[2];
	uint32_t acc;
	unsigned n, lb, c, i, tmo;

	*rx_bits = 0;
	nr_cmd(0xDB);				/* clear the FIFO */
	nr_mod(0x12, 0x02, 0x00);		/* no EMV no-response timer */
	nr_mod(0x0A, 0x80, 0x80);		/* the CRC is ours */
	/* NFC_XF_PARITY: the caller's stream carries the parity bits (9 bits
	 * a byte, MIFARE Classic's Crypto-1 encrypts them); otherwise the
	 * chip makes and checks them. */
	nr_mod(0x05, 0xC0, (flags & 0x04) ? 0xC0 : 0x00);
	(void) nr_irq();
	nr_enable(NR_IRQ_TXE | NR_IRQ_RXE | NR_IRQ_ERR);
	if (flags & 0x10) {			/* NFC_XF_SHORT: REQA, WUPA */
		nr_w(0x23, 0);
		nr_cmd(tx[0] == 0x52 ? 0xC7 : 0xC6);
	} else {
		n = (tx_bits + 7) / 8;
		if ((flags & 0x01) && ((tx_bits & 7) || (flags & 0x04))) {
			nr_enable(0);
			return -4;		/* a CRC needs whole plain bytes */
		}
		if (n + 2 > 512) { nr_enable(0); return -3; }
		c = (flags & 0x01) ? nr_crc_a(tx, n) : 0;	/* NFC_XF_CRC_TX */
		i = tx_bits + ((flags & 0x01) ? 16 : 0);
		nr_w(0x23, (uint8_t) i);	/* the bits to send */
		nr_w(0x22, (uint8_t) (i >> 8));
		NFC_CS(1);
		(void) spi1_xfer(0x80);		/* FIFO load */
		for (i = 0; i < n; i++) (void) spi1_xfer(tx[i]);
		if (flags & 0x01) {
			(void) spi1_xfer((uint8_t) c);
			(void) spi1_xfer((uint8_t) (c >> 8));
		}
		while (SPI1->SR & SPI_SR_BSY) ;
		NFC_CS(0);
		nr_cmd(0xC5);			/* TRANSMIT_WITHOUT_CRC */
	}
	acc = nr_wait(NR_IRQ_TXE | NR_IRQ_RXE, 20);
	if (!(acc & (NR_IRQ_TXE | NR_IRQ_RXE))) { nr_enable(0); return -2; }
	if (flags & 0x08) { nr_enable(0); return 0; }	/* NFC_XF_NO_RX */
	tmo = fwt != 0 ? fwt / 13560 + 2 : 10;
	if (tmo > 100) tmo = 100;
	if (!(acc & NR_IRQ_RXE)) acc |= nr_wait(NR_IRQ_RXE, tmo);
	nr_enable(0);
	if (!(acc & NR_IRQ_RXE)) return (acc & 0x20) ? -2 : -1;	/* RXS only */
	if ((acc & NR_IRQ_ERR) && !(flags & 0x04)) return -2;
	i = nr_r(0x1F);
	n = nr_r(0x1E) | (i & 0xC0) << 2;	/* bytes in the FIFO */
	lb = i >> 1 & 7;			/* bits of an incomplete last byte */
	if (n == 0) return -1;
	if (flags & 0x02) {			/* NFC_XF_CRC_RX */
		if (n < 3 || lb != 0 || (flags & 0x04)) return -2;
		n -= 2;
	}
	if (n * 8 > rx_room + 7) return -3;
	NFC_CS(1);
	(void) spi1_xfer(0x9F);			/* FIFO read */
	for (i = 0; i < n; i++) rx[i] = spi1_xfer(0);
	if (flags & 0x02) {
		crc[0] = spi1_xfer(0);
		crc[1] = spi1_xfer(0);
	}
	while (SPI1->SR & SPI_SR_BSY) ;
	NFC_CS(0);
	if ((flags & 0x02) && nr_crc_a(rx, n) != (crc[0] | (unsigned) crc[1] << 8))
		return -2;
	*rx_bits = lb != 0 ? (n - 1) * 8 + lb : n * 8;
	return 0;
}

void hal_nfc_stop(void)
{
	nr_cmd(0xC2);				/* STOP */
	nr_mod(0x02, 0xCF, 0x00);		/* everything off */
	spi1_off();
}

/* A text report for the first bring-up: the chip, the oscillator, the
 * supply, then the field and a REQA.  Returns the length. */
int hal_nfc_report(char *b, int n)
{
	static const char hx[] = "0123456789abcdef";
	unsigned mv = 0;
	int r, a, k = 0, i, j;
	char d[8];

	r = hal_nfc_start(&mv);
	for (i = 0; i < 2; i++) {
		const char *t = i == 0 ? "nfc start=" : " mv=";
		int v = i == 0 ? r : (int) mv;

		for (; *t; t++) b[k++] = *t;
		if (v < 0) { b[k++] = '-'; v = -v; }
		j = 0;
		do d[j++] = '0' + v % 10; while ((v /= 10) != 0);
		while (j > 0) b[k++] = d[--j];
	}
	if (r == 0) {
		uint32_t seen = 0;
		const char *t;

		unsigned amp;

		hal_nfc_mode_a();
		hal_nfc_field(1);
		nr_enable(NR_IRQ_DCT);			/* the amplitude on RFI */
		nr_cmd(0xD3);
		(void) nr_wait(NR_IRQ_DCT, 10);
		nr_enable(0);
		amp = nr_r(0x25);
		a = hal_nfc_reqa(&seen, 0);
		if (a < 0) a = hal_nfc_reqa(&seen, 1);
		t = " atqa=";
		for (; *t; t++) b[k++] = *t;
		if (a < 0) b[k++] = '-';
		else for (i = 12; i >= 0; i -= 4) b[k++] = hx[a >> i & 15];
		t = " amp=";
		for (; *t; t++) b[k++] = *t;
		b[k++] = hx[amp >> 4 & 15];
		b[k++] = hx[amp & 15];
		t = " irq=";
		for (; *t; t++) b[k++] = *t;
		for (i = 20; i >= 0; i -= 4) b[k++] = hx[seen >> i & 15];
		t = " regs=";
		for (; *t; t++) b[k++] = *t;
		for (i = 0; i < 6; i++) {	/* op control, aux display,
						 * regulator, rssi, fifo 1, 2 */
			static const uint8_t ra[6] = { 0x02, 0x31, 0x2C, 0x2D,
								0x1E, 0x1F };
			unsigned v = nr_r(ra[i]);

			b[k++] = hx[v >> 4 & 15];
			b[k++] = hx[v & 15];
			b[k++] = ' ';
		}
		hal_nfc_stop();
	}
	b[k++] = '\n';
	return k > n ? n : k;
}

/*===========================================================================*
 *			Sub-GHz receive (R8.1, part 1)			     *
 *===========================================================================*/
/* The CC1101 as a receiver only: the stock firmware's OOK 650 kHz preset
 * (lib/subghz/devices/cc1101_configs.c; GPL), a frequency in the stock
 * bands, the RF path switch as stock (PC4 and GDO2), RX, and the RSSI.
 * There is no transmit here at all: no STX strobe and no PA table write;
 * transmitting comes with the kernel's band limits (R8.2). */
static const uint8_t cc_rx_regs[] = {
	0x02, 0x0D,	/* IOCFG0: GDO0 async serial data */
	0x03, 0x07,	/* FIFOTHR */
	0x08, 0x32,	/* PKTCTRL0: async, continuous */
	0x0B, 0x06,	/* FSCTRL1: IF 152 kHz */
	0x14, 0x00,	/* MDMCFG0 */
	0x13, 0x00,	/* MDMCFG1 */
	0x12, 0x30,	/* MDMCFG2: ASK/OOK, no preamble/sync */
	0x11, 0x32,	/* MDMCFG3 */
	0x10, 0x17,	/* MDMCFG4: RX bandwidth 650 kHz */
	0x18, 0x18,	/* MCSM0: calibrate from idle to RX */
	0x19, 0x18,	/* FOCCFG */
	0x1D, 0x91,	/* AGCCTRL0 */
	0x1C, 0x00,	/* AGCCTRL1 */
	0x1B, 0x07,	/* AGCCTRL2 */
	0x20, 0xFB,	/* WORCTRL */
	0x21, 0xB6,	/* FREND1 */
};

static uint8_t cc_xfer2(uint8_t a, uint8_t v)	/* address byte, data byte */
{
	uint8_t r;

	(void) cc_ready();			/* CS low, CHIP_RDYn */
	(void) spi1_xfer(a);
	r = spi1_xfer(v);
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);
	return r;
}

static void cc_strobe(uint8_t s)
{
	(void) cc_ready();
	(void) spi1_xfer(s);
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);
}

static int cc_band(uint32_t hz)		/* stock's bands: 315 433 868, or -1 */
{
	if (hz >= 299999755u && hz <= 348000335u) return 315;
	if (hz >= 386999938u && hz <= 464000000u) return 433;
	if (hz >= 778999847u && hz <= 928000000u) return 868;
	return -1;
}

long hal_subghz_rx(uint32_t hz)
{
	uint32_t w, t0;
	unsigned i;
	int band = cc_band(hz);

	if (band < 0) return -1;
	w = (uint32_t) (((uint64_t) hz << 16) / 26000000u);	/* 26 MHz xtal */
	gpio_cfg(GPIOC, 4, MODE_OUT, PULL_NONE, 0);	/* RF switch 0 */
	spi1_on(0);
	/* Already set up (a sweep retunes): only idle, frequency, path, RX,
	 * about 1 ms instead of 3. */
	if (cc_xfer2(0x10 | 0x80, 0) != 0x17 || cc_xfer2(0x08 | 0x80, 0) != 0x32) {
		cc_strobe(0x30);			/* SRES */
		(void) cc_ready();
		CC_CS(0);
		for (i = 0; i < sizeof cc_rx_regs; i += 2)
			(void) cc_xfer2(cc_rx_regs[i], cc_rx_regs[i + 1]);
	} else
		cc_strobe(0x36);			/* SIDLE first */
	(void) cc_xfer2(0x0D, (uint8_t) (w >> 16));	/* FREQ2..0 */
	(void) cc_xfer2(0x0E, (uint8_t) (w >> 8));
	(void) cc_xfer2(0x0F, (uint8_t) w);
	/* the path, as stock furi_hal_subghz_set_path */
	gpio_set(GPIOC, 4, band != 433);
	(void) cc_xfer2(0x00, band == 315 ? 0x2F : 0x2F | 0x40);	/* IOCFG2 */
	cc_strobe(0x36);				/* SIDLE */
	cc_strobe(0x34);				/* SRX (calibrates) */
	/* The radio is listening when this returns: calibration takes about
	 * 1 ms, and a read right after the write (Spectrum's) found "off"
	 * and said "no radio".  RX is MARCSTATE 0x0D; 5 ms at most. */
	t0 = DWT->CYCCNT;
	while ((cc_xfer2(0x35 | 0xC0, 0) & 0x1F) != 0x0D
	       && DWT->CYCCNT - t0 < hal_cpu_hz() / 200) ;
	spi1_off();
	return (long) (((uint64_t) w * 26000000u) >> 16);
}

int hal_subghz_rssi(uint32_t *hz)	/* dBm, or HAL_SUBGHZ_OFF */
{
	uint8_t st, f2, f1, f0;
	int raw;

	spi1_on(0);
	st = cc_xfer2(0x35 | 0xC0, 0) & 0x1F;		/* MARCSTATE */
	raw = (int8_t) cc_xfer2(0x34 | 0xC0, 0);		/* RSSI */
	f2 = cc_xfer2(0x0D | 0x80, 0);
	f1 = cc_xfer2(0x0E | 0x80, 0);
	f0 = cc_xfer2(0x0F | 0x80, 0);
	spi1_off();
	*hz = (uint32_t) ((((uint64_t) f2 << 16 | f1 << 8 | f0) * 26000000u) >> 16);
	if (st != 0x0D) return HAL_SUBGHZ_OFF;		/* not in RX */
	return raw / 2 - 74;			/* datasheet: RSSI offset 74 dB */
}

void hal_subghz_off(void)
{
	spi1_on(0);
	cc_strobe(0x36);				/* SIDLE */
	cc_strobe(0x39);				/* SPWD: asleep */
	spi1_off();
	gpio_set(GPIOC, 4, 0);
}

/* Receiving packets (R8.2, receive half): stock's chat preset, GFSK at
 * 9.99 kBd, sync word 0x464C, variable length, CRC checked by the chip
 * (stock lib/subghz/devices/cc1101_configs.c,
 * subghz_device_cc1101_preset_gfsk_9_99kb_async_regs; GPL).  Receive only:
 * no PA table, no STX.  The preset is loaded at the first call on a tuned
 * radio, keeping the frequency and the RF path; hal_subghz_rx sees the
 * other modem settings and loads its own again (the registers say which
 * preset is in: no static RAM).  Poll it: it never waits. */
static const uint8_t cc_pkt_regs[] = {
	0x02, 0x06,	/* IOCFG0: GDO0 high from sync to the packet's end */
	0x03, 0x47,	/* FIFOTHR */
	0x08, 0x05,	/* PKTCTRL0: variable length, CRC */
	0x0B, 0x06,	/* FSCTRL1 */
	0x04, 0x46,	/* SYNC1 */
	0x05, 0x4C,	/* SYNC0 */
	0x09, 0x00,	/* ADDR */
	0x06, 0x00,	/* PKTLEN (variable: the first byte) */
	0x10, 0xC8,	/* MDMCFG4: 9.99 kBd */
	0x11, 0x93,	/* MDMCFG3 */
	0x12, 0x12,	/* MDMCFG2: GFSK, 16/16 sync bits */
	0x15, 0x34,	/* DEVIATN: 19 kHz */
	0x17, 0x3F,	/* MCSM1: stay in RX after a packet, and go to RX
			 * after one is sent (stock goes
			 * idle and re-arms; here packets queue in the FIFO:
			 * stock's chat sends a line as several short ones) */
	0x18, 0x18,	/* MCSM0 */
	0x19, 0x16,	/* FOCCFG */
	0x1B, 0x43,	/* AGCCTRL2 */
	0x1C, 0x40,	/* AGCCTRL1 */
	0x1D, 0x91,	/* AGCCTRL0 */
	0x20, 0xFB,	/* WORCTRL */
};

/* SPI on.  0: the packet preset is in; 1: it was loaded just now (on the
 * tuned OOK receiver, frequency and path kept, RX started); -1: the radio
 * is not tuned. */
static int cc_pkt_mode(void)
{
	uint8_t f[3], g2;
	unsigned i;

	if (cc_xfer2(0x10 | 0x80, 0) == 0xC8 && cc_xfer2(0x08 | 0x80, 0) == 0x05)
		return 0;
	if ((cc_xfer2(0x35 | 0xC0, 0) & 0x1F) != 0x0D) return -1;
	for (i = 0; i < 3; i++) f[i] = cc_xfer2((0x0D + i) | 0x80, 0);
	g2 = cc_xfer2(0x00 | 0x80, 0);		/* IOCFG2: the path */
	cc_strobe(0x36);				/* SIDLE */
	cc_strobe(0x30);				/* SRES */
	(void) cc_ready();
	CC_CS(0);
	for (i = 0; i < sizeof cc_pkt_regs; i += 2)
		(void) cc_xfer2(cc_pkt_regs[i], cc_pkt_regs[i + 1]);
	for (i = 0; i < 3; i++) (void) cc_xfer2(0x0D + i, f[i]);
	(void) cc_xfer2(0x00, g2);
	cc_strobe(0x34);				/* SRX (calibrates) */
	return 1;
}

int hal_subghz_pkt_rx(uint8_t *p, unsigned max)
{
	uint8_t n, lqi;
	unsigned k;
	int r = 0;

	spi1_on(0);
	if ((r = cc_pkt_mode()) != 0) {
		spi1_off();
		return r < 0 ? -1 : 0;
	}
	if ((cc_xfer2(0x35 | 0xC0, 0) & 0x1F) == 0x11) {	/* RX overflow */
		cc_strobe(0x3A);			/* SFRX */
		cc_strobe(0x34);
		spi1_off();
		return 0;
	}
	/* Only with no packet coming in (GDO0 low), so every packet in the
	 * FIFO is complete: length, data, then RSSI and LQI|CRC_OK. */
	if ((cc_xfer2(0x38 | 0xC0, 0) & 0x01)		/* PKTSTATUS GDO0 */
	    || (cc_xfer2(0x3B | 0xC0, 0) & 0x7F) == 0) {	/* RXBYTES */
		spi1_off();
		return 0;
	}
	(void) cc_ready();				/* FIFO burst read */
	(void) spi1_xfer(0x3F | 0xC0);
	n = spi1_xfer(0);				/* the length byte */
	if (n > 61) {					/* not a length: lost sync */
		while (SPI1->SR & SPI_SR_BSY) ;
		CC_CS(0);
		cc_strobe(0x36);			/* SIDLE */
		cc_strobe(0x3A);			/* SFRX */
		cc_strobe(0x34);			/* SRX */
		spi1_off();
		return 0;
	}
	for (k = 0; k < n; k++) {
		uint8_t b = spi1_xfer(0);

		if (k < max) p[k] = b;
	}
	(void) spi1_xfer(0);				/* RSSI */
	lqi = spi1_xfer(0);				/* LQI, bit 7 CRC_OK */
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);
	if (lqi & 0x80) r = n < max ? n : (int) max;
	spi1_off();
	return r;
}

/* Transmit (R8.2).  The kernel has checked the region, the band and the
 * hour's air time (sg_tx_check) before any of these is called; they only
 * drive the chip, on the frequency it is tuned to.  Power as stock's
 * presets: PATABLE 0xC0, +10 dBm.
 *
 * OOK frames: hal_subghz_tx_begin puts the tuned OOK receiver into async
 * TX (FREND0 0x11: a 1 on GDO0 is PATABLE[1], a 0 is PATABLE[0] = off)
 * and gives PA1 (GDO0, the chip's data input in TX) to TIM2_CH2 in toggle
 * mode; the kernel's IR transmit engine (TIM2, its ring of durations)
 * then times the segments and the timer flips the pin at each boundary
 * in hardware, as stock's async TX does (hal_ir_target(2)).  hal_subghz_tx_end: low, idle, PATABLE off, RX again. */
static void cc_patable(uint8_t on)
{
	unsigned i;

	(void) cc_ready();
	(void) spi1_xfer(0x3E | 0x40);			/* PATABLE, burst */
	(void) spi1_xfer(0x00);
	(void) spi1_xfer(on);
	for (i = 2; i < 8; i++) (void) spi1_xfer(0x00);
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);
}

static int cc_wait_state(uint8_t st)		/* MARCSTATE, 5 ms at most */
{
	uint32_t t0 = DWT->CYCCNT;

	while ((cc_xfer2(0x35 | 0xC0, 0) & 0x1F) != st)
		if (DWT->CYCCNT - t0 >= hal_cpu_hz() / 200) return -1;
	return 0;
}

static int hal_subghz_tx_begin(void)
{
	int r = 0;

	spi1_on(0);
	if (cc_xfer2(0x10 | 0x80, 0) != 0x17 || cc_xfer2(0x08 | 0x80, 0) != 0x32
	    || (cc_xfer2(0x35 | 0xC0, 0) & 0x1F) != 0x0D) {
		spi1_off();
		return -1;			/* not the tuned OOK receiver */
	}
	/* GDO0 (PA1) as TIM2_CH2, toggled by the timer itself 1 us after
	 * every segment starts (CCR2 = 1, toggle on match): the edges are
	 * as exact as the transmit engine's ARR, whatever the interrupt
	 * latency, as stock's async TX.  Low until the first toggle. */
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->CR1 = 0;
	TIM2->CCER = 0;
	TIM2->CCR2 = 1;
	TIM2->CCMR1 = 4u << TIM_CCMR1_OC2M_Pos;		/* forced low */
	TIM2->CCER = TIM_CCER_CC2E;
	TIM2->CCMR1 = 3u << TIM_CCMR1_OC2M_Pos;		/* toggle on match */
	gpio_cfg(GPIOA, 1, MODE_AF, PULL_NONE, 1);	/* TIM2_CH2 */
	cc_strobe(0x36);				/* SIDLE */
	(void) cc_xfer2(0x22, 0x11);			/* FREND0 */
	cc_patable(0xC0);
	cc_strobe(0x35);				/* STX (calibrates) */
	if (cc_wait_state(0x13) < 0) {			/* TX */
		cc_strobe(0x36);
		cc_patable(0x00);
		cc_strobe(0x34);
		r = -1;
	}
	spi1_off();
	if (r < 0) {
		TIM2->CCER = 0;
		TIM2->CCMR1 = 0;
		gpio_cfg(GPIOA, 1, MODE_ANALOG, PULL_NONE, 0);
	}
	return r;
}

/* The frame by DMA, as stock's async TX: DMA1 channel 5 on TIM2's update
 * request writes each duration (us, the writer's pinned memory: src, n
 * words) into ARR as its segment starts (no preload: the value written at
 * an update is the segment that update begins), and TIM2_CH2 flips GDO0 2 us
 * into every segment (CCR2 = 2; the short first period, ARR = 1, never
 * reaches it).  No interrupt per segment: channel 5's transfer-complete
 * comes when the last duration is in (hal_subghz_tx_last then asks for the
 * update that ends it).  A segment lasts its value + 1 us. */
int hal_subghz_tx_dma(uint32_t src, unsigned n)
{
	if (n < 2) return -1;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->CR1 = 0;
	TIM2->DIER = 0;
	TIM2->SMCR = 0;
	TIM2->CR2 = 0;
	TIM2->OR = 0;
	TIM2->PSC = SystemCoreClock / 1000000u - 1;	/* 1 us */
	TIM2->ARR = 1;
	TIM2->CCR2 = 2;
	TIM2->CNT = 0;
	TIM2->CR1 = TIM_CR1_URS;
	TIM2->EGR = TIM_EGR_UG;				/* PSC now */
	TIM2->SR = 0;
	DMA1_Channel5->CCR = 0;
	DMA1->IFCR = DMA_IFCR_CGIF5;
	DMAMUX1_Channel4->CCR = 32;			/* TIM2_UP -> channel 5 */
	DMA1_Channel5->CPAR = (uint32_t) &TIM2->ARR;
	DMA1_Channel5->CMAR = src;
	DMA1_Channel5->CNDTR = n;
	DMA1_Channel5->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_PSIZE_1
		| DMA_CCR_MSIZE_1 | DMA_CCR_TCIE | DMA_CCR_EN;
	TIM2->DIER = TIM_DIER_UDE;
	TIM2->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
	return 0;
}

/* Channel 5's interrupt in a DMA transmission: 1 if it was the transfer
 * complete (acked; the last segment is playing, its end comes as TIM2's
 * update interrupt now), else 0. */
int hal_subghz_tx_last(void)
{
	uint32_t isr = DMA1->ISR;

	DMA1->IFCR = DMA_IFCR_CGIF5 | DMA_IFCR_CTCIF5 | DMA_IFCR_CHTIF5;
	if (!(isr & DMA_ISR_TCIF5)) return 0;
	TIM2->SR = 0;
	TIM2->DIER = TIM_DIER_UIE;		/* no more DMA: the end, please */
	return 1;
}

void hal_subghz_tx_end(void)
{
	DMA1_Channel5->CCR = 0;			/* a DMA transmission, if any */
	DMA1->IFCR = DMA_IFCR_CGIF5;
	TIM2->DIER = 0;
	TIM2->CR1 = 0;
	TIM2->CCMR1 = 4u << TIM_CCMR1_OC2M_Pos;		/* forced low */
	TIM2->CCER = 0;
	TIM2->CCMR1 = 0;			/* the receiver's capture sets its own */
	gpio_set(GPIOA, 1, 0);
	gpio_cfg(GPIOA, 1, MODE_OUT, PULL_NONE, 0);	/* low while the PA stops */
	spi1_on(0);
	cc_strobe(0x36);				/* SIDLE */
	cc_patable(0x00);
	(void) cc_xfer2(0x22, 0x10);			/* FREND0 default */
	cc_strobe(0x34);				/* SRX: listening again */
	/* until it is (calibration, about 1 ms): the next frame of the same
	 * transmission is written at once, and the kernel's check reads the
	 * state back ("not tuned" otherwise) */
	(void) cc_wait_state(0x0D);
	spi1_off();
	gpio_cfg(GPIOA, 1, MODE_ANALOG, PULL_NONE, 0);	/* as stock */
}

int hal_subghz_tx(uint32_t hz, uint32_t src, unsigned n)
{
	(void) hz; (void) src; (void) n;	/* the kernel's engine sends */
	return hal_subghz_tx_begin() < 0 ? -1 : 1;
}

/* Packets: stock's chat preset (loaded by the first read or send).  The packet goes into the
 * TX FIFO with its length byte, STX sends it, and MCSM1 brings the chip
 * back to RX by itself: nothing waits here.  -1: not in the packet preset;
 * -2: the one before is still going out. */
int hal_subghz_pkt_tx(uint32_t hz, const uint8_t *p, unsigned n)
{
	unsigned i;
	uint8_t st;

	(void) hz;
	if (n == 0 || n > HAL_SUBGHZ_PKT) return -1;
	spi1_on(0);
	if ((i = (unsigned) cc_pkt_mode()) == (unsigned) -1) {
		spi1_off();
		return -1;			/* not tuned */
	}
	if (i == 1) (void) cc_wait_state(0x0D);	/* just loaded: calibrating */
	st = cc_xfer2(0x35 | 0xC0, 0) & 0x1F;
	if (st == 0x13 || st == 0x14 || st == 0x15) {	/* TX, TX_END, RX->TX */
		spi1_off();
		return -2;
	}
	cc_strobe(0x36);				/* SIDLE */
	cc_strobe(0x3B);				/* SFTX */
	(void) cc_xfer2(0x22, 0x10);			/* FREND0: PATABLE[0] */
	(void) cc_ready();
	(void) spi1_xfer(0x3E | 0x40);			/* PATABLE[0] = 0xC0 */
	(void) spi1_xfer(0xC0);
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);
	(void) cc_ready();
	(void) spi1_xfer(0x3F | 0x40);			/* TX FIFO, burst */
	(void) spi1_xfer((uint8_t) n);
	for (i = 0; i < n; i++) (void) spi1_xfer(p[i]);
	while (SPI1->SR & SPI_SR_BSY) ;
	CC_CS(0);
	cc_strobe(0x35);				/* STX */
	spi1_off();
	return 0;
}
int hal_subghz_fake(int what) { (void) what; return -1; }
int hal_subghz_fake_take(int what) { (void) what; return 0; }
int hal_subghz_pkt_fake(const uint8_t *p, unsigned n) { (void) p; (void) n; return -1; }
int hal_subghz_line(uint32_t *hash) { (void) hash; return -1; }

/*===========================================================================*
 *				Clocks					     *
 *===========================================================================*/
/* HSE 32 MHz / M 2 = 16 MHz PLL input, * N 8 = 128 MHz VCO, / R 2 = 64 MHz.
 * Falls back to the 4 MHz MSI if the crystal does not start. */
static void clock_init(void)
{
	uint32_t timeout;

	/* Coming from the DFU bootloader the CPU may already run from the
	 * PLL; it cannot be reconfigured while in use.  Go back to MSI. */
	if ((RCC->CFGR & RCC_CFGR_SWS) != 0) {
		RCC->CR |= RCC_CR_MSION;
		while (!(RCC->CR & RCC_CR_MSIRDY)) ;
		RCC->CFGR &= ~RCC_CFGR_SW;
		while ((RCC->CFGR & RCC_CFGR_SWS) != 0) ;
	}

	/* 3 wait states are required at 64 MHz in voltage range 1; set them
	 * before raising the clock (harmless at 4 MHz). */
	FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | FLASH_ACR_LATENCY_3WS
		   | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN | FLASH_ACR_DCEN;
	while ((FLASH->ACR & FLASH_ACR_LATENCY) != FLASH_ACR_LATENCY_3WS) ;

	RCC->CR |= RCC_CR_HSEON;
	for (timeout = 200000; !(RCC->CR & RCC_CR_HSERDY); )
		if (--timeout == 0) return;		/* stay on MSI */

	RCC->CR &= ~RCC_CR_PLLON;
	while (RCC->CR & RCC_CR_PLLRDY) ;
	RCC->PLLCFGR = (3u << RCC_PLLCFGR_PLLSRC_Pos)	/* HSE */
		     | ((2u - 1) << RCC_PLLCFGR_PLLM_Pos)
		     | (8u << RCC_PLLCFGR_PLLN_Pos)
		     | ((2u - 1) << RCC_PLLCFGR_PLLR_Pos)
		     | RCC_PLLCFGR_PLLREN;
	RCC->CR |= RCC_CR_PLLON;
	for (timeout = 200000; !(RCC->CR & RCC_CR_PLLRDY); )
		if (--timeout == 0) return;

	/* CPU2's clock is SYSCLK / 2: the Cortex-M0+ takes 32 MHz at most
	 * (stock: LL_C2_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_2)).  Left at
	 * /1 it ran at 64 MHz: the BLE stack and FUS still answered simple
	 * commands, but FUS found every stack image "not authentic"
	 * (FUS_GetState 10.03, 2026-11-27): its signature check read or
	 * computed wrongly.  Set before the clock goes up. */
	RCC->EXTCFGR = (RCC->EXTCFGR & ~RCC_EXTCFGR_C2HPRE)
		     | (8u << RCC_EXTCFGR_C2HPRE_Pos);
	for (timeout = 200000; !(RCC->EXTCFGR & RCC_EXTCFGR_C2HPREF); )
		if (--timeout == 0) break;

	RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (3u << RCC_CFGR_SW_Pos);
	while ((RCC->CFGR & RCC_CFGR_SWS) != (3u << RCC_CFGR_SWS_Pos)) ;
	SystemCoreClock = 64000000;

	/* The SMPS step-down converter instead of the LDO for the core
	 * (T14a): about half the run current.  Its clock as the stock
	 * firmware sets it (furi_hal_clock.c): HSI16, SMPSDIV 01, within the
	 * 2-8 MHz it needs; HSI16 stays on for it. */
	RCC->CR |= RCC_CR_HSION;
	for (timeout = 200000; !(RCC->CR & RCC_CR_HSIRDY); )
		if (--timeout == 0) return;		/* LDO, as before */
	RCC->SMPSCR = (RCC->SMPSCR & ~(RCC_SMPSCR_SMPSSEL | RCC_SMPSCR_SMPSDIV))
		    | RCC_SMPSCR_SMPSDIV_0;		/* HSI16 */
	PWR->CR5 |= PWR_CR5_SMPSEN;
}

__attribute__((weak)) void SysTick_Handler(void)
{
	hal_ms++;
	if (hal_tick_hook) hal_tick_hook();
}

/* Busy-wait with the cycle counter, so delays also work inside the MINIX
 * kernel, which owns SysTick and runs with interrupts masked at times. */
static void cycles_init(void)
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t ms_since(uint32_t start)
{
	return (DWT->CYCCNT - start) / (SystemCoreClock / 1000);
}

void hal_delay_ms(uint32_t ms)
{
	uint32_t start = DWT->CYCCNT;

	while (ms_since(start) < ms) ;
}

/*===========================================================================*
 *				USART1 (GPIO header pins 13 TX / 14 RX)	     *
 *===========================================================================*/
static void uart_init(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
	(void) RCC->APB2ENR;
	gpio_cfg(GPIOB, 6, MODE_AF, PULL_NONE, 7);	/* TX */
	gpio_cfg(GPIOB, 7, MODE_AF, PULL_UP, 7);	/* RX */
	USART1->CR1 = 0;
	USART1->BRR = (SystemCoreClock + 115200 / 2) / 115200;
	USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE;
}

void hal_uart_putc(int c)
{
	/* A fault before uart_init() must not spin forever on a dead port. */
	if (!(RCC->APB2ENR & RCC_APB2ENR_USART1EN) || !(USART1->CR1 & USART_CR1_UE))
		return;
	while (!(USART1->ISR & USART_ISR_TXE_TXFNF)) ;
	USART1->TDR = (uint8_t) c;
}

int hal_uart_getc(void)
{
	/* Clear overrun/framing/noise errors so reception keeps working. */
	if (USART1->ISR & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE))
		USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
	if (!(USART1->ISR & USART_ISR_RXNE_RXFNE)) return -1;
	return USART1->RDR & 0xFF;
}

int hal_uart_irq(void)
{
	return USART1_IRQn;
}

void hal_uart_rx_irq(int on)
{
	if (on) USART1->CR1 |= USART_CR1_RXNEIE_RXFNEIE;
	else USART1->CR1 &= ~USART_CR1_RXNEIE_RXFNEIE;
}

/*===========================================================================*
 *				SPI2 ("spi_d": display + SD card)	     *
 *===========================================================================*/
static void spi2_init(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_SPI2EN;
	(void) RCC->APB1ENR1;
	gpio_cfg(GPIOD, 1, MODE_AF, PULL_NONE, 5);	/* SCK  */
	gpio_cfg(GPIOB, 15, MODE_AF, PULL_NONE, 5);	/* MOSI */
	gpio_cfg(GPIOC, 2, MODE_AF, PULL_UP, 5);	/* MISO */

	SPI2->CR1 = 0;
	SPI2->CR2 = (7u << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;	/* 8-bit */
	/* Mode 0, master, software NSS, fPCLK/16 = 4 MHz (display limit). */
	SPI2->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI
		  | (3u << SPI_CR1_BR_Pos) | SPI_CR1_SPE;
}

/* Baud rate divider code: fPCLK / 2^(code+1).  The display and the SD card
 * share the bus and use different speeds; set it before each transaction. */
#define SPI_DIV_DISPLAY	2	/* /8 = 8 MHz (stock: 4; the ST7567 takes 20) */
#define SPI_DIV_SD_INIT	7	/* /256 = 250 kHz, card identification */
#define SPI_DIV_SD_FAST	1	/* /4 = 16 MHz (the stock firmware's SD speed) */

static void spi2_speed(unsigned code)
{
	uint32_t cr1 = SPI2->CR1;

	if (((cr1 & SPI_CR1_BR) >> SPI_CR1_BR_Pos) == code) return;
	SPI2->CR1 = cr1 & ~SPI_CR1_SPE;
	SPI2->CR1 = (cr1 & ~SPI_CR1_BR) | (code << SPI_CR1_BR_Pos);
}

static uint8_t spi2_xfer(uint8_t out)
{
	while (!(SPI2->SR & SPI_SR_TXE)) ;
	*(volatile uint8_t *) &SPI2->DR = out;	/* 8-bit access: no packing */
	while (!(SPI2->SR & SPI_SR_RXNE)) ;
	return *(volatile uint8_t *) &SPI2->DR;
}

/* A block: out (NULL: all 0xFF) is sent, what comes back goes to in
 * (NULL: dropped).  The next byte goes into the TX FIFO before the last
 * one is read back, so the bus does not stop between bytes; at most two
 * are in flight, which the 4-byte RX FIFO holds even if an interrupt
 * comes in between. */
static void spi2_block(const uint8_t *out, uint8_t *in, unsigned len)
{
	volatile uint8_t *dr = (volatile uint8_t *) &SPI2->DR;
	unsigned tx = 0, rx = 0;
	uint8_t c;

	while (rx < len) {
		if (tx < len && tx - rx < 2 && (SPI2->SR & SPI_SR_TXE)) {
			*dr = out != NULL ? out[tx] : 0xFF;
			tx++;
		}
		if (SPI2->SR & SPI_SR_RXNE) {
			c = *dr;
			if (in != NULL) in[rx] = c;
			rx++;
		}
	}
}

/* SD data blocks by DMA (T4b step 2): DMA1 channel 1 = SPI2 RX (DMAMUX
 * request 8), channel 2 = SPI2 TX (request 9; numbers from ST's
 * stm32wbxx_ll_dmamux.h).  The disk task sleeps meanwhile (hal_disk_wait,
 * woken by the transfer-complete interrupt), so user processes run.  The
 * bytes not wanted (a write's answers, a read's 0xFF) come from and go to
 * this function's own stack, which stays while it waits.  After waking up,
 * the flag is checked again in the hardware: an early or stale wakeup
 * only ends in polling. */
#define SD_DMA_RX	DMA1_Channel1
#define SD_DMA_TX	DMA1_Channel2

static void spi2_dma(const uint8_t *out, uint8_t *in, unsigned len)
{
	uint8_t sink = 0, ones = 0xFF;
	uint32_t start = DWT->CYCCNT;

	SD_DMA_RX->CCR = 0;
	SD_DMA_TX->CCR = 0;
	DMA1->IFCR = DMA_IFCR_CGIF1 | DMA_IFCR_CGIF2;
	SD_DMA_RX->CPAR = (uint32_t) &SPI2->DR;
	SD_DMA_RX->CMAR = (uint32_t) (in != NULL ? in : &sink);
	SD_DMA_RX->CNDTR = len;
	SD_DMA_TX->CPAR = (uint32_t) &SPI2->DR;
	SD_DMA_TX->CMAR = (uint32_t) (out != NULL ? out : &ones);
	SD_DMA_TX->CNDTR = len;
	SD_DMA_RX->CCR = (in != NULL ? DMA_CCR_MINC : 0) | DMA_CCR_PL_1
		| (NVIC_GetEnableIRQ(DMA1_Channel1_IRQn) ? DMA_CCR_TCIE : 0);
	SD_DMA_TX->CCR = DMA_CCR_DIR | (out != NULL ? DMA_CCR_MINC : 0)
		| DMA_CCR_PL_1;
	SPI2->CR2 |= SPI_CR2_RXDMAEN;		/* RX first (RM0434) */
	SD_DMA_RX->CCR |= DMA_CCR_EN;
	SD_DMA_TX->CCR |= DMA_CCR_EN;
	SPI2->CR2 |= SPI_CR2_TXDMAEN;
	if (SD_DMA_RX->CCR & DMA_CCR_TCIE) hal_disk_wait();
	while (!(DMA1->ISR & DMA_ISR_TCIF1))	/* RX done: every byte is in */
		if (ms_since(start) > 100) break;	/* 512 B take 0.26 ms */
	SPI2->CR2 &= ~(SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN);
	SD_DMA_RX->CCR = 0;
	SD_DMA_TX->CCR = 0;
	DMA1->IFCR = DMA_IFCR_CGIF1 | DMA_IFCR_CGIF2;
	(void) sink;
}

int hal_disk_dma_irq(void) { return DMA1_Channel1_IRQn; }

void hal_disk_dma_ack(void)	/* the interrupt: off; the flag stays */
{
	SD_DMA_RX->CCR &= ~DMA_CCR_TCIE;
}

static void spi2_wait_idle(void)
{
	while (SPI2->SR & SPI_SR_FTLVL) ;
	while (SPI2->SR & SPI_SR_BSY) ;
}

/*===========================================================================*
 *				ST7567 display				     *
 *===========================================================================*/
#define LCD_CS(on)	gpio_set(GPIOC, 11, !(on))	/* active low */
#define LCD_DATA(on)	gpio_set(GPIOB, 1, (on))	/* A0: 0 cmd, 1 data */
#define LCD_RST(on)	gpio_set(GPIOB, 0, !(on))	/* active low */
#define SD_CS(on)	gpio_set(GPIOC, 12, !(on))

#define OTP_DISPLAY_MGG	2	/* FuriHalVersionDisplayMgg */

static void i2c1_init(void);
static int lp5562_init(void);
static void charger_init(void);
static uint32_t boot_reg(void);
static void boot_reg_set(uint32_t r);
#define BOOT_POR	0x200u	/* this start follows a power-on (a fresh BOR flag): set by hal_early_boot at every start (BKP19R bit 9, with BOOT_DEV and BOOT_CAUSE below); the charging mode (#407) looks at it */
static void bkp_set(volatile uint32_t *reg, uint32_t v);
static void rtc_alarm_out(int on);
static void rtc_boot(void);
static void rtc_off(void);

static void lcd_cmds(const uint8_t *cmd, unsigned n)
{
	spi2_speed(SPI_DIV_DISPLAY);
	LCD_DATA(0);
	LCD_CS(1);
	while (n-- > 0) spi2_xfer(*cmd++);
	spi2_wait_idle();
	LCD_CS(0);
}

/* Which glass is fitted is recorded in the factory OTP (layout v2). */
static int lcd_is_mgg(void)
{
	const volatile uint8_t *otp = (const volatile uint8_t *) OTP_AREA_BASE;

	if (otp[0] != 0xBE || otp[1] != 0xBA) return 0;	/* magic 0xBABE */
	if (otp[2] != 2) return 0;				/* OTP v2 */
	return otp[12] == OTP_DISPLAY_MGG;
}

void hal_lcd_init(void)
{
	static const uint8_t init_erc[] =
		{ 0xE2, 0xA2, 0xA0, 0xC8, 0x40, 0x25, 0x81, 0x20, 0x2F, 0xA4, 0xAF };
	static const uint8_t init_mgg[] =
		{ 0xE2, 0xA2, 0xA0, 0xC8, 0x40, 0x26, 0x81, 0x1C, 0x2F, 0xA4, 0xAF };

	LCD_CS(0);
	SD_CS(0);
	LCD_RST(1);
	hal_delay_ms(1);
	LCD_RST(0);
	hal_delay_ms(1);
	if (lcd_is_mgg()) lcd_cmds(init_mgg, sizeof init_mgg);
	else lcd_cmds(init_erc, sizeof init_erc);

	i2c1_init();
	if (lp5562_init() == 0) hal_backlight(255);
	charger_init();
}

int hal_lcd_flush(const uint8_t *fb)
{
	uint8_t addr[3];
	unsigned page;

	/* The SD card is selected: a transfer is open (its task may sleep in
	 * the middle of it, T4b) and SPI2 is its.  The caller draws later. */
	if (!(GPIOC->ODR & (1u << 12))) return -1;

	for (page = 0; page < LCD_H / 8; page++) {
		addr[0] = 0xB0 | page;		/* page address */
		addr[1] = 0x10;			/* column high nibble = 0 */
		addr[2] = 0x00;			/* column low nibble = 0 */
		lcd_cmds(addr, sizeof addr);
		LCD_DATA(1);
		LCD_CS(1);
		spi2_block(&fb[page * LCD_W], NULL, LCD_W);
		spi2_wait_idle();
		LCD_CS(0);
	}
	return 0;
}

/*===========================================================================*
 *				The charging mode (#407)		     *
 *===========================================================================*/
/* A Flipper switched off and then plugged into USB (the charger's ship mode
 * ends with the VBUS and the CPU starts) does not boot the system: it shows
 * the charging screen and waits.  Back held for a second starts the system
 * (a software reset: the next start is a normal one); the cable pulled out
 * switches the Flipper off again.  Only a start after a power-on (BOOT_POR)
 * with VBUS present and Back not held: a start by Back (the charger's QON)
 * is a normal start, and so is every reset, the watchdog's, a DFU leave. */
extern const unsigned char charge_fb[];		/* hal/common/chargeart.c */
extern const unsigned char font_4x6[95][6];	/* hal/common/font4x6.c */

static void cm_pix(uint8_t *pg, unsigned page, int x, int y, int on)
{
	if ((unsigned) x >= LCD_W || (unsigned) y >> 3 != page) return;
	if (on) pg[x] |= 1u << (y & 7);
	else pg[x] &= ~(1u << (y & 7));
}

static void cm_text(uint8_t *pg, unsigned page, int x, int y, const char *s)
{
	int gx, gy, bits;

	for (; *s; s++, x += 4)
		for (gy = 0; gy < 6; gy++) {
			bits = font_4x6[(unsigned char) *s - 32][gy];
			for (gx = 0; gx < 4; gx++)
				cm_pix(pg, page, x + gx, y + gy, bits & (0x80 >> gx));
		}
}

/* The screen, page by page (128 B on the stack): the picture, the battery
 * on the Flipper's screen in it filled to soc (12 columns; the last one off
 * while charging in the odd phase), the percentage in a box at the top
 * left.  soc < 0: no gauge value, the outline only. */
static void cm_draw(int soc, int charging, int phase)
{
	uint8_t pg[LCD_W], addr[3];
	char txt[5];
	unsigned page;
	int x, y, c, cols, n, w;

	n = 0;
	if (soc >= 100) { txt[n++] = '1'; txt[n++] = '0'; txt[n++] = '0'; }
	else if (soc >= 10) { txt[n++] = '0' + soc / 10; txt[n++] = '0' + soc % 10; }
	else if (soc >= 0) txt[n++] = '0' + soc;
	if (soc >= 0) txt[n++] = '%';
	txt[n] = 0;
	w = n * 4 + 3;
	cols = soc < 0 ? 0 : (soc * 12 + 50) / 100;
	for (page = 0; page < LCD_H / 8; page++) {
		for (x = 0; x < LCD_W; x++) pg[x] = charge_fb[page * LCD_W + x];
		for (y = 46; y <= 49; y++)		/* dark inside ... */
			for (x = 15; x <= 30; x++) cm_pix(pg, page, x, y, 1);
		for (x = 15; x <= 28; x++) {		/* ... the battery light */
			cm_pix(pg, page, x, 46, 0);
			cm_pix(pg, page, x, 49, 0);
		}
		for (y = 46; y <= 49; y++) {
			cm_pix(pg, page, 15, y, 0);
			cm_pix(pg, page, 28, y, 0);
		}
		cm_pix(pg, page, 29, 47, 0);
		cm_pix(pg, page, 29, 48, 0);
		for (c = 0; c < cols; c++) {
			if (charging && soc < 100 && c == cols - 1 && phase)
				continue;
			cm_pix(pg, page, 16 + c, 47, 0);
			cm_pix(pg, page, 16 + c, 48, 0);
		}
		if (soc >= 0) {
			for (y = 1; y <= 9; y++)	/* the box */
				for (x = 1; x <= 1 + w; x++)
					cm_pix(pg, page, x, y,
					    y == 1 || y == 9 || x == 1 || x == 1 + w);
			cm_text(pg, page, 3, 2, txt);
		}
		addr[0] = 0xB0 | page;
		addr[1] = 0x10;
		addr[2] = 0x00;
		lcd_cmds(addr, sizeof addr);
		LCD_DATA(1);
		LCD_CS(1);
		spi2_block(pg, NULL, LCD_W);
		spi2_wait_idle();
		LCD_CS(0);
	}
}

/* The ikurrina sequence of the kernel (red, green, white; 1 s each, 0.2 s
 * glide into the colour) in 40 ms steps, for the loops that run without the
 * kernel's LED task. */
static void cm_led(unsigned n)
{
	static const uint8_t col[3][3] = {
		{ 200, 0, 0 }, { 0, 128, 0 }, { 200, 128, 96 } };
	unsigned c = n / 30 % 3, k = n % 30, p = (c + 2) % 3, i, v[3];

	for (i = 0; i < 3; i++)
		v[i] = k < 5 ? (col[p][i] * (4 - k) + col[c][i] * (k + 1)) / 5
			     : col[c][i];
	hal_led_pwm(v[0], v[1], v[2]);
}

void hal_charge_mode(void)
{
	uint32_t back = 0, t0;
	unsigned n = 0, gone = 0;
	int r, soc;

	if (!(boot_reg() & BOOT_POR)) return;
	boot_reg_set(boot_reg() & ~BOOT_POR);	/* once per start */
	if (hal_buttons() & BTN_BACK) return;	/* a start by Back */
	if ((r = hal_charger_read(HAL_CHG_STATUS)) < 0 || (r >> 5) == 0)
		return;				/* no VBUS */
	hal_backlight(60);			/* dim: the charge is the point */
	hal_led(0, 0, 0);			/* the LED engines off, direct */
	for (;;) {
		hal_wdt_kick();
		t0 = DWT->CYCCNT;
		cm_led(n);
		if (n % 25 == 0) {		/* every second */
			soc = hal_gauge_read(HAL_GAUGE_SOC);
			r = hal_charger_read(HAL_CHG_STATUS);
			cm_draw(soc > 100 ? 100 : soc,
				r >= 0 && ((r >> 3) & 3) >= 1 && ((r >> 3) & 3) <= 2,
				(n / 50) & 1);
		}
		r = hal_charger_read(HAL_CHG_STATUS);
		gone = (r >= 0 && (r >> 5) == 0) ? gone + 1 : 0;
		if (gone >= 12) {			/* the cable is out */
			hal_led(0, 0, 0);
			hal_power_off();
		}
		if (!(hal_buttons() & BTN_BACK)) back = 0;
		else if (back == 0) back = t0 | 1;
		else if (ms_since(back) >= 1000) {
			hal_led(0, 0, 0);
			hal_lcd_flush(charge_fb);	/* no stale level */
			NVIC_SystemReset();		/* a normal start */
		}
		while (ms_since(t0) < 40) ;
		n++;
	}
}

int hal_lcd_save(int on)
{
	/* as u8g2's ST7567 power save (stock firmware's display driver) */
	static const uint8_t save[] = { 0xAE, 0xA5 }, wake[] = { 0xA4, 0xAF };

	if (!(GPIOC->ODR & (1u << 12))) return -1;	/* the SD's transfer */
	if (on) lcd_cmds(save, sizeof save);
	else lcd_cmds(wake, sizeof wake);
	return 0;
}

/*===========================================================================*
 *				I2C1 ("power" bus) and LP5562		     *
 *===========================================================================*/
/* I2C1 on PA9 (SCL) / PA10 (SDA), 100 kHz, as the stock firmware (timing
 * value for a 64 MHz PCLK1).  The bus also carries the charger and fuel
 * gauge.  The LP5562 LED driver runs the display backlight (white channel)
 * and the RGB status LED. */
#define I2C_TIMING_100K	0x10707DBC
#define LP5562_ADDR	0x30		/* 7-bit (0x60 in 8-bit notation) */
#define I2C_TIMEOUT_MS	5

static int i2c_ready;

static void i2c1_init(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_I2C1EN;
	(void) RCC->APB1ENR1;
	gpio_cfg(GPIOA, 9, MODE_AF, PULL_NONE, 4);
	gpio_cfg(GPIOA, 10, MODE_AF, PULL_NONE, 4);
	GPIOA->OTYPER |= (1u << 9) | (1u << 10);		/* open drain */
	I2C1->CR1 = 0;
	I2C1->TIMINGR = I2C_TIMING_100K;
	I2C1->CR1 = I2C_CR1_PE;
	i2c_ready = 1;
}

/* Write len bytes to a device; 0 on success, -1 on NACK or timeout. */
static int i2c1_write(uint8_t addr, const uint8_t *buf, unsigned len)
{
	uint32_t start = DWT->CYCCNT;
	int r = 0;

	if (!i2c_ready) return -1;
	while (I2C1->ISR & I2C_ISR_BUSY)
		if (ms_since(start) > I2C_TIMEOUT_MS) return -1;
	I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
	I2C1->CR2 = ((uint32_t) addr << 1) | (len << I2C_CR2_NBYTES_Pos)
		  | (1u << I2C_CR2_AUTOEND_Pos) | (1u << I2C_CR2_START_Pos);
	while (len > 0) {
		if (I2C1->ISR & I2C_ISR_NACKF) { r = -1; break; }
		if (I2C1->ISR & I2C_ISR_TXIS) {
			I2C1->TXDR = *buf++;
			len--;
			continue;
		}
		if (ms_since(start) > I2C_TIMEOUT_MS) { r = -1; break; }
	}
	while (!(I2C1->ISR & I2C_ISR_STOPF))
		if (ms_since(start) > I2C_TIMEOUT_MS) { r = -1; break; }
	if (I2C1->ISR & I2C_ISR_NACKF) r = -1;
	I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
	return r;
}

/* Read len bytes from register reg: the register written, a repeated
 * start, the bytes.  0 on success, -1 on NACK or timeout. */
static int i2c1_read(uint8_t addr, uint8_t reg, uint8_t *buf, unsigned len)
{
	uint32_t start = DWT->CYCCNT;

	if (!i2c_ready) return -1;
	while (I2C1->ISR & I2C_ISR_BUSY)
		if (ms_since(start) > I2C_TIMEOUT_MS) return -1;
	I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
	I2C1->CR2 = ((uint32_t) addr << 1) | (1u << I2C_CR2_NBYTES_Pos)
		  | I2C_CR2_START;			/* no AUTOEND: TC */
	while (!(I2C1->ISR & (I2C_ISR_TXIS | I2C_ISR_NACKF)))
		if (ms_since(start) > I2C_TIMEOUT_MS) goto fail;
	if (I2C1->ISR & I2C_ISR_NACKF) goto fail;
	I2C1->TXDR = reg;
	while (!(I2C1->ISR & (I2C_ISR_TC | I2C_ISR_NACKF)))
		if (ms_since(start) > I2C_TIMEOUT_MS) goto fail;
	if (I2C1->ISR & I2C_ISR_NACKF) goto fail;
	I2C1->CR2 = ((uint32_t) addr << 1) | I2C_CR2_RD_WRN
		  | (len << I2C_CR2_NBYTES_Pos) | I2C_CR2_AUTOEND | I2C_CR2_START;
	while (len > 0) {
		if (I2C1->ISR & I2C_ISR_RXNE) {
			*buf++ = I2C1->RXDR;
			len--;
			continue;
		}
		if ((I2C1->ISR & I2C_ISR_NACKF)
				|| ms_since(start) > I2C_TIMEOUT_MS) goto fail;
	}
	while (!(I2C1->ISR & I2C_ISR_STOPF))
		if (ms_since(start) > I2C_TIMEOUT_MS) goto fail;
	I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
	return 0;
fail:
	if (!(I2C1->ISR & I2C_ISR_STOPF)) I2C1->CR2 |= I2C_CR2_STOP;
	start = DWT->CYCCNT;
	while (!(I2C1->ISR & I2C_ISR_STOPF) && ms_since(start) <= 1) ;
	I2C1->ICR = I2C_ICR_STOPCF | I2C_ICR_NACKCF;
	return -1;
}

/* The BQ27220 fuel gauge (T15a): standard commands only, read only.  Its
 * data memory is flash, and the stock firmware configured it: nothing is
 * ever written to it here. */
#define BQ27220_ADDR	0x55

int hal_gauge_read(unsigned cmd)
{
	uint8_t b[2];

	if (i2c1_read(BQ27220_ADDR, (uint8_t) cmd, b, 2) < 0) return -1;
	return b[0] | b[1] << 8;
}

/* The charger (BQ25896, T14c): read its registers; at boot only its ADC is
 * set to convert on demand (the stock firmware leaves it converting every
 * second, which the battery pays for while we sleep).  Charge currents,
 * voltages, ICO and the watchdog stay as the stock firmware set them. */
#define BQ25896_ADDR	0x6B

int hal_charger_read(unsigned reg)
{
	uint8_t r;

	return i2c1_read(BQ25896_ADDR, (uint8_t) reg, &r, 1) < 0 ? -1 : r;
}

/* Read-modify-write of charger register reg: (old & ~mask) | val. */
static int charger_rmw(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t b[2];

	if (i2c1_read(BQ25896_ADDR, reg, &b[1], 1) < 0) return -1;
	if ((b[1] & mask) == val) return 0;
	b[0] = reg;
	b[1] = (uint8_t) ((b[1] & ~mask) | val);
	return i2c1_write(BQ25896_ADDR, b, 2);
}

/* R6.1 (owner 2026-11-15): the battery profile and gentle charging, kept
 * in RTC backup register 14 (magic 0xC in the top nibble; bit 0 care,
 * bit 1 gentle).  Only the end-of-charge voltage and the charge current
 * change, and never above the stock values: VREG 4.208 V (full) or 4.000
 * V (care); ICHG 2048 mA (the charger's default, which the stock firmware
 * keeps) or 1024 mA (gentle).  Nothing is written while the register was
 * never set: the charger stays as the stock firmware left it. */
#define CHG_REG		(RTC->BKP14R)
#define CHG_MAGIC	0xC0000000u
#define VOL_SET		0x00010000u	/* bits 8-16: the default volume */
#define VOL_BITS	0x0001FF00u
#define VREG_FULL	((4208 - 3840) / 16)
#define VREG_CARE	((4000 - 3840) / 16)
#define ICHG_STOCK	(2048 / 64)
#define ICHG_GENTLE	(1024 / 64)

/* The charger's I2C watchdog (REG07 WATCHDOG, 40 s after the charger's
 * own power-on reset) would put REG03/04/06 back to their defaults: the
 * profile, gentle charging and the kernel's temperature pause would be
 * undone 40 s later.  Off, as the stock firmware's bq25896_init does. */
static void charger_nowdog(void)
{
	(void) charger_rmw(0x07, 0x30, 0);
}

static void charger_apply(uint32_t s)
{
	charger_nowdog();
	(void) charger_rmw(0x06, 0xFC, (s & 1 ? VREG_CARE : VREG_FULL) << 2);
	(void) charger_rmw(0x04, 0x7F, s & 2 ? ICHG_GENTLE : ICHG_STOCK);
}

int hal_charger_set(int what, int on)
{
	uint32_t s;

	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	s = (CHG_REG & 0xF0000000u) == CHG_MAGIC ? CHG_REG & 3 : 0;
	switch (what) {
	case HAL_CHG_CARE:	s = on ? s | 1 : s & ~1u; break;
	case HAL_CHG_GENTLE:	s = on ? s | 2 : s & ~2u; break;
	case HAL_CHG_OTG:	/* 5 V on the header; not kept over a reset */
		charger_nowdog();	/* a watchdog reset turns OTG on */
		return charger_rmw(0x03, 0x20, on ? 0x20 : 0);
	case HAL_CHG_PAUSE:	/* the kernel's temperature rule */
		charger_nowdog();
		return charger_rmw(0x03, 0x10, on ? 0 : 0x10);
	default:		return -1;
	}
	bkp_set(&CHG_REG, CHG_MAGIC | (CHG_REG & VOL_BITS) | s);
	charger_apply(s);
	return 0;
}

/* The system's default speaker volume (K.7 Sound), 0-100, in bits 8-15 of
 * the same register with bit 16 saying it was set (the charging bits and
 * their magic untouched): survives a reset, 0 B of RAM.  Never set: 100,
 * as before. */
int hal_vol_default(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	return (CHG_REG & VOL_SET) ? (int) (CHG_REG >> 8 & 0xFF) : 100;
}

void hal_vol_default_set(int v)
{
	if (v < 0) v = 0;
	if (v > 100) v = 100;
	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	bkp_set(&CHG_REG, (CHG_REG & ~VOL_BITS) | VOL_SET | (uint32_t) v << 8);
}

static void charger_init(void)
{
	uint8_t b[2];

	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	charger_nowdog();
	if ((CHG_REG & 0xF0000000u) == CHG_MAGIC) charger_apply(CHG_REG & 3);
	if (i2c1_read(BQ25896_ADDR, 0x02, &b[1], 1) < 0) return;
	if (!(b[1] & 0x40)) return;		/* CONV_RATE: continuous */
	b[0] = 0x02;
	b[1] &= ~0xC0;				/* CONV_RATE, CONV_START off */
	i2c1_write(BQ25896_ADDR, b, 2);
}

static int lp5562_write(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };

	return i2c1_write(LP5562_ADDR, b, 2);
}

/* Change only some bits: engine 1 belongs to the backlight's fade, engine
 * 2 to the LED patterns (R7b.5); each leaves the other's bits alone. */
static void lp5562_rmw(uint8_t reg, uint8_t mask, uint8_t val)
{
	uint8_t v;

	if (i2c1_read(LP5562_ADDR, reg, &v, 1) < 0) return;
	if ((v & mask) != val) lp5562_write(reg, (uint8_t) ((v & ~mask) | val));
}

/* Register sequence from the stock firmware (lib/drivers/lp5562.c). */
static int lp5562_init(void)
{
	if (lp5562_write(0x0D, 0xFF) < 0) return -1;	/* reset */
	lp5562_write(0x08, 0x61);	/* config: int. clock, power save, PWM HF */
	lp5562_write(0x70, 0x00);	/* LED map: all channels under I2C control */
	lp5562_write(0x00, 0xC0);	/* enable: CHIP_EN, LOG_EN */
	hal_delay_ms(1);		/* > 488 us after writing register 0 */
	lp5562_write(0x0F, 150);	/* white (backlight) current: 15 mA */
	lp5562_write(0x07, 20);		/* red, green, blue: 2 mA */
	lp5562_write(0x06, 20);
	lp5562_write(0x05, 20);
	return 0;
}

void hal_backlight(unsigned level)
{
	/* at once: the white channel back under I2C control (a fade may have
	 * left it with engine 1), engine 1 stopped, then the level; engine 2
	 * (an LED pattern) runs on */
	lp5562_rmw(0x70, 0xC0, 0x00);	/* LED map: white direct */
	lp5562_rmw(0x01, 0x30, 0x00);	/* op mode: engine 1 disabled */
	lp5562_rmw(0x00, 0xF0, 0xC0);	/* enable: CHIP_EN, LOG_EN, eng 1 hold */
	lp5562_write(0x0E, level > 255 ? 255 : level);
}

/* Fade the backlight down from `from` to `to` in about `ms` milliseconds,
 * done by the LP5562's engine 1 on its own: no CPU, no I2C while it runs;
 * the engine holds the end value.  The program and register sequence
 * follow the stock firmware (lib/drivers/lp5562.c, lp5562_execute_ramp;
 * GPL-3): set PWM, ramp down half the way, loop once more, end.
 * hal_backlight() takes it back. */
void hal_backlight_fade(unsigned from, unsigned to, unsigned ms)
{
	uint8_t prog[1 + 32];
	uint16_t w[4];
	unsigned diff, step, pre = 0, i;
	uint32_t t0;

	if (from > 255) from = 255;
	if (to >= from) { hal_backlight(to); return; }
	diff = from - to;
	if (diff < 2) { hal_backlight(to); return; }
	step = ms * 2 / diff;		/* 0.49 ms units (stock arithmetic) */
	if (step > 0x3F) { step /= 32; pre = 1; }	/* 15.6 ms units */
	if (step == 0) step = 1;
	if (step > 0x3F) step = 0x3F;
	w[0] = 0x4000 | from;				/* set PWM */
	w[1] = (pre << 14) | (step << 8) | 0x80 | ((diff / 2) & 0x7F);	/* down */
	w[2] = 0xA001 | ((2 - 1) << 7);		/* loop to 1, twice */
	w[3] = 0xC000;				/* end */

	lp5562_rmw(0x70, 0xC0, 0x00);	/* white direct while loading */
	lp5562_rmw(0x01, 0x30, 0x10);	/* engine 1: load program */
	t0 = DWT->CYCCNT;
	while (DWT->CYCCNT - t0 < hal_cpu_hz() / 10000) ;	/* 100 us */
	prog[0] = 0x10;			/* engine 1's program memory */
	for (i = 0; i < 16; i++) {	/* big-endian words, the rest zero */
		prog[1 + 2 * i] = i < 4 ? w[i] >> 8 : 0;
		prog[2 + 2 * i] = i < 4 ? w[i] & 0xFF : 0;
	}
	(void) i2c1_write(LP5562_ADDR, prog, sizeof prog);
	lp5562_rmw(0x01, 0x30, 0x20);	/* engine 1: run */
	lp5562_rmw(0x00, 0xF0, 0xC0 | 0x20);	/* enable: engine 1 runs */
	lp5562_rmw(0x70, 0xC0, 0x40);	/* white from engine 1 */
	lp5562_write(0x0E, to);		/* the direct value afterwards */
}

#define LP_CUR		20			/* stock RGB current, 0.1 mA */
static void lp5562_cur(unsigned r, unsigned g, unsigned b);

void hal_led(unsigned r, unsigned g, unsigned b)
{
	lp5562_rmw(0x70, 0x3F, 0x00);	/* R, G, B direct: a pattern stops */
	lp5562_rmw(0x01, 0x0F, 0x00);	/* engines 2 and 3 disabled */
	lp5562_rmw(0x00, 0x0F, 0x00);
	lp5562_cur(LP_CUR, LP_CUR, LP_CUR);	/* as a pattern may have left */
	hal_led_pwm(r, g, b);
}

void hal_led_pwm(unsigned r, unsigned g, unsigned b)
{
	lp5562_write(0x04, r);
	lp5562_write(0x03, g);
	lp5562_write(0x02, b);
}

/* R7b.5: LED patterns run by the LP5562's engines 2 and 3 on their own
 * (the CPU loads them once and may sleep; engine 1 is the backlight's
 * fade).  An engine gives one PWM level to all the channels mapped to it,
 * so a colour gets one engine per level, both started by one write and
 * timed alike so they never drift apart.  Mixing by the channels'
 * currents does not work: at 0.1-0.3 mA the green LED is far too bright
 * against the red (seen on the device), so the current only trims a
 * third channel that shares an engine with a close level (white).
 * Instructions as the datasheet's (the stock fade's encoding): set PWM
 * 0x40nn, wait/ramp (prescale 15.6 or 0.49 ms, step time, sign, count),
 * branch 0xA000 (loop count 0: for ever). */
#define LP_WAIT(n)	(0x4000 | (n) << 8)	/* n x 15.6 ms, n 1..63 */
#define LP_SET(v)	(0x4000 | (v))
#define LP_RAMP(t, up, n) ((t) << 8 | ((up) ? 0 : 0x80) | (n))	/* x 0.49 ms */
#define LP_LOOP		0xA000			/* to step 0, for ever */
#define LP_PEAK		0xFFFF			/* in lp_pat: set PWM to the level */
#define LP_PULSE	0xFFFE			/* in lp_pat: see lp_load() */
#define LP_WAITF(n)	((n) << 8)		/* n x 0.49 ms, n 1..63 */
static const struct { char name[8]; uint16_t w[12]; } lp_pat[] = {
	{ "blink",  { LP_PEAK, LP_WAIT(6), LP_SET(0), LP_WAIT(63), LP_LOOP } },
	{ "fast",   { LP_PEAK, LP_WAIT(6), LP_SET(0), LP_WAIT(6), LP_LOOP } },
	{ "double", { LP_PEAK, LP_WAIT(5), LP_SET(0), LP_WAIT(8),
		      LP_PEAK, LP_WAIT(5), LP_SET(0), LP_WAIT(63),
		      LP_WAIT(40), LP_LOOP } },
	{ "pulse",  { LP_PULSE } },		/* built by lp_load() */
};
/* White is 200:128:96 in PWM (seen on the device); #rrggbb is
 * gamma-coded (light ~ v squared). */
static const uint8_t lp_white[3] = { 200, 128, 96 };

static void lp5562_cur(unsigned r, unsigned g, unsigned b)
{
	lp5562_rmw(0x07, 0xFF, (uint8_t) r);
	lp5562_rmw(0x06, 0xFF, (uint8_t) g);
	lp5562_rmw(0x05, 0xFF, (uint8_t) b);
}

/* Load pattern k at level pwm into the engine program memory at addr; top
 * is the highest level of the colour.  The PWM scale is logarithmic
 * (LOG_EN, as stock), so a colour stays the same colour while it fades
 * only if the engines keep the same *difference* of levels: a pulse
 * rises and falls one level per 1.96 ms on every engine, and a lower one
 * starts later and ends sooner by its difference.  Every engine runs the
 * same number of instructions, so they never drift apart. */
static void lp_load(uint8_t addr, unsigned k, unsigned pwm, unsigned top)
{
	uint8_t prog[1 + 32];
	uint16_t w[16];
	unsigned i, u, q;

	for (i = 0; i < 16; i++) w[i] = i < 12 ? lp_pat[k].w[i] : 0;
	if (w[0] == LP_PULSE) {
		/* the wait before and after: 4 (top - pwm) + 64 x 0.49 ms, as
		 * q x 15.6 ms (q >= 1) and 1..32 x 0.49 ms */
		u = 4 * (top - pwm) + 64;
		q = (u - 1) / 32;
		u -= 32 * q;
		i = 0;
		w[i++] = LP_SET(0);
		w[i++] = LP_WAIT(q);
		w[i++] = LP_WAITF(u);
		w[i++] = LP_RAMP(4, 1, pwm / 2);
		w[i++] = LP_RAMP(4, 1, pwm - pwm / 2);
		w[i++] = LP_RAMP(4, 0, pwm - pwm / 2);
		w[i++] = LP_RAMP(4, 0, pwm / 2);
		w[i++] = LP_WAIT(q);
		w[i++] = LP_WAITF(u);
		w[i++] = LP_WAIT(50);
		w[i++] = LP_LOOP;
		while (i < 16) w[i++] = 0;
	}
	prog[0] = addr;
	for (i = 0; i < 16; i++) {
		if (w[i] == LP_PEAK) w[i] = LP_SET(pwm);
		prog[1 + 2 * i] = (uint8_t) (w[i] >> 8);
		prog[2 + 2 * i] = (uint8_t) w[i];
	}
	(void) i2c1_write(LP5562_ADDR, prog, sizeof prog);
}

int hal_led_pattern(const char *name, unsigned long rgb)
{
	unsigned i, k, p[3], cur[3], eng[3], lvl[2], ne = 0, hi, mid, lo;
	uint8_t map = 0;

	for (k = 0; k < sizeof lp_pat / sizeof lp_pat[0]; k++) {
		for (i = 0; name[i] == lp_pat[k].name[i] && name[i]; i++) ;
		if (name[i] == lp_pat[k].name[i]) break;
	}
	if (k == sizeof lp_pat / sizeof lp_pat[0]) return -1;
	for (i = 0; i < 3; i++) {	/* red, green, blue: PWM 0..200 */
		unsigned long v = rgb >> (16 - 8 * i) & 0xFF;

		p[i] = (unsigned) ((v * v * lp_white[i] + 32512) / 65025);
		cur[i] = LP_CUR;
		eng[i] = 2;			/* none */
	}
	if ((p[0] | p[1] | p[2]) == 0) return -1;
	/* three different levels: the middle one shares the engine of its
	 * nearer neighbour (by ratio) and is trimmed by its current */
	if (p[0] && p[1] && p[2] && p[0] != p[1] && p[1] != p[2]
							&& p[0] != p[2]) {
		hi = p[0] > p[1] ? 0 : 1;
		if (p[2] > p[hi]) hi = 2;
		lo = p[0] < p[1] ? 0 : 1;
		if (p[2] < p[lo]) lo = 2;
		mid = 3 - hi - lo;
		if (p[hi] * p[lo] < p[mid] * p[mid]) {	/* nearer to hi */
			cur[mid] = (LP_CUR * p[mid] + p[hi] / 2) / p[hi];
			p[mid] = p[hi];
		} else {
			cur[lo] = (LP_CUR * p[lo] + p[mid] / 2) / p[mid];
			p[lo] = p[mid];
		}
	}
	for (i = 0; i < 3; i++) {	/* one engine per level */
		if (p[i] == 0) continue;
		if (ne > 0 && lvl[0] == p[i]) eng[i] = 0;
		else if (ne > 1 && lvl[1] == p[i]) eng[i] = 1;
		else { lvl[ne] = p[i]; eng[i] = ne++; }
	}
	hal_led(0, 0, 0);			/* stops a running one */
	lp5562_rmw(0x01, 0x0F, ne > 1 ? 0x05 : 0x04);	/* load */
	hal_ow_delay(100);
	if (ne < 2) lvl[1] = 0;
	for (i = 0; i < ne; i++)		/* a pulse's ramps: >= 1 each */
		if (lvl[i] < 2) lvl[i] = 2;
	hi = lvl[0] > lvl[1] ? lvl[0] : lvl[1];
	lp_load(0x30, k, lvl[0], hi);		/* engine 2 */
	if (ne > 1) lp_load(0x50, k, lvl[1], hi);	/* engine 3 */
	lp5562_cur(cur[0], cur[1], cur[2]);
	for (i = 0; i < 3; i++)		/* engine 2: 10, engine 3: 11 */
		if (eng[i] < 2) map |= (2 + eng[i]) << (4 - 2 * i);
	lp5562_rmw(0x01, 0x0F, ne > 1 ? 0x0A : 0x08);	/* run */
	lp5562_rmw(0x00, 0x0F, ne > 1 ? 0x0A : 0x08);	/* together */
	lp5562_rmw(0x70, 0x3F, map);
	return 0;
}

void hal_led_get(unsigned char rgb[3])
{
	if (i2c1_read(LP5562_ADDR, 0x04, &rgb[0], 1) < 0) rgb[0] = 0;
	if (i2c1_read(LP5562_ADDR, 0x03, &rgb[1], 1) < 0) rgb[1] = 0;
	if (i2c1_read(LP5562_ADDR, 0x02, &rgb[2], 1) < 0) rgb[2] = 0;
}

/*===========================================================================*
 *				microSD card (SPI mode)			     *
 *===========================================================================*/
/* SD Physical Layer Simplified Specification, SPI mode.  Polled, one block
 * per command (CMD17/CMD24); SDSC and SDHC/SDXC cards. */
#define CMD0	0	/* GO_IDLE_STATE */
#define CMD8	8	/* SEND_IF_COND */
#define CMD9	9	/* SEND_CSD */
#define CMD10	10	/* SEND_CID */
#define CMD16	16	/* SET_BLOCKLEN */
#define CMD12	12	/* STOP_TRANSMISSION */
#define CMD17	17	/* READ_SINGLE_BLOCK */
#define CMD18	18	/* READ_MULTIPLE_BLOCK */
#define CMD24	24	/* WRITE_BLOCK */
#define CMD25	25	/* WRITE_MULTIPLE_BLOCK */
#define TOKEN_MULTI	0xFC	/* data token for CMD25 */
#define TOKEN_STOP	0xFD	/* ends a CMD25 transfer */
#define CMD13	13	/* SEND_STATUS; after CMD55: SD_STATUS (ACMD13) */
#define CMD32	32	/* ERASE_WR_BLK_START_ADDR */
#define CMD33	33	/* ERASE_WR_BLK_END_ADDR */
#define CMD38	38	/* ERASE; argument 1: DISCARD (SD 5.1) */
#define CMD55	55	/* APP_CMD */
#define CMD58	58	/* READ_OCR */
#define ACMD41	41	/* SD_SEND_OP_COND */
#define R1_IDLE		0x01
#define R1_ILLEGAL	0x04
#define TOKEN_START	0xFE
#define SD_TIMEOUT_MS	500
#define SD_READ_MS	250	/* data token after a read command: the SD spec
				 * allows 100 ms; this is the longest stretch
				 * the disk task holds the CPU (security review
				 * 2026-11-16, K3) */

#define CMD59	59	/* CRC_ON_OFF */

static int sd_ready;		/* initialised */
static unsigned sd_fast = SPI_DIV_SD_FAST;	/* lowered after CRC errors */

/* CRCs are on (CMD59): commands carry a CRC7, data blocks a CRC16, so a
 * transfer garbled on the bus is refused by the card (writes) or noticed
 * here (reads) instead of reaching the file system. */
static uint8_t crc7(const uint8_t *p, unsigned n)
{
	unsigned crc = 0, i;

	while (n-- > 0) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x80) ? (crc << 1) ^ (0x09 << 1) : crc << 1;
	}
	return (uint8_t) (crc | 1);		/* CRC7 << 1, end bit */
}

/* The STM32WB's CRC unit (R4b.9): CRC-16/XMODEM in hardware, a 512 B
 * sector in ~150 cycles instead of ~10 000.  Words go in big-endian (the
 * unit takes the MSB first).  Used once hal_disk_init() checked it on a
 * test vector (SD_HWCRC in sd_ready); only the disk task calls it, and
 * tasks do not preempt each other, so nobody shares the unit. */
#define SD_HWCRC	2		/* in sd_ready: the CRC unit is in use */
#define SD_DISCARD	4		/* in sd_ready: the card takes DISCARD */
static uint16_t crc16(const uint8_t *p, unsigned n);

static uint16_t crc16_hw(uint16_t crc, const uint8_t *p, unsigned n)
{
	CRC->INIT = crc;
	CRC->CR = CRC_CR_POLYSIZE_0 | CRC_CR_RESET;	/* 16 bits, as is */
	if (((uint32_t) p & 3) == 0)
		for (; n >= 4; n -= 4, p += 4)
			CRC->DR = __REV(*(const uint32_t *) p);
	for (; n > 0; n--)
		*(volatile uint8_t *) &CRC->DR = *p++;
	return (uint16_t) CRC->DR;
}

static int crc16_hw_init(void)
{
	/* aligned: the word path, the byte tail, and a continuation */
	static const union { uint32_t w[3]; char b[12]; } v = { .b = "123456789" };
	const uint8_t *b = (const uint8_t *) v.b;

	RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
	(void) RCC->AHB1ENR;
	CRC->POL = 0x1021;
	return crc16_hw(0, b, 9) == 0x31C3
		&& crc16_hw(crc16_hw(0, b, 4), b + 4, 5) == 0x31C3
		&& crc16_hw(0, b + 1, 3) == crc16(b + 1, 3);
}

static uint16_t crc16_more(uint16_t crc, const uint8_t *p, unsigned n)
{
	static const uint16_t t[16] = {
		0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
		0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
	};

	if (sd_ready & SD_HWCRC) return crc16_hw(crc, p, n);
	while (n-- > 0) {
		crc = (uint16_t) ((crc << 4) ^ t[(crc >> 12) ^ (*p >> 4)]);
		crc = (uint16_t) ((crc << 4) ^ t[(crc >> 12) ^ (*p++ & 0x0F)]);
	}
	return crc;
}

static uint16_t crc16(const uint8_t *p, unsigned n)	/* CCITT, 0x1021 */
{
	return crc16_more(0, p, n);
}

static int sd_block_addr;	/* SDHC/SDXC: block (not byte) addresses */
static uint32_t sd_nsectors;
uint32_t hal_disk_errors;	/* retried transfers, for the log */
unsigned hal_disk_mhz = 16;	/* SPI clock for data */

static void sd_select(int on)
{
	SD_CS(on);
	spi2_xfer(0xFF);	/* give the card clocks to release DO */
}

/* While the card is busy (programming after a write: up to ~9 ms
 * measured, see PORTING.md R0.8b) it is deselected now and then - it goes
 * on programming (SD spec, SPI mode) - and the other kernel tasks run
 * (hal_disk_yield, the disk task's task_yield; they may use SPI2 for the
 * LCD meanwhile, so the speed in use is set again).  Only called by the disk
 * task. */
static int sd_wait_ready(void)
{
	uint32_t start = DWT->CYCCNT;
	unsigned n = 0;
	/* the speed in use: also the slow one while the card is initialised */
	unsigned code = (SPI2->CR1 & SPI_CR1_BR) >> SPI_CR1_BR_Pos;

	while (spi2_xfer(0xFF) != 0xFF) {
		if (ms_since(start) > SD_TIMEOUT_MS) return -1;
		if (++n % 64 == 0) {		/* ~40 us of polling */
			sd_select(0);
			hal_disk_yield();
			spi2_speed(code);
			sd_select(1);
		}
	}
	return 0;
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg)
{
	uint8_t r;
	int n;

	if (cmd == ACMD41) {
		r = sd_cmd(CMD55, 0);
		if (r > R1_IDLE) return r;
	}
	sd_select(0);
	sd_select(1);
	if (cmd != CMD0 && sd_wait_ready() < 0) return 0xFF;

	{
		uint8_t f[6];

		f[0] = 0x40 | cmd;
		f[1] = arg >> 24;
		f[2] = arg >> 16;
		f[3] = arg >> 8;
		f[4] = arg;
		f[5] = crc7(f, 5);
		spi2_block(f, NULL, 6);
	}

	for (n = 0; n < 10; n++)	/* R1 within 8 bytes */
		if (!((r = spi2_xfer(0xFF)) & 0x80)) break;
	return r;
}

static int sd_read_data(uint8_t *buf, unsigned len)
{
	uint32_t start = DWT->CYCCNT;
	uint16_t crc;
	uint8_t t;

	while ((t = spi2_xfer(0xFF)) == 0xFF)
		if (ms_since(start) > SD_READ_MS) return -1;
	if (t != TOKEN_START) return -1;
	spi2_dma(NULL, buf, len);
	crc = (uint16_t) (spi2_xfer(0xFF) << 8);
	crc |= spi2_xfer(0xFF);
	return crc == crc16(buf, len) ? 0 : -2;
}

/* R12.40: does the card take DISCARD?  ACMD13 returns the 512-bit SD
 * Status (R2, then a 64-byte data block); DISCARD_SUPPORT is bit 313, in
 * byte 24.  Read 16 bytes at a time through `buf` (the disk task's stack is
 * small), the CRC kept on the way.  1 yes, 0 no or no answer. */
static int sd_discard_probe(uint8_t *buf)
{
	uint32_t start = DWT->CYCCNT;
	uint16_t crc = 0;
	int i, j, yes = 0;
	uint8_t t;

	if (sd_cmd(CMD55, 0) > R1_IDLE || sd_cmd(CMD13, 0) != 0
	    || spi2_xfer(0xFF) != 0) return 0;	/* R2's second byte */
	while ((t = spi2_xfer(0xFF)) == 0xFF)
		if (ms_since(start) > SD_READ_MS) return 0;
	if (t != TOKEN_START) return 0;
	for (i = 0; i < 64; i += 16) {
		for (j = 0; j < 16; j++) buf[j] = spi2_xfer(0xFF);
		crc = crc16_more(crc, buf, 16);
		if (i == 16) yes = (buf[24 - 16] & 0x02) != 0;
	}
	t = spi2_xfer(0xFF);
	return crc == (uint16_t) (t << 8 | spi2_xfer(0xFF)) ? yes : 0;
}

/* The card's power off and on, as stock's furi_hal_sd_init(power_reset):
 * the peripheral 3.3 V switch (PA3, on unless pulled low) off, the bus
 * and card detect low so that the card is not fed through its pins, 250 ms;
 * then all back and 100 ms for the card to start.  The LCD shares SCK and
 * MOSI: no other task may run meanwhile, so the wait is a busy one. */
static void sd_power_cycle(void)
{
	uint32_t start;

	gpio_set(GPIOA, 3, 0);
	gpio_cfg(GPIOA, 3, MODE_OUT, PULL_NONE, 0);
	gpio_set(GPIOD, 1, 0);
	gpio_set(GPIOB, 15, 0);
	gpio_set(GPIOC, 2, 0);
	gpio_set(GPIOC, 10, 0);
	gpio_set(GPIOC, 12, 0);
	gpio_cfg(GPIOD, 1, MODE_OUT, PULL_NONE, 0);
	gpio_cfg(GPIOB, 15, MODE_OUT, PULL_NONE, 0);
	gpio_cfg(GPIOC, 2, MODE_OUT, PULL_NONE, 0);
	gpio_cfg(GPIOC, 10, MODE_OUT, PULL_NONE, 0);
	start = DWT->CYCCNT;
	while (ms_since(start) < 250) ;
	gpio_set(GPIOC, 12, 1);
	gpio_cfg(GPIOD, 1, MODE_AF, PULL_NONE, 5);
	gpio_cfg(GPIOB, 15, MODE_AF, PULL_NONE, 5);
	gpio_cfg(GPIOC, 2, MODE_AF, PULL_UP, 5);
	gpio_cfg(GPIOC, 10, MODE_IN, PULL_UP, 0);
	gpio_cfg(GPIOA, 3, MODE_ANALOG, PULL_NONE, 0);
	start = DWT->CYCCNT;
	while (ms_since(start) < 100) ;
}

/* hal_disk_init returns 0, or minus why it failed: the step (1 CMD0, 2 CMD59,
 * 3 CMD8, 4 ACMD41 timed out, 5 CMD58, 6 CMD16, 7 CMD9/CSD) << 8 and the
 * last R1 the card sent (0xFF: nothing), for the kernel's "no card (init
 * step N, R1 0xNN)". */
#define SD_FAIL(step, r1)	do { why = (step) << 8 | ((r1) & 0xFF); goto fail; } while (0)

int hal_disk_init(void)
{
	uint8_t ocr[4], csd[16];
	uint32_t start, c_size;
	int i, v2, r, why = 1 << 8;

	sd_ready = 0;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	(void) RCC->AHB1ENR;
	DMAMUX1_Channel0->CCR = 8;		/* DMA1 ch 1: SPI2_RX */
	DMAMUX1_Channel1->CCR = 9;		/* DMA1 ch 2: SPI2_TX */
	gpio_cfg(GPIOC, 10, MODE_IN, PULL_UP, 0);	/* card detect */
	spi2_speed(SPI_DIV_SD_INIT);
	/* CMD0 until the card says idle: some cards do not answer the first
	 * one after power-up.  The clocks with CS high again before each
	 * try; 20 tries, about 200 ms.  A card that still says nothing (a
	 * 128 GB SDXC on the bench, after any reset of the CPU alone) gets
	 * what stock gives every card: its power off, then 20 tries more. */
	for (v2 = 0; ; v2++) {
		if (v2 == 20) sd_power_cycle();
		SD_CS(0);
		for (i = 0; i < 10; i++) spi2_xfer(0xFF);	/* >= 74 clocks */
		if ((r = sd_cmd(CMD0, 0)) == R1_IDLE) break;
		if (v2 == 39) SD_FAIL(1, r);
		sd_select(0);
		/* (security review 2026-11-23, K9: the other tasks meanwhile,
		 * as below; an empty slot held the CPU for 40 x 10 ms at
		 * every open) */
		start = DWT->CYCCNT;
		while (ms_since(start) < 10) hal_disk_yield();
		spi2_speed(SPI_DIV_SD_INIT);
	}
	if ((r = sd_cmd(CMD59, 1)) > R1_IDLE) SD_FAIL(2, r);	/* CRCs on */
	v2 = 0;
	if (!((r = sd_cmd(CMD8, 0x1AA)) & R1_ILLEGAL)) {	/* SD v2+ */
		for (i = 0; i < 4; i++) ocr[i] = spi2_xfer(0xFF);
		if (ocr[2] != 0x01 || ocr[3] != 0xAA) SD_FAIL(3, ocr[2] << 4 | ocr[3] & 0x0F);
		v2 = 1;
	}
	start = DWT->CYCCNT;
	while ((r = sd_cmd(ACMD41, v2 ? 0x40000000 : 0)) != 0) {	/* HCS */
		if (ms_since(start) > 1000) SD_FAIL(4, r);
		/* up to a second: the other tasks between tries (K3) */
		sd_select(0);
		hal_disk_yield();
		spi2_speed(SPI_DIV_SD_INIT);
	}

	sd_block_addr = 0;
	if (v2) {
		if ((r = sd_cmd(CMD58, 0)) != 0) SD_FAIL(5, r);
		for (i = 0; i < 4; i++) ocr[i] = spi2_xfer(0xFF);
		sd_block_addr = (ocr[0] & 0x40) != 0;	/* CCS */
	}
	if (!sd_block_addr && (r = sd_cmd(CMD16, 512)) != 0) SD_FAIL(6, r);

	if ((r = sd_cmd(CMD9, 0)) != 0 || sd_read_data(csd, sizeof csd) < 0)
		SD_FAIL(7, r);
	if ((csd[0] >> 6) == 1) {		/* CSD version 2 */
		c_size = ((uint32_t) (csd[7] & 0x3F) << 16)
			| ((uint32_t) csd[8] << 8) | csd[9];
		sd_nsectors = (c_size + 1) * 1024;
	} else {				/* CSD version 1 */
		uint32_t read_bl_len = csd[5] & 0x0F;
		uint32_t c_size_mult = ((csd[9] & 0x03) << 1) | (csd[10] >> 7);
		c_size = ((uint32_t) (csd[6] & 0x03) << 10)
			| ((uint32_t) csd[7] << 2) | (csd[8] >> 6);
		sd_nsectors = (c_size + 1) << (c_size_mult + 2 + read_bl_len - 9);
	}
	/* The CRC unit is CPU2's once CPU2 runs (see hal_cpu2_start) */
	sd_ready = !(PWR->CR4 & PWR_CR4_C2BOOT) && crc16_hw_init()
		 ? 1 | SD_HWCRC : 1;
	if (sd_discard_probe(csd)) sd_ready |= SD_DISCARD;
	sd_select(0);
	spi2_speed(sd_fast);
	return 0;
fail:
	sd_select(0);
	return -why;
}

uint32_t hal_disk_sectors(void)
{
	return sd_ready ? sd_nsectors : 0;
}

/* Spec N.6: the slot's card-detect switch closes to ground with a card in
 * (PC10, pulled up in hal_disk_init; stock furi_hal_sd_is_present).  Its
 * EXTI line belongs to the Up button, so it is read, never interrupts. */
int hal_disk_present(void)
{
	return !(GPIOC->IDR & (1u << 10));
}

/* The card's CID (CMD10), hashed (FNV-1a): the same card has the same
 * manufacturer, product name, serial number and date. */
int hal_disk_id(uint32_t *id)
{
	uint8_t cid[16];
	uint32_t h = 2166136261u;
	int i, r;

	if (!sd_ready) return -1;
	spi2_speed(sd_fast);
	r = sd_cmd(CMD10, 0) == 0 && sd_read_data(cid, sizeof cid) == 0;
	sd_select(0);
	if (!r) return -1;
	for (i = 0; i < 15; i++) h = (h ^ cid[i]) * 16777619u;	/* no CRC7 */
	*id = h;
	return 0;
}

int hal_disk_fake(int state)
{
	(void) state;
	return -1;
}

/* R12.40: tell the card sectors first..last hold nothing (DISCARD: CMD32,
 * CMD33, CMD38 with argument 1).  The card only marks them unused; it is
 * busy meanwhile (R1b), and that wait is not polled in a loop (spec 0.8):
 * the card is deselected and the disk task naps a clock tick at a time
 * (hal_disk_nap), so everyone else runs and the CPU may sleep - which also
 * stops the cycle counter, so the time limit counts naps.  0 done, -1 an
 * error or still busy after SD_DISCARD_NAPS, -2 the card has no DISCARD.
 * last < first only asks (0 or -2). */
#define SD_DISCARD_NAPS	120		/* 2 s of 1/60 s ticks */

int hal_disk_discard(uint32_t first, uint32_t last)
{
	int naps = 0, r = -1;

	if (!(sd_ready & SD_DISCARD)) return -2;
	if (last < first) return 0;		/* only asked: it has DISCARD */
	spi2_speed(sd_fast);
	if (sd_cmd(CMD32, sd_block_addr ? first : first * 512) != 0
	    || sd_cmd(CMD33, sd_block_addr ? last : last * 512) != 0
	    || sd_cmd(CMD38, 1) != 0) goto out;
	while (spi2_xfer(0xFF) != 0xFF) {	/* busy: DO held low */
		if (++naps > SD_DISCARD_NAPS) goto out;
		sd_select(0);
		hal_disk_nap();
		spi2_speed(sd_fast);
		sd_select(1);
	}
	r = 0;
out:
	sd_select(0);
	return r;
}

/* R12.33: ERASE comes with the disk driver work of T18b (CMD38 with
 * argument 0, the card's erase timeout from its SD Status, card-detect
 * checks as N.6).  Until then DIOCERASE answers ENOTTY and /etc/setup goes
 * on without it. */
int hal_disk_erase(uint32_t first, uint32_t last)
{
	(void) first;
	(void) last;
	return -2;
}

/* End a CMD18 transfer: CMD12 goes out while the card is streaming, so
 * no select toggle or ready wait first; a stuff byte, R1, then busy. */
static int sd_stop(void)
{
	uint8_t f[6], r;
	int n;

	f[0] = 0x40 | CMD12;
	f[1] = f[2] = f[3] = f[4] = 0;
	f[5] = crc7(f, 5);
	spi2_block(f, NULL, 6);
	spi2_xfer(0xFF);			/* stuff byte */
	for (n = 0; n < 10; n++)
		if (!((r = spi2_xfer(0xFF)) & 0x80)) break;
	if (sd_wait_ready() < 0) return -1;
	return r == 0 ? 0 : -1;
}

/* A CRC error or timeout is retried; after the second one in a row the bus
 * goes back to 8 MHz for good. */
static void sd_trouble(int *tries)
{
	if (++*tries >= 2 && sd_fast != 2) {
		sd_fast = 2;
		hal_disk_mhz = 8;
	}
	sd_select(0);
	/* The other tasks between tries: a card that never sends its data
	 * held the CPU for 5 tries x 500 ms, the watchdog reset the board
	 * (security review 2026-11-16, K3). */
	hal_disk_yield();
	spi2_speed(sd_fast);
}

/* Bytes off..off+len of one sector, without a sector buffer: the block is
 * clocked through 64 bytes at a time and its CRC checked on the way (the
 * partition table: 66 bytes at 446). */
int hal_disk_read_part(uint32_t sector, unsigned off, unsigned len, void *buf)
{
	uint8_t chunk[64], *out = buf;
	uint32_t start;
	uint16_t crc = 0, got;
	unsigned pos, n, k;
	int tries = 0, r;
	uint8_t t;

	if (!sd_ready || off + len > HAL_SECTOR_SIZE) return -1;
	spi2_speed(sd_fast);
	for (;;) {
		r = -1;
		if (sd_cmd(CMD17, sd_block_addr ? sector : sector * 512) != 0)
			goto bad;
		start = DWT->CYCCNT;
		while ((t = spi2_xfer(0xFF)) == 0xFF)
			if (ms_since(start) > SD_READ_MS) goto bad;
		if (t != TOKEN_START) goto bad;
		for (pos = 0; pos < HAL_SECTOR_SIZE; pos += n) {
			n = HAL_SECTOR_SIZE - pos < sizeof(chunk)
				? HAL_SECTOR_SIZE - pos : sizeof(chunk);
			spi2_block(NULL, chunk, n);
			crc = crc16_more(crc, chunk, n);
			for (k = 0; k < n; k++)
				if (pos + k >= off && pos + k < off + len)
					out[pos + k - off] = chunk[k];
		}
		got = (uint16_t) (spi2_xfer(0xFF) << 8);
		got |= spi2_xfer(0xFF);
		if (got == crc) r = 0;
	bad:
		if (r == 0 || tries >= 4) break;
		hal_disk_errors++;
		crc = 0;
		sd_trouble(&tries);
	}
	sd_select(0);
	return r;
}

int hal_disk_read(uint32_t sector, void *buf, unsigned count)
{
	uint8_t *p = buf;
	int tries = 0;

	if (!sd_ready) return -1;
	spi2_speed(sd_fast);
	while (count > 0) {
		if (count == 1) {
			if (sd_cmd(CMD17, sd_block_addr ? sector : sector * 512)
				!= 0 || sd_read_data(p, HAL_SECTOR_SIZE) < 0)
				goto bad;
			count--;
			break;
		}
		/* Several sectors: one CMD18 saves a command and the card's
		 * access time per sector. */
		if (sd_cmd(CMD18, sd_block_addr ? sector : sector * 512) != 0)
			goto bad;
		while (count > 0) {
			if (sd_read_data(p, HAL_SECTOR_SIZE) < 0) {
				(void) sd_stop();
				goto bad;
			}
			tries = 0;
			count--, sector++, p += HAL_SECTOR_SIZE;
		}
		if (sd_stop() < 0) {	/* the data came through (CRC) */
			hal_disk_errors++;
			sd_select(0);
		}
		break;
	bad:
		hal_disk_errors++;
		if (tries >= 4) break;
		sd_trouble(&tries);
	}
	sd_select(0);
	return count == 0 ? 0 : -1;
}

/* One data block of a write: token, data, CRC16; the card answers 0x05
 * (accepted) or 0x0B (CRC error) and is busy while it programs. */
static int sd_write_data(uint8_t token, const uint8_t *p)
{
	uint16_t crc = crc16(p, HAL_SECTOR_SIZE);
	uint8_t resp;

	spi2_xfer(0xFF);
	spi2_xfer(token);
	spi2_dma(p, NULL, HAL_SECTOR_SIZE);
	spi2_xfer(crc >> 8);
	spi2_xfer(crc & 0xFF);
	resp = spi2_xfer(0xFF) & 0x1F;
	if (sd_wait_ready() < 0 || resp != 0x05) return -1;
	return 0;
}

int hal_disk_write(uint32_t sector, const void *buf, unsigned count)
{
	const uint8_t *p = buf;
	int tries = 0, r;

	if (!sd_ready) return -1;
	spi2_speed(sd_fast);
	while (count > 0) {
		if (count == 1) {
			if (sd_cmd(CMD24, sd_block_addr ? sector : sector * 512)
				!= 0 || sd_write_data(TOKEN_START, p) < 0)
				goto bad;
			count--;
			break;
		}
		/* Several sectors: one CMD25, ended by the stop token. */
		if (sd_cmd(CMD25, sd_block_addr ? sector : sector * 512) != 0)
			goto bad;
		r = 0;
		while (count > 0) {
			if (sd_write_data(TOKEN_MULTI, p) < 0) {
				r = -1;
				break;
			}
			tries = 0;
			count--, sector++, p += HAL_SECTOR_SIZE;
		}
		spi2_xfer(TOKEN_STOP);
		spi2_xfer(0xFF);
		if (sd_wait_ready() < 0) r = -1;
		if (r == 0) break;
		/* The blocks before the bad one were accepted; on with the
		 * rest. */
	bad:
		hal_disk_errors++;
		if (tries >= 4) break;
		sd_trouble(&tries);
	}
	sd_select(0);
	return count == 0 ? 0 : -1;
}

/*===========================================================================*
 *				Buttons					     *
 *===========================================================================*/
#define BTN_LINES	(EXTI_IMR1_IM3 | EXTI_IMR1_IM6 | EXTI_IMR1_IM10 \
			| EXTI_IMR1_IM11 | EXTI_IMR1_IM12 | EXTI_IMR1_IM13)

static void buttons_init(void)
{
	gpio_cfg(GPIOB, 10, MODE_IN, PULL_UP, 0);	/* up */
	gpio_cfg(GPIOC, 6, MODE_IN, PULL_UP, 0);	/* down */
	gpio_cfg(GPIOB, 11, MODE_IN, PULL_UP, 0);	/* left */
	gpio_cfg(GPIOB, 12, MODE_IN, PULL_UP, 0);	/* right */
	gpio_cfg(GPIOH, 3, MODE_IN, PULL_DOWN, 0);	/* ok (active high) */
	gpio_cfg(GPIOC, 13, MODE_IN, PULL_UP, 0);	/* back */

	/* EXTI lines 3, 6, 10-13 on both edges, masked until the kernel
	 * arms them (T14b).  SYSCFG is always clocked on the WB.  Port
	 * codes: B 1, C 2, H 7. */
	SYSCFG->EXTICR[0] = (SYSCFG->EXTICR[0] & ~SYSCFG_EXTICR1_EXTI3)
					| 7u << SYSCFG_EXTICR1_EXTI3_Pos;
	SYSCFG->EXTICR[1] = (SYSCFG->EXTICR[1] & ~SYSCFG_EXTICR2_EXTI6)
					| 2u << SYSCFG_EXTICR2_EXTI6_Pos;
	SYSCFG->EXTICR[2] = (SYSCFG->EXTICR[2] & ~(SYSCFG_EXTICR3_EXTI10
						| SYSCFG_EXTICR3_EXTI11))
			| 1u << SYSCFG_EXTICR3_EXTI10_Pos
			| 1u << SYSCFG_EXTICR3_EXTI11_Pos;
	SYSCFG->EXTICR[3] = (SYSCFG->EXTICR[3] & ~(SYSCFG_EXTICR4_EXTI12
						| SYSCFG_EXTICR4_EXTI13))
			| 1u << SYSCFG_EXTICR4_EXTI12_Pos
			| 2u << SYSCFG_EXTICR4_EXTI13_Pos;
	EXTI->IMR1 &= ~BTN_LINES;
	EXTI->RTSR1 |= BTN_LINES;
	EXTI->FTSR1 |= BTN_LINES;
	EXTI->PR1 = BTN_LINES;
}

int hal_btn_irq(int i)
{
	static const signed char irq[] =
		{ EXTI3_IRQn, EXTI9_5_IRQn, EXTI15_10_IRQn, -1 };

	return irq[(unsigned) i < 3 ? i : 3];
}

/* 1 if a button line has an edge waiting (its vector may be another's:
 * the iButton's line 14 shares EXTI15_10). */
int hal_btn_pending(void)
{
	return (EXTI->PR1 & EXTI->IMR1 & BTN_LINES) != 0;
}

void hal_btn_arm(int on)
{
	if (on) {
		EXTI->PR1 = BTN_LINES;
		EXTI->IMR1 |= BTN_LINES;
	} else {
		EXTI->IMR1 &= ~BTN_LINES;
		EXTI->PR1 = BTN_LINES;
	}
}

/* R6.4: edges of header pins 0 (PA7, EXTI line 7) and 2 (PA4, line 4), the
 * only ones with a line nobody uses (buttons 3, 6, 10-13; IR 0, CC1101 1,
 * NFC 2, iButton 14).  Both edges; they wake the CPU from Stop2.  Line 7
 * shares the EXTI9_5 vector with button line 6, so the kernel's button
 * handler acks these too (hal_gpio_edge_ack). */
#define GP_LINES	(EXTI_IMR1_IM4 | EXTI_IMR1_IM7)

unsigned hal_gpio_edge_pins(void)
{
	return 0x05;			/* pins 0 and 2 */
}

int hal_gpio_edge_irq(void)
{
	return EXTI4_IRQn;		/* line 7: the buttons' EXTI9_5 */
}

int hal_gpio_edge(unsigned pin, int on)
{
	uint32_t line;

	if (pin == 0) line = EXTI_IMR1_IM7;
	else if (pin == 2) line = EXTI_IMR1_IM4;
	else return -1;
	if (on) {			/* (SYSCFG is always clocked on the WB) */
		if (pin == 0)
			SYSCFG->EXTICR[1] &= ~SYSCFG_EXTICR2_EXTI7; /* port A */
		else
			SYSCFG->EXTICR[1] &= ~SYSCFG_EXTICR2_EXTI4;
		EXTI->RTSR1 |= line;
		EXTI->FTSR1 |= line;
		EXTI->PR1 = line;
		EXTI->IMR1 |= line;
	} else {
		EXTI->IMR1 &= ~line;
		EXTI->PR1 = line;
	}
	return 0;
}

unsigned hal_gpio_edge_ack(void)
{
	uint32_t pr = EXTI->PR1 & GP_LINES & EXTI->IMR1;

	if (pr == 0) return 0;
	EXTI->PR1 = pr;
	return (pr & EXTI_IMR1_IM7 ? 1u : 0) | (pr & EXTI_IMR1_IM4 ? 4u : 0);
}

/*===========================================================================*
 *				The ms timer				     *
 *===========================================================================*/
/* LPTIM1 (R4b.2), free running on the 32768 Hz LSE, which the stock
 * firmware keeps running on the battery for the RTC (on the LSI when the
 * LSE is not up: a drained battery), and which keeps counting in Stop2.
 * One compare: the kernel puts its earliest due time there.  A compare
 * write takes effect some LSE cycles later (CMPOK, an interrupt too): the
 * kernel writes the next one only after that, and never waits for it. */
static void mt_init(void)
{
	uint32_t i;

	RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_LPTIM1SEL)
		| ((RCC->BDCR & RCC_BDCR_LSERDY) ? 3u : 1u)
						<< RCC_CCIPR_LPTIM1SEL_Pos;
	if (!(RCC->BDCR & RCC_BDCR_LSERDY)) {
		RCC->CSR |= RCC_CSR_LSI1ON;
		for (i = 0; i < 100000 && !(RCC->CSR & RCC_CSR_LSI1RDY); i++) ;
	}
	RCC->APB1ENR1 |= RCC_APB1ENR1_LPTIM1EN;
	(void) RCC->APB1ENR1;
	LPTIM1->CR = 0;
	LPTIM1->CFGR = 0;			/* internal clock, /1 */
	LPTIM1->IER = LPTIM_IER_CMPMIE | LPTIM_IER_CMPOKIE; /* only when off */
	LPTIM1->CR = LPTIM_CR_ENABLE;
	LPTIM1->ARR = 0xFFFF;
	for (i = 0; i < 100000 && !(LPTIM1->ISR & LPTIM_ISR_ARROK); i++) ;
	LPTIM1->CMP = 0;
	for (i = 0; i < 100000 && !(LPTIM1->ISR & LPTIM_ISR_CMPOK); i++) ;
	LPTIM1->ICR = LPTIM_ICR_ARROKCF | LPTIM_ICR_CMPOKCF | LPTIM_ICR_CMPMCF;
	LPTIM1->CR = LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT;
}

int hal_mt_irq(void)
{
	return LPTIM1_IRQn;
}

unsigned hal_mt_now(void)
{
	uint32_t a, b;

	/* the counter runs on its own clock: two equal reads (RM0434) */
	do { a = LPTIM1->CNT; b = LPTIM1->CNT; } while (a != b);
	return a & 0xFFFF;
}

void hal_mt_set(unsigned due)
{
	LPTIM1->ICR = LPTIM_ICR_CMPOKCF;
	LPTIM1->CMP = due & 0xFFFF;
}

unsigned hal_mt_cmp(void)
{
	return LPTIM1->CMP & 0xFFFF;
}

unsigned hal_mt_ack(void)
{
	uint32_t f = LPTIM1->ISR & (LPTIM_ISR_CMPM | LPTIM_ISR_CMPOK);

	LPTIM1->ICR = f;
	return (f & LPTIM_ISR_CMPM ? HAL_MT_MATCH : 0)
				| (f & LPTIM_ISR_CMPOK ? HAL_MT_READY : 0);
}

int hal_buttons_fake(unsigned mask)
{
	(void) mask;
	return -1;			/* real buttons only */
}

unsigned hal_buttons(void)
{
	unsigned b = 0;

	if (!gpio_get(GPIOB, 10)) b |= BTN_UP;
	if (!gpio_get(GPIOC, 6)) b |= BTN_DOWN;
	if (!gpio_get(GPIOB, 11)) b |= BTN_LEFT;
	if (!gpio_get(GPIOB, 12)) b |= BTN_RIGHT;
	if (gpio_get(GPIOH, 3)) b |= BTN_OK;
	if (!gpio_get(GPIOC, 13)) b |= BTN_BACK;
	return b;
}

/*===========================================================================*
 *				Stop2					     *
 *===========================================================================*/
/* Stop2 (T14b, R6.8) as the stock furi_hal_power_deep_sleep (with CPU2's
 * semaphores once CPU2 was started, see below): onto HSI16, Stop2 for both CPUs,
 * SLEEPDEEP, WFI; the wake-up comes on HSI16 (STOPWUCK), then back to HSE
 * and the PLL (and PLLSAI1, USB's 48 MHz, if it ran).  Stop2 turns HSE and
 * the PLLs off.  The crystal's start-up is waited for here, interrupts
 * masked, before anything else runs: the kernel switches tasks on the way
 * out of an interrupt, and nothing may run on a quarter of the clock.
 * Returns what woke it (HAL_WAKE_*), -1 if the CPU is not on the PLL (no
 * HSE at boot: no Stop2 either). */
int hal_can_stop2(void)
{
	return (RCC->CFGR & RCC_CFGR_SWS) == (3u << RCC_CFGR_SWS_Pos);
}

/* With CPU2 running (R2b.10) the clocks are shared, so as stock's
 * furi_hal_power_deep_sleep: HSEM 3 (RCC) around every clock change, and
 * HSEM 4 (entry to Stop): onto HSI16 only if CPU2 is asleep too (or holds
 * HSEM 4), else stay on the PLL - CPU2 runs on it; CPU2's own low-power
 * mode (C2CR1) is CPU2's.  After the wake-up the PLL comes back only if
 * the system clock really left it. */
#define C2_COREID	(4u << HSEM_R_COREID_Pos)	/* CPU1 */
#define C2_SEM_RCC	3
#define C2_SEM_STOP	4

int hal_stop2(void)
{
	uint32_t sai = RCC->CR & RCC_CR_PLLSAI1ON, n;
	int w, c2 = (PWR->CR4 & PWR_CR4_C2BOOT) != 0, hsi = 1;

	if ((RCC->CFGR & RCC_CFGR_SWS) != (3u << RCC_CFGR_SWS_Pos)) return -1;
	if (c2) {
		for (n = 0; HSEM->RLR[C2_SEM_RCC] != (HSEM_RLR_LOCK | C2_COREID); )
			if (++n == 1000000u) return -1;	/* no Stop2 now */
		if (HSEM->RLR[C2_SEM_STOP] == (HSEM_RLR_LOCK | C2_COREID)) {
			if (PWR->EXTSCR & (PWR_EXTSCR_C2DS | PWR_EXTSCR_C2SBF))
				HSEM->R[C2_SEM_STOP] = C2_COREID;
			else hsi = 0;		/* CPU2 runs on the PLL */
		}
	}
	if (hsi) {
		RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_STOPWUCK
						| (1u << RCC_CFGR_SW_Pos);
		while ((RCC->CFGR & RCC_CFGR_SWS) != (1u << RCC_CFGR_SWS_Pos)) ;
	}
	if (c2) HSEM->R[C2_SEM_RCC] = C2_COREID;
	PWR->CR1 = (PWR->CR1 & ~PWR_CR1_LPMS) | PWR_CR1_LPMS_1;
	if (!c2) PWR->C2CR1 = (PWR->C2CR1 & ~PWR_C2CR1_LPMS) | PWR_C2CR1_LPMS_1;
	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
	__DSB();
	__WFI();
	SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
	/* Stop0 selected again, as stock: a stray deep sleep is no Stop2 */
	PWR->CR1 &= ~PWR_CR1_LPMS;
	if (!c2) PWR->C2CR1 &= ~PWR_C2CR1_LPMS;
	PWR->EXTSCR = PWR_EXTSCR_C1CSSF;
	w = (EXTI->PR1 & BTN_LINES) ? HAL_WAKE_BTN : 0;
	if (LPTIM1->ISR & (LPTIM_ISR_CMPM | LPTIM_ISR_CMPOK)) w |= HAL_WAKE_TIMER;

	if (c2) {
		HSEM->R[C2_SEM_STOP] = C2_COREID;	/* if ours */
		/* bounded: a CPU2 that died holding it must not hang us; the
		 * clock is restored without it then */
		for (n = 0; HSEM->RLR[C2_SEM_RCC] != (HSEM_RLR_LOCK | C2_COREID); )
			if (++n == 1000000u) break;
	}
	if ((RCC->CFGR & RCC_CFGR_SWS) != (3u << RCC_CFGR_SWS_Pos)) {
		RCC->CR |= RCC_CR_HSEON;
		while (!(RCC->CR & RCC_CR_HSERDY)) ;
		RCC->CR |= RCC_CR_PLLON;
		while (!(RCC->CR & RCC_CR_PLLRDY)) ;
		if (sai) {
			RCC->CR |= RCC_CR_PLLSAI1ON;
			while (!(RCC->CR & RCC_CR_PLLSAI1RDY)) ;
		}
		RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (3u << RCC_CFGR_SW_Pos);
		while ((RCC->CFGR & RCC_CFGR_SWS) != (3u << RCC_CFGR_SWS_Pos)) ;
	}
	if (c2) HSEM->R[C2_SEM_RCC] = C2_COREID;
	return w;
}

/*===========================================================================*
 *				DFU entry				     *
 *===========================================================================*/
/* Like the stock firmware: if LEFT is held at reset (Left+Back reset, then
 * release Back), enter ST's ROM USB DFU bootloader so a new image can be
 * flashed.  This runs first thing in Reset_Handler, straight out of reset,
 * so peripherals are in their reset state as the ROM expects, and it works
 * even if the rest of this image is broken.  Runs before .data/.bss setup:
 * no globals here. */
#define SYSMEM_BASE 0x1FFF0000u

/* RTC backup register 19 (kept over resets and, with the battery, power
 * off; stock firmware uses the low registers): "GZ" in the top half, the
 * dev bit, and the reset flags of the last reset (RCC_CSR >> 24).  The
 * dev bit is set by "reboot -d" and cleared by a normal reboot or halt:
 * only on a board being developed does a watchdog reset go to DFU; on a
 * user's board it boots normally (review of #2, R4a.4). */
#define BOOT_REG	(RTC->BKP19R)
#define BOOT_MAGIC	0x475A0000u
#define BOOT_DEV	0x100u
#define BOOT_CAUSE	0xFFu

static uint32_t boot_reg(void)		/* no globals: early boot */
{
	uint32_t r;

	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	r = BOOT_REG;
	return (r & 0xFFFF0000u) == BOOT_MAGIC ? r : BOOT_MAGIC;
}

static void boot_reg_set(uint32_t r)
{
	PWR->CR1 |= PWR_CR1_DBP;		/* backup domain writable */
	BOOT_REG = r;
	(void) BOOT_REG;
	PWR->CR1 &= ~PWR_CR1_DBP;
}

void hal_early_boot(void)
{
	const volatile uint32_t *sysmem = (const volatile uint32_t *) SYSMEM_BASE;
	volatile uint32_t settle;
	uint32_t sp, pc, csr = RCC->CSR, br = boot_reg();
	int left;

	/* Keep the cause of this reset (not after the ROM loader's jump,
	 * which is no reset: its flags were cleared). */
	if ((csr >> 24) != 0) br = (br & ~BOOT_CAUSE) | (csr >> 24);

	RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
	(void) RCC->AHB2ENR;
	GPIOB->PUPDR = (GPIOB->PUPDR & ~(3u << 22)) | (1u << 22);	/* PB11 pull-up */
	GPIOB->MODER &= ~(3u << 22);					/* PB11 input */
	/* The Left button has an RC debounce: after a cold start (the charger's
	 * ship mode, then USB plugged in) its capacitor is empty and the
	 * internal pull-up needs several ms to charge it, so a pin read after
	 * 2000 loops (~2 ms at the reset clock) said 'held' and the start went
	 * to ST's DFU loader: dark, Back did nothing (owner, 2026-11-20).  Held
	 * means low after the settle time too: read it again, ~40 ms on. */
	for (settle = 0; settle < 2000; settle++) ;
	left = !(GPIOB->IDR & (1u << 11));
	if (left) {
		for (settle = 0; settle < 40000; settle++) ;
		left = !(GPIOB->IDR & (1u << 11));
	}

	/* Put PB11 and the GPIOB clock back to their reset state. */
	GPIOB->MODER |= 3u << 22;
	GPIOB->PUPDR &= ~(3u << 22);
	RCC->AHB2ENR &= ~RCC_AHB2ENR_GPIOBEN;

	/* DFU without the buttons: asked for by hal_dfu_request() ("reboot
	 * -d"), or after the watchdog reset a hung or panicked kernel - a
	 * new image that fails comes back to DFU by itself. */
	if (*HAL_BOOT_WORD == HAL_BOOT_DFU) br |= BOOT_DEV;
	if (*HAL_BOOT_WORD == HAL_BOOT_DFU
			|| ((csr & RCC_CSR_IWDGRSTF) && (br & BOOT_DEV))) {
		*HAL_BOOT_WORD = 0;
		left = 1;
	}
	br = (csr & RCC_CSR_BORRSTF) ? br | BOOT_POR : br & ~BOOT_POR;
	boot_reg_set(br);
	if (left) RCC->CSR |= RCC_CSR_RMVF;
	if (!left) {
		/* ST's DFU loader starts the new image ("leave") with a jump,
		 * not a reset, and leaves the flash controller with CFGBSY
		 * stuck: no erase or program until a reset (the flash text
		 * cache would stay off).  Reset once - not again if the last
		 * reset was this one. */
		RCC->CSR = csr | RCC_CSR_RMVF;	/* clear the reset flags */
		if ((FLASH->SR & FLASH_SR_CFGBSY) && !(csr & RCC_CSR_SFTRSTF))
			NVIC_SystemReset();
		return;
	}

	sp = sysmem[0];
	pc = sysmem[1];
	SYSCFG->MEMRMP = (SYSCFG->MEMRMP & ~SYSCFG_MEMRMP_MEM_MODE) | SYSCFG_MEMRMP_MEM_MODE_0;
	SCB->VTOR = SYSMEM_BASE;
	__set_MSP(sp);
	((void (*)(void)) pc)();
	for (;;) ;
}

/*===========================================================================*
 *				Init / halt				     *
 *===========================================================================*/
void hal_init(void)
{
	clock_init();
	cycles_init();

	RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN
		      | RCC_AHB2ENR_GPIOCEN | RCC_AHB2ENR_GPIODEN
		      | RCC_AHB2ENR_GPIOEEN | RCC_AHB2ENR_GPIOHEN;
	(void) RCC->AHB2ENR;

	/* Deselect everything on the shared SPI2 bus before touching it. */
	gpio_set(GPIOC, 11, 1);
	gpio_cfg(GPIOC, 11, MODE_OUT, PULL_NONE, 0);	/* display CS */
	gpio_set(GPIOC, 12, 1);
	gpio_cfg(GPIOC, 12, MODE_OUT, PULL_NONE, 0);	/* SD CS */
	gpio_cfg(GPIOB, 0, MODE_OUT, PULL_NONE, 0);	/* display RST */
	gpio_cfg(GPIOB, 1, MODE_OUT, PULL_NONE, 0);	/* display A0 */
	hal_vibro(0);					/* PA8: motor off */

	SysTick_Config(SystemCoreClock / 1000);
	hal_wdt_start();
	uart_init();
	spi2_init();
	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	if (hal_rtc_ok()) rtc_boot();		/* before the output goes */
	if (RTC->CR & RTC_CR_OSEL) rtc_alarm_out(0);	/* PC13 is Back */
	buttons_init();
	mt_init();
	hal_radios_park();			/* T14a: -1.7 mA and more */
}

void hal_halt(int status)
{
	__disable_irq();
	hal_tone(0, 0);
	/* Keep the USB console alive so a panic message still reaches the
	 * host (polled: interrupts are off).  After a panic the watchdog is
	 * left to run out: the reset takes the board to DFU (hal_early_boot)
	 * once the message had time to get out. */
	for (;;) {
		hal_usb_poll();
		if (status == 0) hal_wdt_kick();
	}
}

/*===========================================================================*
 *				Watchdog, DFU request			     *
 *===========================================================================*/
/* IWDG on the 32 kHz LSI: /64 = 500 Hz, 1000 counts = 2 s (R4a.4).  Kicked
 * by the clock task, which the clock interrupt wakes at least every 32
 * ticks: a kernel task that never gives the CPU back, or an interrupt
 * storm, starves it and the board resets.  hal_halt() keeps kicking after
 * a halt, not after a panic (the reset comes 2 s later).  A system reset
 * stops the watchdog again (software watchdog option). */
void hal_wdt_start(void)
{
	IWDG->KR = 0xCCCC;			/* start (and LSI) */
	IWDG->KR = 0x5555;			/* unlock PR, RLR */
	IWDG->PR = 4;				/* /64 */
	IWDG->RLR = 1000;
	while (IWDG->SR & (IWDG_SR_PVU | IWDG_SR_RVU)) ;
	IWDG->KR = 0xAAAA;
}

void hal_wdt_kick(void)
{
	IWDG->KR = 0xAAAA;
}

void hal_dev_mode(int on)
{
	uint32_t br = boot_reg();

	boot_reg_set(on ? br | BOOT_DEV : br & ~BOOT_DEV);
}

int hal_dev(void)
{
	return (boot_reg() & BOOT_DEV) != 0;
}

/* Flash erase accounting (R2b.11).  Backup registers, from the top (the
 * stock firmware uses the low ones; map in PORTING.md):
 *   BKP18R  0xA magic (4 bits), budget (8), cache pages erased today (8),
 *           the day (12 bits: RTC date, or days of uptime without an RTC)
 *   BKP17R  cache pages erased in all
 *   BKP16R  installed-region pages erased in all
 * In the battery domain: a reboot loop cannot reset today's budget. */
#define FC_REG		(RTC->BKP18R)
#define FC_MAGIC	0xA0000000u

static unsigned fc_day(unsigned upday)
{
	uint32_t tr, dr;

	if ((RCC->BDCR & RCC_BDCR_RTCEN) && (RTC->ISR & RTC_ISR_INITS)) {
		tr = RTC->TR;			/* locks DR until it is read */
		dr = RTC->DR;
		(void) tr;
		return (((dr >> 20 & 15) * 10 + (dr >> 16 & 15)) * 372
			+ ((dr >> 12 & 1) * 10 + (dr >> 8 & 15)) * 31
			+ (dr >> 4 & 3) * 10 + (dr & 15)) & 0xFFF;
	}
	return upday & 0xFFF;
}

static uint32_t fc_reg(unsigned upday)	/* today's word, renewed daily */
{
	uint32_t r = FC_REG, day = fc_day(upday);

	if ((r & 0xF0000000u) != FC_MAGIC)
		r = FC_MAGIC | (uint32_t) HAL_FC_BUDGET << 20 | day;
	if ((r & 0xFFF) != day) r = (r & 0xFFF00000u) | day;	/* a new day */
	return r;
}

static void bkp_set(volatile uint32_t *reg, uint32_t v)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	PWR->CR1 |= PWR_CR1_DBP;		/* backup domain writable */
	*reg = v;
	(void) *reg;
	PWR->CR1 &= ~PWR_CR1_DBP;
}

/* The RTC's alarm output on PC13 (Back, also the charger's QON): Alarm A,
 * open drain, active low, as stock furi_hal_rtc_set_alarm_out.  On only
 * for power-off (stock: "shutting down with ext 3V3 off is causing LSE to
 * stop"; and the alarm that starts the Flipper again, T14c part 2); at
 * boot it goes off, or Back could not be read. */
static void rtc_alarm_out(int on)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	PWR->CR1 |= PWR_CR1_DBP;
	RTC->WPR = 0xCA;
	RTC->WPR = 0x53;
	RTC->CR = (RTC->CR & ~RTC_CR_OSEL) | RTC_CR_POL
						| (on ? RTC_CR_OSEL_0 : 0);
	RTC->OR &= ~RTC_OR_ALARMOUTTYPE;		/* open drain */
	RTC->WPR = 0xFF;
	PWR->CR1 &= ~PWR_CR1_DBP;
}

void hal_power_off(void)
{
	uint8_t b[2];
	int r;

	__disable_irq();
	hal_tone(0, 0);
	/* On USB the Flipper stays powered (below): the screen stays as the
	 * kernel left it (flipdev_off_notice: the boot screen and 'You can now
	 * unplug the USB cable'), so that one sees the kernel is alive and
	 * that Back starts it again (the owner, 2026-11-20).  On the battery
	 * it is off in a moment: dark at once. */
	if ((r = hal_charger_read(0x0B)) >= 0 && (r >> 5) != 0)
		;			/* the screen stays as the kernel left it */
	else {
		hal_backlight(0);
		{
			static const uint8_t off[] = { 0xAE, 0xA5 }; /* LCD sleeps */

			if (GPIOC->ODR & (1u << 12)) lcd_cmds(off, sizeof off);
		}
	}
	rtc_off();				/* the alarm, early by the lead */
	rtc_alarm_out(1);
	hal_vibro(1);				/* as stock: a short buzz */
	hal_delay_ms(50);
	if (i2c1_read(BQ25896_ADDR, 0x09, &b[1], 1) == 0) {
		b[0] = 0x09;
		b[1] |= 0x20;			/* BATFET_DIS: ship mode */
		i2c1_write(BQ25896_ADDR, b, 2);
	}
	hal_vibro(0);
	{	/* on USB: powered until the cable goes.  Back held for a second
		 * starts the system again (the owner, 2026-11-20: it stayed dark
		 * and Back did nothing; only Left + Back, a reset, woke it). */
		uint32_t back = 0;

		for (;;) {
			hal_usb_poll();
			hal_wdt_kick();
			if (!(hal_buttons() & BTN_BACK)) back = 0;
			else if (back == 0) back = DWT->CYCCNT | 1;
			else if (ms_since(back) >= 1000) NVIC_SystemReset();
		}
	}
}

int hal_fc_erase_ok(int cache, unsigned upday)
{
	uint32_t r;

	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	if (!cache) {
		bkp_set(&RTC->BKP16R, RTC->BKP16R + 1);
		return 1;
	}
	r = fc_reg(upday);
	if ((r >> 12 & 0xFF) >= (r >> 20 & 0xFF)) {	/* spent */
		if (r != FC_REG) bkp_set(&FC_REG, r);
		return 0;
	}
	bkp_set(&FC_REG, r + (1u << 12));
	bkp_set(&RTC->BKP17R, RTC->BKP17R + 1);
	return 1;
}

void hal_fc_counts(uint32_t *c, unsigned upday)
{
	uint32_t r;

	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	r = fc_reg(upday);
	c[0] = RTC->BKP17R;
	c[1] = RTC->BKP16R;
	c[2] = r >> 12 & 0xFF;
	c[3] = r >> 20 & 0xFF;
}

void hal_fc_budget(unsigned pages, unsigned upday)
{
	uint32_t r;

	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	r = fc_reg(upday);
	bkp_set(&FC_REG, (r & ~0x0FF00000u) | (uint32_t) (pages & 0xFF) << 20);
}

/* /dev/mem ranges: SRAM1 and the SRAM2 parts MINIX uses (the rest may be
 * CPU2's secure SRAM); flash up to CPU2's (SFSA), also at its alias at 0;
 * the ROM loader and the OTP area; the 64-bit unique id (settings about,
 * spec K.7).  Anything else (holes, peripherals, CPU2's areas) is refused
 * instead of a bus fault in the MEMORY task. */
#define RAM_RANGES	{ 0x20000000u, 0x20030000u }, \
			{ 0x20030000u, 0x20032800u }, \
			{ 0x20038000u, 0x2003A800u }
#define ROM_SPANS \
	sfsa = FLASH_BASE + (FLASH->SFR & FLASH_SFR_SFSA) * HAL_FLASH_PAGE; \
	if ((n = span_in(addr, len, FLASH_BASE, sfsa)) != 0) \
		return n; \
	if ((n = span_in(addr, len, 0, sfsa - FLASH_BASE)) != 0) \
		return n; \
	if ((n = span_in(addr, len, 0x1FFF0000u, 0x1FFF7400u)) != 0) \
		return n; \
	if ((n = span_in(addr, len, 0x1FFF7580u, 0x1FFF7588u)) != 0) \
		return n; \
	/* bring-up (T30b), read only: the option bytes, and RCC, PWR and \
	 * FLASH's registers (no read there has a side effect; HSEM's \
	 * read-lock registers and IPCC stay out) */ \
	if ((n = span_in(addr, len, 0x1FFF8000u, 0x1FFF8080u)) != 0) \
		return n; \
	if ((n = span_in(addr, len, 0x58000000u, 0x58000800u)) != 0) \
		return n; \
	if ((n = span_in(addr, len, 0x58004000u, 0x58004100u)) != 0) \
		return n;

static uint32_t span_in(uint32_t addr, uint32_t len, uint32_t lo, uint32_t hi)
{
	if (addr < lo || addr >= hi) return 0;
	return len < hi - addr ? len : hi - addr;
}

uint32_t hal_mem_span(uint32_t addr, uint32_t len, int write)
{
	static const uint32_t ram[][2] = { RAM_RANGES };
	uint32_t sfsa, n;	/* no layout table: the MEMORY task's stack */
	unsigned i;

	for (i = 0; i < sizeof ram / sizeof ram[0]; i++)
		if ((n = span_in(addr, len, ram[i][0], ram[i][1])) != 0)
			return n;
	if (write) return 0;		/* flash and ROM only read */
	ROM_SPANS
	return 0;
}

/* The RTC (R6.2, R7b.1): the calendar (BCD; the stock firmware keeps it
 * running on the battery) as seconds since 1970, UTC, years 2000-2099;
 * Alarm A; smooth calibration.  Civil date <-> days: Howard Hinnant's
 * algorithms.  The alarm's due time and boot lead are kept in backup
 * registers (0 B RAM, register map in PORTING.md):
 *   BKP13R  the due time (s since 1970), 0: none
 *   BKP12R  magic 0xD (bits 28-31), this boot came from the alarm (16),
 *           it fired since it was set (17), the boot lead in s (0-15) */
#define RTCA_DUE	(RTC->BKP13R)
#define RTCA_REG	(RTC->BKP12R)
#define RTCA_MAGIC	0xD0000000u
#define RTCA_WOKE	(1u << 16)
#define RTCA_FIRED	(1u << 17)
#define RTCA_LEAD0	10		/* s, until a boot is measured */
#define RTC_EXTI	(1u << 17)	/* EXTI line 17: the RTC alarms */

static void rtc_unlock(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	PWR->CR1 |= PWR_CR1_DBP;
	RTC->WPR = 0xCA;
	RTC->WPR = 0x53;
}

static void rtc_lock(void)
{
	RTC->WPR = 0xFF;
	PWR->CR1 &= ~PWR_CR1_DBP;
}

/* rc_w0 flags: write 0 to the one, 1 to the others; INIT unchanged */
static void rtc_clear(uint32_t flag)
{
	RTC->ISR = ~(flag | RTC_ISR_INIT) | (RTC->ISR & RTC_ISR_INIT);
}

int hal_rtc_ok(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	return (RCC->BDCR & RCC_BDCR_RTCEN) && (RCC->BDCR & RCC_BDCR_LSERDY);
}

static uint32_t bcd(uint32_t v) { return v / 10 << 4 | v % 10; }
static uint32_t unbcd(uint32_t v) { return (v >> 4) * 10 + (v & 15); }

static uint32_t days_from_civil(uint32_t y, uint32_t m, uint32_t d)
{
	uint32_t era, yoe, doy, doe;

	if (m <= 2) y--;
	era = y / 400;
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civil_from_days(uint32_t z, uint32_t *y, uint32_t *m,
								uint32_t *d)
{
	uint32_t era, doe, yoe, doy, mp;

	z += 719468;
	era = z / 146097;
	doe = z - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = yoe + era * 400 + (*m <= 2);
}

uint32_t hal_rtc_time(void)
{
	uint32_t tr, dr, y, m, d, i;

	if (!hal_rtc_ok() || !(RTC->ISR & RTC_ISR_INITS)) return 0;
	/* the shadow registers stand still in Stop2: wait for a fresh copy
	 * (every 2 RTCCLK, ~61 us at most) */
	PWR->CR1 |= PWR_CR1_DBP;
	rtc_clear(RTC_ISR_RSF);
	PWR->CR1 &= ~PWR_CR1_DBP;
	for (i = 0; i < 100000 && !(RTC->ISR & RTC_ISR_RSF); i++) ;
	tr = RTC->TR;			/* locks DR until it is read */
	dr = RTC->DR;
	y = 2000 + unbcd(dr >> 16 & 0xFF);
	m = unbcd(dr >> 8 & 0x1F);
	d = unbcd(dr & 0x3F);
	if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
	return days_from_civil(y, m, d) * 86400
		+ unbcd(tr >> 16 & 0x3F) * 3600 + unbcd(tr >> 8 & 0x7F) * 60
		+ unbcd(tr & 0x7F);
}

int hal_rtc_set(uint32_t t)
{
	uint32_t y, m, d, days = t / 86400, s = t % 86400, i, fc;
	int today;

	civil_from_days(days, &y, &m, &d);
	if (!hal_rtc_ok() || y < 2000 || y > 2099) return -1;
	/* R2b.11: setting the clock is not a new day for the cache's erase
	 * budget (a jump there and back gave a fresh budget, #109): what
	 * today spent moves to the new date.  A word of an older day is left,
	 * so a day that really began meanwhile still starts afresh. */
	fc = FC_REG;
	today = (fc & 0xF0000000u) == FC_MAGIC && (fc & 0xFFF) == fc_day(0);
	rtc_unlock();
	RTC->ISR = 0xFFFFFFFF;			/* INIT: stops the calendar */
	for (i = 0; i < 100000 && !(RTC->ISR & RTC_ISR_INITF); i++) ;
	RTC->TR = bcd(s / 3600) << 16 | bcd(s / 60 % 60) << 8 | bcd(s % 60);
	RTC->DR = bcd(y - 2000) << 16 | ((days + 3) % 7 + 1) << 13
						| bcd(m) << 8 | bcd(d);
	RTC->CR &= ~RTC_CR_FMT;			/* 24 hours */
	RTC->ISR &= ~RTC_ISR_INIT;		/* runs from here */
	rtc_lock();
	if (today)	/* fc_day()'s number, from the date just set (the
			 * shadow registers may still hold the old one) */
		bkp_set(&FC_REG, (fc & ~0xFFFu)
			| (((y - 2000) * 372 + m * 31 + d) & 0xFFF));
	return 0;
}

/* Alarm A at t (date, h, m, s all compared: up to a month ahead). */
static void rtc_arm(uint32_t t)
{
	uint32_t y, m, d, s = t % 86400, i;

	civil_from_days(t / 86400, &y, &m, &d);
	rtc_unlock();
	RTC->CR &= ~(RTC_CR_ALRAE | RTC_CR_ALRAIE);
	for (i = 0; i < 100000 && !(RTC->ISR & RTC_ISR_ALRAWF); i++) ;
	RTC->ALRMAR = bcd(d) << 24 | bcd(s / 3600) << 16
				| bcd(s / 60 % 60) << 8 | bcd(s % 60);
	RTC->ALRMASSR = 0;			/* sub-seconds not compared */
	rtc_clear(RTC_ISR_ALRAF);
	RTC->CR |= RTC_CR_ALRAE | RTC_CR_ALRAIE;
	rtc_lock();
	EXTI->RTSR1 |= RTC_EXTI;		/* the alarm wakes Stop2 too */
	EXTI->IMR1 |= RTC_EXTI;
}

static void rtc_disarm(void)
{
	rtc_unlock();
	RTC->CR &= ~(RTC_CR_ALRAE | RTC_CR_ALRAIE);
	rtc_clear(RTC_ISR_ALRAF);
	rtc_lock();
	EXTI->PR1 = RTC_EXTI;
}

static unsigned rtc_lead(void)
{
	uint32_t r = RTCA_REG;

	return (r & 0xF0000000u) == RTCA_MAGIC && (r & 0xFFFF) != 0
						? r & 0xFFFF : RTCA_LEAD0;
}

int hal_rtc_alarm(uint32_t due, unsigned lead)
{
	uint32_t now = hal_rtc_time(), r;

	if (lead == 0 || lead > 0xFFFF) lead = rtc_lead();
	r = RTCA_REG;
	r = RTCA_MAGIC | ((r & 0xF0000000u) == RTCA_MAGIC ? r & RTCA_WOKE : 0)
									| lead;
	if (due == 0) {
		if (hal_rtc_ok()) rtc_disarm();
		bkp_set(&RTCA_DUE, 0);
		bkp_set(&RTCA_REG, r);
		return 0;
	}
	if (now == 0 || due <= now || due - now > 27 * 86400u) return -1;
	bkp_set(&RTCA_DUE, due);
	bkp_set(&RTCA_REG, r);
	rtc_arm(due);
	return 0;
}

uint32_t hal_rtc_alarm_get(unsigned *lead, unsigned *flags)
{
	uint32_t r = RTCA_REG;

	*lead = rtc_lead();
	*flags = 0;
	if ((r & 0xF0000000u) == RTCA_MAGIC) {
		if (r & RTCA_WOKE) *flags |= HAL_RTC_WOKE;
		if (r & RTCA_FIRED) *flags |= HAL_RTC_FIRED;
	}
	if (RTC->CR & RTC_CR_ALRAE) *flags |= HAL_RTC_ARMED;
	return (r & 0xF0000000u) == RTCA_MAGIC ? RTCA_DUE : 0;
}

int hal_rtc_alarm_irq(void)
{
	return hal_rtc_ok() ? RTC_Alarm_IRQn : -1;
}

int hal_rtc_alarm_ack(void)
{
	uint32_t r;

	EXTI->PR1 = RTC_EXTI;
	if (!(RTC->ISR & RTC_ISR_ALRAF)) return 0;
	rtc_disarm();			/* once: the date would match next month */
	r = RTCA_REG;
	if ((r & 0xF0000000u) == RTCA_MAGIC) bkp_set(&RTCA_REG, r | RTCA_FIRED);
	return 1;
}

/* At boot (hal_init), before the alarm output goes and Back is read: did
 * the alarm start us?  The output was on (we powered off with it) and
 * Alarm A fired. */
static void rtc_boot(void)
{
	uint32_t r = RTCA_REG, woke;

	woke = (RTC->CR & RTC_CR_OSEL) && (RTC->ISR & RTC_ISR_ALRAF);
	if ((r & 0xF0000000u) == RTCA_MAGIC) {
		r &= ~RTCA_WOKE;
		if (woke) r |= RTCA_WOKE | RTCA_FIRED;
		bkp_set(&RTCA_REG, r);
	}
	if (woke) rtc_disarm();
	else if (RTC->CR & RTC_CR_ALRAE) {	/* still armed: wake us */
		EXTI->RTSR1 |= RTC_EXTI;
		EXTI->IMR1 |= RTC_EXTI;
	}
}

/* At power-off: the alarm moves earlier by the boot lead, so the boot is
 * over when it is due (timed then waits for the real time). */
static void rtc_off(void)
{
	uint32_t due = RTCA_DUE, now = hal_rtc_time(), at;
	unsigned lead = rtc_lead();

	if ((RTCA_REG & 0xF0000000u) != RTCA_MAGIC || due == 0 || now == 0
				|| !(RTC->CR & RTC_CR_ALRAE) || due <= now)
		return;
	at = due > now + lead + 2 ? due - lead : now + 2;
	rtc_arm(at);
}

int hal_rtc_cal(int set, int v)
{
	uint32_t c, i;

	if (!hal_rtc_ok()) return 0;
	if (set) {
		if (v > 512) v = 512;
		if (v < -511) v = -511;
		c = v > 0 ? RTC_CALR_CALP | (uint32_t) (512 - v) : (uint32_t) -v;
		for (i = 0; i < 100000 && (RTC->ISR & RTC_ISR_RECALPF); i++) ;
		rtc_unlock();
		RTC->CALR = c;
		rtc_lock();
	}
	c = RTC->CALR;
	return (c & RTC_CALR_CALP ? 512 : 0) - (int) (c & RTC_CALR_CALM);
}

/* The backlight's settings (hal.h): BKP15R, magic 0xB in the top nibble
 * (register map in PORTING.md). */
#define BL_REG		(RTC->BKP15R)
#define BL_MAGIC	0xB0000000u

uint32_t hal_bl_state(void)
{
	uint32_t r = BL_REG;	/* the RTC's APB clock is on (hal_early_boot) */

	if ((r & 0xF0000000u) != BL_MAGIC) return 255u << 16 | 15u;
	return r & 0x0FFFFFFFu;
}

void hal_bl_state_set(uint32_t st)
{
	bkp_set(&BL_REG, BL_MAGIC | (st & 0x0FFFFFFFu));
}

int hal_reset_bits(void)
{
	return (int) (boot_reg() & (BOOT_DEV | BOOT_CAUSE));
}

const char *hal_reset_cause(void)
{
	uint32_t c = boot_reg() << 24;		/* back to RCC_CSR bits */

	if (c & RCC_CSR_IWDGRSTF) return "watchdog";
	if (c & RCC_CSR_WWDGRSTF) return "window watchdog";
	if (c & RCC_CSR_LPWRRSTF) return "low power";
	if (c & RCC_CSR_BORRSTF) return "power on";
	if (c & RCC_CSR_SFTRSTF) return "software";
	if (c & RCC_CSR_OBLRSTF) return "option bytes";
	if (c & RCC_CSR_PINRSTF) return "reset pin";
	return "unknown";
}

void hal_dfu_request(void)
{
	*HAL_BOOT_WORD = HAL_BOOT_DFU;
	__DSB();
	NVIC_SystemReset();
}

/*===========================================================================*
 *				Vibro, GPIO header			     *
 *===========================================================================*/
void hal_vibro(int on)
{
	gpio_set(GPIOA, 8, on);
	gpio_cfg(GPIOA, 8, MODE_OUT, PULL_NONE, 0);
}

static const struct { GPIO_TypeDef *port; unsigned pin; } hdr[HAL_GPIO_PINS] = {
	{ GPIOA, 7 }, { GPIOA, 6 }, { GPIOA, 4 }, { GPIOB, 3 },
	{ GPIOB, 2 }, { GPIOC, 3 }, { GPIOC, 1 }, { GPIOC, 0 },
};

int hal_gpio(unsigned pin, unsigned op)
{
	GPIO_TypeDef *port;
	unsigned n;

	if (pin >= HAL_GPIO_PINS) return -1;
	port = hdr[pin].port;
	n = hdr[pin].pin;
	switch (op) {
	case HAL_GPIO_LOW:
	case HAL_GPIO_HIGH:
		gpio_set(port, n, op == HAL_GPIO_HIGH);	/* level first */
		gpio_cfg(port, n, MODE_OUT, PULL_NONE, 0);
		break;
	case HAL_GPIO_IN:	gpio_cfg(port, n, MODE_IN, PULL_NONE, 0); break;
	case HAL_GPIO_IN_UP:	gpio_cfg(port, n, MODE_IN, PULL_UP, 0); break;
	case HAL_GPIO_IN_DOWN:	gpio_cfg(port, n, MODE_IN, PULL_DOWN, 0); break;
	case HAL_GPIO_OFF:	gpio_cfg(port, n, MODE_ANALOG, PULL_NONE, 0); break;
	default:		return -1;
	}
	return 0;
}

int hal_gpio_read(unsigned pin)
{
	if (pin >= HAL_GPIO_PINS) return -1;
	return gpio_get(hdr[pin].port, hdr[pin].pin);
}

/* ADC on the header pins (R6.4, part 1: one-shot).  ADC1 channels of the
 * eight pins (0: none, PB3/PB2), and VREFINT (channel 0) to know VDDA:
 * its factory value was taken at 3.6 V (VREFINT_CAL, ST's LL header).
 * Synchronous clock HCLK/4 (16 MHz), 92.5-cycle sampling, 16x hardware
 * oversampling (result shifted back to 12 bits).  The ADC is powered only
 * for the reading, then back in deep power-down. */
static const uint8_t adc_ch[HAL_GPIO_PINS] = { 12, 11, 9, 0, 0, 4, 2, 1 };
#define VREFINT_CAL	(*(const volatile uint16_t *) 0x1FFF75AAu)

static int adc_wait(volatile uint32_t *reg, uint32_t bit, int set)
{
	uint32_t start = DWT->CYCCNT;

	while (((*reg & bit) != 0) != set)
		if (ms_since(start) > 2) return -1;
	return 0;
}

static int adc_conv(unsigned ch)
{
	ADC1->SQR1 = ch << ADC_SQR1_SQ1_Pos;		/* one conversion */
	ADC1->ISR = ADC_ISR_EOC;
	ADC1->CR |= ADC_CR_ADSTART;
	if (adc_wait(&ADC1->ISR, ADC_ISR_EOC, 1) < 0) return -1;
	return (int) (ADC1->DR & 0xFFF);
}

/* ADC power-up: VREF+ from the internal buffer (it is not tied to VDDA on
 * the Flipper: 2.5 V with its factory trim, as stock furi_hal_adc does),
 * the regulator, calibration, enable.  0, or -1 (then adc_down). */
static int adc_up(void)
{
	uint32_t start;

	VREFBUF->CCR = *(const volatile uint8_t *) 0x1FFF7530u & 0x3F;
	VREFBUF->CSR = VREFBUF_CSR_VRS | VREFBUF_CSR_ENVR;	/* 2.5 V, not HIZ */
	start = DWT->CYCCNT;			/* the VREF+ capacitor */
	while (!(VREFBUF->CSR & VREFBUF_CSR_VRR))
		if (ms_since(start) > 50) return -1;
	RCC->AHB2ENR |= RCC_AHB2ENR_ADCEN;
	(void) RCC->AHB2ENR;
	ADC1_COMMON->CCR = (ADC1_COMMON->CCR & ~ADC_CCR_CKMODE)
				| (3u << ADC_CCR_CKMODE_Pos) | ADC_CCR_VREFEN;
	ADC1->CR &= ~ADC_CR_DEEPPWD;
	ADC1->CR |= ADC_CR_ADVREGEN;
	hal_ow_delay(25);			/* the regulator: 20 us */
	ADC1->CR |= ADC_CR_ADCAL;
	if (adc_wait(&ADC1->CR, ADC_CR_ADCAL, 0) < 0) return -1;
	ADC1->ISR = ADC_ISR_ADRDY;
	ADC1->CR |= ADC_CR_ADEN;
	if (adc_wait(&ADC1->ISR, ADC_ISR_ADRDY, 1) < 0) return -1;
	ADC1->SMPR1 = 0x2DB6DB6Du;		/* 92.5 cycles, channels 0-9 */
	ADC1->SMPR2 = 0x05B6DB6Du;		/* and 10-18 */
	return 0;
}

static void adc_down(void)
{
	if (ADC1->CR & ADC_CR_ADSTART) {
		ADC1->CR |= ADC_CR_ADSTP;
		(void) adc_wait(&ADC1->CR, ADC_CR_ADSTART, 0);
	}
	if (ADC1->CR & ADC_CR_ADEN) {
		ADC1->CR |= ADC_CR_ADDIS;
		(void) adc_wait(&ADC1->CR, ADC_CR_ADEN, 0);
	}
	ADC1->CFGR = 0;
	ADC1->CR &= ~ADC_CR_ADVREGEN;
	ADC1->CR |= ADC_CR_DEEPPWD;
	ADC1_COMMON->CCR &= ~ADC_CCR_VREFEN;
	RCC->AHB2ENR &= ~RCC_AHB2ENR_ADCEN;
	VREFBUF->CSR = VREFBUF_CSR_HIZ;
}

static int adc_pin_ok(unsigned pin)	/* an ADC input, left analog */
{
	return pin < HAL_GPIO_PINS && adc_ch[pin] != 0
		&& ((hdr[pin].port->MODER >> (hdr[pin].pin * 2)) & 3) == MODE_ANALOG;
}

int hal_adc_mv(int *mv)		/* HAL_GPIO_PINS values, -1 not measured */
{
	unsigned pin;
	int vref, raw, r = -1;

	if (adc_up() < 0) goto off;
	ADC1->CFGR = 0;				/* 12 bits, right, single */
	ADC1->CFGR2 = ADC_CFGR2_ROVSE | (3u << ADC_CFGR2_OVSR_Pos)
						| (4u << ADC_CFGR2_OVSS_Pos);
	/* VREFINT before and after the pins, averaged: the buffer is still
	 * settling right after VRR (seen: +-1.5 % between readings) */
	if ((vref = adc_conv(0)) <= 0) goto off;
	for (pin = 0; pin < HAL_GPIO_PINS; pin++) {
		mv[pin] = -1;
		/* only pins left analog ("off"): a reading never changes a
		 * pin someone set up */
		if (!adc_pin_ok(pin) || (raw = adc_conv(adc_ch[pin])) < 0)
			continue;
		/* VREF+ = 3600 * CAL / vref (the factory VREFINT reading was
		 * taken at 3.6 V); mV = VREF+ * raw / 4095; at full scale the
		 * pin is above VREF+: HAL_ADC_OVER */
		mv[pin] = raw >= 4095 ? HAL_ADC_OVER : (int) (3600u * VREFINT_CAL
				/ (unsigned) vref * (unsigned) raw / 4095u);
	}
	if ((raw = adc_conv(0)) > 0) vref = (vref + raw + 1) / 2;
	r = (int) (3600u * VREFINT_CAL / (unsigned) vref);	/* VREF+ */
off:
	adc_down();
	return r;
}

/* R6.4 part 2: a block of samples of one pin straight into the reader's
 * buffer (R4b.5: the kernel pins the process): TIM2's update at the rate
 * triggers each conversion, DMA1 channel 3 (DMAMUX request 5, ADC1)
 * writes the 12-bit results.  TIM2 is also the IR pulse engine: the
 * kernel never runs both.  No interrupt: the kernel polls the count. */
int hal_adc_stream(unsigned pin, unsigned rate, uint32_t dst, unsigned n)
{
	if (!adc_pin_ok(pin) || rate < 100 || rate > 100000 || n == 0
					|| n > 0xFFFF || (dst & 1)) return -1;
	if (adc_up() < 0) {
		adc_down();
		return -1;
	}
	ADC1->CFGR2 = 0;			/* no oversampling */
	ADC1->SQR1 = adc_ch[pin] << ADC_SQR1_SQ1_Pos;
	ADC1->CFGR = ADC_CFGR_DMAEN | (1u << ADC_CFGR_EXTEN_Pos)	/* rising */
		   | (0xBu << ADC_CFGR_EXTSEL_Pos) | ADC_CFGR_OVRMOD;	/* TIM2 TRGO */
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	(void) RCC->AHB1ENR;
	DMA1_Channel3->CCR = 0;
	DMAMUX1_Channel2->CCR = 5;			/* ADC1 */
	DMA1_Channel3->CPAR = (uint32_t) &ADC1->DR;
	DMA1_Channel3->CMAR = dst;
	DMA1_Channel3->CNDTR = n;
	DMA1_Channel3->CCR = DMA_CCR_MINC | DMA_CCR_PSIZE_0 | DMA_CCR_MSIZE_0
							| DMA_CCR_EN;
	ADC1->ISR = ADC_ISR_OVR | ADC_ISR_EOC;
	ADC1->CR |= ADC_CR_ADSTART;			/* waits for TRGO */
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->CR1 = 0;
	TIM2->PSC = 0;
	TIM2->ARR = SystemCoreClock / rate - 1;
	TIM2->CNT = 0;
	TIM2->CR2 = 2u << TIM_CR2_MMS_Pos;		/* update -> TRGO */
	TIM2->EGR = TIM_EGR_UG;
	TIM2->CR1 = TIM_CR1_CEN;
	return 0;
}

unsigned hal_adc_stream_left(void)
{
	return DMA1_Channel3->CCR & DMA_CCR_EN ? DMA1_Channel3->CNDTR : 0;
}

void hal_adc_stream_stop(void)
{
	TIM2->CR1 = 0;
	TIM2->CR2 = 0;
	DMA1_Channel3->CCR = 0;
	DMAMUX1_Channel2->CCR = 0;
	adc_down();
}

/*===========================================================================*
 *				Infrared				     *
 *===========================================================================*/
/* As on the stock firmware: the receiver (TSOP, active low, own pull-up)
 * on PA0, timed by the pulse engine (hal_pulse_rx); the LED driver on PB9,
 * TIM1 channel 3N (AF1), PWM at the carrier frequency.  Carrier off: the
 * channel is forced inactive (the LED is dark at low level).  Marks and
 * spaces are timed by TIM2 (the "pulse engine", spec R6.6): 1 MHz, the
 * next duration preloaded, so the interrupt only switches the carrier and
 * a late interrupt never adds up. */
#define IR_CARRIER_HZ	38000u

static uint32_t ir_period = 0, ir_on;	/* carrier, in timer clocks */

uint32_t hal_cycles(void)
{
	return DWT->CYCCNT;
}

void hal_ir_freq(unsigned hz, unsigned duty)
{
	ir_period = SystemCoreClock / hz;
	ir_on = ir_period * duty / 100;
}

static unsigned char ir_ready;			/* TIM1 set up for the carrier; the
					 * RFID field takes TIM1 and clears it */
static unsigned char ir_tgt;			/* 0 the IR LED, 1 the RFID antenna,
					 * 2 the CC1101 (Sub-GHz OOK) */

/* The transmitter drives the IR LED (TIM1 CH3N, PB9) or, for writing a
 * T5577 tag (R6.5), the 125 kHz antenna (TIM1 CH1N, PB13; PA2 high: the
 * antenna not grounded, as stock t5577_start; iButton low). */
void hal_ir_target(int rfid)
{
	if (rfid == ir_tgt && ir_ready) return;
	ir_tgt = rfid;
	ir_ready = 0;
	if (rfid == 2) return;		/* the CC1101's GDO0: hal_subghz_tx_begin */
	if (rfid) {
		gpio_set(GPIOA, 2, 1);
		gpio_cfg(GPIOA, 2, MODE_OUT, PULL_NONE, 0);
		hal_ow_line(1);
		gpio_cfg(GPIOC, 5, MODE_ANALOG, PULL_NONE, 0);
		gpio_cfg(GPIOA, 15, MODE_ANALOG, PULL_NONE, 0);	/* JTDI */
	}
}

void hal_ir_carrier(int on)
{
	if (ir_tgt == 2) return;	/* Sub-GHz OOK: TIM2_CH2 toggles GDO0 */
	if (!ir_ready) {
		RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
		(void) RCC->APB2ENR;
		TIM1->CR1 = 0;
		TIM1->PSC = 0;
		if (ir_period == 0) hal_ir_freq(IR_CARRIER_HZ, 33);
		TIM1->ARR = ir_period - 1;
		if (ir_tgt) {
			TIM1->CCR1 = ir_on;
			TIM1->CCMR1 = (4u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
			TIM1->CCMR2 = 0;
			TIM1->CCER = TIM_CCER_CC1NE;
		} else {
			TIM1->CCR3 = ir_on;
			TIM1->CCMR1 = 0;
			TIM1->CCMR2 = (4u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;
			TIM1->CCER = TIM_CCER_CC3NE;
		}
		TIM1->BDTR = TIM_BDTR_MOE;
		TIM1->EGR = TIM_EGR_UG;
		TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
		if (ir_tgt) {
			gpio_set(GPIOB, 13, 0);
			gpio_cfg(GPIOB, 13, MODE_AF, PULL_NONE, 1);
		} else {
			gpio_set(GPIOB, 9, 0);
			gpio_cfg(GPIOB, 9, MODE_AF, PULL_NONE, 1);
		}
		ir_ready = 1;
	}
	if (on) {				/* preloaded, taken by UG */
		TIM1->ARR = ir_period - 1;
		if (ir_tgt) TIM1->CCR1 = ir_on;
		else TIM1->CCR3 = ir_on;
	}
	/* The antenna (a series LC): a gap is the outputs off (MOE 0, the
	 * pin floats), as stock's furi_hal_rfid_tim_read_pause; held low
	 * the circuit stays closed and the field rings on.  The IR LED:
	 * PWM mode 1 (110) or forced inactive (100).  Either way a fresh
	 * period, so every mark begins with a whole carrier cycle. */
	if (ir_tgt) {
		TIM1->CCMR1 = (TIM1->CCMR1 & ~TIM_CCMR1_OC1M)
			| (6u << TIM_CCMR1_OC1M_Pos);
		TIM1->BDTR = on ? TIM_BDTR_MOE : 0;
	} else
		TIM1->CCMR2 = (TIM1->CCMR2 & ~TIM_CCMR2_OC3M)
			| ((on ? 6u : 4u) << TIM_CCMR2_OC3M_Pos);
	if (on) TIM1->EGR = TIM_EGR_UG;
}

int hal_ir_tx_irq(void)
{
	return TIM2_IRQn;
}

/* R4b.7, the pulse engine's capture: TIM2 at 1 MHz restarts at every edge
 * of the input (slave mode reset, both edges) and a channel captures the
 * count it had: the time since the edge before, in us.  DMA1 channel 4
 * writes those 32-bit durations straight into the reader's buffer
 * (R4b.5).  The first edge, which only ends the silence before, goes to
 * the interrupt instead; it turns the DMA on.  The other channel compares
 * at the gap: silence that long ends the frame.
 *   input 0: the IR receiver on PA0 (TIM2_CH1, AF1), active low: a frame
 *            starts with a falling edge.  Reset on TI1F_ED, IC1 on TRC.
 *   input 1: the CC1101's GDO0 on PA1 (TIM2_CH2, AF1), its demodulated
 *            OOK data (IOCFG0 0x0D), high with carrier: a frame starts
 *            with a rising edge.  Reset on TI2FP2 with both edges
 *            (CC2P|CC2NP), IC2 on TI2.
 * TIM2 is also the IR transmitter's and the ADC stream's: the kernel runs
 * one at a time. */
#define PULSE_IN1	(TIM2->SMCR & (2u << TIM_SMCR_TS_Pos))	/* TS 110 */

int hal_pulse_rx(unsigned input, uint32_t dst, unsigned n, unsigned gap_us)
{
	if (input > 1 || n == 0 || gap_us == 0) return -1;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->CR1 = 0;
	TIM2->DIER = 0;
	DMA1_Channel4->CCR = 0;
	DMAMUX1_Channel3->CCR = input ? 29 : 28;	/* TIM2_CH2, TIM2_CH1 */
	DMA1_Channel4->CPAR = (uint32_t) (input ? &TIM2->CCR2 : &TIM2->CCR1);
	DMA1_Channel4->CMAR = dst;
	DMA1_Channel4->CNDTR = n;
	DMA1_Channel4->CCR = DMA_CCR_MINC | DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1
							| DMA_CCR_EN;
	TIM2->PSC = SystemCoreClock / 1000000u - 1;	/* 1 us */
	TIM2->ARR = 0xFFFFFFFF;
	TIM2->CR2 = 0;
	TIM2->CCMR2 = 0;
	if (input == 0) {
		TIM2->SMCR = (4u << TIM_SMCR_TS_Pos) | (4u << TIM_SMCR_SMS_Pos);
		TIM2->CCMR1 = (3u << TIM_CCMR1_CC1S_Pos)	/* IC1 on TRC */
			| (3u << TIM_CCMR1_IC1F_Pos);		/* 125 ns */
		TIM2->CCR2 = gap_us;
		TIM2->CCER = TIM_CCER_CC1E;
	} else {
		TIM2->SMCR = (6u << TIM_SMCR_TS_Pos) | (4u << TIM_SMCR_SMS_Pos);
		TIM2->CCMR1 = (1u << TIM_CCMR1_CC2S_Pos)	/* IC2 on TI2 */
			| (3u << TIM_CCMR1_IC2F_Pos);
		TIM2->CCR1 = gap_us;
		TIM2->CCER = TIM_CCER_CC2E | TIM_CCER_CC2P | TIM_CCER_CC2NP;
	}
	TIM2->CR1 = TIM_CR1_URS;
	TIM2->EGR = TIM_EGR_UG;				/* PSC now */
	TIM2->SR = 0;
	NVIC_ClearPendingIRQ(TIM2_IRQn);
	gpio_cfg(GPIOA, input, MODE_AF, PULL_NONE, 1);	/* TIM2_CH1, CH2 */
	TIM2->DIER = TIM_DIER_CC1IE | TIM_DIER_CC2IE;	/* edge, gap */
	TIM2->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
	return 0;
}

/* #302: the CC1101's GDO0 (PA1, TIM2_CH2) as a gapless stream, as stock's
 * furi_hal_subghz_start_async_rx does it (an ISR per edge; here circular
 * DMA, the interrupt per half is hal_rfid_stream_irq's).  TIM2 runs free at
 * 1 MHz and channel 2 captures both edges of TI2 straight into the caller's
 * ring; stock's input filter (fDTS/4/32, 8 samples: 16 us) drops shorter
 * spikes.  Returns the pin's level before the first edge. */
int hal_subghz_stream(uint32_t dst, unsigned n)
{
	if (n == 0) return -1;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	ir_ready = 0;
	TIM2->CR1 = 0;
	TIM2->DIER = 0;
	TIM2->SMCR = 0;
	TIM2->CR2 = 0;
	TIM2->OR = 0;
	DMA1_Channel4->CCR = 0;
	DMAMUX1_Channel3->CCR = 29;			/* TIM2_CH2 */
	DMA1_Channel4->CPAR = (uint32_t) &TIM2->CCR2;
	DMA1_Channel4->CMAR = dst;
	DMA1_Channel4->CNDTR = n;
	DMA1->IFCR = DMA_IFCR_CGIF4;
	DMA1_Channel4->CCR = DMA_CCR_MINC | DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1
		| DMA_CCR_CIRC | DMA_CCR_HTIE | DMA_CCR_TCIE | DMA_CCR_EN;
	TIM2->PSC = SystemCoreClock / 1000000u - 1;	/* 1 us */
	TIM2->ARR = 0xFFFFFFFF;
	TIM2->CCMR1 = (1u << TIM_CCMR1_CC2S_Pos) | (15u << TIM_CCMR1_IC2F_Pos);
	TIM2->CCMR2 = 0;
	TIM2->CCER = TIM_CCER_CC2E | TIM_CCER_CC2P | TIM_CCER_CC2NP;
	TIM2->CNT = 0;
	TIM2->EGR = TIM_EGR_UG;
	TIM2->SR = 0;
	gpio_cfg(GPIOA, 1, MODE_AF, PULL_NONE, 1);	/* TIM2_CH2 */
	TIM2->DIER = TIM_DIER_CC2DE;
	TIM2->CR1 = TIM_CR1_URS | TIM_CR1_CEN | (2u << TIM_CR1_CKD_Pos);
	return gpio_get(GPIOA, 1) ? 1 : 0;
}

int hal_pulse_ack(void)
{
	uint32_t sr = TIM2->SR, dier = TIM2->DIER, cap, gap, de;
	int in1 = PULSE_IN1 != 0, r = 0;

	cap = in1 ? TIM_SR_CC2IF : TIM_SR_CC1IF;
	gap = in1 ? TIM_SR_CC1IF : TIM_SR_CC2IF;
	de = in1 ? TIM_DIER_CC2DE : TIM_DIER_CC1DE;
	TIM2->SR = ~sr;				/* those seen, nothing else */
	if ((sr & cap) && !(dier & de)) {
		(void) (in1 ? TIM2->CCR2 : TIM2->CCR1);	/* the silence before */
		/* a mark began: the frame (IR low, GDO0 high) */
		if (gpio_get(GPIOA, in1) == in1) {
			TIM2->DIER = de | (in1 ? TIM_DIER_CC1IE : TIM_DIER_CC2IE);
			r |= HAL_PULSE_START;
		}
	}
	if ((sr & gap) && (dier & de))
		r |= HAL_PULSE_GAP;		/* silence after the frame */
	return r;
}

unsigned hal_pulse_left(void)
{
	return DMA1_Channel4->CNDTR;
}

void hal_pulse_stop(void)
{
	int in1;

	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	in1 = PULSE_IN1 != 0;
	TIM2->DIER = 0;
	TIM2->CR1 = 0;
	TIM2->SMCR = 0;
	TIM2->CCER = 0;
	TIM2->CCMR1 = 0;
	TIM2->SR = 0;
	DMA1_Channel4->CCR = 0;
	DMAMUX1_Channel3->CCR = 0;
	if (in1) gpio_cfg(GPIOA, 1, MODE_ANALOG, PULL_NONE, 0);	/* as stock */
	else gpio_cfg(GPIOA, 0, MODE_IN, PULL_NONE, 0);
}

void hal_ir_tx_start(uint32_t first_us, uint32_t next_us)
{
	if (first_us == 0) first_us = 1;	/* ARR 0 - 1: 71 minutes */
	if (next_us == 0) next_us = 1;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->CR1 = 0;
	TIM2->DIER = 0;
	TIM2->PSC = SystemCoreClock / 1000000u - 1;	/* 1 us */
	TIM2->ARR = first_us - 1;
	TIM2->CNT = 0;
	/* URS before UG: the load sets no update flag.  Cleared right after
	 * the UG instead it was still set when the interrupt was enabled, and
	 * the first mark ended after 4 us. */
	TIM2->CR1 = TIM_CR1_URS;
	TIM2->EGR = TIM_EGR_UG;			/* PSC and ARR now */
	TIM2->SR = 0;
	NVIC_ClearPendingIRQ(TIM2_IRQn);
	TIM2->CR1 = TIM_CR1_ARPE | TIM_CR1_URS;
	TIM2->ARR = next_us - 1;		/* preload: the segment after */
	TIM2->DIER = TIM_DIER_UIE;
	TIM2->CR1 |= TIM_CR1_CEN;
}

int hal_ir_tx_ack(void)
{
	/* Clear the update flag at once and read it back: cleared only at
	 * the end, it is still set when the handler returns and the
	 * interrupt comes again - a spurious second step. */
	uint32_t sr = TIM2->SR;

	TIM2->SR = 0;				/* every flag, whoever set it */
	(void) TIM2->SR;
	return (sr & TIM_SR_UIF) != 0;
}

void hal_ir_tx_next(uint32_t us)
{
	TIM2->ARR = us ? us - 1 : 0;		/* taken at the next update */
}

void hal_ir_tx_stop(void)
{
	/* Also at start: the ROM DFU loader starts the image without a reset
	 * and may leave a timer running with its interrupts on. */
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->DIER = 0;
	TIM2->CR1 = 0;
	TIM2->SR = 0;
}

/*===========================================================================*
 *			CPU2 probe (R2b.10)				     *
 *===========================================================================*/
/* The radio CPU (Cortex-M0+, ST's FUS and wireless stack, flash from SFSA)
 * is started on request (/dev/power "cpu2") and only as far as its
 * "system ready" event: the shared tables of ST's transport layer
 * (STM32CubeWB tl_mbox.c, mbox_def.h), in SRAM2a where CPU2 looks for the
 * reference table (0x20030000, the first 832 bytes, .cpu2map; the FS
 * buffer cache starts after it), the system event queue and the memory
 * manager's spare buffers, IPCC and HSEM clocked, CPU2 told to boot.  CPU2
 * fills the device information table (FUS and stack versions, stack type)
 * and raises IPCC channel 2.  Nothing is sent to it: no BLE.  A reset
 * stops it again.  The state is the hardware's (C2BOOT, the IPCC flag): no
 * RAM in SRAM1.
 *
 * CPU1's flash erase and program take HSEM 2 and 7 and wait for PESD while
 * CPU2 runs (c2_flash_begin below). */
struct cpu2_list { uint32_t next, prev; };
struct cpu2_map {
	uint32_t ref[10];	/* MB_RefTable_t: devinfo, ble, thread, sys, mm,
				 * traces, mac, zigbee, lld tests, ble lld */
	uint32_t devinfo[16];	/* a stack writes 8 words: safe boot ver; FUS
				 * ver, mem, info; stack ver, mem, InfoStack,
				 * reserved.  FUS writes 14 (ST's
				 * MB_FUS_DeviceInfoTable_t): 0xA94656B9,
				 * state bytes, safe boot ver, FUS ver, mem,
				 * stack ver, mem, BLE info, Thread info,
				 * reserved, UID64, device id.  With room
				 * for 8 only, FUS's device id landed on
				 * sys[0], the command buffer's address,
				 * and no FUS command was answered
				 * (2026-11-27) */
	uint32_t ble[4];	/* cmd, cs, evt queue, acl */
	uint32_t sys[2];	/* cmd buffer, event queue */
	uint32_t mm[7];		/* spare ble, spare sys, ble pool, size,
				 * free queue, traces pool, size */
	uint32_t traces[1];
	struct cpu2_list sys_q, free_q, ble_q, traces_q;
	uint32_t cmd[68];	/* TL_CmdPacket_t: 8 + 1 + 3 + 255, rounded */
	uint32_t sys_spare[68];	/* the spare system event buffer */
};
static struct cpu2_map cpu2 __attribute__((section(".cpu2map"), used));
/* SRAM2b, after the FS buffers there (.cpu2mem2, the last 832 bytes): the
 * spare BLE event buffer and the asynchronous event pool CPU2's stack
 * takes at its start (stock gives them before booting it; left NULL, the
 * BLE light stack hard-faulted at its start, 2026-11-25). */
struct cpu2_mem2 {
	uint32_t ble_spare[68];
	uint32_t pool[136];		/* two event frames */
};
static struct cpu2_mem2 cpu2b __attribute__((section(".cpu2mem2"), used));

#define C2_ADDR(x)	((uint32_t) (x))

int hal_cpu2_start(void)
{
	struct cpu2_map *m = &cpu2;
	unsigned i;

	if (PWR->CR4 & PWR_CR4_C2BOOT) return 1;	/* started already */
	for (i = 0; i < sizeof *m / 4; i++) ((volatile uint32_t *) m)[i] = 0;
	for (i = 0; i < sizeof cpu2b / 4; i++) ((volatile uint32_t *) &cpu2b)[i] = 0;
	m->ref[0] = C2_ADDR(m->devinfo);
	m->ref[1] = C2_ADDR(m->ble);
	m->ref[3] = C2_ADDR(m->sys);
	m->ref[4] = C2_ADDR(m->mm);
	m->ref[5] = C2_ADDR(m->traces);
	m->sys_q.next = m->sys_q.prev = C2_ADDR(&m->sys_q);	/* empty lists */
	m->free_q.next = m->free_q.prev = C2_ADDR(&m->free_q);
	m->ble_q.next = m->ble_q.prev = C2_ADDR(&m->ble_q);
	m->traces_q.next = m->traces_q.prev = C2_ADDR(&m->traces_q);
	m->sys[0] = C2_ADDR(m->cmd);
	m->sys[1] = C2_ADDR(&m->sys_q);
	m->ble[0] = C2_ADDR(m->cmd);			/* no BLE yet: valid */
	m->ble[1] = C2_ADDR(m->cmd);
	m->ble[2] = C2_ADDR(&m->ble_q);
	m->ble[3] = C2_ADDR(m->cmd);
	m->mm[0] = C2_ADDR(cpu2b.ble_spare);
	m->mm[1] = C2_ADDR(m->sys_spare);
	m->mm[2] = C2_ADDR(cpu2b.pool);
	m->mm[3] = sizeof cpu2b.pool;
	m->mm[4] = C2_ADDR(&m->free_q);
	m->traces[0] = C2_ADDR(&m->traces_q);
	__asm volatile ("dsb" ::: "memory");
	RCC->AHB3ENR |= RCC_AHB3ENR_IPCCEN | RCC_AHB3ENR_HSEMEN;
	/* The crypto units CPU2 uses, clocked from CPU1's side as stock's
	 * furi_hal_bt_init does (AES2, PKA, CRC) before it starts CPU2:
	 * FUS checks an image's signature with them. */
	RCC->AHB3ENR |= RCC_AHB3ENR_AES2EN | RCC_AHB3ENR_PKAEN;
	RCC->AHB1ENR |= RCC_AHB1ENR_CRCEN;
	(void) RCC->AHB3ENR;
	if (RCC->BDCR & RCC_BDCR_LSERDY)		/* the RF wake-up clock */
		RCC->CSR = (RCC->CSR & ~RCC_CSR_RFWKPSEL_Msk) | RCC_CSR_RFWKPSEL_0;
	/* CPU2 wakes on IPCC through EXTI line 41, as ST's HW_IPCC_Enable */
	EXTI->C2EMR2 |= 1u << (41 - 32);
	EXTI->RTSR2 |= 1u << (41 - 32);
	/* USB runs on the 48 MHz clock, which CPU2 may switch for the RNG
	 * unless CPU1 holds HSEM 5 (AN5289; stock's furi_hal_usb): kept */
	(void) (HSEM->RLR[5]);
	__asm volatile ("sev; wfe; wfe" ::: "memory");	/* as ST, clears the event */
	/* The CRC unit goes to CPU2: FUS checks an image with it (signature,
	 * then integrity after its own resets) and does not set it up
	 * (AN5185 table 7: CPU1 must reset it).  Ours was left at the SD
	 * card's CRC-16 (POL 0x1021), so FUS found every image not authentic
	 * (10.03), or corrupted (10.02) when only the command reset it.  The
	 * card uses the software CRC from here until the next reset. */
	sd_ready &= ~SD_HWCRC;
	RCC->AHB1RSTR |= RCC_AHB1RSTR_CRCRST;
	(void) RCC->AHB1RSTR;
	RCC->AHB1RSTR &= ~RCC_AHB1RSTR_CRCRST;
	(void) RCC->AHB1RSTR;
	PWR->CR4 |= PWR_CR4_C2BOOT;
	return 0;
}

/* -1: not started; 0: started, no ready event yet; 1: ready, v[] =
 * FUS version, stack version, InfoStack, the ready event's SysStatus
 * (0 the wireless stack runs, 1 FUS; 0xFF unknown). */
int hal_cpu2_info(uint32_t v[4])
{
	struct cpu2_map *m = &cpu2;
	const uint8_t *e;

	if (!(PWR->CR4 & PWR_CR4_C2BOOT)) return -1;
	if (m->ref[0] == 0x1170FD0Fu) {		/* CPU2's hard fault record */
		v[0] = m->ref[1];		/* pc */
		v[1] = m->ref[2];		/* lr */
		v[2] = m->ref[3];		/* sp */
		return 2;
	}
	/* the ready event: IPCC channel 2 flagged, or (once hal_cpu2_shci
	 * has acknowledged the flag) still in the system event queue */
	if (!(IPCC->C2TOC1SR & (1u << 1))
	    && m->sys_q.next == C2_ADDR(&m->sys_q)) return 0;
	if (m->devinfo[0] == 0xA94656B9u) {	/* FUS runs: its own table */
		v[0] = m->devinfo[3];
		v[1] = m->devinfo[5];
		v[2] = m->devinfo[1] >> 24;	/* the installed stack's type */
		v[3] = 1;
		return 1;
	}
	v[0] = m->devinfo[1];
	v[1] = m->devinfo[4];
	v[2] = m->devinfo[6];
	v[3] = 0xFF;
	if (m->sys_q.next != C2_ADDR(&m->sys_q)) {
		/* TL_EvtPacket_t: list node, type, evtcode, plen, payload:
		 * subevtcode (2, 0x9200 ready) then SysStatus */
		e = (const uint8_t *) m->sys_q.next;
		if (e[9] == 0xFF && (e[11] | e[12] << 8) == 0x9200) v[3] = e[13];
	}
	return 1;
}

/* The IPCC flags, for /dev/power: CPU1's channels in use (commands out)
 * in bits 0-5, CPU2's (events for us) in bits 8-13. */
unsigned hal_cpu2_ipcc(void)
{
	unsigned i, h = 0;

	if (!(PWR->CR4 & PWR_CR4_C2BOOT)) return 0;
	for (i = 0; i < 8; i++)		/* the hardware semaphores held, 0-7 */
		if (HSEM->R[i] & HSEM_R_LOCK) h |= 1u << i;
	return (IPCC->C1TOC2SR & 0x3F) | (IPCC->C2TOC1SR & 0x3F) << 8 | h << 16;
}

/* HSEM 5, the 48 MHz clock (AN5289): held by CPU1 since hal_cpu2_start so
 * that CPU2 does not switch the clock USB runs on.  hold 0 gives it up
 * (CPU2 may then run its RNG on HSI48: FUS, to check an image), 1 takes
 * it again. */
void hal_cpu2_clk48(int hold)
{
	if (!(PWR->CR4 & PWR_CR4_C2BOOT)) return;
	if (hold) (void) (HSEM->RLR[5]);
	else HSEM->R[5] = HSEM_CR_COREID_CURRENT;
}

/* The answer in the command buffer, if one is there: its status (0-255),
 * v[0] the opcode and v[1] the byte after the status (FUS_GetState's
 * error code; 0x100 when the answer has none); -1 none.  No state of our
 * own: the buffer is it. */
int hal_cpu2_last(uint32_t v[4])
{
	const volatile uint8_t *c = (const volatile uint8_t *) cpu2.cmd;

	/* the raw start of the packet, for a look when it is not understood:
	 * type, event, length, count / opcode (2), status, next byte */
	v[2] = c[8] | c[9] << 8 | c[10] << 16 | (uint32_t) c[11] << 24;
	v[3] = c[12] | c[13] << 8 | c[14] << 16 | (uint32_t) c[15] << 24;
	if (!(PWR->CR4 & PWR_CR4_C2BOOT) || (IPCC->C1TOC2SR & (1u << 1)))
		return -1;
	if (c[0] == 0x11 && c[1] == 0x0E && c[2] >= 4) ;	/* FUS: no header */
	else if (c[9] == 0x0E && c[10] >= 4) c += 8;		/* a stack */
	else return -1;
	v[0] = c[4] | c[5] << 8;
	v[1] = c[2] >= 5 ? c[7] : 0x100;
	return c[6];
}

/* A system command to CPU2 (ST's SHCI, shci_tl.c: the transport both the
 * wireless stack and FUS take), R15.1 and T30b: the packet goes into the
 * command buffer of the reference table (type 0x10, opcode, length,
 * parameters), IPCC channel 2 is flagged, and CPU2 writes its answer over
 * the command (event 0x0E "command complete": count, opcode, then the
 * status and any data) and frees the channel.  Polled: an answer takes
 * well under a tick, and nothing here may sleep.  Returns the bytes after
 * the opcode copied to rsp (the first is the status), -1 when CPU2 is not
 * ready, -2 when it did not answer, -3 when the answer is not for op. */
int hal_cpu2_shci(unsigned op, const uint8_t *par, unsigned plen,
					uint8_t *rsp, unsigned rmax)
{
	struct cpu2_map *m = &cpu2;
	volatile uint8_t *c = (volatile uint8_t *) m->cmd;
	uint32_t v[4], n;
	unsigned i, len;

	if (hal_cpu2_info(v) != 1 || plen > 255) return -1;
	if (IPCC->C1TOC2SR & (1u << 1)) return -2;	/* one still out */
	/* Tell CPU2 its event was taken (ST's HW_IPCC_SYS_EvtNot clears the
	 * flag): the wireless stack answers without it, FUS does not (seen
	 * 2026-11-27: FUS_GetState got no answer).  The event stays in the
	 * queue, which hal_cpu2_info reads. */
	if (IPCC->C2TOC1SR & (1u << 1)) IPCC->C1SCR = 1u << 1;
	for (i = 0; i < 8; i++) c[i] = 0;		/* the list node */
	c[8] = 0x10;					/* a system command */
	c[9] = (uint8_t) op;
	c[10] = (uint8_t) (op >> 8);
	c[11] = (uint8_t) plen;
	for (i = 0; i < plen; i++) c[12 + i] = par[i];
	__asm volatile ("dsb" ::: "memory");
	IPCC->C1SCR = 1u << (16 + 1);			/* channel 2: go */
	for (n = 0; IPCC->C1TOC2SR & (1u << 1); )
		if (++n == 8000000u) return -2;		/* ~0.5 s */
	/* The answer: type, event 0x0E, length, count, opcode, payload.
	 * FUS writes it at the start of the buffer (ST's shci_send: "the
	 * command complete of a system command does not have the header");
	 * the BLE light stack 1.20.0 wrote it after the 8-byte list node,
	 * where the command was.  Seen on the device, both taken. */
	if (c[0] == 0x11 && c[1] == 0x0E && c[2] >= 3
	    && (unsigned) (c[4] | c[5] << 8) == op) ;
	else if (c[9] == 0x0E && c[10] >= 3
		 && (unsigned) (c[12] | c[13] << 8) == op) c += 8;
	else return -3;
	len = c[2] - 3u;
	for (i = 0; i < len && i < rmax; i++) rsp[i] = c[6 + i];
	return (int) len;
}

/*===========================================================================*
 *				Random numbers				     *
 *===========================================================================*/
/* The true RNG (RM0434 23), clocked from the 48 MHz USB clock if it runs,
 * else from the LSI (on anyway for the watchdog; then the clock error
 * check, which wants more than HCLK/32, is off).
 * CPU2 (the radio stack) shares the RNG and the 48 MHz clock choice: ST's
 * convention (AN5289, hw_conf.h) is hardware semaphore 0 around the RNG
 * and 5 around the CLK48 setting.  CPU2 is not started yet, so they are
 * always free; when CPU2 holds one, hal_random() fails at once (-1, the
 * caller gives a short read) instead of waiting (Phase 4a, #2 review). */
#define SEM_RNG		0
#define SEM_CLK48	5

static int sem_take(int n)		/* 1-step lock: 0 if ours */
{
	return HSEM->RLR[n] == (HSEM_RLR_LOCK | HSEM_CR_COREID_CURRENT) ? 0 : -1;
}

static void sem_give(int n)
{
	HSEM->R[n] = HSEM_CR_COREID_CURRENT;	/* lock bit 0: release */
}

int hal_random(uint32_t *w)
{
	static int on;
	uint32_t n;
	int r = 0;

	if (!on) {
		RCC->AHB3ENR |= RCC_AHB3ENR_RNGEN | RCC_AHB3ENR_HSEMEN;
		(void) RCC->AHB3ENR;
		if (sem_take(SEM_CLK48) < 0) return -1;
		RCC->CCIPR &= ~RCC_CCIPR_RNGSEL;		/* CLK48 */
		if (!(RCC->CR & RCC_CR_PLLSAI1RDY)) {
			RCC->CCIPR |= 1u << RCC_CCIPR_RNGSEL_Pos;	/* LSI */
			RNG->CR |= RNG_CR_CED;
		}
		if (!(PWR->CR4 & PWR_CR4_C2BOOT))	/* else held for USB */
			sem_give(SEM_CLK48);
	}
	if (sem_take(SEM_RNG) < 0) return -1;
	on = 1;
	/* On only while we hold the semaphore, as stock's furi_hal_random:
	 * CPU2 shares the RNG (HSEM 0) and must find it off.  We left it
	 * running for good, in our configuration (T30b: looked at when FUS
	 * found every stack image not authentic). */
	RNG->CR |= RNG_CR_RNGEN;
	for (n = 0; !(RNG->SR & RNG_SR_DRDY); n++) {
		if ((RNG->SR & (RNG_SR_SECS | RNG_SR_CECS)) || n == 1000000u) {
			/* seed or clock error: restart it, the caller may
			 * try again */
			RNG->CR &= ~RNG_CR_RNGEN;
			RNG->SR = 0;
			RNG->CR |= RNG_CR_RNGEN;
			r = -1;
			break;
		}
	}
	if (r == 0) *w = RNG->DR;
	RNG->CR &= ~RNG_CR_RNGEN;
	sem_give(SEM_RNG);
	return r;
}

/*===========================================================================*
 *				LF RFID (R6.5)				     *
 *===========================================================================*/
/* Reading a 125 kHz tag, as the stock firmware does (furi_hal_rfid.c):
 * the field from TIM1 channel 1N on PB13 (AF1), 125 kHz at half duty;
 * the antenna's envelope on PC5 into COMP1 against half of VREFINT (high
 * hysteresis), COMP1's output into TIM2 channel 4 (TIM2_OR TI4_RMP).
 * PA2 (the antenna pull) and PB14 (iButton) low meanwhile, as stock.
 * TI4 cannot reset the timer, so TIM2 runs free at 1 MHz and channel 4
 * captures both edges: DMA1 channel 4 (DMAMUX request 31, TIM2_CH4)
 * writes 32-bit timestamps into the reader's buffer; the kernel makes
 * durations of them.  TIM1 and TIM2 are the IR transmitter's too: the
 * kernel runs one at a time. */
int hal_rfid_read(uint32_t dst, unsigned n, unsigned carrier)
{
	/* R9.0.4: the field: 125 kHz/50 %, PSK 62.5 kHz/25 % (stock
	 * furi_hal_rfid_tim_read_start(62500, 0.25)), 134.2 kHz/50 % */
	uint32_t t0, hz = (carrier & 3) == 1 ? 62500u
				: (carrier & 3) == 2 ? 134200u : 125000u;
	uint32_t period = (SystemCoreClock + hz / 2) / hz;

	if (n == 0) return -1;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
	(void) RCC->APB2ENR;
	ir_ready = 0;
	gpio_set(GPIOA, 2, 0);				/* antenna not pulled */
	gpio_cfg(GPIOA, 2, MODE_OUT, PULL_NONE, 0);
	hal_ow_line(1);					/* iButton low */
	gpio_cfg(GPIOC, 5, MODE_ANALOG, PULL_NONE, 0);	/* COMP1 IO1 */
	gpio_cfg(GPIOA, 15, MODE_ANALOG, PULL_NONE, 0);	/* the carrier node:
						 * after reset JTDI, pulled up */
	/* the field */
	TIM1->CR1 = 0;
	TIM1->PSC = 0;
	TIM1->ARR = period - 1;
	TIM1->CCR1 = (carrier & 3) == 1 ? period / 4 : period / 2;
	TIM1->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
	TIM1->CCMR2 = 0;
	TIM1->CCER = TIM_CCER_CC1NE;
	TIM1->BDTR = TIM_BDTR_MOE;
	TIM1->EGR = TIM_EGR_UG;
	TIM1->CR1 = TIM_CR1_CEN;
	gpio_cfg(GPIOB, 13, MODE_AF, PULL_NONE, 1);	/* TIM1_CH1N */
	/* the comparator */
	COMP1->CSR = COMP_CSR_PWRMODE_0			/* medium speed */
		| COMP_CSR_INMSEL_0 | COMP_CSR_SCALEN | COMP_CSR_BRGEN
		| COMP_CSR_HYST_1 | COMP_CSR_HYST_0;	/* IO1 (PC5), VREFINT/2 */
	COMP1->CSR |= COMP_CSR_EN;
	t0 = DWT->CYCCNT;
	while (DWT->CYCCNT - t0 < hal_cpu_hz() / 5000) ;	/* scaler: 200 us */
	/* the capture */
	TIM2->CR1 = 0;
	TIM2->DIER = 0;
	TIM2->SMCR = 0;
	TIM2->CR2 = 0;
	DMA1_Channel4->CCR = 0;
	DMAMUX1_Channel3->CCR = 31;			/* TIM2_CH4 */
	DMA1_Channel4->CPAR = (uint32_t) &TIM2->CCR4;
	DMA1_Channel4->CMAR = dst;
	DMA1_Channel4->CNDTR = n;
	DMA1->IFCR = DMA_IFCR_CGIF4;
	DMA1_Channel4->CCR = DMA_CCR_MINC | DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1
		| (carrier & HAL_RFID_STREAM ? DMA_CCR_CIRC | DMA_CCR_HTIE
				| DMA_CCR_TCIE : 0)		/* R9.0.5 */
		| DMA_CCR_EN;
	TIM2->OR = TIM2_OR_TI4_RMP_0;			/* TI4 = COMP1 */
	TIM2->PSC = SystemCoreClock / 1000000u - 1;	/* 1 us */
	TIM2->ARR = 0xFFFFFFFF;
	TIM2->CCMR1 = 0;
	TIM2->CCMR2 = 1u << TIM_CCMR2_CC4S_Pos;		/* IC4 on TI4 */
	TIM2->CCER = TIM_CCER_CC4E | TIM_CCER_CC4P | TIM_CCER_CC4NP;
	TIM2->CNT = 0;
	TIM2->CR1 = TIM_CR1_URS;
	TIM2->EGR = TIM_EGR_UG;
	TIM2->SR = 0;
	TIM2->DIER = TIM_DIER_CC4DE;
	t0 = (COMP1->CSR & COMP_CSR_VALUE) != 0;	/* the level the first
						 * captured edge leaves */
	TIM2->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
	return (int) t0;
}

/* R9.0.5: the stream's DMA interrupts (the half and the end of the ring):
 * which halves have been filled since the last call (bit 0 the first, bit 1
 * the second), acked. */
int hal_rfid_stream_irq(void)
{
	return DMA1_Channel4_IRQn;
}

unsigned hal_rfid_stream_ack(void)
{
	uint32_t isr = DMA1->ISR;
	unsigned r = (isr & DMA_ISR_HTIF4 ? 1u : 0) | (isr & DMA_ISR_TCIF4 ? 2u : 0);

	DMA1->IFCR = DMA_IFCR_CHTIF4 | DMA_IFCR_CTCIF4 | DMA_IFCR_CGIF4;
	return r;
}

void hal_rfid_stop(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
	(void) RCC->APB2ENR;
	TIM2->DIER = 0;
	TIM2->CR1 = 0;
	TIM2->CCER = 0;
	TIM2->CCMR1 = 0;				/* (#302: the Sub-GHz stream's) */
	TIM2->CCMR2 = 0;
	TIM2->OR = 0;
	TIM2->SR = 0;
	DMA1_Channel4->CCR = 0;
	DMAMUX1_Channel3->CCR = 0;
	gpio_cfg(GPIOA, 1, MODE_ANALOG, PULL_NONE, 0);	/* GDO0, as stock */
	COMP1->CSR = 0;
	TIM1->CR1 = 0;
	TIM1->CCER = 0;
	TIM1->BDTR = 0;
	ir_ready = 0;
	gpio_set(GPIOB, 13, 0);				/* field off */
	gpio_cfg(GPIOB, 13, MODE_OUT, PULL_NONE, 0);
	gpio_set(GPIOA, 2, 1);				/* as stock's reset */
	hal_ow_line(0);					/* iButton released */
	gpio_cfg(GPIOC, 5, MODE_ANALOG, PULL_NONE, 0);
}

/* R6.5 part 3: a T5577 block read needs the field to stay up between the
 * read command (gaps, played by the IR transmitter onto the antenna,
 * hal_ir_target) and the tag's reply (the comparator captured below);
 * dropping the carrier would reset the tag.  hal_rfid_field holds it up
 * through the whole exchange and hal_rfid_capture arms the comparator on
 * top of the running field, where hal_rfid_read raises a field of its
 * own.  The carrier is the transmitter's (TIM1 CH1N), so a gap is its
 * outputs off, as when writing; the capture is TIM2 as in hal_rfid_read. */
void hal_rfid_field(int on)
{
	if (on) {
		hal_ir_freq(125000, 50);
		hal_ir_target(1);		/* pins: iButton low, PC5/PA15
						 * analog; the carrier on PB13;
						 * PA2 high, ready for a command */
		hal_ir_carrier(1);		/* TIM1 CH1N, 125 kHz, held */
	} else {
		hal_rfid_stop();		/* TIM2/COMP1 off, TIM1 down */
		hal_ir_target(0);		/* the transmitter back to IR */
	}
}

/* The antenna pull pin (PA2) between the two phases of a block read: a
 * command wants it released (high), the capture wants it pulled low so the
 * envelope swings on PC5, as stock's pin_pull_release / pin_pull_pulldown.
 * The field stays up throughout. */
void hal_rfid_pull(int release)
{
	gpio_set(GPIOA, 2, release ? 1 : 0);
}

/* Stop the comparator capture but leave the field (and COMP1) up, so the
 * next command and reply find the tag still powered.  hal_rfid_field(0)
 * ends the whole session. */
void hal_rfid_capture_stop(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->DIER = 0;
	TIM2->CR1 = 0;
	TIM2->CCER = 0;
	TIM2->CCMR2 = 0;
	TIM2->OR = 0;
	TIM2->SR = 0;
	DMA1_Channel4->CCR = 0;
	DMAMUX1_Channel3->CCR = 0;
}

/*===========================================================================*
 *			LF RFID emulation (R6.5 part 4, R8.4)		     *
 *===========================================================================*/
/* Pretending to be a 125 kHz tag, as the stock firmware does
 * (furi_hal_rfid_tim_emulate_dma_start): the *reader* makes the field, so
 * our own carrier (PB13) stays off and TIM2 counts the reader's carrier
 * instead of the CPU clock - external clock mode 2 on ETR, which is the
 * antenna's carrier node PA15 (AF2), inverted as stock.  The tag answers by
 * loading the antenna, which here is PWM on the pull pin PA2 (AF1, TIM2
 * channel 3): a segment's length comes from ARR and how long the pull is on
 * from CCR3.  Two circular DMA channels (DMA1 5 and 6, both on the TIM2
 * update request) feed those from the caller's pinned buffers, so the whole
 * pattern repeats for as long as the reader's field lasts with no CPU work
 * at all - the timing is the reader's own clock, so it cannot drift.
 *
 * dur[i] is a segment in carrier periods minus 1 (what ARR counts to) and
 * pulse[i] how many of them the pull is on (0 none, dur[i] + 1 all).  Both
 * are physical addresses of n 32-bit words; the caller pins the memory. */
int hal_rfid_emulate(uint32_t dur, uint32_t pulse, unsigned n)
{
	unsigned irq = n & HAL_RFID_REFILL;	/* R9.0.6: a ring, refilled */

	n &= ~HAL_RFID_REFILL;
	if (n == 0) return -1;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	ir_ready = 0;				/* TIM1/TIM2 are ours now */
	/* pins, as stock furi_hal_rfid_pins_emulate */
	hal_ow_line(1);				/* iButton low */
	gpio_set(GPIOB, 13, 0);			/* our own field off */
	gpio_cfg(GPIOB, 13, MODE_OUT, PULL_NONE, 0);
	gpio_cfg(GPIOA, 2, MODE_AF, PULL_NONE, 1);	/* pull: TIM2_CH3 */
	gpio_cfg(GPIOA, 15, MODE_AF, PULL_NONE, 2);	/* carrier in: TIM2_ETR */
	/* TIM2 clocked by the reader's carrier */
	TIM2->CR1 = 0;
	TIM2->DIER = 0;
	TIM2->CR2 = 0;
	TIM2->PSC = 0;
	TIM2->ARR = 1;
	TIM2->OR = 0;
	TIM2->SMCR = TIM_SMCR_ECE | TIM_SMCR_ETP;	/* external clock 2, inverted */
	TIM2->CCMR1 = 0;
	TIM2->CCMR2 = 6u << TIM_CCMR2_OC3M_Pos;		/* PWM mode 1 on CH3 */
	TIM2->CCR3 = 1;
	TIM2->CCER = TIM_CCER_CC3E;			/* CH3 out, active high */
	TIM2->EGR = TIM_EGR_UG;
	TIM2->SR = 0;
	/* the pattern: ARR from dur[], CCR3 from pulse[], round and round */
	DMA1_Channel5->CCR = 0;
	DMA1_Channel6->CCR = 0;
	DMA1->IFCR = DMA_IFCR_CGIF5 | DMA_IFCR_CGIF6;
	DMAMUX1_Channel4->CCR = 32;			/* TIM2_UP -> channel 5 */
	DMA1_Channel5->CPAR = (uint32_t) &TIM2->ARR;
	DMA1_Channel5->CMAR = dur;
	DMA1_Channel5->CNDTR = n;
	DMAMUX1_Channel5->CCR = 32;			/* TIM2_UP -> channel 6 */
	DMA1_Channel6->CPAR = (uint32_t) &TIM2->CCR3;
	DMA1_Channel6->CMAR = pulse;
	DMA1_Channel6->CNDTR = n;
	DMA1_Channel5->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC
		| DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1
		| (irq ? DMA_CCR_HTIE | DMA_CCR_TCIE : 0) | DMA_CCR_EN;
	DMA1_Channel6->CCR = DMA_CCR_DIR | DMA_CCR_MINC | DMA_CCR_CIRC
		| DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1 | DMA_CCR_EN;
	TIM2->DIER = TIM_DIER_UDE;		/* every update takes the next pair */
	TIM2->CNT = 0;
	TIM2->CR1 = TIM_CR1_CEN;
	return 0;
}

/* R9.0.6: the emulation ring's half/complete interrupt (channel 6 takes
 * its pairs on the same trigger, so channel 5's tells for both): which
 * halves have been played since the last call (bit 0 the first), acked. */
int hal_rfid_emu_irq(void)
{
	return DMA1_Channel5_IRQn;
}

unsigned hal_rfid_emu_ack(void)
{
	uint32_t isr = DMA1->ISR;
	unsigned r = (isr & DMA_ISR_HTIF5 ? 1u : 0) | (isr & DMA_ISR_TCIF5 ? 2u : 0);

	DMA1->IFCR = DMA_IFCR_CHTIF5 | DMA_IFCR_CTCIF5 | DMA_IFCR_CGIF5;
	return r;
}

void hal_rfid_emulate_stop(void)
{
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	TIM2->DIER = 0;
	TIM2->CR1 = 0;
	TIM2->CCER = 0;
	TIM2->CCMR2 = 0;
	TIM2->SMCR = 0;
	TIM2->SR = 0;
	DMA1_Channel5->CCR = 0;
	DMA1_Channel6->CCR = 0;
	DMAMUX1_Channel4->CCR = 0;
	DMAMUX1_Channel5->CCR = 0;
	DMA1->IFCR = DMA_IFCR_CGIF5 | DMA_IFCR_CGIF6;
	gpio_set(GPIOA, 2, 1);			/* the pull released, as a reset */
	gpio_cfg(GPIOA, 2, MODE_OUT, PULL_NONE, 0);
	gpio_cfg(GPIOA, 15, MODE_ANALOG, PULL_NONE, 0);
	hal_ow_line(0);				/* iButton released */
}

/* Arm the comparator capture on the already-running field (hal_rfid_field
 * must be on).  The second half of hal_rfid_read: COMP1 on PC5 into TIM2
 * channel 4, DMA to dst, n 32-bit timestamps.  hal_pulse_left and
 * hal_rfid_stop (through hal_rfid_field(0)) belong to it as to a read. */
int hal_rfid_capture(uint32_t dst, unsigned n)
{
	uint32_t t0;

	if (n == 0) return -1;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	(void) RCC->APB1ENR1;
	/* PA2 was pulled low before the settle (hal_rfid_pull(0), the driver),
	 * so the envelope on PC5 is already swinging by now. */
	gpio_cfg(GPIOC, 5, MODE_ANALOG, PULL_NONE, 0);	/* COMP1 IO1 */
	if (!(COMP1->CSR & COMP_CSR_EN)) {
		COMP1->CSR = COMP_CSR_PWRMODE_0		/* medium speed */
			| COMP_CSR_INMSEL_0 | COMP_CSR_SCALEN | COMP_CSR_BRGEN
			| COMP_CSR_HYST_1 | COMP_CSR_HYST_0;	/* PC5, VREFINT/2 */
		COMP1->CSR |= COMP_CSR_EN;
		t0 = DWT->CYCCNT;
		while (DWT->CYCCNT - t0 < hal_cpu_hz() / 5000) ;	/* 200 us */
	}
	TIM2->CR1 = 0;
	TIM2->DIER = 0;
	TIM2->SMCR = 0;
	TIM2->CR2 = 0;
	DMA1_Channel4->CCR = 0;
	DMAMUX1_Channel3->CCR = 31;			/* TIM2_CH4 */
	DMA1_Channel4->CPAR = (uint32_t) &TIM2->CCR4;
	DMA1_Channel4->CMAR = dst;
	DMA1_Channel4->CNDTR = n;
	DMA1_Channel4->CCR = DMA_CCR_MINC | DMA_CCR_PSIZE_1 | DMA_CCR_MSIZE_1
							| DMA_CCR_EN;
	TIM2->OR = TIM2_OR_TI4_RMP_0;			/* TI4 = COMP1 */
	TIM2->PSC = SystemCoreClock / 1000000u - 1;	/* 1 us */
	TIM2->ARR = 0xFFFFFFFF;
	TIM2->CCMR1 = 0;
	TIM2->CCMR2 = 1u << TIM_CCMR2_CC4S_Pos;		/* IC4 on TI4 */
	TIM2->CCER = TIM_CCER_CC4E | TIM_CCER_CC4P | TIM_CCER_CC4NP;
	TIM2->CNT = 0;
	TIM2->CR1 = TIM_CR1_URS;
	TIM2->EGR = TIM_EGR_UG;
	TIM2->SR = 0;
	TIM2->DIER = TIM_DIER_CC4DE;
	TIM2->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
	return 0;
}

/*===========================================================================*
 *			iButton: Cyfral and Metakom (R6.7 part 3b)	     *
 *===========================================================================*/
/* Reading, as the stock firmware does it (protocol_group_misc.c, read):
 * the pull pin PA2 low, the iButton pin released to analog, the key's
 * signal sensed "through the analog part of the RFID schematic": COMP1 on
 * PC5 against half of VREFINT, its output into TIM2 channel 4, both edges
 * timestamped by DMA1 channel 4 (the same capture the LF RFID read uses,
 * hal_rfid_capture, but with no 125 kHz field).  The kernel waits on the ms
 * timer and makes durations of the timestamps.  The stock firmware also
 * powers the 5 V rail for a read (power_enable_otg); here it is up to the
 * user (`echo otg on > /dev/power`) until the hardware test says whether
 * it is needed.  Returns the comparator's level when the capture began:
 * the segment after the first edge has that level in the stock firmware's
 * sense (stock hands the callback "output is low"). */
int hal_ib_capt_arm(uint32_t dst, unsigned nwords)
{
	uint32_t t0;
	int lv;

	if (nwords == 0) return 1;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	RCC->APB1ENR1 |= RCC_APB1ENR1_TIM2EN;
	RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
	(void) RCC->APB2ENR;
	ir_ready = 0;
	TIM1->CR1 = 0;				/* ours, not the IR LED's */
	TIM1->BDTR = 0;
	TIM1->CCER = 0;
	gpio_set(GPIOA, 2, 0);			/* the pull pin low, as stock */
	gpio_cfg(GPIOA, 2, MODE_OUT, PULL_NONE, 0);
	gpio_set(GPIOB, 13, 0);			/* no field */
	gpio_cfg(GPIOB, 13, MODE_OUT, PULL_NONE, 0);
	gpio_cfg(GPIOB, 14, MODE_ANALOG, PULL_NONE, 0);	/* the contact: reset */
	gpio_cfg(GPIOA, 15, MODE_ANALOG, PULL_NONE, 0);
	gpio_cfg(GPIOC, 5, MODE_ANALOG, PULL_NONE, 0);	/* COMP1 IO1 */
	COMP1->CSR = COMP_CSR_PWRMODE_0			/* medium speed */
		| COMP_CSR_INMSEL_0 | COMP_CSR_SCALEN | COMP_CSR_BRGEN
		| COMP_CSR_HYST_1 | COMP_CSR_HYST_0;	/* PC5, VREFINT/2 */
	COMP1->CSR |= COMP_CSR_EN;
	t0 = DWT->CYCCNT;
	while (DWT->CYCCNT - t0 < hal_cpu_hz() / 5000) ;	/* scaler: 200 us */
	lv = (COMP1->CSR & COMP_CSR_VALUE) != 0;
	if (hal_rfid_capture(dst, nwords) < 0) return -1;
	return lv;
}

void hal_ib_capt_stop(void)
{
	hal_rfid_stop();			/* the DMA, TIM2, COMP1 off; PA2
						 * high, the contact released */
}

/* Playing a pulse list, as the stock firmware does it (furi_hal_ibutton
 * emulate_start): TIM1 counts microseconds, its update interrupt starts
 * the next segment (ARR = its length, the pin = its level) and the pin is
 * the iButton contact, open drain: level 1 released (high), 0 pulled low.
 * The list stays in the caller's memory (pinned by the kernel) and is
 * read from there: no copy.  RAM: 12 B. */
static const uint16_t *ib_ps;
static uint16_t ib_pn, ib_pi;
static uint8_t ib_plv;

int hal_ib_play_irq(void)
{
	return TIM1_UP_TIM16_IRQn;
}

int hal_ib_play(uint32_t src, unsigned n, int level)
{
	RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
	(void) RCC->APB2ENR;
	TIM1->DIER = 0;
	TIM1->CR1 = 0;
	TIM1->SR = 0;
	if (n == 0) {				/* stop: the contact released */
		hal_ow_line(0);
		return 0;
	}
	ir_ready = 0;				/* TIM1 is the IR LED's too */
	ib_ps = (const uint16_t *) src;
	ib_pn = (uint16_t) n;
	ib_pi = 0;
	ib_plv = (uint8_t) (level != 0);
	hal_ow_line(!ib_plv);
	TIM1->BDTR = 0;				/* no outputs of its own */
	TIM1->CCER = 0;
	TIM1->PSC = hal_cpu_hz() / 1000000u - 1;	/* 1 us */
	TIM1->ARR = ib_ps[0] - 1u;
	TIM1->CNT = 0;
	TIM1->EGR = TIM_EGR_UG;
	TIM1->SR = 0;
	TIM1->DIER = TIM_DIER_UIE;
	TIM1->CR1 = TIM_CR1_URS | TIM_CR1_CEN;
	return 0;
}

/* The update interrupt: the segment is over, the next starts. */
void hal_ib_play_step(void)
{
	unsigned i = (unsigned) ib_pi + 1;

	TIM1->SR = ~TIM_SR_UIF;
	if (ib_pn == 0) return;
	if (i >= ib_pn) i = 0;
	ib_pi = (uint16_t) i;
	ib_plv ^= 1;
	TIM1->ARR = ib_ps[i] - 1u;
	gpio_set(GPIOB, 14, ib_plv);		/* open drain: 1 released */
}

/*===========================================================================*
 *				Speaker					     *
 *===========================================================================*/
/* Piezo on PB8, TIM16 channel 1 (AF14), as on the stock firmware.  A
 * square wave at hz, 0 stops it.  The timer counts at the core clock (or
 * a fraction of it for low notes, so ARR fits 16 bits); the fine count
 * keeps quiet volumes audible.  The duty is 50 % * (vol/100)^3, the cubic
 * curve of stock furi_hal_speaker_start: vol 100 is the old full sound. */
void hal_tone(unsigned hz, unsigned vol)
{
	static int ready;
	uint32_t arr, psc, ccr;

	if (hz == 0 || vol == 0) {
		if (ready) {
			TIM16->CR1 &= ~TIM_CR1_CEN;
			gpio_cfg(GPIOB, 8, MODE_ANALOG, PULL_NONE, 0);
		}
		return;
	}
	if (!ready) {
		RCC->APB2ENR |= RCC_APB2ENR_TIM16EN;
		(void) RCC->APB2ENR;
		TIM16->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
		TIM16->CCER = TIM_CCER_CC1E;
		TIM16->BDTR = TIM_BDTR_MOE;
		ready = 1;
	}
	if (hz < 20) hz = 20;
	if (vol > 100) vol = 100;
	psc = SystemCoreClock / (hz * 65536u);	/* 0 above ~980 Hz at 64 MHz */
	arr = SystemCoreClock / ((psc + 1) * hz) - 1;
	/* (vol^3 / 100) is 0..10000; arr / 2 * 10000 fits 32 bits */
	ccr = (arr / 2) * (vol * vol * vol / 100) / 10000u;
	if (ccr == 0) ccr = 1;
	/* The same tone again (the next 100 ms piece of a long note): leave
	 * the timer running, as a new start resets its phase and the note
	 * sounds rough (the user, 2026-11-26). */
	if ((TIM16->CR1 & TIM_CR1_CEN) && TIM16->PSC == psc
	    && TIM16->ARR == arr && TIM16->CCR1 == ccr) return;
	TIM16->PSC = psc;
	TIM16->ARR = arr;
	TIM16->CCR1 = ccr;
	TIM16->EGR = TIM_EGR_UG;
	gpio_cfg(GPIOB, 8, MODE_AF, PULL_NONE, 14);
	TIM16->CR1 |= TIM_CR1_ARPE | TIM_CR1_CEN;
}


/*
 * Internal flash: the program text cache (mm/fcache.c).
 *
 * The bottom 128 KB hold the system image (kernel, MM, FS, INIT: about
 * 64 KB now), the next 384 KB the cache, all far below the CPU2 radio stack
 * and FUS at the top of the 1 MB.  CPU2 is not started, so no semaphores
 * are needed (RM0434 3.3.8: with CPU2 running, PESD and the flash
 * semaphore would have to be respected).  The CPU stalls on flash reads
 * while a page is erased or written; the kernel masks interrupts.
 */
#define FLASH_KEY1v	0x45670123u
#define FLASH_KEY2v	0xCDEF89ABu
#define FLASH_ERRS	(FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR \
			 | FLASH_SR_PGAERR | FLASH_SR_SIZERR | FLASH_SR_PGSERR \
			 | FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR \
			 | FLASH_SR_OPTVERR)

/* Flash layout (spec Phase 2b, R2b.1), worked out at boot from where CPU2's
 * secure area starts (SFSA, FLASH->SFR, in 4 KB pages), as the stock
 * firmware does:
 *	0x08000000  system image (kernel, MM, FS, INIT)	HAL_SYS_IMAGE
 *	0x08030000  shared libc (R2b.9, <minix/shlibc.h>)	HAL_LIBC_SIZE
 *	0x0803C000  shared libfuri + libapp (R2b.9)	  (both, 96 KB)
 *	0x08048000  installed programs (Phase 2b)	up to the cache
 *	...         run-time text cache			HAL_CACHE_SIZE
 *	SFSA - 4 pages: a margin (it was 16 "for FUS": with a stack swap that
 *		    deletes first FUS needs none below SFSA, AN5185; stock
 *		    keeps none)
 *	SFSA ..     CPU2 wireless stack and FUS, never touched
 */
#define HAL_SYS_IMAGE	0x30000u	/* 192 KB */
#define HAL_LIBC_SIZE	0x18000u	/* 96 KB, SHLIBC_MAX + SHFURI_MAX;
					 * both libraries are in minix.bin */
#define HAL_CACHE_SIZE	0x20000u	/* 128 KB */
#define GUARD_PAGES	4u
/* T30b, changing CPU2's stack.  With FUS alone in CPU2's flash (the old
 * stack deleted) SFSA is FUS's own start, above every stack's install
 * address: seen from HAL_NOSTACK up.  Then there is no cache (it would be
 * laid where the new image goes) and the installed region is the one of
 * the BLE light stack, so MM sees no change; the image is written into
 * the radio window, HAL_RADIO_FLOOR .. SFSA.  With a stack installed the
 * window is the guard, which nothing of ours uses: a place to try the
 * writer. */
#define HAL_NOSTACK	(FLASH_BASE + 0xF0000u)
#define HAL_RADIO_FLOOR	(FLASH_BASE + 0xB3000u)

void hal_flash_layout(uint32_t *l)
{
	uint32_t sfsa, top, inst = FLASH_BASE + HAL_SYS_IMAGE + HAL_LIBC_SIZE;

	sfsa = FLASH_BASE + (FLASH->SFR & FLASH_SFR_SFSA) * HAL_FLASH_PAGE;
	top = sfsa - GUARD_PAGES * HAL_FLASH_PAGE;
	l[HAL_FL_SFSA] = sfsa;
	if (sfsa < inst + GUARD_PAGES * HAL_FLASH_PAGE + HAL_CACHE_SIZE) {
		/* no room: neither region is used */
		l[HAL_FL_INST] = l[HAL_FL_INST_SIZE] = 0;
		l[HAL_FL_CACHE] = l[HAL_FL_CACHE_SIZE] = 0;
		l[HAL_FL_ROM] = l[HAL_FL_ROM_SIZE] = 0;
		return;
	}
	l[HAL_FL_ROM] = inst + HAL_ROM_OFF;
	l[HAL_FL_ROM_SIZE] = HAL_ROM_SIZE;
	if (sfsa >= HAL_NOSTACK) {		/* FUS alone: see above */
		l[HAL_FL_CACHE] = l[HAL_FL_CACHE_SIZE] = 0;
		l[HAL_FL_INST] = inst;
		l[HAL_FL_INST_SIZE] = HAL_RADIO_FLOOR - inst;
		return;
	}
	l[HAL_FL_CACHE] = top - HAL_CACHE_SIZE;
	l[HAL_FL_CACHE_SIZE] = HAL_CACHE_SIZE;
	l[HAL_FL_INST] = inst;
	l[HAL_FL_INST_SIZE] = top - HAL_CACHE_SIZE - inst;
}

/* Whether [addr, addr+len) is in the radio window (no table: small). */
int hal_flash_radio_ok(uint32_t addr, uint32_t len)
{
	uint32_t sfsa = FLASH_BASE + (FLASH->SFR & FLASH_SFR_SFSA) * HAL_FLASH_PAGE;
	uint32_t lo = sfsa >= HAL_NOSTACK ? HAL_RADIO_FLOOR
					  : sfsa - GUARD_PAGES * HAL_FLASH_PAGE;

	return len != 0 && addr >= lo && addr < sfsa && len <= sfsa - addr;
}

/* For the MEMORY task, whose stack has no room for the layout table. */
uint32_t hal_rom_base(void)
{
	return FLASH_BASE + HAL_SYS_IMAGE + HAL_LIBC_SIZE + HAL_ROM_OFF;
}

void hal_flash_region(uint32_t *base, uint32_t *size)
{
	uint32_t l[HAL_FL_N];

	hal_flash_layout(l);
	*base = l[HAL_FL_CACHE];
	*size = l[HAL_FL_CACHE_SIZE];
}

uint32_t hal_flash_sr;			/* SR of the last failure, for the log */

/* A page erase takes about 22 ms, a double word 82 us (DS11929); give up
 * after a few hundred ms instead of hanging with interrupts masked. */
static int flash_wait(void)
{
	uint32_t sr, n;

	for (n = 0; FLASH->SR & FLASH_SR_BSY; )
		if (++n == 4000000u) goto timeout;
	sr = FLASH->SR;
	/* CFGBSY stays set until the error flags are cleared (RM0434 3.3.8):
	 * clear them first, then wait for it. */
	FLASH->SR = (sr & FLASH_ERRS) | FLASH_SR_EOP;	/* write 1 to clear */
	for (n = 0; FLASH->SR & FLASH_SR_CFGBSY; )
		if (++n == 4000000u) goto timeout;
	if (sr & FLASH_ERRS) {
		hal_flash_sr = sr;
		return -1;
	}
	return 0;
timeout:
	hal_flash_sr = FLASH->SR | 0x80000000u;
	return -1;
}

/* Erase and program are timed by HSI16 (RM0434 3.3.5); the clock setup
 * runs from HSE and the PLL only, so start it here.  It stays on. */
static int flash_unlock(void)
{
	uint32_t n;

	if (!(RCC->CR & RCC_CR_HSIRDY)) {
		RCC->CR |= RCC_CR_HSION;
		for (n = 0; !(RCC->CR & RCC_CR_HSIRDY); )
			if (++n == 1000000u) {
				hal_flash_sr = 0x40000000u;
				return -1;
			}
	}
	if (FLASH->CR & FLASH_CR_LOCK) {
		FLASH->KEYR = FLASH_KEY1v;
		FLASH->KEYR = FLASH_KEY2v;
	}
	if (FLASH->CR & FLASH_CR_LOCK) {
		hal_flash_sr = 0x20000000u;
		return -1;
	}
	/* Errors left over - the ROM DFU loader jumps to the image without a
	 * reset and leaves its flags set - are not ours; only busy counts. */
	(void) flash_wait();
	return (FLASH->SR & (FLASH_SR_BSY | FLASH_SR_CFGBSY)) ? -1 : 0;
}

/* Flash ECC (R4a.5): what the controller saw since the last call, cleared
 * (HAL_ECC_*), and the double word's address.  A double error also raises
 * the NMI; a corrected one only sets the flag (the kernel polls it). */
unsigned hal_flash_ecc(uint32_t *addr)
{
	uint32_t r = FLASH->ECCR;

	*addr = FLASH_BASE + (r & FLASH_ECCR_ADDR_ECC) * 8;
	if (!(r & (FLASH_ECCR_ECCC | FLASH_ECCR_ECCD))) return 0;
	FLASH->ECCR = r;			/* the flags are write-1-to-clear */
	return (r & FLASH_ECCR_ECCD ? HAL_ECC_DOUBLE : 0)
				| (r & FLASH_ECCR_ECCC ? HAL_ECC_FIXED : 0);
}

/* The flash caches may hold the old contents of a page just changed. */
static void flash_done(void)
{
	uint32_t acr = FLASH->ACR & (FLASH_ACR_ICEN | FLASH_ACR_DCEN);

	/* FLASH_CR is not writable while the controller is busy (bus error) */
	if (FLASH->SR & (FLASH_SR_BSY | FLASH_SR_CFGBSY)) return;
	FLASH->CR |= FLASH_CR_LOCK;
	FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
	FLASH->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;
	FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
	FLASH->ACR |= acr;
}

static int flash_ok(uint32_t addr, unsigned len)	/* cache, installed */
{
	uint32_t l[HAL_FL_N];

	if (hal_flash_radio_ok(addr, len)) return 1;	/* the kernel asked */
	hal_flash_layout(l);
	if (addr >= l[HAL_FL_CACHE] && len <= l[HAL_FL_CACHE_SIZE]
			&& addr - l[HAL_FL_CACHE] <= l[HAL_FL_CACHE_SIZE] - len)
		return 1;
	return addr >= l[HAL_FL_INST] && len <= l[HAL_FL_INST_SIZE]
		&& addr - l[HAL_FL_INST] <= l[HAL_FL_INST_SIZE] - len;
}

/* With CPU2 running (R2b.10), ST's rule for CPU1's flash operations
 * (AN5289 4.7, as stock's furi_hal_flash_begin_with_core2): hold HSEM 2
 * (the flash controller), wait until the controller is not suspended
 * (PESD: CPU2's radio timing), and take HSEM 7 so CPU2 makes no
 * time-critical request meanwhile (not while CPU2 holds HSEM 6, its own
 * request).  Interrupts are off only while the semaphores are taken, not
 * for the 22 ms of an erase.  Without CPU2: nothing. */
#define C2_SEM_FLASH	2
#define C2_SEM_CPU2	7
#define C2_SEM_CPU1	6
#define C2_COREID	(4u << HSEM_R_COREID_Pos)	/* CPU1 */

static int c2_sem_take(unsigned n)	/* one-step lock: 0 taken */
{
	return HSEM->RLR[n] == (HSEM_RLR_LOCK | C2_COREID) ? 0 : -1;
}

static void c2_sem_give(unsigned n)
{
	HSEM->R[n] = C2_COREID;
}

static int c2_flash_begin(void)
{
	uint32_t n, pm;

	if (!(PWR->CR4 & PWR_CR4_C2BOOT)) return 0;
	for (n = 0; c2_sem_take(C2_SEM_FLASH) < 0; )
		if (++n == 4000000u) return -1;
	for (n = 0; ; ) {
		if (++n == 4000000u) {
			c2_sem_give(C2_SEM_FLASH);
			hal_flash_sr = 0x10000000u;	/* CPU2 kept it */
			return -1;
		}
		if (FLASH->SR & FLASH_SR_PESD) continue;
		pm = __get_PRIMASK();
		__disable_irq();
		if ((HSEM->R[C2_SEM_CPU1] & HSEM_R_LOCK)
		    || c2_sem_take(C2_SEM_CPU2) < 0) {
			__set_PRIMASK(pm);
			continue;
		}
		__set_PRIMASK(pm);
		return 0;
	}
}

static void c2_flash_end(void)
{
	if (!(PWR->CR4 & PWR_CR4_C2BOOT)) return;
	c2_sem_give(C2_SEM_CPU2);
	c2_sem_give(C2_SEM_FLASH);
}

int hal_flash_erase(uint32_t page_addr)
{
	int r;

	if (page_addr % HAL_FLASH_PAGE != 0
			|| !flash_ok(page_addr, HAL_FLASH_PAGE)) return -1;
	if (c2_flash_begin() < 0) return -1;
	if (flash_unlock() < 0) { flash_done(); c2_flash_end(); return -1; }
	FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB) | FLASH_CR_PER
		| (((page_addr - FLASH_BASE) / HAL_FLASH_PAGE)
							<< FLASH_CR_PNB_Pos);
	FLASH->CR |= FLASH_CR_STRT;
	r = flash_wait();
	FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
	flash_done();
	c2_flash_end();
	return r;
}

int hal_flash_write(uint32_t dst, const void *src, unsigned len)
{
	const uint8_t *s = src;
	uint32_t w0, w1;
	int r = 0;

	if (dst % 8 != 0 || len % 8 != 0 || !flash_ok(dst, len)) return -1;
	if (c2_flash_begin() < 0) return -1;
	if (flash_unlock() < 0) { flash_done(); c2_flash_end(); return -1; }
	FLASH->CR |= FLASH_CR_PG;
	for (; len > 0; len -= 8, dst += 8, s += 8) {
		w0 = s[0] | s[1] << 8 | s[2] << 16 | (uint32_t) s[3] << 24;
		w1 = s[4] | s[5] << 8 | s[6] << 16 | (uint32_t) s[7] << 24;
		if (w0 == 0xFFFFFFFFu && w1 == 0xFFFFFFFFu) continue;
		{	/* both words, in order, nothing in between */
			uint32_t pm = __get_PRIMASK();

			__disable_irq();
			*(volatile uint32_t *) dst = w0;
			*(volatile uint32_t *) (dst + 4) = w1;
			__set_PRIMASK(pm);
		}
		if ((r = flash_wait()) < 0) break;
	}
	FLASH->CR &= ~FLASH_CR_PG;
	flash_done();
	c2_flash_end();
	return r;
}
