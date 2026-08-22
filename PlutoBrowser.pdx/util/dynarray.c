// dynarray.c — generic dynamic array implementation.

#include "dynarray.h"

#include <string.h>

#include "mem.h"

void da_init(DynArray* a, size_t elemsize)
{
    a->items = NULL;
    a->count = 0;
    a->cap = 0;
    a->elemsize = elemsize;
}

void da_free(DynArray* a)
{
    if (a->items != NULL) {
        pluto_free(a->items);
    }
    a->items = NULL;
    a->count = 0;
    a->cap = 0;
}

void da_clear(DynArray* a)
{
    a->count = 0;
}

int da_reserve(DynArray* a, size_t extra)
{
    size_t newCap;
    void* ni;

    if (a->cap - a->count >= extra) {
        return 1;
    }
    newCap = (a->cap == 0) ? 8 : a->cap;
    while (newCap < a->count + extra) {
        newCap *= 2;
    }
    ni = pluto_realloc(a->items, newCap * a->elemsize);
    if (ni == NULL) {
        return 0;
    }
    a->items = ni;
    a->cap = newCap;
    return 1;
}

void* da_push_new(DynArray* a)
{
    if (!da_reserve(a, 1)) {
        return NULL;
    }
    return (char*)a->items + (a->count++ * a->elemsize);
}

void* da_push(DynArray* a, const void* elem)
{
    void* slot = da_push_new(a);
    if (slot != NULL) {
        memcpy(slot, elem, a->elemsize);
    }
    return slot;
}

void* da_get(DynArray* a, size_t i)
{
    if (i >= a->count) {
        return NULL;
    }
    return (char*)a->items + (i * a->elemsize);
}

void* da_last(DynArray* a)
{
    return da_get(a, a->count == 0 ? 0 : a->count - 1);
}

void da_pop(DynArray* a)
{
    if (a->count > 0) {
        a->count--;
    }
}

void da_remove_at(DynArray* a, size_t i)
{
    if (a == NULL || i >= a->count) {
        return;
    }
    if (i + 1 < a->count) {
        memmove((char*)a->items + i * a->elemsize,
                (char*)a->items + (i + 1) * a->elemsize,
                (a->count - i - 1) * a->elemsize);
    }
    a->count--;
}
