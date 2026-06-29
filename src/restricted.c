/* ============================================================================
 * vsh - Vanguard Shell
 * restricted.c - Restricted execution mode policy
 *
 * The policy is deliberately deny-by-default: a command must appear on the
 * allow-list AND survive a set of structural checks that close the classic
 * rbash escape routes (absolute paths, output redirection, and re-pointing
 * the shell's search path via PATH/SHELL/ENV/LD_* assignments).
 * ============================================================================ */

#include "restricted.h"

#include <string.h>

/* ---- Allow-list --------------------------------------------------------- *
 * A conservative, mostly read-only set of commands.  Anything not listed is
 * refused.  Kept sorted for readability; membership is a linear scan (the
 * list is short and this runs once per command, not in a hot loop). */
static const char *const ALLOW_LIST[] = {
    "cal",     "calc",    "cat",     "clear",   "colors",  "date",
    "df",      "dirs",    "du",      "echo",    "false",   "free",
    "grep",    "head",    "help",    "history", "hostname","id",
    "jobs",    "ls",      "printenv","pwd",     "sort",    "sysinfo",
    "tail",    "true",    "type",    "uname",   "uniq",    "uptime",
    "wc",      "whoami",
};
static const int ALLOW_COUNT = (int)(sizeof(ALLOW_LIST) / sizeof(ALLOW_LIST[0]));

/* ---- Environment variables that must not be reassigned ------------------ *
 * Re-pointing any of these would let a confined user break out of the
 * allow-list (e.g. PATH= to shadow a trusted name, or ENV/BASH_ENV to run an
 * arbitrary startup file). */
static int is_protected_var(const char *assignment)
{
    static const char *const PROTECTED[] = {
        "PATH", "SHELL", "ENV", "BASH_ENV", "IFS", "LD_PRELOAD",
        "LD_LIBRARY_PATH",
    };
    static const int N = (int)(sizeof(PROTECTED) / sizeof(PROTECTED[0]));

    const char *eq = strchr(assignment, '=');
    size_t klen = eq ? (size_t)(eq - assignment) : strlen(assignment);

    for (int i = 0; i < N; i++) {
        if (strlen(PROTECTED[i]) == klen &&
            strncmp(assignment, PROTECTED[i], klen) == 0) {
            return 1;
        }
    }
    return 0;
}

bool restricted_is_allowed(const char *name)
{
    if (!name) return false;
    for (int i = 0; i < ALLOW_COUNT; i++) {
        if (strcmp(name, ALLOW_LIST[i]) == 0)
            return true;
    }
    return false;
}

const char *restricted_check_assignments(char *const *assignments, int nassign)
{
    for (int i = 0; i < nassign; i++) {
        if (is_protected_var(assignments[i]))
            return "modifying PATH/SHELL/ENV is not permitted";
    }
    return NULL;
}

const char *restricted_check(const char *name,
                             char *const *assignments, int nassign,
                             const Redirection *redirs)
{
    if (!name || name[0] == '\0')
        return "empty command name";

    /* Path-based names bypass the allow-list -- refuse anything with a '/'. */
    if (strchr(name, '/'))
        return "path-based command names are not permitted";

    /* Output redirection would let a confined user create or clobber files. */
    for (const Redirection *r = redirs; r; r = r->next) {
        if (r->type == REDIR_OUTPUT || r->type == REDIR_APPEND ||
            r->type == REDIR_DUP_OUT) {
            return "output redirection is not permitted";
        }
    }

    /* Reassigning the search path / startup env is an escape vector. */
    const char *areason = restricted_check_assignments(assignments, nassign);
    if (areason) return areason;

    /* Finally, the name itself must be explicitly allow-listed. */
    if (!restricted_is_allowed(name))
        return "command is not on the allow-list";

    return NULL;
}
