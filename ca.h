/* ca.h - CoreAudio's device layer (HAL) of the host for Intel programs (ca.c) */
#ifndef CA_H
#define CA_H
#include "thunk.h"
#include <stddef.h>

/* function for a HAL name (Audio*; without _ and $ suffix, n characters), 0 if not bridged */
Stub ca_stub(const char *s, size_t n);

#endif