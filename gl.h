/* gl.h - OpenGL for translated programs: the gl and CGL functions of the Intel OpenGL go to the PowerPC OpenGL */
#ifndef GL_H
#define GL_H

#include "thunk.h"

/* replacement of the exported function name (without the underscore, n characters) of the Intel OpenGL, or 0 */
Stub gl_stub(const char *name, size_t n);

/* replacement at a dispatch table entry of a guest context (addresses that only gl.c hands out), or 0 */
Stub gl_tramp(uint32_t eip);

/* name of a replacement (statistics), or 0 */
const char *gl_name(Stub fn);

/* mach_msg.c: the guest received the port of a new WindowServer connection (reply 29901, header in host byte
   order, content in the server's) */
void gl_ws_connection(uint32_t port, const uint8_t *m, uint32_t n);

#endif
