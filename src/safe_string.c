/* ============================================================================
 * vsh - Vanguard Shell
 * safe_string.c - Bounds-checked dynamic string buffer
 *
 * All mutations are bounds-checked and auto-grow via 2x reallocation.
 * The buffer is always null-terminated. NULL inputs are handled gracefully
 * throughout -- no function will dereference a NULL pointer.
 * ============================================================================ */

#include "safe_string.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* ---- Construction / destruction ----------------------------------------- */

SafeString *sstr_new(size_t initial_cap)
{
    SafeString *s = malloc(sizeof(SafeString));
    if (!s)
        return NULL;

    if (initial_cap < SSTR_INIT_CAP)
        initial_cap = SSTR_INIT_CAP;

    s->data = malloc(initial_cap);
    if (!s->data) {
        free(s);
        return NULL;
    }

    s->len     = 0;
    s->cap     = initial_cap;
    s->data[0] = '\0';
    return s;
}

SafeString *sstr_from(const char *cstr)
{
    if (!cstr)
        return sstr_new(SSTR_INIT_CAP);

    size_t len = strlen(cstr);
    size_t cap = len + 1;
    if (cap < SSTR_INIT_CAP)
        cap = SSTR_INIT_CAP;

    SafeString *s = malloc(sizeof(SafeString));
    if (!s)
        return NULL;

    s->data = malloc(cap);
    if (!s->data) {
        free(s);
        return NULL;
    }

    memcpy(s->data, cstr, len + 1);
    s->len = len;
    s->cap = cap;
    return s;
}

SafeString *sstr_from_n(const char *data, size_t n)
{
    if (!data)
        return sstr_new(SSTR_INIT_CAP);

    size_t cap = n + 1;
    if (cap < SSTR_INIT_CAP)
        cap = SSTR_INIT_CAP;

    SafeString *s = malloc(sizeof(SafeString));
    if (!s)
        return NULL;

    s->data = malloc(cap);
    if (!s->data) {
        free(s);
        return NULL;
    }

    memcpy(s->data, data, n);
    s->data[n] = '\0';
    s->len = n;
    s->cap = cap;
    return s;
}

void sstr_free(SafeString *s)
{
    if (!s)
        return;

    free(s->data);
    free(s);
}

/* ---- Capacity management ------------------------------------------------ */

bool sstr_ensure(SafeString *s, size_t needed)
{
    if (!s)
        return false;

    size_t required = s->len + needed + 1; /* +1 for null terminator */
    if (required <= s->cap)
        return true;

    /* 2x growth strategy */
    size_t new_cap = s->cap * 2;
    while (new_cap < required)
        new_cap *= 2;

    char *new_data = realloc(s->data, new_cap);
    if (!new_data)
        return false;

    s->data = new_data;
    s->cap  = new_cap;
    return true;
}

/* ---- Append operations -------------------------------------------------- */

bool sstr_append(SafeString *s, const char *cstr)
{
    if (!s)
        return false;
    if (!cstr)
        return true;

    size_t add_len = strlen(cstr);
    if (!sstr_ensure(s, add_len))
        return false;

    memcpy(s->data + s->len, cstr, add_len + 1);
    s->len += add_len;
    return true;
}

bool sstr_append_n(SafeString *s, const char *data, size_t n)
{
    if (!s)
        return false;
    if (!data || n == 0)
        return true;

    if (!sstr_ensure(s, n))
        return false;

    memcpy(s->data + s->len, data, n);
    s->len += n;
    s->data[s->len] = '\0';
    return true;
}

bool sstr_append_char(SafeString *s, char c)
{
    if (!s)
        return false;

    if (!sstr_ensure(s, 1))
        return false;

    s->data[s->len]     = c;
    s->data[s->len + 1] = '\0';
    s->len++;
    return true;
}

bool sstr_appendf(SafeString *s, const char *fmt, ...)
{
    if (!s || !fmt)
        return false;

    va_list ap;

    /* First pass: measure how many bytes we need */
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n < 0)
        return false;

    size_t needed = (size_t)n;
    if (!sstr_ensure(s, needed))
        return false;

    /* Second pass: write into the buffer */
    va_start(ap, fmt);
    vsnprintf(s->data + s->len, needed + 1, fmt, ap);
    va_end(ap);

    s->len += needed;
    return true;
}

/* ---- Mutators ----------------------------------------------------------- */

bool sstr_set(SafeString *s, const char *cstr)
{
    if (!s)
        return false;

    sstr_clear(s);
    return sstr_append(s, cstr);
}

void sstr_truncate(SafeString *s, size_t len)
{
    if (!s)
        return;

    if (len < s->len) {
        s->len      = len;
        s->data[len] = '\0';
    }
}

void sstr_clear(SafeString *s)
{
    if (!s)
        return;

    s->len     = 0;
    s->data[0] = '\0';
}

/* ---- Accessors ---------------------------------------------------------- */

const char *sstr_cstr(const SafeString *s)
{
    if (!s)
        return NULL;

    return s->data;
}

char *sstr_data(SafeString *s)
{
    if (!s)
        return NULL;

    return s->data;
}

bool sstr_empty(const SafeString *s)
{
    if (!s)
        return true;

    return s->len == 0;
}

bool sstr_eq(const SafeString *s, const char *cstr)
{
    if (!s || !cstr)
        return false;

    return strcmp(s->data, cstr) == 0;
}

SafeString *sstr_dup(const SafeString *s)
{
    if (!s)
        return NULL;

    return sstr_from_n(s->data, s->len);
}

/* ---- String manipulation ------------------------------------------------ */

void sstr_trim(SafeString *s)
{
    if (!s || s->len == 0)
        return;

    /* Find first non-whitespace character */
    size_t start = 0;
    while (start < s->len && isspace((unsigned char)s->data[start]))
        start++;

    /* Find last non-whitespace character */
    size_t end = s->len;
    while (end > start && isspace((unsigned char)s->data[end - 1]))
        end--;

    size_t new_len = end - start;

    if (start > 0)
        memmove(s->data, s->data + start, new_len);

    s->len           = new_len;
    s->data[new_len] = '\0';
}

bool sstr_insert_char(SafeString *s, size_t pos, char c)
{
    if (!s)
        return false;
    if (pos > s->len)
        return false;

    if (!sstr_ensure(s, 1))
        return false;

    /* Shift everything from pos rightward by 1 (including the null terminator) */
    memmove(s->data + pos + 1, s->data + pos, s->len - pos + 1);
    s->data[pos] = c;
    s->len++;
    return true;
}

void sstr_delete(SafeString *s, size_t pos, size_t n)
{
    if (!s)
        return;
    if (pos >= s->len)
        return;

    /* Clamp n so we don't go past the end */
    if (pos + n > s->len)
        n = s->len - pos;

    /* Shift left (including the null terminator) */
    memmove(s->data + pos, s->data + pos + n, s->len - pos - n + 1);
    s->len -= n;
}
