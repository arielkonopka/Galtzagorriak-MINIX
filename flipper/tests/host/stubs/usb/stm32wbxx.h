/* Host stub of the STM32WB register header for hal/flipper/usb.c: the
 * registers it touches are plain memory.  PLLSAI1RDY is the same bit as
 * PLLSAI1ON, so the clock "locks" as soon as it is switched on. */
#include <stdint.h>

typedef struct {
	uint32_t CR, PLLCFGR, PLLSAI1CFGR, CCIPR, AHB2ENR;
} RCC_TypeDef;
typedef struct { uint32_t CR2; } PWR_TypeDef;
typedef struct { uint32_t MODER, OSPEEDR, PUPDR, AFR[2]; } GPIO_TypeDef;

extern RCC_TypeDef t_rcc;
extern PWR_TypeDef t_pwr;
extern GPIO_TypeDef t_gpioa;
#define RCC	(&t_rcc)
#define PWR	(&t_pwr)
#define GPIOA	(&t_gpioa)

#define RCC_PLLCFGR_PLLSRC_Pos		0
#define RCC_PLLCFGR_PLLSRC		(3u << RCC_PLLCFGR_PLLSRC_Pos)
#define RCC_CR_PLLSAI1ON		(1u << 26)
#define RCC_CR_PLLSAI1RDY		RCC_CR_PLLSAI1ON
#define RCC_PLLSAI1CFGR_PLLN_Pos	8
#define RCC_PLLSAI1CFGR_PLLQ_Pos	21
#define RCC_PLLSAI1CFGR_PLLQEN		(1u << 20)
#define RCC_CCIPR_CLK48SEL		(3u << 26)
#define RCC_CCIPR_CLK48SEL_0		(1u << 26)
#define PWR_CR2_USV			(1u << 10)
#define RCC_AHB2ENR_GPIOAEN		(1u << 0)
#define USB_LP_IRQn			20
