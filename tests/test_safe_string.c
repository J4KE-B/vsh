/* ============================================================================
 * vsh - Vanguard Shell
 * test_safe_string.c - Safe string tests
 * ============================================================================ */

#include "safe_string.h"
#include "test.h"

void test_safe_string(void) {
    printf("\n--- Safe String ---\n");

    /* Test creation */
    SafeString *s = sstr_new(16);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->len, (size_t)0);
    ASSERT_TRUE(s->cap >= 16);
    ASSERT_TRUE(sstr_empty(s));

    /* Test append */
    sstr_append(s, "hello");
    ASSERT_EQ(s->len, (size_t)5);
    ASSERT_STR_EQ(sstr_cstr(s), "hello");

    sstr_append(s, " world");
    ASSERT_STR_EQ(sstr_cstr(s), "hello world");
    ASSERT_EQ(s->len, (size_t)11);

    /* Test append_char */
    sstr_append_char(s, '!');
    ASSERT_STR_EQ(sstr_cstr(s), "hello world!");

    /* Test from */
    SafeString *s2 = sstr_from("test string");
    ASSERT_STR_EQ(sstr_cstr(s2), "test string");
    ASSERT_EQ(s2->len, (size_t)11);

    /* Test eq */
    ASSERT_TRUE(sstr_eq(s2, "test string"));
    ASSERT_TRUE(!sstr_eq(s2, "other"));

    /* Test set */
    sstr_set(s, "replaced");
    ASSERT_STR_EQ(sstr_cstr(s), "replaced");
    ASSERT_EQ(s->len, (size_t)8);

    /* Test truncate */
    sstr_truncate(s, 4);
    ASSERT_STR_EQ(sstr_cstr(s), "repl");
    ASSERT_EQ(s->len, (size_t)4);

    /* Test clear */
    sstr_clear(s);
    ASSERT_TRUE(sstr_empty(s));
    ASSERT_STR_EQ(sstr_cstr(s), "");

    /* Test appendf */
    sstr_appendf(s, "num=%d str=%s", 42, "ok");
    ASSERT_STR_EQ(sstr_cstr(s), "num=42 str=ok");

    /* Test insert_char */
    sstr_clear(s);
    sstr_append(s, "hllo");
    sstr_insert_char(s, 1, 'e');
    ASSERT_STR_EQ(sstr_cstr(s), "hello");

    /* Test delete */
    sstr_delete(s, 1, 2);
    ASSERT_STR_EQ(sstr_cstr(s), "hlo");

    /* Test dup */
    sstr_set(s, "duplicate me");
    SafeString *s3 = sstr_dup(s);
    ASSERT_STR_EQ(sstr_cstr(s3), "duplicate me");
    ASSERT_TRUE(sstr_data(s) != sstr_data(s3));

    /* Test trim */
    sstr_set(s, "  hello world  ");
    sstr_trim(s);
    ASSERT_STR_EQ(sstr_cstr(s), "hello world");

    /* Test auto-grow (append a lot) */
    sstr_clear(s);
    for (int i = 0; i < 1000; i++) {
        sstr_append_char(s, 'x');
    }
    ASSERT_EQ(s->len, (size_t)1000);
    ASSERT_TRUE(s->cap >= 1000);

    /* Test from_n */
    SafeString *s4 = sstr_from_n("hello world", 5);
    ASSERT_STR_EQ(sstr_cstr(s4), "hello");

    /* Cleanup */
    sstr_free(s);
    sstr_free(s2);
    sstr_free(s3);
    sstr_free(s4);

    /* NULL safety */
    sstr_free(NULL);

    printf("  SafeString tests complete\n");
}
