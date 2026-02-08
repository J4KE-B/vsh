/* ============================================================================
 * vsh - Vanguard Shell
 * test_arena.c - Arena allocator tests
 * ============================================================================ */

#include "arena.h"
#include "test.h"

void test_arena(void) {
    printf("\n--- Arena Allocator ---\n");

    /* Test creation */
    Arena *arena = arena_create();
    ASSERT_TRUE(arena != NULL);

    /* Test basic allocation */
    void *p1 = arena_alloc(arena, 16);
    ASSERT_TRUE(p1 != NULL);

    void *p2 = arena_alloc(arena, 32);
    ASSERT_TRUE(p2 != NULL);
    ASSERT_TRUE(p1 != p2);

    /* Test alignment (8-byte) */
    void *p3 = arena_alloc(arena, 1);
    ASSERT_TRUE(((size_t)p3 % ARENA_ALIGNMENT) == 0);

    void *p4 = arena_alloc(arena, 7);
    ASSERT_TRUE(((size_t)p4 % ARENA_ALIGNMENT) == 0);

    /* Test calloc (zeroed memory) */
    int *nums = arena_calloc(arena, 10, sizeof(int));
    ASSERT_TRUE(nums != NULL);
    int all_zero = 1;
    for (int i = 0; i < 10; i++) {
        if (nums[i] != 0) all_zero = 0;
    }
    ASSERT_TRUE(all_zero);

    /* Test strdup */
    char *s1 = arena_strdup(arena, "hello world");
    ASSERT_STR_EQ(s1, "hello world");

    /* Test strndup */
    char *s2 = arena_strndup(arena, "hello world", 5);
    ASSERT_STR_EQ(s2, "hello");

    /* Test large allocation (bigger than page) */
    void *big = arena_alloc(arena, 8192);
    ASSERT_TRUE(big != NULL);

    /* Test bytes used */
    size_t used = arena_bytes_used(arena);
    ASSERT_TRUE(used > 0);

    /* Test reset */
    arena_reset(arena);
    size_t after_reset = arena_bytes_used(arena);
    ASSERT_EQ(after_reset, (size_t)0);

    /* Allocate after reset should work */
    void *p5 = arena_alloc(arena, 64);
    ASSERT_TRUE(p5 != NULL);

    /* Test destroy */
    arena_destroy(arena);

    /* Test NULL safety */
    ASSERT_TRUE(arena_alloc(NULL, 10) == NULL);
    ASSERT_TRUE(arena_strdup(NULL, "test") == NULL);

    printf("  Arena tests complete\n");
}
