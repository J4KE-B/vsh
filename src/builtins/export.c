/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/export.c - Export and unset variable builtins
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * export [VAR=value] [VAR] ...
 *
 * No args: list all exported variables as "declare -x VAR=value" or
 *          "declare -x VAR" (if no value).
 * With "VAR=value": parse, set the variable, and mark as exported.
 * With "VAR" (no '='): mark existing variable as exported.
 */
int builtin_export(Shell *shell, int argc, char **argv) {
    if (argc < 2) {
        /* List all exported variables */
        for (int i = 0; i < ENV_HASH_SIZE; i++) {
            EnvEntry *e = shell->env->buckets[i];
            while (e) {
                if (e->exported) {
                    if (e->value) {
                        printf("declare -x %s=\"%s\"\n", e->key, e->value);
                    } else {
                        printf("declare -x %s\n", e->key);
                    }
                }
                e = e->next;
            }
        }
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            /* VAR=value form */
            size_t keylen = (size_t)(eq - argv[i]);
            char *key = malloc(keylen + 1);
            if (!key) {
                fprintf(stderr, "vsh: export: allocation failed\n");
                return 1;
            }
            memcpy(key, argv[i], keylen);
            key[keylen] = '\0';

            const char *value = eq + 1;
            env_set(shell->env, key, value, true);
            free(key);
        } else {
            /* VAR form: just mark as exported */
            env_export(shell->env, argv[i]);
        }
    }

    return 0;
}

/*
 * unset VAR ...
 *
 * Unset each named variable from the environment.
 */
int builtin_unset(Shell *shell, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "vsh: unset: not enough arguments\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        env_unset(shell->env, argv[i]);
    }

    return 0;
}
