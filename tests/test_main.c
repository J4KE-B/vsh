/* ============================================================================
 * vsh - Vanguard Shell
 * test_main.c - Test runner
 * ============================================================================ */

#include "test.h"
#include <stdio.h>
#include <stdlib.h>

int tests_run = 0;
int tests_passed = 0;
int tests_failed = 0;

int main(void) {
    printf("=== vsh Test Suite ===\n");

    test_arena();
    test_safe_string();
    test_lexer();
    test_parser();

    printf("\n=== Results: %d/%d passed", tests_passed, tests_run);
    if (tests_failed > 0) {
        printf(", %d FAILED", tests_failed);
    }
    printf(" ===\n");

    return tests_failed > 0 ? 1 : 0;
}
