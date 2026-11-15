#!/usr/bin/env python3
"""gl_gen.py SDK_HEADERS [OUT] - gl_table.h for gl.c from the OpenGL headers of the Mac OS X 10.5 SDK
(System/Library/Frameworks/OpenGL.framework/Headers): gliDispatch.h gives the dispatch table of a context (order
and prototypes), CGLMacro.h which gl function uses which entry. One line per entry: the gl name (or 0) and a
signature that says how gl.c converts the arguments from the Intel stack for the PowerPC function.

Signature: return kind, then one item per argument (after the GLIContext), separated by commas.
  v i b p f d    return: nothing, 32-bit integer, GLboolean, pointer, float, double
  i B c s S f d  32-bit integer (enum, sizei, handle ...), GLubyte/GLboolean, GLbyte, GLshort, GLushort, float, double
  q              64-bit integer
  p              pointer passed as it is (bytes, strings, pixel data: the context swaps those itself)
  <dir><count><type> array to convert: dir I (in) or O (out); count a number, A<k> (the value of argument k),
                 A<k>x<m> (times m) or P<k> (depends on the pname in argument k); type f d i s (4, 8, 4, 2 bytes)
  X              handled by its own function in gl.c
  ?              unknown pointer: passed as it is, reported once
"""
import re
import sys

SCALAR = {
    'GLenum': 'i', 'GLint': 'i', 'GLuint': 'i', 'GLsizei': 'i', 'GLbitfield': 'i', 'GLhandleARB': 'i',
    'GLintptr': 'i', 'GLsizeiptr': 'i', 'GLintptrARB': 'i', 'GLsizeiptrARB': 'i', 'GLfixed': 'i',
    'GLboolean': 'B', 'GLubyte': 'B', 'GLbyte': 'c', 'GLshort': 's', 'GLushort': 'S', 'GLchar': 'c', 'GLcharARB': 'c',
    'GLfloat': 'f', 'GLclampf': 'f', 'GLdouble': 'd', 'GLclampd': 'd',
    'GLint64EXT': 'q', 'GLuint64EXT': 'q',
}
ESIZE = {'GLfloat': 'f', 'GLclampf': 'f', 'GLdouble': 'd', 'GLclampd': 'd', 'GLint': 'i', 'GLuint': 'i',
         'GLenum': 'i', 'GLsizei': 'i', 'GLshort': 's', 'GLushort': 's', 'GLhandleARB': 'i', 'GLfixed': 'i',
         'GLint64EXT': 'q', 'GLuint64EXT': 'q'}
BYTES = {'GLubyte', 'GLbyte', 'GLboolean', 'GLchar', 'GLcharARB', 'char'}

# functions gl.c handles itself (client arrays, buffers, strings of strings, deferred outputs, pixel store, gets ...)
CUSTOM = {
    'glVertexPointer', 'glNormalPointer', 'glColorPointer', 'glIndexPointer', 'glTexCoordPointer', 'glEdgeFlagPointer',
    'glSecondaryColorPointer', 'glSecondaryColorPointerEXT', 'glFogCoordPointer', 'glFogCoordPointerEXT',
    'glVertexAttribPointer', 'glVertexAttribPointerARB', 'glInterleavedArrays', 'glElementPointerAPPLE',
    'glEnableClientState', 'glDisableClientState', 'glClientActiveTexture', 'glClientActiveTextureARB',
    'glEnableVertexAttribArray', 'glEnableVertexAttribArrayARB', 'glDisableVertexAttribArray',
    'glDisableVertexAttribArrayARB', 'glPushClientAttrib', 'glPopClientAttrib',
    'glDrawArrays', 'glDrawElements', 'glDrawRangeElements', 'glDrawRangeElementsEXT', 'glArrayElement',
    'glMultiDrawArrays', 'glMultiDrawElements', 'glMultiDrawArraysEXT', 'glMultiDrawElementsEXT',
    'glDrawElementArrayAPPLE', 'glDrawRangeElementArrayAPPLE', 'glMultiDrawElementArrayAPPLE',
    'glMultiDrawRangeElementArrayAPPLE', 'glLockArraysEXT', 'glUnlockArraysEXT',
    'glBindBuffer', 'glBindBufferARB', 'glBufferData', 'glBufferDataARB', 'glBufferSubData', 'glBufferSubDataARB',
    'glDeleteBuffers', 'glDeleteBuffersARB', 'glMapBuffer', 'glMapBufferARB', 'glUnmapBuffer', 'glUnmapBufferARB',
    'glFlushMappedBufferRangeAPPLE', 'glGetBufferSubData', 'glGetBufferSubDataARB',
    'glBindVertexArrayAPPLE', 'glDeleteVertexArraysAPPLE',
    'glGetPointerv', 'glGetVertexAttribPointerv', 'glGetVertexAttribPointervARB',
    'glPixelStorei', 'glPixelStoref', 'glGetBooleanv', 'glGetIntegerv', 'glGetFloatv', 'glGetDoublev',
    'glShaderSource', 'glShaderSourceARB', 'glCallLists', 'glFeedbackBuffer', 'glSelectBuffer', 'glRenderMode',
    'glMap1f', 'glMap1d', 'glMap2f', 'glMap2d', 'glGetMapfv', 'glGetMapdv', 'glGetMapiv',
    'glGetUniformfv', 'glGetUniformiv', 'glGetUniformfvARB', 'glGetUniformivARB',
    'glTexImage1D', 'glTexImage2D', 'glTexImage3D', 'glTexSubImage1D', 'glTexSubImage2D', 'glTexSubImage3D',
    'glDrawPixels', 'glReadPixels', 'glGetTexImage', 'glFinish', 'glFlush',
}
# arrays of a fixed length
FIXED = {'glLoadMatrixf': 16, 'glLoadMatrixd': 16, 'glMultMatrixf': 16, 'glMultMatrixd': 16,
         'glLoadTransposeMatrixf': 16, 'glLoadTransposeMatrixd': 16, 'glMultTransposeMatrixf': 16,
         'glMultTransposeMatrixd': 16, 'glLoadTransposeMatrixfARB': 16, 'glLoadTransposeMatrixdARB': 16,
         'glMultTransposeMatrixfARB': 16, 'glMultTransposeMatrixdARB': 16, 'glClipPlane': 4, 'glGetClipPlane': 4}
VECTOR = re.compile(r'^gl(Vertex|Normal|Color|SecondaryColor|TexCoord|MultiTexCoord|RasterPos|WindowPos|Index|'
                    r'EdgeFlag|FogCoord|EvalCoord|VertexAttrib|Rect|ProgramEnvParameter|ProgramLocalParameter|'
                    r'Tangent|Binormal|ColorTable|ColorSubTable|Rects)?\D*?(\d)?N?(b|s|i|f|d|ub|us|ui)v(ARB|EXT|NV|APPLE|ATI)?$')
UNIFORM = re.compile(r'^glUniform(\d)(f|i)v(ARB)?$')
UNIFORM_MATRIX = re.compile(r'^glUniformMatrix(\d)(x(\d))?fv(ARB)?$')
GENDEL = re.compile(r'^gl(Gen|Delete)(Textures|Buffers|Queries|Framebuffers|Renderbuffers|Programs|Fences|'
                    r'VertexArrays|Lists)(ARB|EXT|APPLE|NV|ATI)?$')


def macros(text):
    """[(gl name, dispatch field)] from CGLMacro.h, aliases included"""
    text = text.replace('\\\n', ' ')
    return [(m.group(1), m.group(2)) for m in
            re.finditer(r'#define\s+(gl\w+)\s*\(.*?\)\s*\(\*\s*\(CGL_MACRO_CONTEXT\)->disp\.(\w+)\)', text)]


def norm(s):
    return re.sub(r'[^a-z0-9]', '', s.lower())


SUFFIX = re.compile(r'(ARB|EXT|ATIX|ATI|NV|APPLE|SGIS|SGIX|IBM|MESA)$')


def dispatch(text):
    """[(field, return type, [parameter declarations])] in table order, from gliDispatch.h"""
    body = text[text.index('{', text.index('typedef struct __GLIFunctionDispatchRec')):text.index('} GLIFunctionDispatch;')]
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    out = []
    for m in re.finditer(r'([\w\s\*]+?)\(\s*\*\s*(\w+)\s*\)\s*\(([^)]*)\)\s*;', body):
        params = [p.strip() for p in m.group(3).split(',')]
        out.append((m.group(2), ' '.join(m.group(1).split()), params[1:]))      # without the GLIContext
    return out


def split_param(p):
    """'const GLfloat *params' -> (base type, pointer depth, const, name)"""
    p = ' '.join(p.replace('*', ' * ').split())
    words = p.split()
    const = 'const' in words
    words = [w for w in words if w != 'const']
    depth = words.count('*')
    words = [w for w in words if w != '*']
    if len(words) == 1:
        return words[0], depth, const, ''
    return words[0], depth, const, words[-1]


def kind_ret(t):
    t = ' '.join(t.replace('*', ' * ').split())
    if '*' in t:
        return 'p'
    t = t.replace('const ', '')
    if t == 'void':
        return 'v'
    if t == 'GLboolean':
        return 'b'
    k = SCALAR.get(t)
    if k in ('f', 'd'):
        return k
    return 'i'


def pointer_item(name, params, k, base, depth, const, pname):
    """conversion of pointer argument k of function name"""
    if depth == 2 and base in ('GLvoid', 'void') and not const and name.endswith(('Pointerv', 'PointervARB', 'PointervAPPLE')):
        return 'O1i'                                 # a pointer of the host returned: one word
    if depth > 1:
        return '?'
    if base in BYTES:
        return 'p'                                   # bytes: no byte order
    if base in ('GLvoid', 'void'):
        return 'p'                                   # pixel data and the like (swap bytes in the context)
    t = ESIZE.get(base)
    if not t:
        return '?'
    d = 'I' if const else 'O'
    if name in FIXED:
        return '%s%d%s' % (d, FIXED[name], t)
    m = UNIFORM.match(name)
    if m:
        return '%sA1x%s%s' % (d, m.group(1), t)
    m = UNIFORM_MATRIX.match(name)
    if m:
        r = int(m.group(1))
        c = int(m.group(3)) if m.group(3) else r
        return '%sA1x%d%s' % (d, r * c, t)
    m = GENDEL.match(name)
    if m and k == 1:
        return '%sA0%s' % (d, t)
    if name in ('glAreTexturesResident', 'glPrioritizeTextures', 'glPrioritizeTexturesEXT', 'glDrawBuffers',
                'glDrawBuffersARB', 'glDrawBuffersATI', 'glAreTexturesResidentEXT') and k >= 1:
        return '%sA0%s' % (d, t)
    if name in ('glPixelMapfv', 'glPixelMapuiv', 'glPixelMapusv') and k == 2:
        return 'IA1%s' % t
    if name in ('glGetPixelMapfv', 'glGetPixelMapuiv', 'glGetPixelMapusv'):
        return 'X'
    if name.startswith('glRect') and name.endswith('v'):
        return '%s2%s' % (d, t)
    m = VECTOR.match(name)
    if m and len(params) - 1 == k and not pname:
        n = int(m.group(2)) if m.group(2) else 1
        if name.startswith('glEvalCoord'):
            n = int(m.group(2))
        return '%s%d%s' % (d, n, t)
    if pname is not None:
        return '%sP%d%s' % (d, pname, t)
    if base in ('GLsizei', 'GLint') and not const and params[k].split()[-1].lstrip('*') in ('length', 'size', 'count'):
        return 'O1i'                                 # length of a returned string, size/count of an active variable
    if base == 'GLenum' and not const and params[k].split()[-1].lstrip('*') == 'type':
        return 'O1i'
    if base in ('GLuint', 'GLhandleARB') and not const and name in ('glGetAttachedShaders', 'glGetAttachedObjectsARB'):
        return 'OA1%s' % t
    return '?'


def signature(name, ret, params):
    if name in CUSTOM:
        return 'X'
    items = [kind_ret(ret)]
    pname = None
    for k, p in enumerate(params):
        base, depth, const, pn = split_param(p)
        if not depth and pn == 'pname':
            pname = k
    for k, p in enumerate(params):
        base, depth, const, pn = split_param(p)
        if p in ('void', ''):
            continue
        if depth:
            items.append(pointer_item(name, params, k, base, depth, const, pname))
        else:
            items.append(SCALAR.get(base, 'i'))
    if 'X' in items:
        return 'X'
    return ','.join(items)


CTYPE = {'i': 'uint32_t', 'B': 'uint32_t', 'c': 'int32_t', 's': 'int32_t', 'S': 'uint32_t', 'f': 'float',
         'd': 'double', 'q': 'uint64_t'}
READ = {'i': 'GI', 'B': 'GB', 'c': 'GC', 's': 'GS', 'S': 'GU', 'f': 'GF', 'd': 'GD', 'q': 'GQ'}
SIZE = {'i': 4, 'B': 4, 'c': 4, 's': 4, 'S': 4, 'f': 4, 'd': 8, 'q': 8, 'p': 4}
ELEM = {'f': ('float', 'f'), 'd': ('double', 'd'), 'i': ('uint32_t', 'i'), 's': ('uint16_t', 's'),
        'q': ('uint64_t', 'd')}


def array_parts(item):
    """'IA1x4f' -> (dir, count source, multiplier, element type)"""
    m = re.match(r'^([IO])(?:(\d+)|A(\d+)(?:x(\d+))?|P(\d+))([fdisq])$', item)
    if not m:
        raise ValueError('signature item %r' % item)
    d, fixed, arg, mul, pn, t = m.groups()
    return d, fixed, arg, mul, pn, t


def body(slot, field, name, sig, params):
    """C code of entry slot: converts the arguments at guest address a and calls the host's entry"""
    if sig == 'X':
        return None
    items = sig.split(',')
    ret, args = items[0], items[1:]
    proto, vals, pre, post, off, unknown = ['void *'], ['x->rend'], [], [], 0, False
    # two or more floating-point arguments: 16 bytes of the Intel stack at a time with one AltiVec load and swap
    # (a float through lwbrx, stw and lfs costs a load-hit-store stall each, ~20 cycles on the G4)
    vec, loads, o2 = sum(1 for it in args if it in ('f', 'd')) >= 2, [], 0
    for it in args:
        if vec and it in ('f', 'd'):
            sz = 4 if it == 'f' else 8
            if not any(t == sz and b <= o2 and o2 + sz <= b + 16 and (o2 - b) % sz == 0 for b, t in loads):
                loads.append((o2, sz))
        o2 += SIZE[it] if it in SIZE else 4
    for j, (b, t) in enumerate(loads):
        pre.append('V16 q%d; gl_vld(&q%d, a + %d, %d);' % (j, j, b, t))
    for k, it in enumerate(args):
        if it in ('f', 'd') and vec:
            sz = 4 if it == 'f' else 8
            j = next(j for j, (b, t) in enumerate(loads) if t == sz and b <= off and off + sz <= b + 16 and (off - b) % sz == 0)
            proto.append(CTYPE[it])
            vals.append('q%d.%s[%d]' % (j, 'f' if sz == 4 else 'd', (off - loads[j][0]) // sz))
            off += SIZE[it]
            continue
        if it in CTYPE:
            proto.append(CTYPE[it])
            vals.append('%s(%d)' % (READ[it], off))
            off += SIZE[it]
            continue
        proto.append('void *')
        if it in ('p', '?'):
            vals.append('GP(%d)' % off)
            unknown = unknown or it == '?'
        else:
            d, fixed, arg, mul, pn, t = array_parts(it)
            esz = {'f': 4, 'd': 8, 'i': 4, 's': 2, 'q': 8}[t]
            if fixed:
                n = fixed
            elif arg:
                n = 'GI(%d)%s' % (sum(SIZE[a if a in SIZE else 'p'] for a in args[:int(arg)]), ' * %s' % mul if mul else '')
            else:
                n = 'gl_pcount(c, %d, GI(%d))' % (slot, sum(SIZE[a if a in SIZE else 'p'] for a in args[:int(pn)]))
            ct, fn = ELEM[t]
            pre.append('uint32_t n%d = %s;' % (k, n))
            if arg:                                  # count from the program: scratch memory
                pre.append('void *p%d = GP(%d), *t%d = p%d ? gl_tmp(%d, n%d, %d) : 0;' % (k, off, k, k, k, k, esz))
            else:                                    # at most 16 (pname) or a fixed length: on the stack; room for
                size = int(fixed) if fixed else 64   # what the host writes for a pname it knows better
                pre.append('uint64_t b%d[%d]; void *p%d = GP(%d), *t%d = p%d ? (void *)b%d : 0;' %
                           (k, (size * esz + 7) // 8, k, off, k, k, k))
            if d == 'I':
                pre.append('if (p%d) gl_in_%s(t%d, p%d, n%d);' % (k, fn if t != 'q' else 'd', k, k, k))
            else:
                post.append('if (p%d) gl_out_%s(p%d, t%d, n%d);' % (k, fn if t != 'q' else 'd', k, k, k))
            vals.append('t%d' % k)
        off += 4
    rt = {'v': 'void', 'i': 'uint32_t', 'b': 'uint32_t', 'p': 'uintptr_t', 'f': 'float', 'd': 'double'}[ret]
    if ret == 'b':
        rt = 'unsigned char'
    call = '((%s (*)(%s))x->disp[%d])(%s)' % (rt, ', '.join(proto), slot, ', '.join(vals))
    out = ['static void gl_b%d(Cpu *c, GlCx *x, uint32_t a)    /* %s */' % (slot, name), '{']
    out += ['    ' + l for l in pre]
    if unknown:
        out.append('    gl_unknown(%d);' % slot)
    if ret == 'v':
        out.append('    %s;' % call)
    elif ret in ('f', 'd'):
        out.append('    gl_st0(c, (double)%s);' % call)
    else:
        out.append('    c->r[EAX] = (uint32_t)%s;' % call)
    out += ['    ' + l for l in post]
    out.append('    (void)c; (void)a;')
    out.append('}')
    return out


def main():
    hdr = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else 'gl_table.h'
    exports = [l.strip().lstrip('_') for l in open(sys.argv[3])] if len(sys.argv) > 3 else []
    pairs = macros(open(hdr + '/CGLMacro.h').read())
    table = dispatch(open(hdr + '/gliDispatch.h').read())
    slot = {f: i for i, (f, r, p) in enumerate(table)}
    byfield = {}
    names = {}                                       # gl name -> slot
    for n, f in pairs:
        if f in slot:
            names.setdefault(n, slot[f])
            byfield.setdefault(f, n)
    normfield = {norm(f): i for i, (f, r, p) in enumerate(table)}
    for f, i in slot.items():                        # entries without a macro: name from the field
        if f not in byfield:
            n = 'gl' + ''.join(w if w.isupper() else w[:1].upper() + w[1:] for w in f.split('_'))
            byfield[f] = n
            names.setdefault(n, i)
    missing = []
    for e in exports:                                # exported aliases (ARB, EXT ...) of the same entry
        if not e.startswith('gl') or e in names:
            continue
        base = SUFFIX.sub('', e)
        if base in names:
            names[e] = names[base]
        elif norm(e[2:]) in normfield:
            names[e] = normfield[norm(e[2:])]
        elif norm(base[2:]) in normfield:
            names[e] = normfield[norm(base[2:])]
        else:
            missing.append(e)
    lines = ['/* gl_table.h - generated by tools/gl_gen.py from the OpenGL headers of the Mac OS X 10.5 SDK: the dispatch',
             '   table of a context in order, with the gl function that uses each entry and how gl.c converts its',
             '   arguments (see gl_gen.py); then the exported names of libGL with their entry. Do not edit. */',
             '#define GL_NSLOT %d' % len(table),
             'static const struct { const char *name, *field, *sig; } g_glslot[GL_NSLOT] = {']
    unknown = 0
    for field, ret, params in table:
        name = byfield[field]
        sig = signature(name, ret, params)
        unknown += '?' in sig.split(',')[1:]
        lines.append('    { "%s", "%s", "%s" },' % (name, field, sig))
    lines.append('};')
    lines.append('')
    lines.append('/* per entry: the conversion (gl_b<slot>), or the hand-written function GLX_<field> of gl.c; then the')
    lines.append('   entries of the direct call (arguments from esp + 4) and of the call through the dispatch table of the')
    lines.append('   guest context (GLIContext at esp + 4, arguments from esp + 8) */')
    for slot, (field, ret, params) in enumerate(table):
        name = byfield[field]
        sig = signature(name, ret, params)
        b = body(slot, field, name, sig, params)
        if b is None:
            lines.append('#ifndef GLX_%s' % field)
            lines.append('#define GLX_%s gl_raw_%d' % (field, slot))
            raw = body(slot, field, name, ','.join([kind_ret(ret)] + [
                SCALAR.get(split_param(p)[0], 'i') if not split_param(p)[1] else '?' for p in params if p not in ('void', '')]),
                params)
            raw[0] = raw[0].replace('gl_b%d' % slot, 'gl_raw_%d' % slot)
            lines += raw
            lines.append('#endif')
            lines.append('#define gl_b%d(c, x, a) GLX_%s(c, x, a)' % (slot, field))
        else:
            lines += b
        lines.append('static int ga_%d(Cpu *c) { GlCx *x = c->gl_ctx; if (x) gl_b%d(c, x, c->r[ESP] + 4); return 0; }' % (slot, slot))
        lines.append('static int gd_%d(Cpu *c) { GlCx *x = gl_rend(c, ld32(c->r[ESP] + 4)); if (x) gl_b%d(c, x, c->r[ESP] + 8); return 0; }' % (slot, slot))
    lines.append('static const Stub g_gl_api[GL_NSLOT] = {')
    lines += ['    ' + ', '.join('ga_%d' % i for i in range(j, min(j + 10, len(table)))) + ',' for j in range(0, len(table), 10)]
    lines.append('};')
    lines.append('static const Stub g_gl_disp[GL_NSLOT] = {')
    lines += ['    ' + ', '.join('gd_%d' % i for i in range(j, min(j + 10, len(table)))) + ',' for j in range(0, len(table), 10)]
    lines.append('};')
    lines.append('#define GL_NNAME %d' % len(names))
    lines.append('static const struct { const char *name; short slot; } g_glname[GL_NNAME] = {')
    for n in sorted(names):
        lines.append('    { "%s", %d },' % (n, names[n]))
    lines.append('};')
    open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('%d entries, %d names, %d with an unknown pointer; exports without an entry: %s' %
          (len(table), len(names), unknown, ' '.join(missing)))


main()
