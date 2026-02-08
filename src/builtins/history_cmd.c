/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/history_cmd.c - History display and management builtin
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * history [-c] [-n N]
 *
 * No args: print all history entries with numbers.
 * -c:     clear the history.
 * -n N:   show only the last N entries.
 *
 * Note: ! expansions (e.g. !!, !N, !string) are handled in shell_exec_line,
 * not in this builtin.
 */
int builtin_history(Shell *shell, int argc, char **argv) {
    if (!shell->history) {
        fprintf(stderr, "vsh: history: history not initialized\n");
        return 1;
    }

    int show_last = -1;  /* -1 means show all */

    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            history_clear(shell->history);
            return 0;
        } else if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "vsh: history: -n: option requires an argument\n");
                return 1;
            }
            i++;
            char *endp;
            long val = strtol(argv[i], &endp, 10);
            if (*endp != '\0' || val <= 0) {
                fprintf(stderr, "vsh: history: %s: invalid count\n", argv[i]);
                return 1;
            }
            show_last = (int)val;
        } else {
            fprintf(stderr, "vsh: history: %s: invalid option\n", argv[i]);
            return 1;
        }
    }

    int total = history_count(shell->history);
    int start = 0;

    if (show_last >= 0 && show_last < total)
        start = total - show_last;

    for (int i = start; i < total; i++) {
        const char *line = history_get(shell->history, i);
        if (line) {
            printf("  %4d  %s\n", i + 1, line);
        }
    }

    return 0;
}
