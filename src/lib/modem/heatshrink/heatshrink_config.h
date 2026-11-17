#ifndef HEATSHRINK_CONFIG_H
#define HEATSHRINK_CONFIG_H

/* Galtzagorriak MINIX (spec P.3a, gmodem -z): static buffers, no malloc;
 * window 2^8 = 256 B, lookahead 2^4 = 16 B (the format every gmodem
 * speaks; changing them changes the format), no index (it would cost
 * 1 KB of RAM for speed the radios do not need). */
#define HEATSHRINK_DYNAMIC_ALLOC 0
#define HEATSHRINK_STATIC_INPUT_BUFFER_SIZE 32
#define HEATSHRINK_STATIC_WINDOW_BITS 8
#define HEATSHRINK_STATIC_LOOKAHEAD_BITS 4

#define HEATSHRINK_DEBUGGING_LOGS 0
#define HEATSHRINK_USE_INDEX 0

#endif
