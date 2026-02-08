/* ============================================================================
 * vsh - Vanguard Shell
 * env.h - Environment variable management and expansion
 * ============================================================================ */

#ifndef VSH_ENV_H
#define VSH_ENV_H

#include "shell.h"
#include <stdbool.h>

/* Initialize environment table from environ */
EnvTable *env_create(void);

/* Destroy environment table */
void env_destroy(EnvTable *env);

/* Get a variable value (returns NULL if not set) */
const char *env_get(EnvTable *env, const char *key);

/* Set a variable */
void env_set(EnvTable *env, const char *key, const char *value, bool exported);

/* Unset a variable */
void env_unset(EnvTable *env, const char *key);

/* Mark a variable as exported */
void env_export(EnvTable *env, const char *key);

/* Build an envp array for execve (caller must free the array, not the strings) */
char **env_build_envp(EnvTable *env);

/* Free an envp array built by env_build_envp */
void env_free_envp(char **envp);

/* Perform variable expansion on a string.
 * Handles: $VAR, ${VAR}, ${VAR:-default}, ${VAR:=default},
 *          ${VAR:+alternate}, ${VAR:?error}, $?, $$, $#, $0..$9
 * Returns an arena-allocated string. */
char *env_expand(Shell *shell, const char *input, struct Arena *arena);

/* Expand tilde (~) in a path */
char *env_expand_tilde(Shell *shell, const char *path, struct Arena *arena);

/* Parse a VAR=value assignment. Returns true if it is one.
 * Sets *key and *value (arena-allocated). */
bool env_parse_assignment(const char *str, char **key, char **value,
                          struct Arena *arena);

#endif /* VSH_ENV_H */
