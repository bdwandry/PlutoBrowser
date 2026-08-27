typedef struct PlaydateSystem { void* (*realloc)(void*, size_t); } PlaydateSystem; typedef struct PlaydateAPI { PlaydateSystem* system; } PlaydateAPI;
