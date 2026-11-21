/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey shim (#387 part 3): Stream as an opaque type.  mfkey.h holds
 * a Stream* in its nonce-array and dictionary structs; the engine never
 * dereferences it (the host driver reads the nonce file itself). */
#ifndef LIBNFC_MFKEY_SHIM_BUFFERED_FILE_STREAM_H
#define LIBNFC_MFKEY_SHIM_BUFFERED_FILE_STREAM_H
typedef struct Stream Stream;
#endif
