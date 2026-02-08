/* ============================================================================
 * vsh - Vanguard Shell
 * wildcard.h - Glob/wildcard expansion
 * ============================================================================ */

#ifndef VSH_WILDCARD_H
#define VSH_WILDCARD_H

#include <stdbool.h>

typedef struct Arena Arena;

/* Check if a string contains glob characters */
bool wildcard_has_magic(const char *pattern);

/* Match a pattern against a string (fnmatch-style)
 * Supports: *, ?, [abc], [a-z], [!abc] */
bool wildcard_match(const char *pattern, const char *string);

/* Expand a glob pattern into matching file paths.
 * Returns an array of arena-allocated strings, with *count set.
 * If no matches, returns NULL and *count = 0. */
char **wildcard_expand(const char *pattern, Arena *arena, int *count);

#endif /* VSH_WILDCARD_H */
