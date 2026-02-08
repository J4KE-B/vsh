/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/watch_cmd.c - Execute a command repeatedly at intervals
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

/* Flag set by SIGINT handler to break the watch loop */
static volatile sig_atomic_t watch_interrupted = 0;

static void watch_sigint_handler(int sig) {
    (void)sig;
    watch_interrupted = 1;
}

/* ---- Main entry point --------------------------------------------------- */

/*
 * watch [-n SECONDS] COMMAND...
 *
 * Execute a command repeatedly, refreshing the display at a given interval.
 *   -n SECONDS   Set the update interval (default 2.0, supports decimals)
 *
 * Press Ctrl+C to stop.
 */
int builtin_watch(Shell *shell, int argc, char **argv) {
    (void)shell;

    double interval = 2.0;
    int cmd_start = 1;  /* Index in argv where the command begins */

    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "vsh: watch: -n requires an argument\n");
                return 1;
            }
            char *endp;
            interval = strtod(argv[i + 1], &endp);
            if (*endp != '\0' || interval <= 0) {
                fprintf(stderr, "vsh: watch: invalid interval '%s'\n",
                        argv[i + 1]);
                return 1;
            }
            i++;           /* skip the value */
            cmd_start = i + 1;
        } else if (strncmp(argv[i], "-n", 2) == 0 && argv[i][2] != '\0') {
            /* Handle -n0.5 (no space) */
            char *endp;
            interval = strtod(argv[i] + 2, &endp);
            if (*endp != '\0' || interval <= 0) {
                fprintf(stderr, "vsh: watch: invalid interval '%s'\n",
                        argv[i] + 2);
                return 1;
            }
            cmd_start = i + 1;
        } else {
            cmd_start = i;
            break;
        }
    }

    if (cmd_start >= argc) {
        fprintf(stderr, "Usage: watch [-n SECONDS] COMMAND...\n");
        return 1;
    }

    /* Join remaining arguments into a single command string */
    size_t cmd_len = 0;
    for (int i = cmd_start; i < argc; i++)
        cmd_len += strlen(argv[i]) + 1;

    char *command = malloc(cmd_len + 1);
    if (!command) {
        fprintf(stderr, "vsh: watch: out of memory\n");
        return 1;
    }
    command[0] = '\0';
    for (int i = cmd_start; i < argc; i++) {
        if (i > cmd_start) strcat(command, " ");
        strcat(command, argv[i]);
    }

    /* Get hostname for header */
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0)
        strncpy(hostname, "unknown", sizeof(hostname));

    /* Install our SIGINT handler, saving the old one */
    watch_interrupted = 0;
    struct sigaction sa_new, sa_old;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = watch_sigint_handler;
    sigemptyset(&sa_new.sa_mask);
    sa_new.sa_flags = 0;
    sigaction(SIGINT, &sa_new, &sa_old);

    /* Prepare nanosleep interval */
    struct timespec ts;
    ts.tv_sec  = (time_t)interval;
    ts.tv_nsec = (long)((interval - (double)ts.tv_sec) * 1e9);

    int last_status = 0;

    while (!watch_interrupted) {
        /* Clear screen and move cursor to top-left */
        printf("\x1b[2J\x1b[H");

        /* Header line */
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%a %b %d %H:%M:%S %Y", tm);

        printf("\033[1mEvery %.1fs: \033[0m%-40s \033[2m%s: %s\033[0m\n\n",
               interval, command, hostname, timebuf);
        fflush(stdout);

        /* Execute the command and print its output */
        FILE *fp = popen(command, "r");
        if (!fp) {
            fprintf(stderr, "vsh: watch: failed to execute '%s': %s\n",
                    command, strerror(errno));
            last_status = 1;
        } else {
            char buf[4096];
            while (fgets(buf, sizeof(buf), fp) != NULL) {
                if (watch_interrupted) break;
                fputs(buf, stdout);
            }
            int pstat = pclose(fp);
            last_status = WIFEXITED(pstat) ? WEXITSTATUS(pstat) : 1;
        }
        fflush(stdout);

        if (watch_interrupted) break;

        /* Sleep for the interval, but wake up on signal */
        struct timespec rem = ts;
        while (!watch_interrupted) {
            int ret = nanosleep(&rem, &rem);
            if (ret == 0) break;
            if (errno != EINTR) break;
            /* EINTR: check if we got SIGINT, then loop will exit */
        }
    }

    /* Restore previous SIGINT handler */
    sigaction(SIGINT, &sa_old, NULL);

    free(command);
    return last_status;
}
