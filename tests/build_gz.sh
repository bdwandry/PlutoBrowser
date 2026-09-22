#!/bin/bash
# SW2c: build + run the gzip host suite.
# Generates real gzip fixtures with Python zlib, injects them as escaped C
# string literals + length macros into the test via __GZ_FIXTURES__, then
# compiles http_client.c + deps (spill/mem/strbuf/logger/url/cookies) with
# the sim shim set and runs the binary under ASan+UBSan.
set -e
cd /Users/bwandrych/Desktop/PlutoBrowser || exit 1
export PLAYDATE_SDK_PATH="$HOME/Developer/PlaydateSDK"

D=.build_gz
rm -rf "$D"; mkdir -p "$D"

# ── Fixtures (Python zlib: raw deflate payloads → gzip members) ──────────
python3 - > "$D/fixtures.h" <<'PYEOF'
import zlib

def esc(b):
    out = []
    for byte in b:
        ch = chr(byte)
        if ch == '"': out.append('\\"')
        elif ch == '\\': out.append('\\\\')
        elif ch == '\n': out.append('\\n')
        elif ch == '\r': out.append('\\r')
        elif ch == '\t': out.append('\\t')
        elif 32 <= byte < 127: out.append(ch)
        else: out.append('\\x%02x' % byte)
    # avoid hex-escape gobbling the next char: break after \xNN with a literal
    s = ''.join(out)
    fix = []
    i = 0
    while i < len(s):
        if s[i] == '\\' and i+1 < len(s) and s[i+1] == 'x':
            j = i+2
            while j < len(s) and j < i+4 and s[j] in '0123456789abcdefABCDEF':
                j += 1
            fix.append(s[i:j])
            if j < len(s) and (s[j].isalnum() or s[j] == '_'):
                fix.append('""')
            i = j
        else:
            fix.append(s[i]); i += 1
    return ''.join(fix)

def member(text_bytes, flg=0, lie_isize=False):
    co = zlib.compressobj(9, zlib.DEFLATED, -15)  # raw deflate
    payload = co.compress(text_bytes) + co.flush()
    import struct
    isize = struct.pack('<I', len(text_bytes) - 2 if lie_isize else len(text_bytes))
    crc = struct.pack('<I', zlib.crc32(text_bytes) & 0xffffffff)
    # fixed header: ID ID CM FLG MTIME(4) XFL OS
    head = bytes([0x1f, 0x8b, 8, flg, 0, 0, 0, 0, 0, 3])
    extra = b''
    if flg & 0x04:
        x = b'pl\x00\x01A'  # FEXTRA: len=1, 'A'
        extra += struct.pack('<H', len(x)) + x
    if flg & 0x08:
        extra += b'filename.txt\x00'
    if flg & 0x10:
        extra += b'a comment\x00'
    if flg & 0x02:
        extra += b'\x00\x00'  # FHCRC (unchecked by our parser by design)
    return head + extra + payload + crc + isize

def resp(body, ctype='text/html', extra_head=''):
    return ('HTTP/1.1 200 OK\r\n'
            'Content-Type: %s\r\n' % ctype +
            extra_head +
            'Content-Length: %d\r\n' % len(body) +
            'Connection: close\r\n\r\n').encode() + body

def chunked_resp(member_bytes):
    # split member into two chunks: 0x100 bytes + rest, then 0-terminator
    a, b = member_bytes[:256], member_bytes[256:]
    def hexs(x): return '%x' % len(x)
    body = (hexs(a).encode() + b'\r\n' + a + b'\r\n' +
            hexs(b).encode() + b'\r\n' + b + b'\r\n' +
            b'0\r\n\r\n')
    return ('HTTP/1.1 200 OK\r\n'
            'Content-Type: text/plain\r\n'
            'Content-Encoding: gzip\r\n'
            'Transfer-Encoding: chunked\r\n'
            'Connection: close\r\n\r\n').encode() + body

CE = 'Content-Encoding: gzip\r\n'
text1 = b'<html><body><h1>Gzip works on device</h1></body></html>\n'
m1  = member(text1)
m1l = member(text1, lie_isize=True)          # footer ISIZE 2 bytes short
m3  = member(b'<p>fetch-style member</p>\n', flg=0x1e)  # FEXTRA|FNAME|FCOMMENT|FHCRC
m4  = member(b'')                             # empty payload, ISIZE 0
trunc = member(text1)[:len(m1)-9]             # cut into the footer

def emit(name, data):
    print('#define %s "%s"' % (name, esc(data)))
    print('#define %s_LEN %d' % (name, len(data)))

print('#define GZ1_TEXT "%s"' % esc(text1))
print('#define GZ3_TEXT "%s"' % esc(b'<p>fetch-style member</p>\n'))
emit('GZ1_RESPONSE', resp(m1, extra_head=CE))
emit('GZ2_RESPONSE', chunked_resp(m1))
emit('GZ3_RESPONSE', resp(m3, extra_head=CE))
emit('GZ4_RESPONSE', resp(m4, extra_head=CE))
emit('GZ5_RESPONSE', resp(b'hello world!', ctype='text/plain'))
emit('GZ6_RESPONSE', resp(b'\x37\x13\x99\x88NOTGZIPDATA1234', extra_head=CE))
emit('GZ7_RESPONSE', resp(trunc, extra_head=CE))
emit('GZ8_RESPONSE', resp(m1l, extra_head=CE))
PYEOF

INC="-I. -ISource -ISource/core -ISource/util -ISource/html -ISource/render -ISource/js/muJS -ISource/js/duktape -ISource/js/QuickJS -ISource/js/xs_moddable/includes -ISource/js/xs_moddable/platforms -I$PLAYDATE_SDK_PATH/C_API"
COMMON="-O1 -g -fsanitize=address,undefined -DTARGET_EXTENSION=1 -DPLUTO_SPILL_HOST -DPLUTO_SPILL_DIR=\"/tmp/plutobrowser_spill_gz\" $INC"

set -e
cc $COMMON -c Source/core/http_client.c     -o "$D/http.o"
cc $COMMON -c Source/core/pluto_mem.c       -o "$D/mem.o"
cc $COMMON -c Source/core/pluto_spill.c     -o "$D/spill.o"
cc $COMMON -c Source/core/url.c             -o "$D/url.o"
cc $COMMON -c Source/core/logger.c          -o "$D/logger.o"
cc $COMMON -c Source/core/cookie_jar.c      -o "$D/cookie.o"
cc $COMMON -c Source/core/encoding.c        -o "$D/enc.o"
cc $COMMON -c Source/util/strbuf.c          -o "$D/strbuf.o"
cc $COMMON -c Source/util/strutil.c         -o "$D/strutil.o"
cc $COMMON -c Source/render/decoders/inflate.c -o "$D/inflate.o"
# jsbridge deps pulled in by http_client's about: pages (muJS engine):
for f in Source/js/muJS/*.c; do cc $COMMON -c "$f" -o "$D/mujs_$(basename $f .c).o"; done
cc $COMMON -c Source/html/jsbridge.c        -o "$D/jsbridge.o"
cc $COMMON -c Source/html/jsbridge_mujs.c   -o "$D/mu_bridge.o"
cc $COMMON -c Source/html/document.c        -o "$D/document.o"
cc $COMMON -c Source/html/dom.c             -o "$D/dom.o"
cc $COMMON -c Source/html/tokenizer.c       -o "$D/tokenizer.o"
cc $COMMON -c Source/html/entities.c        -o "$D/entities.o"
cc $COMMON -c Source/html/css.c             -o "$D/css.o"
cc $COMMON -c Source/html/jsext.c           -o "$D/jsext.o"
cc $COMMON -c Source/html/readability.c     -o "$D/readability.o"
cc $COMMON -c Source/core/storage.c         -o "$D/storage.o"
cc $COMMON -c Source/core/constants.c       -o "$D/constants.o"
cc $COMMON -c Source/util/pdtimer.c         -o "$D/pdtimer.o"
cc $COMMON -c Source/core/tasks.c           -o "$D/tasks.o"

sed 's/__GZ_FIXTURES__/#include "fixtures.h"/' tests/gzip_host_test.c > "$D/test.c"
cc $COMMON -I"$D" -c "$D/test.c" -o "$D/test.o"

# Host stubs: pluto_realloc backend (main.c owns it in the real build).
# pluto_pd lives in the test itself (returns the fake API).
cat > "$D/stubs.c" <<'EOF'
#include <stdlib.h>
void *pluto_realloc(void *p, size_t n) { return realloc(p, n); }
EOF
cat > "$D/stubs2.c" <<'EOF'
#include <stdlib.h>
/* pluto_free/pluto_malloc are main.c-owned wrappers over pluto_realloc. */
extern void *pluto_realloc(void *p, size_t n);
void pluto_free(void *p) { pluto_realloc(p, 0); }
void *pluto_malloc(size_t n) { return pluto_realloc(0, n); }
EOF
# The 3 engine vtables not under test: jsbridge.c references all four.
# Duktape + QuickJS need their shim define sets; XS its platform headers.
cc $COMMON -c "$D/stubs.c"  -o "$D/stubs.o"
cc $COMMON -c "$D/stubs2.c" -o "$D/stubs2.o"
SHIMD="-DCONFIG_VERSION=\"2026-06-04\" -D_GNU_SOURCE=1 -D_POSIX_THREADS=1 -D__TM_GMTOFF=tm_gmtoff -Djs_malloc=pluto_qjs_malloc -Djs_free=pluto_qjs_free -Djs_realloc=pluto_qjs_realloc -Djs_strdup=pluto_qjs_strdup"
XSFLAGS='-DINCLUDE_XSPLATFORM -DXSPLATFORM="xs_platform.h"'
cc $COMMON -c Source/html/jsbridge_duktape.c  -o "$D/duk_bridge.o"
cc $COMMON $SHIMD -c Source/html/jsbridge_quickjs.c -o "$D/qjs_bridge.o"
cc $COMMON $XSFLAGS -c Source/html/jsbridge_xs.c    -o "$D/xs_bridge.o"
cc $COMMON -c Source/js/duktape/duktape.c -o "$D/duk_eng.o"
cc $COMMON $SHIMD -c Source/html/qjs_pthread_stubs.c -o "$D/qjs_thr.o"
for f in Source/html/qjs_shim_*.c; do cc $COMMON $SHIMD -c "$f" -o "$D/shim_$(basename $f .c).o"; done
i=0
for f in $(ls Source/js/xs_moddable/sources/*.c | grep -vE "xsum.c|xsffi.c"); do
  cc $COMMON $XSFLAGS -c "$f" -o "$D/xssrc_$i.o"; i=$((i+1))
done
cc -fsanitize=address,undefined "$D"/*.o -o /tmp/gztest
echo BUILT /tmp/gztest
UBSAN_OPTIONS=halt_on_error=0 /tmp/gztest
