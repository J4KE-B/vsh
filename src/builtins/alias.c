/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/alias.c - Alias and unalias builtins with hash table helpers
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- DJB2 hash function ------------------------------------------------- */

static unsigned int alias_hash(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + (unsigned int)c;
    return hash % ALIAS_HASH_SIZE;
}

/* ---- Alias table helpers ------------------------------------------------ */

void alias_set(AliasTable *table, const char *name, const char *value) {
    unsigned int h = alias_hash(name);
    AliasEntry *e = table->buckets[h];

    /* Check if alias already exists; if so, update value */
    while (e) {
        if (strcmp(e->name, name) == 0) {
            free(e->value);
            e->value = strdup(value);
            return;
        }
        e = e->next;
    }

    /* Create a new entry */
    AliasEntry *entry = malloc(sizeof(AliasEntry));
    if (!entry) return;

    entry->name = strdup(name);
    entry->value = strdup(value);
    entry->next = table->buckets[h];
    table->buckets[h] = entry;
    table->count++;
}

const char *alias_get(AliasTable *table, const char *name) {
    unsigned int h = alias_hash(name);
    AliasEntry *e = table->buckets[h];

    while (e) {
        if (strcmp(e->name, name) == 0)
            return e->value;
        e = e->next;
    }
    return NULL;
}

bool alias_remove(AliasTable *table, const char *name) {
    unsigned int h = alias_hash(name);
    AliasEntry *e = table->buckets[h];
    AliasEntry *prev = NULL;

    while (e) {
        if (strcmp(e->name, name) == 0) {
            if (prev)
                prev->next = e->next;
            else
                table->buckets[h] = e->next;
            free(e->name);
            free(e->value);
            free(e);
            table->count--;
            return true;
        }
        prev = e;
        e = e->next;
    }
    return false;
}

/* ---- alias builtin ------------------------------------------------------ */

/*
 * alias [name=value] [name] ...
 *
 * No args: print all aliases as "alias name='value'".
 * With "name=value": define or update the alias.
 * With "name": print that specific alias if it exists.
 */
int builtin_alias(Shell *shell, int argc, char **argv) {
    if (argc < 2) {
        /* Print all aliases */
        for (int i = 0; i < ALIAS_HASH_SIZE; i++) {
            AliasEntry *e = shell->aliases->buckets[i];
            while (e) {
                printf("alias %s='%s'\n", e->name, e->value);
                e = e->next;
            }
        }
        return 0;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq) {
            /* name=value form */
            size_t namelen = (size_t)(eq - argv[i]);
            char *name = malloc(namelen + 1);
            if (!name) {
                fprintf(stderr, "vsh: alias: allocation failed\n");
                return 1;
            }
            memcpy(name, argv[i], namelen);
            name[namelen] = '\0';

            const char *value = eq + 1;
            alias_set(shell->aliases, name, value);
            free(name);
        } else {
            /* Just a name: print that alias */
            const char *val = alias_get(shell->aliases, argv[i]);
            if (val) {
                printf("alias %s='%s'\n", argv[i], val);
            } else {
                fprintf(stderr, "vsh: alias: %s: not found\n", argv[i]);
                ret = 1;
            }
        }
    }

    return ret;
}

/* ---- unalias builtin ---------------------------------------------------- */

/*
 * unalias name ...
 *
 * Remove each named alias from the alias table.
 */
int builtin_unalias(Shell *shell, int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "vsh: unalias: not enough arguments\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (!alias_remove(shell->aliases, argv[i])) {
            fprintf(stderr, "vsh: unalias: %s: not found\n", argv[i]);
            ret = 1;
        }
    }

    return ret;
}
