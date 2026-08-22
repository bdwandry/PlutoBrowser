// internal_pages.h — locally served about: pages (see internal_pages.c).

#ifndef PLUTO_INTERNAL_PAGES_H
#define PLUTO_INTERNAL_PAGES_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* url;
    const char* title;
    const char* html;
    size_t      htmlLen;
} HcInternalPage;

// NULL when url is not one of about:home / about:blank / about:acidtest.
const HcInternalPage* hc_internal_page(const char* url);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_INTERNAL_PAGES_H
