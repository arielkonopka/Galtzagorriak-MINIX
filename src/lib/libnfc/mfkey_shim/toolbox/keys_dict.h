/* SPDX-License-Identifier: GPL-3.0-or-later
 * libnfc mfkey shim (#387 part 3): KeysDict as an opaque type.  mfkey.h
 * declares `struct KeysDict` itself; here it is only named (the engine takes
 * KeysDict* without touching it). */
#ifndef LIBNFC_MFKEY_SHIM_KEYS_DICT_H
#define LIBNFC_MFKEY_SHIM_KEYS_DICT_H
typedef struct KeysDict KeysDict;
#endif
