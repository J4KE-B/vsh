/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/source.c - Source (.) builtin: execute commands from a file
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define SOURCE_MAX_DEPTH 64

/*
 * source FILE  /  . FILE
 *
 * Read and execute commands from FILE in the current shell environment.
 * Guards against infinite recursion with a maximum nesting depth.
 * Returns the exit status of the last command executed.
 */
int builtin_source(Shell *shell, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "vsh: %s: filename argument required\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];

    /* Check recursion depth */
    if (shell->script_depth >= SOURCE_MAX_DEPTH) {
        fprintf(stderr, "vsh: %s: maximum source depth (%d) exceeded\n",
                argv[0], SOURCE_MAX_DEPTH);
        return 1;
    }

    FILE *fp = fopen(filename, "r");
    if (!fp) {
        fprintf(stderr, "vsh: %s: %s: %s\n", argv[0], filename, strerror(errno));
        return 1;
    }

    shell->script_depth++;

    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    int status = 0;

    while ((nread = getline(&line, &len, fp)) != -1) {
        /* Strip trailing newline */
        if (nread > 0 && line[nread - 1] == '\n')
            line[nread - 1] = '\0';

        /* Skip empty lines and comments */
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#')
            continue;

        status = shell_exec_line(shell, line);

        /* If shell was told to stop (e.g. exit), break out */
        if (!shell->running)
            break;
    }

    free(line);
    fclose(fp);

    shell->script_depth--;

    return status;
}
