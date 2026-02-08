/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/dirstack.c - Directory stack builtins: pushd, popd, dirs
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

/* ---- Helper: print the directory stack ---------------------------------- */

static void print_dirstack(Shell *shell) {
    /* Print current directory first */
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)))
        printf("%s", cwd);

    /* Then print stack entries from top to bottom */
    DirStack *ds = shell->dirstack;
    for (int i = ds->top - 1; i >= 0; i--) {
        printf(" %s", ds->dirs[i]);
    }
    printf("\n");
}

/*
 * pushd [dir]
 *
 * No argument: swap the top two directories on the stack.
 * With argument: push the current directory onto the stack and cd to dir.
 * Prints the resulting stack.
 */
int builtin_pushd(Shell *shell, int argc, char **argv) {
    DirStack *ds = shell->dirstack;
    char cwd[PATH_MAX];

    if (!getcwd(cwd, sizeof(cwd))) {
        fprintf(stderr, "vsh: pushd: cannot get current directory: %s\n",
                strerror(errno));
        return 1;
    }

    if (argc < 2) {
        /* Swap top two: current dir and top of stack */
        if (ds->top < 1) {
            fprintf(stderr, "vsh: pushd: no other directory\n");
            return 1;
        }

        /* Top of stack becomes new dir, current dir goes to top of stack */
        char *top_dir = ds->dirs[ds->top - 1];

        if (chdir(top_dir) != 0) {
            fprintf(stderr, "vsh: pushd: %s: %s\n", top_dir, strerror(errno));
            return 1;
        }

        /* Replace top of stack with old cwd */
        free(ds->dirs[ds->top - 1]);
        ds->dirs[ds->top - 1] = strdup(cwd);

        /* Update PWD */
        char newpwd[PATH_MAX];
        if (getcwd(newpwd, sizeof(newpwd)))
            env_set(shell->env, "PWD", newpwd, true);

        print_dirstack(shell);
        return 0;
    }

    /* Push current dir onto stack, cd to target */
    const char *target = argv[1];

    if (ds->top >= DIRSTACK_MAX) {
        fprintf(stderr, "vsh: pushd: directory stack full\n");
        return 1;
    }

    if (chdir(target) != 0) {
        fprintf(stderr, "vsh: pushd: %s: %s\n", target, strerror(errno));
        return 1;
    }

    /* Push the old cwd */
    ds->dirs[ds->top] = strdup(cwd);
    ds->top++;

    /* Update environment */
    env_set(shell->env, "OLDPWD", cwd, true);
    char newpwd[PATH_MAX];
    if (getcwd(newpwd, sizeof(newpwd)))
        env_set(shell->env, "PWD", newpwd, true);

    print_dirstack(shell);
    return 0;
}

/*
 * popd
 *
 * Pop the top directory from the stack and cd to it.
 * Prints the resulting stack.
 */
int builtin_popd(Shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;

    DirStack *ds = shell->dirstack;

    if (ds->top < 1) {
        fprintf(stderr, "vsh: popd: directory stack empty\n");
        return 1;
    }

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        /* Save current dir as OLDPWD */
        env_set(shell->env, "OLDPWD", cwd, true);
    }

    /* Pop the top entry */
    ds->top--;
    char *target = ds->dirs[ds->top];
    ds->dirs[ds->top] = NULL;

    if (chdir(target) != 0) {
        fprintf(stderr, "vsh: popd: %s: %s\n", target, strerror(errno));
        free(target);
        ds->top++;  /* Restore the entry since cd failed */
        return 1;
    }

    free(target);

    /* Update PWD */
    char newpwd[PATH_MAX];
    if (getcwd(newpwd, sizeof(newpwd)))
        env_set(shell->env, "PWD", newpwd, true);

    print_dirstack(shell);
    return 0;
}

/*
 * dirs
 *
 * Display the directory stack, from top (current dir) to bottom.
 */
int builtin_dirs(Shell *shell, int argc, char **argv) {
    (void)argc;
    (void)argv;

    print_dirstack(shell);
    return 0;
}
