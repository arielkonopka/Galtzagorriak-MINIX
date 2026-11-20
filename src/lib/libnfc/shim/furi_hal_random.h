/* libnfc shim (#404): stock's RNG for the pollers' reader nonces.  On the
 * host a fixed LCG (deterministic tests); the device build fills it from
 * the kernel's random source. */
#pragma once
#include <stdint.h>
#include <stddef.h>
uint32_t furi_hal_random_get(void);
void furi_hal_random_fill_buf(uint8_t* buf, uint32_t len);
