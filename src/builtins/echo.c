/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/echo.c - Simple builtins: pwd, echo, type, return, local
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include "env.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

/* ---- pwd ---------------------------------------------------------------- */

/*
 * pwd
 *
 * Print the current working directory.
 */
int builtin_pwd(Shell *shell, int argc, char **argv) {
    (void)shell;
    (void)argc;
    (void)argv;

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("%s\n", cwd);
        return 0;
    }

    perror("vsh: pwd");
    return 1;
}

/* ---- echo --------------------------------------------------------------- */

/*
 * Print a single escape character. Advances *p past the escape sequence.
 * Returns false if \c is encountered (meaning: stop printing).
 */
static bool print_escape(const char **p) {
    switch (**p) {
    case 'n':  putchar('\n'); break;
    case 't':  putchar('\t'); break;
    case '\\': putchar('\\'); break;
    case 'a':  putchar('\a'); break;
    case 'b':  putchar('\b'); break;
    case 'e':  putchar('\033'); break;
    case 'f':  putchar('\f'); break;
    case 'r':  putchar('\r'); break;
    case 'v':  putchar('\v'); break;
    case 'c':  return false;  /* Stop output */
    case '0': {
        /* Octal: \0NNN */
        unsigned int val = 0;
        const char *s = *p + 1;
        for (int j = 0; j < 3 && *s >= '0' && *s <= '7'; j++, s++)
            val = val * 8 + (unsigned int)(*s - '0');
        putchar((int)(val & 0xff));
        *p = s - 1;  /* Will be incremented by caller */
        break;
    }
    case 'x': {
        /* Hex: \xHH */
        unsigned int val = 0;
        const char *s = *p + 1;
        for (int j = 0; j < 2; j++, s++) {
            if (*s >= '0' && *s <= '9')
                val = val * 16 + (unsigned int)(*s - '0');
            else if (*s >= 'a' && *s <= 'f')
                val = val * 16 + (unsigned int)(*s - 'a' + 10);
            else if (*s >= 'A' && *s <= 'F')
                val = val * 16 + (unsigned int)(*s - 'A' + 10);
            else
                break;
        }
        putchar((int)(val & 0xff));
        *p = s - 1;
        break;
    }
    default:
        putchar('\\');
        putchar(**p);
        break;
    }
    return true;
}

/*
 * echo [-n] [-e] [args...]
 *
 * Print arguments separated by spaces, followed by a newline.
 * -n: suppress trailing newline.
 * -e: interpret backslash escape sequences.
 */
int builtin_echo(Shell *shell, int argc, char **argv) {
    (void)shell;

    bool newline = true;
    bool escapes = false;
    int start = 1;

    /* Parse leading flags: stop at first non-flag argument */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0')
            break;

        /* Check that all chars after '-' are valid flag chars */
        bool valid = true;
        for (const char *p = &argv[i][1]; *p; p++) {
            if (*p != 'n' && *p != 'e' && *p != 'E') {
                valid = false;
                break;
            }
        }
        if (!valid)
            break;

        /* Apply the flags */
        for (const char *p = &argv[i][1]; *p; p++) {
            switch (*p) {
            case 'n': newline = false; break;
            case 'e': escapes = true;  break;
            case 'E': escapes = false; break;
            }
        }
        start = i + 1;
    }

    for (int i = start; i < argc; i++) {
        if (i > start)
            putchar(' ');

        if (escapes) {
            const char *s = argv[i];
            bool keep_going = true;
            while (*s && keep_going) {
                if (*s == '\\' && *(s + 1)) {
                    s++;
                    keep_going = print_escape(&s);
                } else {
                    putchar(*s);
                }
                s++;
            }
            if (!keep_going) {
                fflush(stdout);
                return 0;
            }
        } else {
            fputs(argv[i], stdout);
        }
    }

    if (newline)
        putchar('\n');

    fflush(stdout);
    return 0;
}

/* ---- type --------------------------------------------------------------- */

/*
 * Check if a file exists and is executable in any PATH directory.
 * Returns a malloc'd string with the full path, or NULL.
 */
static char *find_in_path(const char *name) {
    /* If name contains a slash, check directly */
    if (strchr(name, '/')) {
        if (access(name, X_OK) == 0)
            return strdup(name);
        return NULL;
    }

    const char *path_env = getenv("PATH");
    if (!path_env)
        return NULL;

    char *path_copy = strdup(path_env);
    if (!path_copy)
        return NULL;

    char *saveptr;
    char *dir = strtok_r(path_copy, ":", &saveptr);
    while (dir) {
        char fullpath[PATH_MAX];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, name);
        if (n > 0 && (size_t)n < sizeof(fullpath)) {
            struct stat st;
            if (stat(fullpath, &st) == 0 && S_ISREG(st.st_mode) &&
                access(fullpath, X_OK) == 0) {
                free(path_copy);
                return strdup(fullpath);
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(path_copy);
    return NULL;
}

/*
 * type NAME ...
 *
 * For each NAME, describe how it would be interpreted:
 *   - builtin
 *   - alias (with expansion)
 *   - external command (with path)
 *   - not found
 */
int builtin_type(Shell *shell, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "vsh: type: not enough arguments\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        const char *name = argv[i];
        bool found = false;

        /* Check aliases */
        if (shell->aliases) {
            const char *val = alias_get(shell->aliases, name);
            if (val) {
                printf("%s is aliased to '%s'\n", name, val);
                found = true;
            }
        }

        /* Check builtins */
        if (!found && builtins_is_builtin(name)) {
            printf("%s is a shell builtin\n", name);
            found = true;
        }

        /* Check external commands in PATH */
        if (!found) {
            char *path = find_in_path(name);
            if (path) {
                printf("%s is %s\n", name, path);
                free(path);
                found = true;
            }
        }

        if (!found) {
            fprintf(stderr, "vsh: type: %s: not found\n", name);
            ret = 1;
        }
    }

    return ret;
}

/* ---- return ------------------------------------------------------------- */

/*
 * return [N]
 *
 * Return from a shell function or sourced script with status N.
 * If not inside a function, print an error.
 */
int builtin_return_cmd(Shell *shell, int argc, char **argv) {
    if (!shell->in_function && shell->script_depth == 0) {
        fprintf(stderr, "vsh: return: can only 'return' from a function or sourced script\n");
        return 1;
    }

    int status = 0;
    if (argc > 1) {
        char *endp;
        long val = strtol(argv[1], &endp, 10);
        if (*endp != '\0') {
            fprintf(stderr, "vsh: return: %s: numeric argument required\n", argv[1]);
            return 2;
        }
        status = (int)(val & 0xff);
    }

    shell->last_status = status;

    /*
     * The actual control flow return is handled by the caller (executor)
     * checking last_status after this builtin runs. For a full implementation,
     * this would use setjmp/longjmp or a return flag. For now we set status.
     */
    return status;
}

/* ---- local -------------------------------------------------------------- */

/*
 * local VAR=value ...
 *
 * Declare a local variable in the current function scope.
 * Full local scope requires a call-frame stack; for now this is a stub
 * that behaves like a regular assignment via env_set.
 */
int builtin_local(Shell *shell, int argc, char **argv) {
    if (!shell->in_function) {
        fprintf(stderr, "vsh: local: can only be used in a function\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            size_t keylen = (size_t)(eq - argv[i]);
            char *key = malloc(keylen + 1);
            if (!key) {
                fprintf(stderr, "vsh: local: allocation failed\n");
                return 1;
            }
            memcpy(key, argv[i], keylen);
            key[keylen] = '\0';

            const char *value = eq + 1;
            env_set(shell->env, key, value, false);
            free(key);
        } else {
            /* Just declare the variable with empty value */
            env_set(shell->env, argv[i], "", false);
        }
    }

    return 0;
}

/* ---- External alias helper declaration ---------------------------------- */
/* alias_get is defined in alias.c; we declare it here for type builtin use. */
extern const char *alias_get(AliasTable *table, const char *name);
