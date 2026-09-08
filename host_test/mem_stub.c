#include "util/mem.h"
#include <stdlib.h>
#include <string.h>
void mem_init(struct PlaydateAPI* pd){(void)pd;}
void* pluto_malloc(size_t n){return malloc(n);}
void* pluto_calloc(size_t a,size_t b){return calloc(a,b);}
void* pluto_realloc(void* p,size_t n){return realloc(p,n);}
void pluto_free(void* p){free(p);}
char* pluto_strdup(const char* s){return s?strdup(s):NULL;}
char* pluto_strndup(const char* s,size_t n){size_t l=strnlen(s,n);char*c=malloc(l+1);if(!c)return NULL;memcpy(c,s,l);c[l]=0;return c;}
