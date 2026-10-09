/*
 * netmon — WiFi status indicator (the Playdate has NO RSSI/signal-strength
 * API: pd->network->getStatus() only reports connected / not-connected /
 * not-available). So the "signal strength" is derived honestly:
 *
 *   - OS status:         kWifiConnected vs everything else (the truth the
 *                        SDK gives us — drives the connected/not-connected
 *                        icon).
 *   - Level (1..3 dots): the browser's own measured first-byte latency of
 *                        the LAST network probe (request-send → first byte,
 *                        i.e. DNS + TLS + TTFB of a fresh connection). A
 *                        fast round-trip = strong link, a slow one = weak.
 *                        Consecutive network faults drop a dot (capped at
 *                        1 — if the OS says connected, show a link).
 *
 * The http_client feeds probes (netmon_probe_ok / netmon_probe_fail) at its
 * real success/failure points; chrome.c paints the icon. User-refused
 * access dialogs and too-many-redirects do NOT count as network faults.
 */
#ifndef PLUTO_NETMON_H
#define PLUTO_NETMON_H

/* Record a successful first byte: reqId dedupes (per-request latch — a
 * streaming body fires many reads, only the FIRST counts), elapsedMs is
 * request-send → first byte. Recomputes the dot level. */
void netmon_probe_ok(int reqId, unsigned elapsedMs);

/* Record a network-layer failure (connect fail / timeout / send fail /
 * no response). Lowers the dot level toward 1; never to 0 by itself —
 * the OS status owns the not-connected state. */
void netmon_probe_fail(void);

/* 1-second OS-status poll (user fix: the icon showed "disconnected" for
 * whole sessions on pages that never fire a network probe). Re-reads
 * pd->network->getStatus() every 1000ms, updates the cached level badge
 * latency band state, and RETURNS 1 exactly when the visible status
 * changed (level 0<->N or the lastLatency band flipped) so the caller can
 * force a redraw. Cheap: one SDK call per second. */
int netmon_poll(void);

/* Current dot level 0..3. 0 = not connected (per the OS), 1..3 = connected
 * at weak/ok/strong per last measured latency + fault streak. */
int netmon_level(void);

/* Human state for logs: "off", "connected", "not-available", "down". */
const char *netmon_state_name(void);

/* Last measured first-byte latency in ms (0 = none yet). */
unsigned netmon_last_latency_ms(void);

/* Paint the icon at (x, y) = top-left of the tallest bar area (9x9 region).
 * level 0: rounded square + X (not connected). level 1..3: three ascending
 * signal bars, filled bottom-up, unfilled ones outlined (macOS style). */
void netmon_draw(int x, int y);

#endif /* PLUTO_NETMON_H */
