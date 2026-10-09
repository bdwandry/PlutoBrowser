/*
 * netmon.c — WiFi status indicator module. See netmon.h for the honest
 * derivation (no RSSI API; OS connected flag + real first-byte latency
 * probes from http_client as the strength proxy).
 */
#include <string.h>

#include "core/netmon.h"
#include "core/logger.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);

/* ── state ───────────────────────────────────────────────────────────────── */
#define NETMON_MAX_LATENCY_MS 60000u /* sane cap: 60s watchdog */

static int g_lastReqId = -1;       /* per-request probe latch */
static unsigned g_lastLatency = 0; /* ms, request-send -> first byte */
static int g_lastLevel = 0;        /* 1..3, derived from g_lastLatency */
static int g_faultStreak = 0;      /* consecutive network faults */
static unsigned g_lastOkMs = 0;    /* timestamp of the last successful probe */
static unsigned g_nowMs = 0;       /* monotonic now, refreshed by poll */

/* Latency bands (ms, first byte on a FRESH connection incl. DNS+TLS).
 * <=800ms strong, <=2500ms ok, else weak; >60s is treated as unknown. */
#define LEVEL3_MAX_MS 800u
#define LEVEL2_MAX_MS 2500u

/* SIM workaround (fix for "always disconnected" on home screen): the
 * Playdate SIMULATOR's C pd->network->getStatus() always returns 0
 * (kWifiNotConnected) even with a live host network — verified by running
 * Panic's own Networking example (Lua) in the SAME simulator at the same
 * time: Lua reports Connected while our C poll reads 0 forever. The DEVICE
 * API is correct. So "connected" is derived from BOTH sources of truth:
 * the OS status OR a recent successful first-byte probe from
 * http_client (PROBE_FRESH_MS window). On device the OS status governs;
 * in the sim the app's own demonstrated-working networking converges the
 * icon to connected after the first real page load. */
#define PROBE_FRESH_MS 600000u /* 10 minutes */

/* How often the 1s poll may re-issue setEnabled(true) when the OS reports
 * not connected. Conservative: association takes seconds, and hammering the
 * radio OS-side is unkind. 15s between kicks. */
#define RADIO_KICK_INTERVAL_MS 15000u

static int os_status(void)
{
    PlaydateAPI *pd = pluto_pd();
    if (!pd || !pd->network || !pd->network->getStatus)
    {
        return (int)kWifiNotConnected; /* defensive: no API = down */
    }
    return (int)pd->network->getStatus();
}

/* Connect/link present = OS says so, OR a probe succeeded recently
 * (covers the simulator's always-0 C getStatus). */
static int link_present(void)
{
    if (os_status() == (int)kWifiConnected)
    {
        return 1;
    }
    return g_lastOkMs != 0 && g_nowMs != 0 &&
           (g_nowMs - g_lastOkMs) <= PROBE_FRESH_MS;
}

static int level_from_latency(unsigned ms)
{
    if (ms == 0 || ms > NETMON_MAX_LATENCY_MS)
    {
        return 1; /* unknown/absurd: assume weak, not dead */
    }
    if (ms <= LEVEL3_MAX_MS)
    {
        return 3;
    }
    if (ms <= LEVEL2_MAX_MS)
    {
        return 2;
    }
    return 1;
}

/* Radio-kick completion callback (R-critical: NEVER pass NULL as the
 * setEnabled callback — the device OS derefs/calls it and hard-crashes
 * with "Error accessing buffer at 0x00000000"; device errorlog 2026-10-09
 * 17:54). It's a fire-and-forget completion: nothing to record. */
static void netmon_kick_cb(PDNetErr err)
{
    (void)err; /* completion of the association request; poll reads truth */
}

void netmon_probe_ok(int reqId, unsigned elapsedMs)
{
    if (reqId == g_lastReqId)
    {
        return; /* same request's later reads don't re-probe */
    }
    g_lastReqId = reqId;
    g_lastLatency = elapsedMs;
    g_lastLevel = level_from_latency(elapsedMs);
    g_lastOkMs = g_nowMs; /* freshness anchor for link_present() */
    g_faultStreak = 0; /* a success breaks the fault streak */
}

void netmon_probe_fail(void)
{
    ++g_faultStreak;
    /* Each fault drops a dot (floor 1: the OS still says a link exists —
     * level 0 is owned exclusively by the OS status). */
    if (g_lastLevel > 1)
    {
        --g_lastLevel;
    }
    if (g_faultStreak >= 3)
    {
        g_lastLevel = 1; /* pinned weak until the next success */
    }
}

int netmon_level(void)
{
    if (!link_present())
    {
        return 0;
    }
    if (g_lastLevel == 0)
    {
        return 3; /* connected, no probe yet: optimistic (boot state) */
    }
    return g_lastLevel;
}

int netmon_poll(void)
{
    static unsigned int g_lastPollMs = 0;
    static unsigned g_lastKickMs = 0;
    static int g_lastVisible = -1; /* 0..3 level as last reported */
    PlaydateAPI *pd = pluto_pd();
    if (!pd || !pd->system)
    {
        return 0;
    }
    unsigned int now = pd->system->getCurrentTimeMilliseconds();
    if (g_lastPollMs != 0 && now - g_lastPollMs < 1000u)
    {
        return 0; /* 1-second cadence (user spec) */
    }
    g_lastPollMs = now;
    g_nowMs = now; /* monotonic clock anchor for link_present() */

    int st = os_status();

    /* First-launch association (user screenshot: at boot the icon shows
     * not-connected and stays that way until a page load — the OS's lazy
     * (re)association only happens during a real request in http_client's
     * radio-heal path). Fix: the 1s poll itself kicks the radio via
     * setEnabled(true) when the OS reports not-connected/not-available,
     * mirroring the radio-heal's recovery. No OFF-vs-lazy guard exists in
     * the SDK (a user's OS-level OFF also reports not-connected); repeated
     * kicks are re-bounced safely by the OS, and the 15s interval keeps it
     * gentle. */
    if ((st == (int)kWifiNotConnected || st == (int)kWifiNotAvailable) &&
        pd->network->setEnabled != NULL)
    {
        unsigned int nowMs = (unsigned int)now;
        if (g_lastKickMs == 0 || nowMs - g_lastKickMs > RADIO_KICK_INTERVAL_MS)
        {
            g_lastKickMs = nowMs;
            pd->network->setEnabled(1, netmon_kick_cb);
            logger_log("[netmon] radio kick at boot (os status=%d)", st);
        }
    }

    /* os_status() lives below; direct call keeps this self-contained. */
    int visible = netmon_level();
    int changed = (g_lastVisible >= 0 && visible != g_lastVisible);
    int prevVisible = g_lastVisible;
    g_lastVisible = visible;
    if (changed)
    {
        logger_log("[netmon] wifi level %d -> %d (os=%s)", prevVisible,
                   visible, netmon_state_name());
        return 1; /* caller: force a chrome redraw */
    }
    return 0;
}

const char *netmon_state_name(void)
{
    int st = os_status();
    if (st == (int)kWifiConnected || link_present())
    {
        /* The sim path: OS reads 0 forever but probes succeed. */
        return st == (int)kWifiConnected ? "connected" : "connected(probe)";
    }
    if (st == (int)kWifiNotAvailable)
    {
        return "not-available";
    }
    return "not-connected";
}

unsigned netmon_last_latency_ms(void)
{
    return g_lastLatency;
}

/* ── icon: 15x13 region, ascending bars (macOS style, enlarged) ─────────── */
void netmon_draw(int x, int y)
{
    PlaydateAPI *pd = pluto_pd();
    if (!pd || !pd->graphics)
    {
        return;
    }
    LCDColor col = kColorWhite; /* designed for the black chrome bar */
    int lvl = netmon_level();

    if (lvl <= 0)
    {
        /* Not connected: 15x11 box with an X (like macOS off). */
        pd->graphics->drawRect(x, y, 15, 11, col);
        pd->graphics->drawLine(x + 3, y + 2, x + 11, y + 8, 1, col);
        pd->graphics->drawLine(x + 3, y + 8, x + 11, y + 2, 1, col);
        return;
    }

    /* Three ascending bars 4px wide, 1px apart: bases on y+12, tops at
     * y+10 (bar1), y+6 (bar2), y+2 (bar3). Filled when lvl >= n, else a
     * 1px outline of the bar's bounding box. */
    static const struct
    {
        int bx; /* bar left, relative */
        int ty; /* bar top, relative */
    } bars[3] = {
        { 0, 10 }, { 5, 6 }, { 10, 2 },
    };
    for (int i = 0; i < 3; ++i)
    {
        int bx = x + bars[i].bx;
        int ty = y + bars[i].ty;
        if (lvl > i)
        {
            pd->graphics->fillRect(bx, ty, 4, 12 - bars[i].ty + 1, col);
        }
        else
        {
            pd->graphics->drawRect(bx, ty, 4, 12 - bars[i].ty + 1, col);
        }
    }
}
