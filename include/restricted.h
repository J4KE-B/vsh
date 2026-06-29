/* ============================================================================
 * vsh - Vanguard Shell
 * restricted.h - Restricted execution mode (command allow-listing)
 *
 * When vsh is started with -r it runs in restricted mode, a sandbox modelled
 * on rbash.  A simple command is permitted only if:
 *   - its name is on the compiled allow-list, AND
 *   - its name contains no '/' (no path-based escapes), AND
 *   - it performs no output redirection (cannot create/clobber files), AND
 *   - it assigns to none of PATH/SHELL/ENV/BASH_ENV/LD_* (no re-pathing).
 *
 * Every attempt -- permitted or denied -- is recorded in the hash-chained
 * audit log, so the record of what a confined user tried to run is itself
 * tamper-evident.
 * ============================================================================ */

#ifndef VSH_RESTRICTED_H
#define VSH_RESTRICTED_H

#include "parser.h"

/* True if `name` is on the restricted-mode allow-list. */
bool restricted_is_allowed(const char *name);

/* Check a set of variable assignments against the protected-variable policy.  Returns NULL if all
 * are permitted, otherwise a static reason.  Exposed so a bare assignment line (no command name)
 * can be policed the same way as inline `VAR=x cmd` assignments. */
const char *restricted_check_assignments(char *const *assignments, int nassign);

/* Evaluate a simple command against the restricted-mode policy.  Returns NULL
 * if the command is permitted, otherwise a static human-readable reason it was
 * denied.  `redirs` and the assignment array may be NULL/empty. */
const char *restricted_check(const char *name,
                             char *const *assignments, int nassign,
                             const Redirection *redirs);

#endif /* VSH_RESTRICTED_H */
