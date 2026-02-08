/* ============================================================================
 * vsh - Vanguard Shell
 * wildcard.c - Glob/wildcard expansion
 *
 * Provides pattern matching (fnmatch-style) and file-glob expansion.
 * wildcard_match() is a hand-rolled recursive matcher used for tab completion
 * and programmatic matching.  wildcard_expand() delegates to POSIX glob(3) for
 * robust filesystem expansion, then copies results into the caller's arena.
 * ============================================================================ */

#include "wildcard.h"
#include "arena.h"

#include <glob.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>

/* ---- Internal helpers --------------------------------------------------- */

/* Compare function for qsort on C strings */
static int cmp_strings(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

/* Match a character-class bracket expression against a single character.
 * *pp points just past the opening '['.  On success, *pp is advanced past
 * the closing ']' and true is returned.  On failure the position of *pp is
 * unspecified but the caller will abandon the match anyway. */
static bool match_bracket(const char **pp, char c)
{
    const char *p = *pp;
    bool negate = false;
    bool matched = false;

    if (*p == '!' || *p == '^') {
        negate = true;
        p++;
    }

    while (*p && *p != ']') {
        char lo = *p;

        /* Range: a-z */
        if (p[1] == '-' && p[2] && p[2] != ']') {
            char hi = p[2];
            if ((unsigned char)c >= (unsigned char)lo &&
                (unsigned char)c <= (unsigned char)hi)
                matched = true;
            p += 3;
        } else {
            if (c == lo)
                matched = true;
            p++;
        }
    }

    if (*p == ']')
        p++;  /* skip closing bracket */

    *pp = p;
    return negate ? !matched : matched;
}

/* ---- Public API --------------------------------------------------------- */

bool wildcard_has_magic(const char *pattern)
{
    if (!pattern)
        return false;

    for (const char *p = pattern; *p; p++) {
        switch (*p) {
        case '*':
        case '?':
        case '[':
            return true;
        case '\\':
            if (p[1])
                p++;  /* skip escaped char */
            break;
        }
    }
    return false;
}

bool wildcard_match(const char *pattern, const char *string)
{
    if (!pattern || !string)
        return false;

    const char *p = pattern;
    const char *s = string;

    while (*p && *s) {
        switch (*p) {
        case '\\':
            /* Escaped character -- match next char literally */
            p++;
            if (*p != *s)
                return false;
            p++;
            s++;
            break;

        case '?':
            /* Match any single character except / */
            if (*s == '/')
                return false;
            p++;
            s++;
            break;

        case '[': {
            /* Character class */
            if (*s == '/')
                return false;
            p++;  /* skip '[' */
            if (!match_bracket(&p, *s))
                return false;
            s++;
            break;
        }

        case '*':
            /* Skip consecutive stars */
            while (*p == '*')
                p++;

            /* Trailing star matches everything (except leading dots) */
            if (*p == '\0')
                return true;

            /* Try matching rest of pattern against every suffix of string.
             * Hidden files: a bare * at the start of a pattern component
             * must not match a leading '.'.  That is enforced by the caller
             * (wildcard_expand) which skips dot-files unless the pattern
             * itself starts with '.'.  Here we just do a pure match. */
            for (; *s; s++) {
                if (wildcard_match(p, s))
                    return true;
            }
            return wildcard_match(p, s);  /* try zero-length tail */

        default:
            /* Literal character */
            if (*p != *s)
                return false;
            p++;
            s++;
            break;
        }
    }

    /* Consume any trailing stars in the pattern */
    while (*p == '*')
        p++;

    return (*p == '\0' && *s == '\0');
}

char **wildcard_expand(const char *pattern, Arena *arena, int *count)
{
    if (!pattern || !arena || !count) {
        if (count) *count = 0;
        return NULL;
    }

    /* If no glob characters, let the caller use the literal string */
    if (!wildcard_has_magic(pattern)) {
        *count = 0;
        return NULL;
    }

    /* Use POSIX glob(3) for the heavy lifting */
    glob_t g;
    int flags = GLOB_NOSORT | GLOB_TILDE | GLOB_MARK;
    int ret = glob(pattern, flags, NULL, &g);

    if (ret != 0 || g.gl_pathc == 0) {
        globfree(&g);
        *count = 0;
        return NULL;
    }

    /* Strip trailing '/' added by GLOB_MARK for directories.  We added
     * GLOB_MARK only so callers can tell dirs apart if they want to, but
     * the traditional shell behaviour is to not append it unless the user
     * typed it.  Actually, most shells strip it, so we do too. */
    for (size_t i = 0; i < g.gl_pathc; i++) {
        size_t len = strlen(g.gl_pathv[i]);
        if (len > 1 && g.gl_pathv[i][len - 1] == '/')
            g.gl_pathv[i][len - 1] = '\0';
    }

    /* Sort results alphabetically (we used GLOB_NOSORT) */
    qsort(g.gl_pathv, g.gl_pathc, sizeof(char *), cmp_strings);

    /* Copy results into the arena */
    int n = (int)g.gl_pathc;
    char **results = arena_alloc(arena, (size_t)n * sizeof(char *));
    if (!results) {
        globfree(&g);
        *count = 0;
        return NULL;
    }

    for (int i = 0; i < n; i++)
        results[i] = arena_strdup(arena, g.gl_pathv[i]);

    globfree(&g);
    *count = n;
    return results;
}
