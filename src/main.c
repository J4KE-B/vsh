/* ============================================================================
 * vsh - Vanguard Shell
 * main.c - Entry point and command-line option handling
 *
 * Supports three modes of operation:
 *   1. Interactive REPL  (default when stdin is a tty)
 *   2. Command string    (-c "command")
 *   3. Script file       (vsh script.sh [args...])
 * ============================================================================ */

#include "shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void print_version(void) {
    printf("vsh 1.0.0 (Vanguard Shell)\n");
    printf("A modern, memory-safe shell written in C\n");
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options] [script [args...]]\n", prog);
    printf("Options:\n");
    printf("  -c CMD    Execute CMD and exit\n");
    printf("  -h        Show this help\n");
    printf("  -v        Show version\n");
}

int main(int argc, char **argv) {
    int opt_c = 0;
    char *cmd_string = NULL;
    int i;

    /* Parse command-line options */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') break;

        if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "vsh: -c requires an argument\n");
                return 1;
            }
            opt_c = 1;
            cmd_string = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--version") == 0) {
            print_version();
            return 0;
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            fprintf(stderr, "vsh: unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    Shell *shell = shell_init(argc, argv);
    int status;

    if (opt_c) {
        /* Execute a single command string */
        status = shell_exec_line(shell, cmd_string);
    } else if (i < argc) {
        /* Script mode: read and execute commands from a file */
        FILE *fp = fopen(argv[i], "r");
        if (!fp) {
            fprintf(stderr, "vsh: cannot open '%s': %s\n",
                    argv[i], strerror(errno));
            shell_destroy(shell);
            return 1;
        }

        char line[4096];
        shell->interactive = false;

        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            if (line[0] == '#')  continue;   /* Skip comments */
            if (line[0] == '\0') continue;   /* Skip empty lines */
            shell_exec_line(shell, line);
        }

        fclose(fp);
        status = shell->last_status;
    } else {
        /* Interactive or piped-stdin mode */
        status = shell_run(shell);
    }

    shell_destroy(shell);
    return status;
}
