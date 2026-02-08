/* ============================================================================
 * vsh - Vanguard Shell
 * test.h - Shared test framework macros and declarations
 * ============================================================================ */

#ifndef VSH_TEST_H
#define VSH_TEST_H

#include <stdio.h>
#include <string.h>

extern int tests_run;
extern int tests_passed;
extern int tests_failed;

#define ASSERT_TRUE(expr) do { \
    tests_run++; \
    if (expr) { tests_passed++; } \
    else { tests_failed++; \
        fprintf(stderr, "  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    tests_run++; \
    if ((a) == (b)) { tests_passed++; } \
    else { tests_failed++; \
        fprintf(stderr, "  FAIL: %s:%d: %s == %s (got %ld vs %ld)\n", \
            __FILE__, __LINE__, #a, #b, (long)(a), (long)(b)); } \
} while(0)

#define ASSERT_STR_EQ(a, b) do { \
    tests_run++; \
    if ((a) && (b) && strcmp((a), (b)) == 0) { tests_passed++; } \
    else { tests_failed++; \
        fprintf(stderr, "  FAIL: %s:%d: \"%s\" != \"%s\"\n", \
            __FILE__, __LINE__, (a) ? (a) : "(null)", (b) ? (b) : "(null)"); } \
} while(0)

/* Test function declarations */
void test_arena(void);
void test_safe_string(void);
void test_lexer(void);
void test_parser(void);

#endif /* VSH_TEST_H */
