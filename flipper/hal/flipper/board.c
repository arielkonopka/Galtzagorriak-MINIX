/* Flipper Zero (STM32WB55RG) board support.
 *
 * Pin assignments are taken from flipperzero-firmware
 * targets/f7/furi_hal/furi_hal_resources.[ch]; the display init sequence
 * from lib/u8g2/u8g2_glue.c.  CPU2 (radio core) is never started.
 */
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

	RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | (3u << RCC_CFGR_SW_Pos);
	while ((RCC->CFGR & RCC_CFGR_SWS) != (3u << RCC_CFGR_SWS_Pos)) ;
	SystemCoreClock = 64000000;
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
#define SPI_DIV_DISPLAY	3	/* /16 = 4 MHz */
#define SPI_DIV_SD_INIT	7	/* /256 = 250 kHz, card identification */
#define SPI_DIV_SD_FAST	2	/* /8 = 8 MHz */

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
}

void hal_lcd_flush(const uint8_t *fb)
{
	uint8_t addr[3];
	unsigned page, x;

	for (page = 0; page < LCD_H / 8; page++) {
		addr[0] = 0xB0 | page;		/* page address */
		addr[1] = 0x10;			/* column high nibble = 0 */
		addr[2] = 0x00;			/* column low nibble = 0 */
		lcd_cmds(addr, sizeof addr);
		LCD_DATA(1);
		LCD_CS(1);
		for (x = 0; x < LCD_W; x++) spi2_xfer(fb[page * LCD_W + x]);
		spi2_wait_idle();
		LCD_CS(0);
	}
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

static int lp5562_write(uint8_t reg, uint8_t val)
{
	uint8_t b[2] = { reg, val };

	return i2c1_write(LP5562_ADDR, b, 2);
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
	lp5562_write(0x0E, level > 255 ? 255 : level);
}

void hal_led(unsigned r, unsigned g, unsigned b)
{
	lp5562_write(0x04, r);
	lp5562_write(0x03, g);
	lp5562_write(0x02, b);
}

/*===========================================================================*
 *				microSD card (SPI mode)			     *
 *===========================================================================*/
/* SD Physical Layer Simplified Specification, SPI mode.  Polled, one block
 * per command (CMD17/CMD24); SDSC and SDHC/SDXC cards. */
#define CMD0	0	/* GO_IDLE_STATE */
#define CMD8	8	/* SEND_IF_COND */
#define CMD9	9	/* SEND_CSD */
#define CMD16	16	/* SET_BLOCKLEN */
#define CMD17	17	/* READ_SINGLE_BLOCK */
#define CMD24	24	/* WRITE_BLOCK */
#define CMD55	55	/* APP_CMD */
#define CMD58	58	/* READ_OCR */
#define ACMD41	41	/* SD_SEND_OP_COND */
#define R1_IDLE		0x01
#define R1_ILLEGAL	0x04
#define TOKEN_START	0xFE
#define SD_TIMEOUT_MS	500

static int sd_ready;		/* initialised */
static int sd_block_addr;	/* SDHC/SDXC: block (not byte) addresses */
static uint32_t sd_nsectors;

static void sd_select(int on)
{
	SD_CS(on);
	spi2_xfer(0xFF);	/* give the card clocks to release DO */
}

static int sd_wait_ready(void)
{
	uint32_t start = DWT->CYCCNT;

	while (spi2_xfer(0xFF) != 0xFF)
		if (ms_since(start) > SD_TIMEOUT_MS) return -1;
	return 0;
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg)
{
	uint8_t r, crc = 0x01;
	int n;

	if (cmd == ACMD41) {
		r = sd_cmd(CMD55, 0);
		if (r > R1_IDLE) return r;
	}
	sd_select(0);
	sd_select(1);
	if (cmd != CMD0 && sd_wait_ready() < 0) return 0xFF;

	if (cmd == CMD0) crc = 0x95;	/* CRC is only checked for these */
	if (cmd == CMD8) crc = 0x87;
	spi2_xfer(0x40 | cmd);
	spi2_xfer(arg >> 24);
	spi2_xfer(arg >> 16);
	spi2_xfer(arg >> 8);
	spi2_xfer(arg);
	spi2_xfer(crc);

	for (n = 0; n < 10; n++)	/* R1 within 8 bytes */
		if (!((r = spi2_xfer(0xFF)) & 0x80)) break;
	return r;
}

static int sd_read_data(uint8_t *buf, unsigned len)
{
	uint32_t start = DWT->CYCCNT;
	uint8_t t;

	while ((t = spi2_xfer(0xFF)) == 0xFF)
		if (ms_since(start) > SD_TIMEOUT_MS) return -1;
	if (t != TOKEN_START) return -1;
	while (len-- > 0) *buf++ = spi2_xfer(0xFF);
	spi2_xfer(0xFF);	/* CRC */
	spi2_xfer(0xFF);
	return 0;
}

int hal_disk_init(void)
{
	uint8_t ocr[4], csd[16];
	uint32_t start, c_size;
	int i, v2;

	sd_ready = 0;
	gpio_cfg(GPIOC, 10, MODE_IN, PULL_UP, 0);	/* card detect */
	spi2_speed(SPI_DIV_SD_INIT);
	SD_CS(0);
	for (i = 0; i < 10; i++) spi2_xfer(0xFF);	/* >= 74 clocks */

	if (sd_cmd(CMD0, 0) != R1_IDLE) goto fail;
	v2 = 0;
	if (!(sd_cmd(CMD8, 0x1AA) & R1_ILLEGAL)) {	/* SD v2+ */
		for (i = 0; i < 4; i++) ocr[i] = spi2_xfer(0xFF);
		if (ocr[2] != 0x01 || ocr[3] != 0xAA) goto fail;
		v2 = 1;
	}
	start = DWT->CYCCNT;
	while (sd_cmd(ACMD41, v2 ? 0x40000000 : 0) != 0)	/* HCS */
		if (ms_since(start) > 1000) goto fail;

	sd_block_addr = 0;
	if (v2) {
		if (sd_cmd(CMD58, 0) != 0) goto fail;
		for (i = 0; i < 4; i++) ocr[i] = spi2_xfer(0xFF);
		sd_block_addr = (ocr[0] & 0x40) != 0;	/* CCS */
	}
	if (!sd_block_addr && sd_cmd(CMD16, 512) != 0) goto fail;

	if (sd_cmd(CMD9, 0) != 0 || sd_read_data(csd, sizeof csd) < 0)
		goto fail;
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
	sd_select(0);
	spi2_speed(SPI_DIV_SD_FAST);
	sd_ready = 1;
	return 0;
fail:
	sd_select(0);
	return -1;
}

uint32_t hal_disk_sectors(void)
{
	return sd_ready ? sd_nsectors : 0;
}

int hal_disk_read(uint32_t sector, void *buf, unsigned count)
{
	uint8_t *p = buf;
	int r = 0;

	if (!sd_ready) return -1;
	spi2_speed(SPI_DIV_SD_FAST);
	for (; count > 0; count--, sector++, p += HAL_SECTOR_SIZE) {
		if (sd_cmd(CMD17, sd_block_addr ? sector : sector * 512) != 0
		    || sd_read_data(p, HAL_SECTOR_SIZE) < 0) {
			r = -1;
			break;
		}
	}
	sd_select(0);
	return r;
}

int hal_disk_write(uint32_t sector, const void *buf, unsigned count)
{
	const uint8_t *p = buf;
	unsigned i;
	int r = 0;

	if (!sd_ready) return -1;
	spi2_speed(SPI_DIV_SD_FAST);
	for (; count > 0; count--, sector++) {
		if (sd_cmd(CMD24, sd_block_addr ? sector : sector * 512) != 0) {
			r = -1;
			break;
		}
		spi2_xfer(0xFF);
		spi2_xfer(TOKEN_START);
		for (i = 0; i < HAL_SECTOR_SIZE; i++) spi2_xfer(*p++);
		spi2_xfer(0xFF);	/* dummy CRC */
		spi2_xfer(0xFF);
		if ((spi2_xfer(0xFF) & 0x1F) != 0x05		/* accepted? */
		    || sd_wait_ready() < 0) {			/* programmed */
			r = -1;
			break;
		}
	}
	sd_select(0);
	return r;
}

/*===========================================================================*
 *				Buttons					     *
 *===========================================================================*/
static void buttons_init(void)
{
	gpio_cfg(GPIOB, 10, MODE_IN, PULL_UP, 0);	/* up */
	gpio_cfg(GPIOC, 6, MODE_IN, PULL_UP, 0);	/* down */
	gpio_cfg(GPIOB, 11, MODE_IN, PULL_UP, 0);	/* left */
	gpio_cfg(GPIOB, 12, MODE_IN, PULL_UP, 0);	/* right */
	gpio_cfg(GPIOH, 3, MODE_IN, PULL_DOWN, 0);	/* ok (active high) */
	gpio_cfg(GPIOC, 13, MODE_IN, PULL_UP, 0);	/* back */
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
 *				DFU entry				     *
 *===========================================================================*/
/* Like the stock firmware: if LEFT is held at reset (Left+Back reset, then
 * release Back), enter ST's ROM USB DFU bootloader so a new image can be
 * flashed.  This runs first thing in Reset_Handler, straight out of reset,
 * so peripherals are in their reset state as the ROM expects, and it works
 * even if the rest of this image is broken.  Runs before .data/.bss setup:
 * no globals here. */
#define SYSMEM_BASE 0x1FFF0000u

void hal_early_boot(void)
{
	const volatile uint32_t *sysmem = (const volatile uint32_t *) SYSMEM_BASE;
	volatile uint32_t settle;
	uint32_t sp, pc;
	int left;

	RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
	(void) RCC->AHB2ENR;
	GPIOB->PUPDR = (GPIOB->PUPDR & ~(3u << 22)) | (1u << 22);	/* PB11 pull-up */
	GPIOB->MODER &= ~(3u << 22);					/* PB11 input */
	for (settle = 0; settle < 2000; settle++) ;
	left = !(GPIOB->IDR & (1u << 11));

	/* Put PB11 and the GPIOB clock back to their reset state. */
	GPIOB->MODER |= 3u << 22;
	GPIOB->PUPDR &= ~(3u << 22);
	RCC->AHB2ENR &= ~RCC_AHB2ENR_GPIOBEN;
	if (!left) return;

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

	SysTick_Config(SystemCoreClock / 1000);
	uart_init();
	spi2_init();
	buttons_init();
}

void hal_halt(int status)
{
	(void) status;
	__disable_irq();
	/* Keep the USB console alive so a panic message still reaches the
	 * host (polled: interrupts are off). */
	for (;;) hal_usb_poll();
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

void hal_flash_region(uint32_t *base, uint32_t *size)
{
	*base = FLASH_BASE + 0x20000;
	*size = 0x60000;
}

static int flash_wait(void)
{
	uint32_t sr;

	while (FLASH->SR & (FLASH_SR_BSY | FLASH_SR_CFGBSY)) {}
	sr = FLASH->SR;
	FLASH->SR = (sr & FLASH_ERRS) | FLASH_SR_EOP;	/* write 1 to clear */
	return (sr & FLASH_ERRS) ? -1 : 0;
}

static void flash_unlock(void)
{
	if (FLASH->CR & FLASH_CR_LOCK) {
		FLASH->KEYR = FLASH_KEY1v;
		FLASH->KEYR = FLASH_KEY2v;
	}
	(void) flash_wait();			/* and clear old errors */
}

/* The flash caches may hold the old contents of a page just changed. */
static void flash_done(void)
{
	uint32_t acr = FLASH->ACR & (FLASH_ACR_ICEN | FLASH_ACR_DCEN);

	FLASH->CR |= FLASH_CR_LOCK;
	FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
	FLASH->ACR |= FLASH_ACR_ICRST | FLASH_ACR_DCRST;
	FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
	FLASH->ACR |= acr;
}

static int flash_ok(uint32_t addr, unsigned len)
{
	uint32_t base, size;

	hal_flash_region(&base, &size);
	return addr >= base && len <= size && addr - base <= size - len;
}

int hal_flash_erase(uint32_t page_addr)
{
	int r;

	if (page_addr % HAL_FLASH_PAGE != 0
			|| !flash_ok(page_addr, HAL_FLASH_PAGE)) return -1;
	flash_unlock();
	FLASH->CR = (FLASH->CR & ~FLASH_CR_PNB) | FLASH_CR_PER
		| (((page_addr - FLASH_BASE) / HAL_FLASH_PAGE)
							<< FLASH_CR_PNB_Pos);
	FLASH->CR |= FLASH_CR_STRT;
	r = flash_wait();
	FLASH->CR &= ~(FLASH_CR_PER | FLASH_CR_PNB);
	flash_done();
	return r;
}

int hal_flash_write(uint32_t dst, const void *src, unsigned len)
{
	const uint8_t *s = src;
	uint32_t w0, w1;
	int r = 0;

	if (dst % 8 != 0 || len % 8 != 0 || !flash_ok(dst, len)) return -1;
	flash_unlock();
	FLASH->CR |= FLASH_CR_PG;
	for (; len > 0; len -= 8, dst += 8, s += 8) {
		w0 = s[0] | s[1] << 8 | s[2] << 16 | (uint32_t) s[3] << 24;
		w1 = s[4] | s[5] << 8 | s[6] << 16 | (uint32_t) s[7] << 24;
		if (w0 == 0xFFFFFFFFu && w1 == 0xFFFFFFFFu) continue;
		*(volatile uint32_t *) dst = w0;	/* both words, in order */
		*(volatile uint32_t *) (dst + 4) = w1;
		if ((r = flash_wait()) < 0) break;
	}
	FLASH->CR &= ~FLASH_CR_PG;
	flash_done();
	return r;
}
