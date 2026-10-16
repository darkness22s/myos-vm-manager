/* kernel/lib.c - string helpers and a first-fit kernel heap. */
#include "kernel.h"

void *memcpy(void *dst, const void *src, u32 n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, u32 n)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

int memcmp(const void *a, const void *b, u32 n)
{
    const u8 *x = a, *y = b;
    for (u32 i = 0; i < n; i++)
        if (x[i] != y[i])
            return (int)x[i] - (int)y[i];
    return 0;
}

u32 strlen(const char *s)
{
    u32 n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8)*a - (int)(u8)*b;
}

/* Heap: a row of blocks, each with a 16-byte header. Adjacent free blocks are
   merged while kmalloc scans for space. */
typedef struct block {
    u32 size;                      /* payload bytes */
    u32 used;
    u32 pad[2];                    /* keeps payloads 16-byte aligned */
} block_t;

static u8 *heap_lo, *heap_hi;

static block_t *next_block(block_t *b)
{
    return (block_t *)((u8 *)(b + 1) + b->size);
}

void heap_init(void *start, u32 size)
{
    heap_lo = start;
    heap_hi = (u8 *)start + size;
    block_t *b = start;
    b->size = size - sizeof(block_t);
    b->used = 0;
}

void *kmalloc(u32 n)
{
    if (n == 0)
        n = 1;
    n = (n + 15u) & ~15u;
    block_t *b = (block_t *)heap_lo;
    while ((u8 *)b < heap_hi) {
        if (!b->used) {
            block_t *nb = next_block(b);
            while ((u8 *)nb < heap_hi && !nb->used) {
                b->size += sizeof(block_t) + nb->size;
                nb = next_block(b);
            }
            if (b->size >= n) {
                if (b->size >= n + sizeof(block_t) + 16) {
                    block_t *rest = (block_t *)((u8 *)(b + 1) + n);
                    rest->size = b->size - n - sizeof(block_t);
                    rest->used = 0;
                    b->size = n;
                }
                b->used = 1;
                return b + 1;
            }
        }
        b = next_block(b);
    }
    return NULL;
}

void kfree(void *p)
{
    if (p)
        ((block_t *)p - 1)->used = 0;
}

void heap_stats(u32 *used, u32 *free_bytes, u32 *largest_free)
{
    u32 u = 0, f = 0, big = 0;
    block_t *b = (block_t *)heap_lo;
    while ((u8 *)b < heap_hi) {
        if (b->used) {
            u += b->size;
        } else {
            f += b->size;
            if (b->size > big)
                big = b->size;
        }
        b = next_block(b);
    }
    *used = u;
    *free_bytes = f;
    *largest_free = big;
}
