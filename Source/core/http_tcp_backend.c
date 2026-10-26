/*
 * PlutoBrowser — http_tcp_backend.c
 * R62 dual-stack plumbing: the COMPILATION WRAPPER for the raw-TCP
 * client. This TU defines PLUTO_COMPILE_TCP_BACKEND — arming the rename
 * macros (http_tcp_rename.h) — and then includes the ENTIRE TCP client
 * source, so the machine compiles ONCE with every public symbol mapped
 * to its http_tcp_* backend name.
 *
 * Source/core/http_client.c is INCLUDED, never edited: its source text
 * is byte-identical to the receipted R61 implementation. The static
 * state (g_buf, g_tcp, the cache, the funnel accounting) lives in this
 * TU exactly as it lived in http_client.o — single instance, single
 * flight, identical behavior.
 *
 * Included BY the Makefile's source list in place of http_client.c.
 */
#define PLUTO_COMPILE_TCP_BACKEND
#include "core/http_tcp_rename.h" /* arms the 10 renames (guard-checked) */
#include "core/http_client.c"
