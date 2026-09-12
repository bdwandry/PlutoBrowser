/*
 * PlutoBrowser — pdtimer.c
 * Timer framework (Phase 4). See pdtimer.h for the contract.
 *
 * Implementation: fixed-capacity array of timer records (the Lua reference
 * never has more than a handful of timers live; 64 slots is generous). A timer
 * whose callback fires is removed; a callback may schedule new timers safely
 * (new entries are appended after the iteration window if needed — we simply
 * note the growable case and let it be picked up next frame).
 */
#include <string.h>

#include "util/pdtimer.h"

#define PDTIMER_MAX_SLOTS 64

typedef struct
{
    int used;
    unsigned int id;
    unsigned int fireAtMs;
    PDTimerCallback cb;
    void *userdata;
} PDTimerSlot;

static PDTimerSlot g_slots[PDTIMER_MAX_SLOTS];
static unsigned int g_nextId = 1;
static PlaydateAPI *g_pd = NULL;

void pdtimer_init(PlaydateAPI *pd)
{
    g_pd = pd;
    memset(g_slots, 0, sizeof(g_slots));
    g_nextId = 1;
}

unsigned int pdtimer_perform_after_delay(PlaydateAPI *pd, unsigned int delayMs,
                                         PDTimerCallback cb, void *userdata)
{
    if (!pd || !cb)
    {
        return 0;
    }
    g_pd = pd;

    for (int i = 0; i < PDTIMER_MAX_SLOTS; i++)
    {
        if (!g_slots[i].used)
        {
            g_slots[i].used = 1;
            g_slots[i].id = g_nextId++;
            g_slots[i].fireAtMs = pd->system->getCurrentTimeMilliseconds() + delayMs;
            g_slots[i].cb = cb;
            g_slots[i].userdata = userdata;
            return g_slots[i].id;
        }
    }
    return 0; /* table full */
}

void pdtimer_update(void)
{
    if (!g_pd)
    {
        return;
    }

    unsigned int now = g_pd->system->getCurrentTimeMilliseconds();

    /* Collect due timers first so a callback that schedules another timer
     * (image-decode 16ms chains) cannot be fired in the same pass. */
    PDTimerCallback due[PDTIMER_MAX_SLOTS];
    void *dueUd[PDTIMER_MAX_SLOTS];
    unsigned int dueId[PDTIMER_MAX_SLOTS];
    int dueCount = 0;

    for (int i = 0; i < PDTIMER_MAX_SLOTS; i++)
    {
        if (g_slots[i].used && (int)(now - g_slots[i].fireAtMs) >= 0)
        {
            due[dueCount] = g_slots[i].cb;
            dueUd[dueCount] = g_slots[i].userdata;
            dueId[dueCount] = g_slots[i].id;
            dueCount++;
            g_slots[i].used = 0; /* one-shot: remove before firing */
        }
    }

    for (int i = 0; i < dueCount; i++)
    {
        (void)dueId[i];
        due[i](dueUd[i]);
    }
}

void pdtimer_cancel_all(void)
{
    for (int i = 0; i < PDTIMER_MAX_SLOTS; i++)
    {
        g_slots[i].used = 0;
    }
}

void pdtimer_cancel(unsigned int id)
{
    if (id == 0)
    {
        return;
    }
    for (int i = 0; i < PDTIMER_MAX_SLOTS; i++)
    {
        if (g_slots[i].used && g_slots[i].id == id)
        {
            g_slots[i].used = 0;
            return;
        }
    }
}

int pdtimer_pending_count(void)
{
    int n = 0;
    for (int i = 0; i < PDTIMER_MAX_SLOTS; i++)
    {
        if (g_slots[i].used)
        {
            n++;
        }
    }
    return n;
}
