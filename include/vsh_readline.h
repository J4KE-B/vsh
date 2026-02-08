/* ============================================================================
 * vsh - Vanguard Shell
 * vsh_readline.h - Line editor with tab completion
 *
 * Provides a readline-like experience with:
 * - Cursor movement (left/right, home/end, word-based)
 * - Kill/yank (Ctrl+K, Ctrl+U, Ctrl+W, Ctrl+Y)
 * - History navigation (up/down, Ctrl+R reverse search)
 * - Tab completion (files, commands, builtins)
 * - Clear screen (Ctrl+L)
 * ============================================================================ */

#ifndef VSH_READLINE_H
#define VSH_READLINE_H

#include "safe_string.h"
#include <stdbool.h>

typedef struct Shell Shell;

/* Read a line of input with full editing capabilities.
 * Returns a malloc'd string, or NULL on EOF.
 * The caller must free() the returned string. */
char *vsh_readline(Shell *shell, const char *prompt);

/* Tab completion callback type */
typedef struct Completions {
    char **entries;
    int    count;
    int    capacity;
} Completions;

/* Generate completions for the current word */
Completions *vsh_complete(Shell *shell, const char *line, int cursor_pos);

/* Free completions */
void completions_free(Completions *comp);

#endif /* VSH_READLINE_H */
