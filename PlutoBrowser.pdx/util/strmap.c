// strmap.c — string-keyed hash map implementation (separate chaining).

#include "strmap.h"

#include <string.h>

#include "mem.h"

typedef struct SMNode {
    struct SMNode* next;
    char* key;
    void* value;
} SMNode;

struct StrMap {
    SMNode** buckets;
    size_t bucketCount;
    size_t count;
};

static unsigned long sm_hash(const char* s)
{
    // FNV-1a 32-bit
    unsigned long h = 2166136261UL;
    while (*s != '\0') {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }
    return h;
}

StrMap* sm_create(size_t initialBuckets)
{
    StrMap* m = (StrMap*)pluto_malloc(sizeof(StrMap));
    if (m == NULL) {
        return NULL;
    }
    if (initialBuckets < 8) {
        initialBuckets = 8;
    }
    m->buckets = (SMNode**)pluto_malloc(initialBuckets * sizeof(SMNode*));
    if (m->buckets == NULL) {
        pluto_free(m);
        return NULL;
    }
    memset(m->buckets, 0, initialBuckets * sizeof(SMNode*));
    m->bucketCount = initialBuckets;
    m->count = 0;
    return m;
}

static void sm_rehash(StrMap* m, size_t newCount)
{
    SMNode** nb = (SMNode**)pluto_malloc(newCount * sizeof(SMNode*));
    size_t i;
    if (nb == NULL) {
        return; // keep old table on OOM
    }
    memset(nb, 0, newCount * sizeof(SMNode*));
    for (i = 0; i < m->bucketCount; i++) {
        SMNode* n = m->buckets[i];
        while (n != NULL) {
            SMNode* next = n->next;
            size_t idx = sm_hash(n->key) % newCount;
            n->next = nb[idx];
            nb[idx] = n;
            n = next;
        }
    }
    pluto_free(m->buckets);
    m->buckets = nb;
    m->bucketCount = newCount;
}

int sm_put(StrMap* m, const char* key, void* value)
{
    size_t idx;
    SMNode* n;

    if ((m->count + 1) > m->bucketCount * 2) {
        sm_rehash(m, m->bucketCount * 2);
    }
    idx = sm_hash(key) % m->bucketCount;
    for (n = m->buckets[idx]; n != NULL; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            n->value = value;
            return 1;
        }
    }
    n = (SMNode*)pluto_malloc(sizeof(SMNode));
    if (n == NULL) {
        return 0;
    }
    n->key = pluto_strdup(key);
    if (n->key == NULL) {
        pluto_free(n);
        return 0;
    }
    n->value = value;
    n->next = m->buckets[idx];
    m->buckets[idx] = n;
    m->count++;
    return 1;
}

void* sm_get(const StrMap* m, const char* key)
{
    size_t idx;
    SMNode* n;
    if (m == NULL || key == NULL) {
        return NULL;
    }
    idx = sm_hash(key) % m->bucketCount;
    for (n = m->buckets[idx]; n != NULL; n = n->next) {
        if (strcmp(n->key, key) == 0) {
            return n->value;
        }
    }
    return NULL;
}

int sm_has(const StrMap* m, const char* key)
{
    return sm_get(m, key) != NULL;
}

int sm_remove(StrMap* m, const char* key)
{
    size_t idx;
    SMNode** pp;
    if (key == NULL) {
        return 0;
    }
    idx = sm_hash(key) % m->bucketCount;
    for (pp = &m->buckets[idx]; *pp != NULL; pp = &(*pp)->next) {
        SMNode* n = *pp;
        if (strcmp(n->key, key) == 0) {
            *pp = n->next;
            pluto_free(n->key);
            pluto_free(n);
            m->count--;
            return 1;
        }
    }
    return 0;
}

size_t sm_count(const StrMap* m)
{
    return m == NULL ? 0 : m->count;
}

void sm_foreach(StrMap* m, SMIterFn fn, void* userdata)
{
    size_t i;
    if (fn == NULL) {
        return;
    }
    for (i = 0; i < m->bucketCount; i++) {
        SMNode* n;
        for (n = m->buckets[i]; n != NULL; n = n->next) {
            fn(n->key, n->value, userdata);
        }
    }
}

void sm_destroy(StrMap* m)
{
    size_t i;
    if (m == NULL) {
        return;
    }
    for (i = 0; i < m->bucketCount; i++) {
        SMNode* n = m->buckets[i];
        while (n != NULL) {
            SMNode* next = n->next;
            pluto_free(n->key);
            pluto_free(n);
            n = next;
        }
    }
    pluto_free(m->buckets);
    pluto_free(m);
}
