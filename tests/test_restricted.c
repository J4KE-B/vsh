/* ============================================================================
 * vsh - Vanguard Shell
 * test_restricted.c - Restricted-mode allow-list policy tests
 * ============================================================================ */

#include "restricted.h"
#include "parser.h"
#include "test.h"

/* Build a one-element redirection list on the stack. */
static Redirection make_redir(RedirType type)
{
    Redirection r;
    r.type   = type;
    r.fd     = -1;
    r.target = "file";
    r.next   = NULL;
    return r;
}

void test_restricted(void)
{
    printf("\n--- Restricted mode ---\n");

    /* ---- Allow-list membership ------------------------------------------ */
    ASSERT_TRUE(restricted_is_allowed("echo"));
    ASSERT_TRUE(restricted_is_allowed("ls"));
    ASSERT_TRUE(restricted_is_allowed("cat"));
    ASSERT_TRUE(!restricted_is_allowed("rm"));
    ASSERT_TRUE(!restricted_is_allowed("bash"));
    ASSERT_TRUE(!restricted_is_allowed("cd"));
    ASSERT_TRUE(!restricted_is_allowed(""));

    /* ---- A plain allow-listed command passes ---------------------------- */
    ASSERT_TRUE(restricted_check("echo", NULL, 0, NULL) == NULL);
    ASSERT_TRUE(restricted_check("ls", NULL, 0, NULL) == NULL);

    /* ---- Non-allow-listed command is denied ----------------------------- */
    ASSERT_TRUE(restricted_check("rm", NULL, 0, NULL) != NULL);
    ASSERT_TRUE(restricted_check("bash", NULL, 0, NULL) != NULL);

    /* ---- Path-based escape is denied even for allow-listed names -------- */
    ASSERT_TRUE(restricted_check("/bin/echo", NULL, 0, NULL) != NULL);
    ASSERT_TRUE(restricted_check("./echo", NULL, 0, NULL) != NULL);
    ASSERT_TRUE(restricted_check("../echo", NULL, 0, NULL) != NULL);

    /* ---- Output redirection is denied ----------------------------------- */
    {
        Redirection out = make_redir(REDIR_OUTPUT);
        Redirection app = make_redir(REDIR_APPEND);
        Redirection dup = make_redir(REDIR_DUP_OUT);
        ASSERT_TRUE(restricted_check("echo", NULL, 0, &out) != NULL);
        ASSERT_TRUE(restricted_check("echo", NULL, 0, &app) != NULL);
        ASSERT_TRUE(restricted_check("echo", NULL, 0, &dup) != NULL);
    }

    /* ---- Input redirection on an allowed command is fine ---------------- */
    {
        Redirection in = make_redir(REDIR_INPUT);
        ASSERT_TRUE(restricted_check("sort", NULL, 0, &in) == NULL);
    }

    /* ---- Reassigning a protected variable is denied --------------------- */
    {
        char *bad_path[]  = { "PATH=/tmp/evil" };
        char *bad_shell[] = { "SHELL=/bin/sh" };
        char *bad_env[]   = { "ENV=/tmp/rc" };
        char *ok_assign[] = { "GREETING=hello" };
        ASSERT_TRUE(restricted_check("echo", bad_path, 1, NULL) != NULL);
        ASSERT_TRUE(restricted_check("echo", bad_shell, 1, NULL) != NULL);
        ASSERT_TRUE(restricted_check("echo", bad_env, 1, NULL) != NULL);
        /* A harmless assignment on an allowed command still passes. */
        ASSERT_TRUE(restricted_check("echo", ok_assign, 1, NULL) == NULL);
    }

    printf("  Restricted mode tests complete\n");
}
