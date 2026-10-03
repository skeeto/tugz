#ifndef GZ_COMMON_H
#define GZ_COMMON_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed int i32;
typedef signed long long i64;
typedef __SIZE_TYPE__ iz;
typedef __UINTPTR_TYPE__ uz;
typedef int b32;
typedef char byte;

#define countof(a) (iz)(sizeof(a) / sizeof(*(a)))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void *memcpy(void *, const void *, iz);
void *memset(void *, int, iz);
void *memmove(void *, const void *, iz);
int memcmp(const void *, const void *, iz);
iz strlen(const char *);

typedef struct {
    byte *beg, *end;
} arena_mark;

void arena_init(void);
void *arena_alloc(iz size, iz align);
void *arena_zalloc(iz size, iz align);
arena_mark arena_save(void);
void arena_restore(arena_mark m);

void fatal(const char *msg);

#endif