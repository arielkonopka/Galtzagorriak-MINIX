/* mbedtls/aes.h - the vendored Mbed TLS AES (src/lib/mbedtls, #447).
 * The NFC code includes <mbedtls/aes.h> as stock does; this forwards to the
 * real header.  Build with -DMBEDTLS_CONFIG_FILE='"nfc_mbedtls_config.h"'
 * and -I src/lib/mbedtls/include.  SPDX: GPL-3.0-or-later */
#ifndef NFCSHIM_MBEDTLS_AES_H
#define NFCSHIM_MBEDTLS_AES_H
#include "../../../mbedtls/include/mbedtls/aes.h"
#endif
