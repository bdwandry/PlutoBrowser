#!/usr/bin/env python3
# PlutoBrowser Network program — deterministic lab server (SIMULATOR ONLY).
#
# Serves scripted HTTP fixtures on 127.0.0.1:8931 for about:netprobe driver
# boots: known bodies, gzip, chunked framing, cookies, redirects (302/307),
# verb echo, conditional-revalidation ETag, Range, a slow endpoint that
# exercises the first-byte watchdog, and a big-body spill target.
#
# This server is TEST INFRASTRUCTURE ONLY — the shipped app never references
# it (the lab origin reaches the app exclusively via the PLUTO_NETLAB_URL
# build define on simulator driver builds). Never runs on the device.
#
# Usage: python3 tests/netlab_server.py            (Ctrl-C to stop)
# Every request is logged to stdout: method, path, body size, and the
# receipt-relevant request headers (Cookie / Range / If-None-Match /
# X-Pluto-Receipt). That stdout is the lab-side receipt trail.

import gzip
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = 8931

TEXT_BODY = "NETLAB-OK-0123456789ABCDEF"
ERR_BODY = "NETLAB-404-BODY-00"
RANGE_BODY = "NETLAB-RANGE-BODY-0123456789"  # len 29
RECEIPT_HEADERS = ("Cookie", "Range", "If-None-Match", "X-Pluto-Receipt")

# Cache-lab versioning (Phase 1): /fresh and /fresh2 alternate a body
# VERSION on every FULL GET (If-None-Match absent, or not matching the
# current version). A client WITHOUT a response cache gets a body bump on
# its second fetch — an honest MISS by body content — while a working
# cache revalidates via 304 or serves fresh locally and keeps the same
# body. No timers needed anywhere: /fresh carries max-age=1 (fresh across
# the battery's back-to-back window), /fresh2 carries max-age=0
# (immediately stale → every fetch revalidates back-to-back).
g_cache_versions = {}
g_state_lock = threading.Lock()


def log_header_brief(h):
    out = []
    for key in RECEIPT_HEADERS:
        val = h.get(key)
        if val:
            out.append("%s=%s" % (key, val))
    return " ".join(out) if out else "-"


class LabHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # Keep the default error-path quiet so receipts stay readable.
    def log_message(self, fmt, *args):
        pass

    def _receipt(self, label):
        size = 0
        self._drainedBody = b""
        self._drainedByLen = 0
        cl = self.headers.get("Content-Length")
        if self.command == "POST" and cl:
            try:
                size = int(cl)
            except ValueError:
                size = 0
            # Drain any request body NOW, whatever the endpoint does with it:
            # leftover body bytes on a keep-alive socket corrupt the next
            # request parse (curl proved it). Close one request per socket —
            # the browser client sends Connection: close anyway.
            try:
                if size:
                    self._drainedBody = self.rfile.read(size)
            except Exception:
                pass
            self._drainedByLen = size
        else:
            try:
                self._drainedBody = self.rfile.read(0)
            except Exception:
                self._drainedBody = b""
        print("[netlab] %s %s bodyLen=%d %s" % (
            self.command, label, size, log_header_brief(self.headers)),
            flush=True)
        self.close_connection = True

    def _send(self, status, body, ctype="text/plain", extra=()):
        payload = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(payload)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(payload)
        self.close_connection = True

    def do_POST(self):
        self.do_GET()

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        query = self.path.split("?", 1)[1] if "?" in self.path else ""

        if path == "/text" or path == "/":
            self._receipt("/text")
            self._send(200, TEXT_BODY)

        elif path == "/gzip":
            self._receipt("/gzip")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Encoding", "gzip")
            chunk = gzip.compress(TEXT_BODY.encode("utf-8"), 6)
            self.send_header("Content-Length", str(len(chunk)))
            self.end_headers()
            self.wfile.write(chunk)

        elif path == "/chunked":
            self._receipt("/chunked")
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            # Two chunks + EOF chunk, then close: exercises the standing
            # decode_chunked path including a mid-body frame boundary.
            self.wfile.write(("%x\r\n" % 10).encode() + TEXT_BODY[:10].encode()
                             + "\r\n".encode())
            self.wfile.write(("%x\r\n" % (len(TEXT_BODY) - 10)).encode()
                             + TEXT_BODY[10:].encode() + "\r\n0\r\n\r\n".encode())
            self.close_connection = True

        elif path == "/status404":
            self._receipt("/status404")
            self._send(404, ERR_BODY)

        elif path == "/setcookie":
            self._receipt("/setcookie")
            self._send(200, "COOKIE-SET", extra=(
                ("Set-Cookie", "nlab=1; Path=/"),
            ))

        elif path == "/setcookieexp":
            # Phase 5: Max-Age=1 — the row fetches immediately (present),
            # waits ~1.2s, and must NOT see it replayed (journaled probe).
            self._receipt("/setcookieexp")
            self._send(200, "COOKIE-EXP-SET", extra=(
                ("Set-Cookie", "exp=1; Max-Age=1; Path=/"),
            ))

        elif path == "/setcookiedmn":
            # Phase 5: Domain attribute on a NUMERIC request-host — RFC 6265
            # §5.1.3/§5.3 says the browser refuses the whole Set-Cookie, so
            # dmn=1 must never reach the jar.
            self._receipt("/setcookiedmn")
            self._send(200, "COOKIE-DMN-SET", extra=(
                ("Set-Cookie", "dmn=1; Domain=127.0.0.1; Path=/"),
            ))

        elif path == "/echo":
            self._receipt("/echo")
            saw = self.headers.get("Cookie") or "none"
            # Phase 1 cache hardening: the echo varies per cookie jar, so
            # the lab advertises it (any Vary → the client skips storing).
            self._send(200, "COOKIE=" + saw, extra=(("Vary", "Cookie"),))

        elif path == "/fresh" or path == "/fresh2":
            self._receipt(path)
            inm = self.headers.get("If-None-Match") or ""
            with g_state_lock:
                cur = g_cache_versions.get(path, 0)
                etag_cur = '"FRESH-%d"' % cur
                if cur > 0 and inm and etag_cur in inm:
                    # Validator matches the CURRENT version → not modified.
                    self._send(304, "", extra=(("ETag", etag_cur),))
                else:
                    cur += 1
                    g_cache_versions[path] = cur
                    # The served ETag must mirror the STORED version: a full
                    # GET that bumps the version answers with the new token,
                    # so the next fetch's If-None-Match (the token the client
                    # JUST stored) matches `etag_cur` and revalidates to 304.
                    # (The pre-increment value made the validator chain
                    # permanently lag by one version — no 304 was ever
                    # answerable; receipted by the first reval304 MISS run.)
                    etag_cur = '"FRESH-%d"' % cur
                    # /fresh: max-age=1 (freshness lane); /fresh2: max-age=0
                    # (immediately stale → always revalidate).
                    maxage = 1 if path == "/fresh" else 0
                    self._send(200, "NETLAB-FRESH-BODY-%d" % cur, extra=(
                        ("ETag", etag_cur),
                        ("Cache-Control", "max-age=%d" % maxage),
                    ))

        elif path == "/range":
            self._receipt("/range")
            rng = self.headers.get("Range") or ""
            if rng == "bytes=0-4":
                part = RANGE_BODY[0:5].encode("utf-8")
                self.send_response(206)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Range", "bytes 0-4/%d" % len(RANGE_BODY))
                self.send_header("Content-Length", str(len(part)))
                self.end_headers()
                self.wfile.write(part)
            else:
                self._send(200, RANGE_BODY, extra=(("Accept-Ranges", "bytes"),))

        elif path == "/hdr":
            self._receipt("/hdr")
            saw = self.headers.get("X-Pluto-Receipt") or "none"
            self._send(200, "HDR=" + saw, extra=(("Vary", "X-Pluto-Receipt"),))

        elif path == "/redirect":
            self._receipt("/redirect")
            mode = "302"
            hops = 0
            for kv in query.split("&"):
                if not kv:
                    continue
                k, _, v = kv.partition("=")
                if k == "mode":
                    mode = v or "302"
                elif k == "hops":
                    hops = int(v) if v.isdigit() else 0
            status = 307 if mode == "307" else 302
            nxt = "/text" if hops <= 0 else "/redirect?mode=%s&hops=%d" % (
                mode, hops - 1)
            self._send(status, "", extra=(("Location", nxt),))

        elif path == "/redirectpost":
            self._receipt("/redirectpost")
            mode = "302"
            for kv in query.split("&"):
                k, _, v = kv.partition("=")
                if k == "mode":
                    mode = v or "302"
            status = 307 if mode == "307" else 302
            self._send(status, "", extra=(("Location", "/verbecho"),))

        elif path == "/verbecho":
            self._receipt("/verbecho")
            body = self._drainedBody
            self._send(200, "method=%s body=%s" % (
                self.command, body.decode("utf-8", "replace") if body else "none"),
                extra=(("Vary", "X-HTTP-Method-Override"),))

        elif path == "/slow":
            self._receipt("/slow")
            try:
                # Phase 4 labtimeout: the row wants a BOUNDED rejection — the
                # client watchdog must fire before the server answers. The old
                # 40s sleep landed BEFORE the 60s request watchdog (and before
                # the 20s open→first-byte watchdog), so the row resolved-ok.
                # 75s > every watchdog → honest bounded rejection.
                time.sleep(75)
            except Exception:
                return
            self._send(200, "NETLAB-SLOW-LATE")

        elif path == "/big":
            self._receipt("/big")
            nbytes = 4194304
            for kv in query.split("&"):
                k, _, v = kv.partition("=")
                if k == "bytes" and v.isdigit():
                    nbytes = max(1024, min(int(v), 16 * 1024 * 1024))
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(nbytes))
            self.end_headers()
            # Deterministic line pattern; streamed so nobody buffers 4MB.
            sent = 0
            while sent < nbytes:
                want = min(65536, nbytes - sent)
                line = ("NETLAB-BIG line %d fill " % (sent // 64)).encode()
                row = (line * (want // len(line) + 1))[:want]
                self.wfile.write(row)
                sent += want
            self.close_connection = True

        else:
            self._receipt(path)
            self._send(404, "NETLAB-NOSUCH")


class LabServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    srv = LabServer((HOST, PORT), LabHandler)
    print("[netlab] listening on %s:%d (simulator lab only)" % (HOST, PORT),
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
        print("[netlab] closed", flush=True)


if __name__ == "__main__":
    main()
