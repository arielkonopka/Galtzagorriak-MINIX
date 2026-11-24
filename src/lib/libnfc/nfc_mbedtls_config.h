/* nfc_mbedtls_config.h - the Mbed TLS build configuration for libnfc
 * (issue #447).  Used instead of mbedtls/config.h through
 * -DMBEDTLS_CONFIG_FILE='"nfc_mbedtls_config.h"'.  Only what the vendored
 * NFC stacks call: AES and DES/3DES in ECB and CBC.  ROM tables keep every
 * table const (flash, XIP): 0 B of .data/.bss, nothing built at run time.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NFC_MBEDTLS_CONFIG_H
#define NFC_MBEDTLS_CONFIG_H
#define MBEDTLS_AES_C
#define MBEDTLS_DES_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_AES_ROM_TABLES
#endif
