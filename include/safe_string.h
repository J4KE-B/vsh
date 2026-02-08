/* ============================================================================
 * vsh - Vanguard Shell
 * safe_string.h - Bounds-checked dynamic string buffer
 *
 * Wraps a heap-allocated char buffer with length and capacity tracking.
 * All operations are bounds-checked. Auto-grows with 2x strategy.
 * Always null-terminated.
 * ============================================================================ */

#ifndef VSH_SAFE_STRING_H
#define VSH_SAFE_STRING_H

#include <stddef.h>
#include <stdbool.h>

#define SSTR_INIT_CAP 64

typedef struct SafeString {
    char  *data;   /* Null-terminated buffer */
    size_t len;    /* Current length (excluding null) */
    size_t cap;    /* Allocated capacity */
} SafeString;

/* Create a new SafeString with given initial capacity */
SafeString *sstr_new(size_t initial_cap);

/* Create a SafeString from a C string */
SafeString *sstr_from(const char *cstr);

/* Create a SafeString from first n bytes of data */
SafeString *sstr_from_n(const char *data, size_t n);

/* Free a SafeString */
void sstr_free(SafeString *s);

/* Ensure capacity for at least `needed` more bytes */
bool sstr_ensure(SafeString *s, size_t needed);

/* Append a C string */
bool sstr_append(SafeString *s, const char *cstr);

/* Append n bytes of data */
bool sstr_append_n(SafeString *s, const char *data, size_t n);

/* Append a single character */
bool sstr_append_char(SafeString *s, char c);

/* Append a formatted string (printf-style) */
bool sstr_appendf(SafeString *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Set the string content (replaces everything) */
bool sstr_set(SafeString *s, const char *cstr);

/* Truncate to given length */
void sstr_truncate(SafeString *s, size_t len);

/* Clear the string (length = 0, keeps capacity) */
void sstr_clear(SafeString *s);

/* Get the C string (read-only) */
const char *sstr_cstr(const SafeString *s);

/* Get a mutable pointer (use carefully) */
char *sstr_data(SafeString *s);

/* Check if empty */
bool sstr_empty(const SafeString *s);

/* Compare with C string */
bool sstr_eq(const SafeString *s, const char *cstr);

/* Duplicate the SafeString */
SafeString *sstr_dup(const SafeString *s);

/* Remove leading/trailing whitespace */
void sstr_trim(SafeString *s);

/* Insert a character at position */
bool sstr_insert_char(SafeString *s, size_t pos, char c);

/* Delete n characters starting at position */
void sstr_delete(SafeString *s, size_t pos, size_t n);

#endif /* VSH_SAFE_STRING_H */
