/* tec.h - the Text Encoding Converter of the host's CarbonCore for Intel programs (tec.c) */
#ifndef TEC_H
#define TEC_H
#include "thunk.h"
#include <stddef.h>

/* function for a TEC name (TEC*; without _ and $ suffix, n characters), 0 if not bridged */
Stub tec_stub(const char *s, size_t n);

#endif