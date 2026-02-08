/* ============================================================================
 * vsh - Vanguard Shell
 * arena.c - Arena (region-based) memory allocator
 *
 * Page-based bump allocator. Each page is a contiguous block with a flexible
 * array member. Allocations bump a pointer forward; deallocation frees all
 * pages at once, eliminating per-object bookkeeping and memory leaks.
 * ============================================================================ */

#include "arena.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- Internal helpers --------------------------------------------------- */

/* Align size up to ARENA_ALIGNMENT boundary */
static inline size_t align_up(size_t size)
{
    return (size + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);
}

/* Allocate a new page with at least `min_size` usable bytes */
static ArenaPage *page_new(size_t min_size)
{
    ArenaPage *page = malloc(sizeof(ArenaPage) + min_size);
    if (!page)
        return NULL;

    page->next = NULL;
    page->size = min_size;
    page->used = 0;
    return page;
}

/* ---- Public API --------------------------------------------------------- */

Arena *arena_create(void)
{
    return arena_create_sized(ARENA_PAGE_SIZE);
}

Arena *arena_create_sized(size_t page_size)
{
    Arena *arena = malloc(sizeof(Arena));
    if (!arena)
        return NULL;

    ArenaPage *page = page_new(page_size);
    if (!page) {
        free(arena);
        return NULL;
    }

    arena->head            = page;
    arena->current         = page;
    arena->page_size       = page_size;
    arena->total_allocated = 0;
    arena->total_pages     = 1;
    return arena;
}

void *arena_alloc(Arena *arena, size_t size)
{
    if (!arena || size == 0)
        return NULL;

    size_t aligned = align_up(size);

    /* Try the current page first */
    ArenaPage *page = arena->current;
    if (page->used + aligned <= page->size) {
        void *ptr = page->data + page->used;
        page->used += aligned;
        arena->total_allocated += aligned;
        return ptr;
    }

    /* Need a new page -- at least page_size, but large enough for this alloc */
    size_t new_size = arena->page_size;
    if (aligned > new_size)
        new_size = aligned;

    ArenaPage *new_page = page_new(new_size);
    if (!new_page)
        return NULL;

    /* Link the new page after current */
    new_page->next   = page->next;
    page->next       = new_page;
    arena->current   = new_page;
    arena->total_pages++;

    void *ptr = new_page->data;
    new_page->used = aligned;
    arena->total_allocated += aligned;
    return ptr;
}

void *arena_calloc(Arena *arena, size_t count, size_t size)
{
    if (!arena || count == 0 || size == 0)
        return NULL;

    size_t total = count * size;
    void *ptr = arena_alloc(arena, total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

char *arena_strdup(Arena *arena, const char *str)
{
    if (!arena || !str)
        return NULL;

    size_t len = strlen(str);
    char *dup = arena_alloc(arena, len + 1);
    if (dup)
        memcpy(dup, str, len + 1);
    return dup;
}

char *arena_strndup(Arena *arena, const char *str, size_t n)
{
    if (!arena || !str)
        return NULL;

    /* Find actual length, bounded by n */
    size_t len = 0;
    while (len < n && str[len] != '\0')
        len++;

    char *dup = arena_alloc(arena, len + 1);
    if (dup) {
        memcpy(dup, str, len);
        dup[len] = '\0';
    }
    return dup;
}

void arena_reset(Arena *arena)
{
    if (!arena)
        return;

    ArenaPage *head = arena->head;

    /* Free every page after the first */
    ArenaPage *page = head->next;
    while (page) {
        ArenaPage *next = page->next;
        free(page);
        page = next;
    }

    /* Reset the first page */
    head->next = NULL;
    head->used = 0;

    arena->current         = head;
    arena->total_allocated = 0;
    arena->total_pages     = 1;
}

void arena_destroy(Arena *arena)
{
    if (!arena)
        return;

    /* Free all pages */
    ArenaPage *page = arena->head;
    while (page) {
        ArenaPage *next = page->next;
        free(page);
        page = next;
    }

    free(arena);
}

size_t arena_bytes_used(const Arena *arena)
{
    if (!arena)
        return 0;

    size_t total = 0;
    for (const ArenaPage *page = arena->head; page; page = page->next)
        total += page->used;
    return total;
}
