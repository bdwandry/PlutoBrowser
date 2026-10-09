/*
 * netmon.c — WiFi status indicator module. See netmon.h for the honest
 * derivation (no RSSI API; OS connected flag + real first-byte latency
 * probes from http_client as the strength proxy).
 */
#include <string.h>

#include "core/netmon.h"
#include "pd_api.h"

extern PlaydateAPI *pluto_pd(void);

/* ── state ───────────────────────────────────────────────────────────────── */
#define NETMON_MAX_LATENCY_MS 60000u /* sane cap: 60s watchdog */

static int g_lastReqId = -1;       /* per-request probe latch */
static unsigned g_lastLatency = 0; /* ms, request-send -> first byte */
static int g_lastLevel = 0;        /* 1..3, derived from g_lastLatency */
static int g_faultStreak = 0;      /* consecutive network faults */

/* Latency bands (ms, first byte on a FRESH connection incl. DNS+TLS).
 * <=800ms strong, <=2500ms ok, else weak; >60s is treated as unknown. */
#define LEVEL3_MAX_MS 800u
#define LEVEL2_MAX_MS 2500u

static int os_status(void)
{
    PlaydateAPI *pd = pluto_pd();
    if (!pd || !pd->network || !pd->network->getStatus)
    {
        return (int)kWifiNotConnected; /* defensive: no API = down */
    }
    return (int)pd->network->getStatus();
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

void netmon_probe_ok(int reqId, unsigned elapsedMs)
{
    if (reqId == g_lastReqId)
    {
        return; /* same request's later reads don't re-probe */
    }
    g_lastReqId = reqId;
    g_lastLatency = elapsedMs;
    g_lastLevel = level_from_latency(elapsedMs);
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
    if (os_status() != (int)kWifiConnected)
    {
        return 0;
    }
    if (g_lastLevel == 0)
    {
        return 3; /* connected, no probe yet: optimistic (boot state) */
    }
    return g_lastLevel;
}

const char *netmon_state_name(void)
{
    switch (os_status())
    {
    case (int)kWifiConnected:
        return "connected";
    case (int)kWifiNotAvailable:
        return "not-available";
    default:
        return "not-connected";
    }
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
