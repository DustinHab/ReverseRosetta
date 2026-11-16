/* al.h - OpenAL of the host for Intel programs (al.c) */
#ifndef AL_H
#define AL_H
#include "thunk.h"
#include <stddef.h>

/* function for an OpenAL name (al*, alc*; without _ and $ suffix, n characters), 0 if unknown */
Stub al_stub(const char *s, size_t n);

#endif
