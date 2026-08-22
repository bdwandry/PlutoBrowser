// dynarray.h — generic dynamic array.
//
// Replaces Lua sequences (t[#t+1] = v). Elements are copied by value into a
// contiguous heap block. Use da_push_new() to grab a slot and fill it, or
// da_push() to copy an element in.

#ifndef PLUTO_DYNARRAY_H
#define PLUTO_DYNARRAY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void*  items;
    size_t count;
    size_t cap;
    size_t elemsize;
} DynArray;

void  da_init(DynArray* a, size_t elemsize);
void  da_free(DynArray* a);
void  da_clear(DynArray* a); // count=0, keep storage

int   da_reserve(DynArray* a, size_t extra); // 0 on OOM

// Returns pointer to the new (uninitialized) slot, or NULL on OOM.
void* da_push_new(DynArray* a);

// Copies *elem into a new slot. Returns slot pointer or NULL on OOM.
void* da_push(DynArray* a, const void* elem);

void* da_get(DynArray* a, size_t i);   // NULL if out of range
void* da_last(DynArray* a);            // NULL if empty
void  da_pop(DynArray* a);             // no-op if empty

// Removes element at index i (memmove down). No-op if out of range.
void  da_remove_at(DynArray* a, size_t i);

#ifdef __cplusplus
}
#endif

#endif // PLUTO_DYNARRAY_H
