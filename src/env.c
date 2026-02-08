/* ============================================================================
 * vsh - Vanguard Shell
 * env.c - Environment variable management and expansion
 *
 * Implements a hash table (djb2 + separate chaining) for shell variables,
 * environment import/export, and the full $-expansion engine including
 * ${VAR:-default}, ${VAR:=default}, ${VAR:+alt}, ${VAR:?err}, positional
 * parameters, special variables, and tilde expansion.
 * ============================================================================ */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>
#include <ctype.h>

#include "env.h"
#include "shell.h"
#include "arena.h"
#include "safe_string.h"

extern char **environ;

/* ---- Hash Function (djb2) ----------------------------------------------- */

static unsigned int env_hash(const char *s)
{
    unsigned long hash = 5381;
    while (*s)
        hash = hash * 33 + (unsigned char)*s++;
    return (unsigned int)(hash % ENV_HASH_SIZE);
}

/* ---- Public API --------------------------------------------------------- */

EnvTable *env_create(void)
{
    EnvTable *env = calloc(1, sizeof(EnvTable));
    if (!env)
        return NULL;

    /* Import every variable from the inherited environment */
    if (environ) {
        for (int i = 0; environ[i]; i++) {
            const char *entry = environ[i];
            const char *eq = strchr(entry, '=');
            if (!eq)
                continue;

            size_t klen = (size_t)(eq - entry);
            char *key = strndup(entry, klen);
            if (!key)
                continue;

            const char *val = eq + 1;
            env_set(env, key, val, true);
            free(key);
        }
    }

    /* Set common defaults if not already present */
    if (!env_get(env, "SHELL")) {
        const char *shell_path = "/bin/vsh";
        env_set(env, "SHELL", shell_path, true);
    }
    if (!env_get(env, "HOME")) {
        const char *home = getenv("HOME");
        if (home)
            env_set(env, "HOME", home, true);
    }
    if (!env_get(env, "USER")) {
        const char *user = getenv("USER");
        if (user)
            env_set(env, "USER", user, true);
    }

    return env;
}

void env_destroy(EnvTable *env)
{
    if (!env)
        return;

    for (int i = 0; i < ENV_HASH_SIZE; i++) {
        EnvEntry *e = env->buckets[i];
        while (e) {
            EnvEntry *next = e->next;
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
    }
    free(env);
}

const char *env_get(EnvTable *env, const char *key)
{
    if (!env || !key)
        return NULL;

    unsigned int h = env_hash(key);
    for (EnvEntry *e = env->buckets[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0)
            return e->value;
    }
    return NULL;
}

void env_set(EnvTable *env, const char *key, const char *value, bool exported)
{
    if (!env || !key)
        return;
    if (!value)
        value = "";

    unsigned int h = env_hash(key);

    /* Search for existing entry */
    for (EnvEntry *e = env->buckets[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            free(e->value);
            e->value = strdup(value);
            e->exported = exported;
            if (exported)
                setenv(key, value, 1);
            return;
        }
    }

    /* Create new entry and prepend to chain */
    EnvEntry *entry = malloc(sizeof(EnvEntry));
    if (!entry)
        return;

    entry->key      = strdup(key);
    entry->value    = strdup(value);
    entry->exported = exported;
    entry->next     = env->buckets[h];
    env->buckets[h] = entry;
    env->count++;

    if (exported)
        setenv(key, value, 1);
}

void env_unset(EnvTable *env, const char *key)
{
    if (!env || !key)
        return;

    unsigned int h = env_hash(key);
    EnvEntry *prev = NULL;

    for (EnvEntry *e = env->buckets[h]; e; prev = e, e = e->next) {
        if (strcmp(e->key, key) == 0) {
            if (prev)
                prev->next = e->next;
            else
                env->buckets[h] = e->next;

            free(e->key);
            free(e->value);
            free(e);
            env->count--;
            unsetenv(key);
            return;
        }
    }
}

void env_export(EnvTable *env, const char *key)
{
    if (!env || !key)
        return;

    unsigned int h = env_hash(key);
    for (EnvEntry *e = env->buckets[h]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            e->exported = true;
            setenv(e->key, e->value, 1);
            return;
        }
    }
}

char **env_build_envp(EnvTable *env)
{
    if (!env)
        return NULL;

    /* Count exported entries */
    int count = 0;
    for (int i = 0; i < ENV_HASH_SIZE; i++) {
        for (EnvEntry *e = env->buckets[i]; e; e = e->next) {
            if (e->exported)
                count++;
        }
    }

    char **envp = malloc(sizeof(char *) * (size_t)(count + 1));
    if (!envp)
        return NULL;

    int idx = 0;
    for (int i = 0; i < ENV_HASH_SIZE; i++) {
        for (EnvEntry *e = env->buckets[i]; e; e = e->next) {
            if (!e->exported)
                continue;

            size_t klen = strlen(e->key);
            size_t vlen = strlen(e->value);
            /* "KEY=VALUE\0" */
            char *str = malloc(klen + 1 + vlen + 1);
            if (!str) {
                /* Best-effort: null-terminate what we have */
                envp[idx] = NULL;
                return envp;
            }
            memcpy(str, e->key, klen);
            str[klen] = '=';
            memcpy(str + klen + 1, e->value, vlen + 1);
            envp[idx++] = str;
        }
    }
    envp[idx] = NULL;
    return envp;
}

void env_free_envp(char **envp)
{
    if (!envp)
        return;
    for (int i = 0; envp[i]; i++)
        free(envp[i]);
    free(envp);
}

/* ---- Variable Expansion Engine ------------------------------------------ */

/*
 * Helper: read a brace-enclosed body up to one of the stop characters,
 * handling nested ${...} so that defaults like ${X:-${Y}} work one level deep.
 * Returns a pointer past the consumed portion; writes the body into `out`.
 */
static const char *read_brace_body(const char *p, SafeString *out)
{
    int depth = 0;
    while (*p) {
        if (*p == '$' && *(p + 1) == '{') {
            sstr_append_n(out, p, 2);
            p += 2;
            depth++;
        } else if (*p == '}') {
            if (depth == 0)
                break;
            sstr_append_char(out, *p);
            p++;
            depth--;
        } else {
            sstr_append_char(out, *p);
            p++;
        }
    }
    return p;
}

/*
 * Expand a single ${...} construct starting just after the '{'.
 * `p` points to the first character inside the braces.
 * Returns a pointer past the closing '}'.
 * Appends the expansion result to `result`.
 */
static const char *expand_brace(Shell *shell, const char *p,
                                SafeString *result)
{
    /* Read the variable name (up to ':', '}', or modifier chars) */
    const char *start = p;
    while (*p && *p != '}' && *p != ':')
        p++;

    size_t namelen = (size_t)(p - start);
    char *varname = strndup(start, namelen);
    if (!varname) return p;

    const char *val = env_get(shell->env, varname);

    if (*p == ':' && *(p + 1)) {
        char op = *(p + 1);
        p += 2; /* skip ':' and operator character */

        /* Read the body (default/alternate/message) up to closing '}' */
        SafeString *body = sstr_new(64);
        p = read_brace_body(p, body);

        /* Expand the body text one level (simple $VAR and ${VAR} only) */
        /* We re-use env_expand for this by creating a temporary arena.
         * For simplicity and to avoid deep recursion, we just do a direct
         * lookup pass on the body string. */

        switch (op) {
        case '-': /* ${VAR:-default} */
            if (!val || val[0] == '\0')
                sstr_append(result, sstr_cstr(body));
            else
                sstr_append(result, val);
            break;

        case '=': /* ${VAR:=default} */
            if (!val || val[0] == '\0') {
                env_set(shell->env, varname, sstr_cstr(body), false);
                sstr_append(result, sstr_cstr(body));
            } else {
                sstr_append(result, val);
            }
            break;

        case '+': /* ${VAR:+alternate} */
            if (val && val[0] != '\0')
                sstr_append(result, sstr_cstr(body));
            /* else: expand to nothing */
            break;

        case '?': /* ${VAR:?message} */
            if (!val || val[0] == '\0') {
                fprintf(stderr, "vsh: %s: %s\n", varname,
                        sstr_empty(body) ? "parameter null or not set"
                                         : sstr_cstr(body));
            } else {
                sstr_append(result, val);
            }
            break;

        default:
            /* Unknown operator – just output the value if set */
            if (val)
                sstr_append(result, val);
            break;
        }

        sstr_free(body);
    } else {
        /* Simple ${VAR} */
        if (val)
            sstr_append(result, val);
    }

    free(varname);

    /* Skip the closing '}' */
    if (*p == '}')
        p++;

    return p;
}

char *env_expand(Shell *shell, const char *input, Arena *arena)
{
    if (!input)
        return arena_strdup(arena, "");

    SafeString *result = sstr_new(strlen(input) + 64);
    const char *p = input;

    while (*p) {
        if (*p != '$') {
            sstr_append_char(result, *p);
            p++;
            continue;
        }

        /* We hit a '$' */
        p++; /* skip the '$' */

        if (!*p) {
            /* Trailing '$' with nothing after it */
            sstr_append_char(result, '$');
            break;
        }

        switch (*p) {

        case '$': { /* $$ – shell PID */
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", (int)shell->shell_pid);
            sstr_append(result, buf);
            p++;
            break;
        }

        case '?': { /* $? – last exit status */
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", shell->last_status);
            sstr_append(result, buf);
            p++;
            break;
        }

        case '#': { /* $# – positional parameter count */
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", shell->pos_count);
            sstr_append(result, buf);
            p++;
            break;
        }

        case '!': { /* $! – last background PID (stub) */
            sstr_append(result, "");
            p++;
            break;
        }

        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9': {
            int idx = *p - '0';
            if (idx == 0) {
                /* $0 – shell name or script name */
                sstr_append(result, "vsh");
            } else if (idx <= shell->pos_count && shell->pos_params) {
                sstr_append(result, shell->pos_params[idx - 1]);
            }
            /* else: expand to nothing */
            p++;
            break;
        }

        case '{': { /* ${...} construct */
            p++; /* skip '{' */
            p = expand_brace(shell, p, result);
            break;
        }

        default: {
            /* $VAR – read alphanumeric + underscore */
            if (isalpha((unsigned char)*p) || *p == '_') {
                const char *start = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_'))
                    p++;
                size_t namelen = (size_t)(p - start);
                char *varname = strndup(start, namelen);
                if (varname) {
                    const char *val = env_get(shell->env, varname);
                    if (val)
                        sstr_append(result, val);
                    free(varname);
                }
            } else {
                /* Unknown $X – output literally */
                sstr_append_char(result, '$');
                sstr_append_char(result, *p);
                p++;
            }
            break;
        }
        }
    }

    char *expanded = arena_strdup(arena, sstr_cstr(result));
    sstr_free(result);
    return expanded;
}

/* ---- Tilde Expansion ---------------------------------------------------- */

char *env_expand_tilde(Shell *shell, const char *path, Arena *arena)
{
    if (!path || path[0] != '~')
        return arena_strdup(arena, path ? path : "");

    const char *rest = path + 1;

    /* ~+ → PWD */
    if (rest[0] == '+' && (rest[1] == '/' || rest[1] == '\0')) {
        const char *pwd = env_get(shell->env, "PWD");
        if (!pwd) pwd = "";
        if (rest[1] == '/') {
            SafeString *s = sstr_new(256);
            sstr_append(s, pwd);
            sstr_append(s, rest + 1); /* includes the '/' */
            char *out = arena_strdup(arena, sstr_cstr(s));
            sstr_free(s);
            return out;
        }
        return arena_strdup(arena, pwd);
    }

    /* ~- → OLDPWD */
    if (rest[0] == '-' && (rest[1] == '/' || rest[1] == '\0')) {
        const char *oldpwd = env_get(shell->env, "OLDPWD");
        if (!oldpwd) oldpwd = "";
        if (rest[1] == '/') {
            SafeString *s = sstr_new(256);
            sstr_append(s, oldpwd);
            sstr_append(s, rest + 1);
            char *out = arena_strdup(arena, sstr_cstr(s));
            sstr_free(s);
            return out;
        }
        return arena_strdup(arena, oldpwd);
    }

    /* ~ or ~/... → current user's HOME */
    if (rest[0] == '/' || rest[0] == '\0') {
        const char *home = env_get(shell->env, "HOME");
        if (!home) home = "";
        if (rest[0] == '/') {
            SafeString *s = sstr_new(256);
            sstr_append(s, home);
            sstr_append(s, rest); /* includes the '/' */
            char *out = arena_strdup(arena, sstr_cstr(s));
            sstr_free(s);
            return out;
        }
        return arena_strdup(arena, home);
    }

    /* ~user or ~user/... → lookup user's home directory */
    const char *slash = strchr(rest, '/');
    char *username;
    if (slash) {
        username = strndup(rest, (size_t)(slash - rest));
    } else {
        username = strdup(rest);
    }
    if (!username)
        return arena_strdup(arena, path);

    struct passwd *pw = getpwnam(username);
    free(username);

    if (!pw)
        return arena_strdup(arena, path);

    if (slash) {
        SafeString *s = sstr_new(256);
        sstr_append(s, pw->pw_dir);
        sstr_append(s, slash); /* includes the '/' */
        char *out = arena_strdup(arena, sstr_cstr(s));
        sstr_free(s);
        return out;
    }
    return arena_strdup(arena, pw->pw_dir);
}

/* ---- Assignment Parsing ------------------------------------------------- */

bool env_parse_assignment(const char *str, char **key, char **value,
                          Arena *arena)
{
    if (!str || !key || !value)
        return false;

    const char *eq = strchr(str, '=');
    if (!eq || eq == str)
        return false;

    /* Validate: portion before '=' must be a valid variable name */
    const char *p = str;
    if (!isalpha((unsigned char)*p) && *p != '_')
        return false;
    p++;
    while (p < eq) {
        if (!isalnum((unsigned char)*p) && *p != '_')
            return false;
        p++;
    }

    size_t klen = (size_t)(eq - str);
    *key   = arena_strndup(arena, str, klen);
    *value = arena_strdup(arena, eq + 1);
    return true;
}
