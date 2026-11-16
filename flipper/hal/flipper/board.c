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
#define SPI_DIV_DISPLAY	3	/* /16 = 4 MHz */
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
	/* at once: the white channel back under I2C control (a fade may have
	 * left it with engine 1), the engine stopped, then the level */
	lp5562_write(0x70, 0x00);	/* LED map: all direct */
	lp5562_write(0x01, 0x00);	/* op mode: engines disabled */
	lp5562_write(0x00, 0xC0);	/* enable: CHIP_EN, LOG_EN, engines hold */
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

	lp5562_write(0x70, 0x00);	/* direct while loading */
	lp5562_write(0x01, 0x10);	/* engine 1: load program */
	t0 = DWT->CYCCNT;
	while (DWT->CYCCNT - t0 < hal_cpu_hz() / 10000) ;	/* 100 us */
	prog[0] = 0x10;			/* engine 1's program memory */
	for (i = 0; i < 16; i++) {	/* big-endian words, the rest zero */
		prog[1 + 2 * i] = i < 4 ? w[i] >> 8 : 0;
		prog[2 + 2 * i] = i < 4 ? w[i] & 0xFF : 0;
	}
	(void) i2c1_write(LP5562_ADDR, prog, sizeof prog);
	lp5562_write(0x01, 0x20);	/* engine 1: run */
	lp5562_write(0x00, 0xC0 | 0x20);	/* enable: engine 1 runs */
	lp5562_write(0x70, 0x40);	/* white from engine 1 */
	lp5562_write(0x0E, to);		/* the direct value afterwards */
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
#define CMD12	12	/* STOP_TRANSMISSION */
#define CMD17	17	/* READ_SINGLE_BLOCK */
#define CMD18	18	/* READ_MULTIPLE_BLOCK */
#define CMD24	24	/* WRITE_BLOCK */
#define CMD25	25	/* WRITE_MULTIPLE_BLOCK */
#define TOKEN_MULTI	0xFC	/* data token for CMD25 */
#define TOKEN_STOP	0xFD	/* ends a CMD25 transfer */
#define CMD55	55	/* APP_CMD */
#define CMD58	58	/* READ_OCR */
#define ACMD41	41	/* SD_SEND_OP_COND */
#define R1_IDLE		0x01
#define R1_ILLEGAL	0x04
#define TOKEN_START	0xFE
#define SD_TIMEOUT_MS	500

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

static uint16_t crc16_more(uint16_t crc, const uint8_t *p, unsigned n)
{
	static const uint16_t t[16] = {
		0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
		0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
	};

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
		if (ms_since(start) > SD_TIMEOUT_MS) return -1;
	if (t != TOKEN_START) return -1;
	spi2_dma(NULL, buf, len);
	crc = (uint16_t) (spi2_xfer(0xFF) << 8);
	crc |= spi2_xfer(0xFF);
	return crc == crc16(buf, len) ? 0 : -2;
}

int hal_disk_init(void)
{
	uint8_t ocr[4], csd[16];
	uint32_t start, c_size;
	int i, v2;

	sd_ready = 0;
	RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN | RCC_AHB1ENR_DMAMUX1EN;
	(void) RCC->AHB1ENR;
	DMAMUX1_Channel0->CCR = 8;		/* DMA1 ch 1: SPI2_RX */
	DMAMUX1_Channel1->CCR = 9;		/* DMA1 ch 2: SPI2_TX */
	gpio_cfg(GPIOC, 10, MODE_IN, PULL_UP, 0);	/* card detect */
	spi2_speed(SPI_DIV_SD_INIT);
	SD_CS(0);
	for (i = 0; i < 10; i++) spi2_xfer(0xFF);	/* >= 74 clocks */

	if (sd_cmd(CMD0, 0) != R1_IDLE) goto fail;
	if (sd_cmd(CMD59, 1) > R1_IDLE) goto fail;	/* CRCs on */
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
	spi2_speed(sd_fast);
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
			if (ms_since(start) > SD_TIMEOUT_MS) goto bad;
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
	for (settle = 0; settle < 2000; settle++) ;
	left = !(GPIOB->IDR & (1u << 11));

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
	buttons_init();
	mt_init();
	hal_radios_park();			/* T14a: -1.7 mA and more */
}

void hal_halt(int status)
{
	__disable_irq();
	hal_tone(0);
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
 * the ROM loader and the OTP area.  Anything else (holes, peripherals,
 * CPU2's areas) is refused instead of a bus fault in the MEMORY task. */
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

/* The RTC calendar (BCD; the stock firmware keeps it running on the
 * battery) in seconds since 1970, years 2000-2099.  Days from the civil
 * date: Howard Hinnant's algorithm. */
uint32_t hal_rtc_time(void)
{
	uint32_t tr, dr, y, m, d, era, yoe, doy, doe, i;

	RCC->APB1ENR1 |= RCC_APB1ENR1_RTCAPBEN;
	(void) RCC->APB1ENR1;
	if (!(RCC->BDCR & RCC_BDCR_RTCEN) || !(RCC->BDCR & RCC_BDCR_LSERDY)
				|| !(RTC->ISR & RTC_ISR_INITS))
		return 0;
	for (i = 0; i < 100000 && !(RTC->ISR & RTC_ISR_RSF); i++) ;
	tr = RTC->TR;			/* locks DR until it is read */
	dr = RTC->DR;
	y = 2000 + (dr >> 20 & 15) * 10 + (dr >> 16 & 15);
	m = (dr >> 12 & 1) * 10 + (dr >> 8 & 15);
	d = (dr >> 4 & 3) * 10 + (dr & 15);
	if (m < 1 || m > 12 || d < 1 || d > 31) return 0;
	if (m <= 2) y--;
	era = y / 400;
	yoe = y - era * 400;
	doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return ((era * 146097 + doe - 719468) * 86400
		+ ((tr >> 20 & 3) * 10 + (tr >> 16 & 15)) * 3600
		+ ((tr >> 12 & 7) * 10 + (tr >> 8 & 15)) * 60
		+ (tr >> 4 & 7) * 10 + (tr & 15));
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

/*===========================================================================*
 *				Infrared				     *
 *===========================================================================*/
/* As on the stock firmware: the receiver (TSOP, active low, own pull-up)
 * on PA0, here through EXTI line 0 on both edges; the LED driver on PB9,
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

int hal_ir_irq(void)
{
	return EXTI0_IRQn;
}

void hal_ir_rx_on(int on)
{
	if (on) {
		gpio_cfg(GPIOA, 0, MODE_IN, PULL_NONE, 0);
		SYSCFG->EXTICR[0] &= ~SYSCFG_EXTICR1_EXTI0;	/* port A */
		EXTI->RTSR1 |= EXTI_RTSR1_RT0;
		EXTI->FTSR1 |= EXTI_FTSR1_FT0;
		EXTI->PR1 = EXTI_PR1_PIF0;
		EXTI->IMR1 |= EXTI_IMR1_IM0;
	} else {
		EXTI->IMR1 &= ~EXTI_IMR1_IM0;
		EXTI->PR1 = EXTI_PR1_PIF0;
	}
}

int hal_ir_rx_edge(void)
{
	EXTI->PR1 = EXTI_PR1_PIF0;
	return gpio_get(GPIOA, 0);
}

void hal_ir_freq(unsigned hz, unsigned duty)
{
	ir_period = SystemCoreClock / hz;
	ir_on = ir_period * duty / 100;
}

void hal_ir_carrier(int on)
{
	static int ready;

	if (!ready) {
		RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
		(void) RCC->APB2ENR;
		TIM1->CR1 = 0;
		TIM1->PSC = 0;
		if (ir_period == 0) hal_ir_freq(IR_CARRIER_HZ, 33);
		TIM1->ARR = ir_period - 1;
		TIM1->CCR3 = ir_on;
		TIM1->CCMR2 = (4u << TIM_CCMR2_OC3M_Pos) | TIM_CCMR2_OC3PE;
		TIM1->CCER = TIM_CCER_CC3NE;
		TIM1->BDTR = TIM_BDTR_MOE;
		TIM1->EGR = TIM_EGR_UG;
		TIM1->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
		gpio_set(GPIOB, 9, 0);
		gpio_cfg(GPIOB, 9, MODE_AF, PULL_NONE, 1);
		ready = 1;
	}
	if (on) {				/* preloaded, taken by UG */
		TIM1->ARR = ir_period - 1;
		TIM1->CCR3 = ir_on;
	}
	/* PWM mode 1 (110) or forced inactive (100); start a fresh period
	 * so every mark begins with a whole carrier cycle */
	TIM1->CCMR2 = (TIM1->CCMR2 & ~TIM_CCMR2_OC3M)
		| ((on ? 6u : 4u) << TIM_CCMR2_OC3M_Pos);
	if (on) TIM1->EGR = TIM_EGR_UG;
}

int hal_ir_tx_irq(void)
{
	return TIM2_IRQn;
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
		sem_give(SEM_CLK48);
	}
	if (sem_take(SEM_RNG) < 0) return -1;
	if (!on) {
		RNG->CR |= RNG_CR_RNGEN;
		on = 1;
	}
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
	sem_give(SEM_RNG);
	return r;
}

/*===========================================================================*
 *				Speaker					     *
 *===========================================================================*/
/* Piezo on PB8, TIM16 channel 1 (AF14), as on the stock firmware.  The
 * timer counts at 1 MHz; a square wave at hz, 0 stops it. */
void hal_tone(unsigned hz)
{
	static int ready;
	uint32_t arr;

	if (hz == 0) {
		if (ready) {
			TIM16->CR1 &= ~TIM_CR1_CEN;
			gpio_cfg(GPIOB, 8, MODE_ANALOG, PULL_NONE, 0);
		}
		return;
	}
	if (!ready) {
		RCC->APB2ENR |= RCC_APB2ENR_TIM16EN;
		(void) RCC->APB2ENR;
		TIM16->PSC = SystemCoreClock / 1000000u - 1;
		TIM16->CCMR1 = (6u << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;
		TIM16->CCER = TIM_CCER_CC1E;
		TIM16->BDTR = TIM_BDTR_MOE;
		ready = 1;
	}
	if (hz < 20) hz = 20;
	arr = 1000000u / hz - 1;
	TIM16->ARR = arr;
	TIM16->CCR1 = arr / 2;
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
 *	0x08030000  installed programs (Phase 2b)	up to the cache
 *	...         run-time text cache			HAL_CACHE_SIZE
 *	SFSA - 16 pages: guard, room for FUS to upgrade the wireless stack
 *	SFSA ..     CPU2 wireless stack and FUS, never touched
 */
#define HAL_SYS_IMAGE	0x30000u	/* 192 KB */
#define HAL_CACHE_SIZE	0x20000u	/* 128 KB */
#define GUARD_PAGES	16u

void hal_flash_layout(uint32_t *l)
{
	uint32_t sfsa, top, inst = FLASH_BASE + HAL_SYS_IMAGE;

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
	l[HAL_FL_CACHE] = top - HAL_CACHE_SIZE;
	l[HAL_FL_CACHE_SIZE] = HAL_CACHE_SIZE;
	l[HAL_FL_INST] = inst;
	l[HAL_FL_INST_SIZE] = top - HAL_CACHE_SIZE - inst;
}

/* For the MEMORY task, whose stack has no room for the layout table. */
uint32_t hal_rom_base(void) { return FLASH_BASE + HAL_SYS_IMAGE + HAL_ROM_OFF; }

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

	hal_flash_layout(l);
	if (addr >= l[HAL_FL_CACHE] && len <= l[HAL_FL_CACHE_SIZE]
			&& addr - l[HAL_FL_CACHE] <= l[HAL_FL_CACHE_SIZE] - len)
		return 1;
	return addr >= l[HAL_FL_INST] && len <= l[HAL_FL_INST_SIZE]
		&& addr - l[HAL_FL_INST] <= l[HAL_FL_INST_SIZE] - len;
}

int hal_flash_erase(uint32_t page_addr)
{
	int r;

	if (page_addr % HAL_FLASH_PAGE != 0
			|| !flash_ok(page_addr, HAL_FLASH_PAGE)) return -1;
	if (flash_unlock() < 0) { flash_done(); return -1; }
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
	if (flash_unlock() < 0) { flash_done(); return -1; }
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
	return r;
}
