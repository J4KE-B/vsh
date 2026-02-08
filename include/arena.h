/* ============================================================================
 * vsh - Vanguard Shell
 * arena.h - Arena (region-based) memory allocator
 *
 * Provides bulk allocation with O(1) deallocation of entire regions.
 * All command parsing uses arenas -- the entire parse tree is freed in one
 * call after execution. This eliminates memory leaks by design.
 * ============================================================================ */

#ifndef VSH_ARENA_H
#define VSH_ARENA_H

#include <stddef.h>
#include <stdbool.h>

/* Default page size: 4KB */
#define ARENA_PAGE_SIZE 4096

/* Alignment for all allocations (8-byte for 64-bit) */
#define ARENA_ALIGNMENT 8

typedef struct ArenaPage {
    struct ArenaPage *next;
    size_t            size;     /* Total usable size of this page */
    size_t            used;     /* Bytes consumed so far */
    char              data[];   /* Flexible array member */
} ArenaPage;

typedef struct Arena {
    ArenaPage *head;           /* First page */
    ArenaPage *current;        /* Current page for allocations */
    size_t     page_size;      /* Default page size */
    size_t     total_allocated; /* Total bytes allocated (stats) */
    size_t     total_pages;    /* Number of pages (stats) */
} Arena;

/* Create a new arena with default page size */
Arena *arena_create(void);

/* Create a new arena with custom page size */
Arena *arena_create_sized(size_t page_size);

/* Allocate memory from the arena (8-byte aligned) */
void *arena_alloc(Arena *arena, size_t size);

/* Allocate zeroed memory from the arena */
void *arena_calloc(Arena *arena, size_t count, size_t size);

/* Duplicate a string into the arena */
char *arena_strdup(Arena *arena, const char *str);

/* Duplicate N bytes of a string into the arena (null-terminated) */
char *arena_strndup(Arena *arena, const char *str, size_t n);

/* Reset the arena (free all pages except the first, reset first page) */
void arena_reset(Arena *arena);

/* Destroy the arena and free all memory */
void arena_destroy(Arena *arena);

/* Get total bytes allocated (for debugging/stats) */
size_t arena_bytes_used(const Arena *arena);

#endif /* VSH_ARENA_H */
