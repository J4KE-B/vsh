/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/cd.c - Change directory builtin
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/*
 * cd [dir]
 *
 * Supports:
 *   cd          - go to HOME
 *   cd -        - go to OLDPWD, print the directory
 *   cd ~        - go to HOME (handled by tilde expansion elsewhere too)
 *   cd <path>   - go to path
 *
 * Updates PWD and OLDPWD environment variables after a successful chdir.
 */
int builtin_cd(Shell *shell, int argc, char **argv) {
    const char *target = NULL;
    char oldpwd[PATH_MAX];
    char newpwd[PATH_MAX];

    /* Save the current working directory before changing */
    if (!getcwd(oldpwd, sizeof(oldpwd))) {
        /* If getcwd fails, try to get from env */
        const char *pwd_env = env_get(shell->env, "PWD");
        if (pwd_env) {
            strncpy(oldpwd, pwd_env, sizeof(oldpwd) - 1);
            oldpwd[sizeof(oldpwd) - 1] = '\0';
        } else {
            oldpwd[0] = '\0';
        }
    }

    if (argc < 2 || argv[1] == NULL) {
        /* cd with no args: go to HOME */
        target = env_get(shell->env, "HOME");
        if (!target || *target == '\0') {
            fprintf(stderr, "vsh: cd: HOME not set\n");
            return 1;
        }
    } else if (strcmp(argv[1], "-") == 0) {
        /* cd -: go to OLDPWD */
        target = env_get(shell->env, "OLDPWD");
        if (!target || *target == '\0') {
            fprintf(stderr, "vsh: cd: OLDPWD not set\n");
            return 1;
        }
        /* Print the directory we're changing to */
        printf("%s\n", target);
    } else {
        target = argv[1];
    }

    /* Perform the directory change */
    if (chdir(target) != 0) {
        fprintf(stderr, "vsh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    /* Update PWD and OLDPWD */
    if (getcwd(newpwd, sizeof(newpwd))) {
        env_set(shell->env, "PWD", newpwd, true);
    }
    if (oldpwd[0] != '\0') {
        env_set(shell->env, "OLDPWD", oldpwd, true);
    }

    return 0;
}
