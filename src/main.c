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
#include "audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

static void print_version(void) {
    printf("vsh 1.0.0 (Vanguard Shell)\n");
    printf("A modern, memory-safe shell written in C\n");
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options] [script [args...]]\n", prog);
    printf("Options:\n");
    printf("  -c CMD             Execute CMD and exit\n");
    printf("  -r                 Restricted mode (command allow-listing + audit)\n");
    printf("  --verify-audit [F] Verify the audit-log hash chain and exit\n");
    printf("  -h                 Show this help\n");
    printf("  -v                 Show version\n");
}

/* Resolve the audit-log path: $VSH_AUDIT_LOG, else ~/.vsh_audit.log */
static const char *audit_log_path(char *buf, size_t cap) {
    const char *env = getenv("VSH_AUDIT_LOG");
    if (env && env[0]) {
        snprintf(buf, cap, "%s", env);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, cap, "%s/.vsh_audit.log", home);
    return buf;
}

int main(int argc, char **argv) {
    int opt_c = 0;
    int opt_r = 0;
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
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--restricted") == 0) {
            opt_r = 1;
        } else if (strcmp(argv[i], "--verify-audit") == 0) {
            /* Verify the audit-log hash chain and exit. An explicit path may
             * follow; otherwise fall back to the default location. */
            char pathbuf[PATH_MAX];
            const char *path;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                path = argv[++i];
            } else {
                path = audit_log_path(pathbuf, sizeof(pathbuf));
            }
            long n = 0, bad = 0;
            AuditStatus st = audit_verify(path, &n, &bad);
            if (st == AUDIT_OK) {
                printf("vsh: audit log intact: %ld entr%s verified (%s)\n",
                       n, n == 1 ? "y" : "ies", path);
                return 0;
            }
            fprintf(stderr, "vsh: audit log %s at entry %ld (%s)\n",
                    audit_status_str(st), bad, path);
            return 1;
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

    /* Enable restricted mode: enforce command allow-listing and attach the
     * tamper-evident audit log.  We refuse to run confined without a working
     * audit log -- an unauditable sandbox is not a sandbox. */
    if (opt_r) {
        char pathbuf[PATH_MAX];
        const char *path = audit_log_path(pathbuf, sizeof(pathbuf));
        shell->audit = audit_open(path);
        if (!shell->audit) {
            fprintf(stderr, "vsh: cannot open audit log '%s': %s\n",
                    path, strerror(errno));
            shell_destroy(shell);
            return 1;
        }
        shell->restricted = true;
        fprintf(stderr, "vsh: restricted mode enabled (audit: %s)\n", path);
    }

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
