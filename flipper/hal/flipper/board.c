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

void hal_delay_ms(uint32_t ms)
{
	uint32_t start = hal_ms;

	while (hal_ms - start < ms)
		__WFI();
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

static void lcd_cmds(const uint8_t *cmd, unsigned n)
{
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
	for (;;) __WFI();
}
