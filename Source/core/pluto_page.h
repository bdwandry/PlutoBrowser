/*
 * pluto_page.h — SW8 disk-backed DOM paging (see pluto_page.c for design).
 *
 * The public entry points live in html/dom.h (dom_page_out / dom_touch /
 * dom_is_paged / dom_page_free_subtree) because both html/ and core/ call
 * them. This header adds the automatic pressure policy entry point.
 */
#ifndef PLUTO_PAGE_H
#define PLUTO_PAGE_H

#include "html/dom.h"

/* Automatic page-out policy (SW3a-aligned): when the live app heap is
 * within PAGE_HEADROOM_TRIGGER of the soft budget, page the largest
 * qualifying DOM subtrees of the doc's live tree to disk until headroom
 * is restored (or nothing worthwhile remains). No-op when the budget is
 * disabled (0) — pure-RAM behavior everywhere else stays untouched.
 * Returns the number of subtrees paged. */
int dom_page_out_under_pressure(DomResult *dom, const char *baseUrl);

/* Number of stubs (paged-out subtrees) currently in the live tree. */
int dom_page_stub_count(DomResult *dom);

/* Autotest hook: override the minimum subtree size (bytes) the pressure
 * policy will page. 0 restores the built-in floor. */
void dom_page_set_subtree_floor(long bytes);

#endif /* PLUTO_PAGE_H */
