/* ============================================================================
 * vsh - Vanguard Shell
 * builtins/help.c - Display help for builtins
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <stdio.h>
#include <string.h>

/*
 * help [command]
 *
 * No args: print all builtins in a formatted table.
 * With arg: print detailed help for that specific builtin.
 */
int builtin_help(Shell *shell, int argc, char **argv) {
    (void)shell;

    int count = 0;
    const BuiltinEntry *table = builtins_table(&count);

    if (argc < 2) {
        /* Print header */
        printf("\033[1mvsh - Vanguard Shell Built-in Commands\033[0m\n\n");

        /* Print each builtin: bold name, usage, dim description */
        for (int i = 0; i < count; i++) {
            printf("  \033[1m%-12s\033[0m %-24s \033[2m%s\033[0m\n",
                   table[i].name,
                   table[i].usage,
                   table[i].help);
        }

        printf("\nType 'help <command>' for detailed help on a specific builtin.\n");
        return 0;
    }

    /* Look up the specified command */
    const BuiltinEntry *entry = builtins_lookup(argv[1]);
    if (!entry) {
        fprintf(stderr, "vsh: help: no help topics match '%s'\n", argv[1]);
        return 1;
    }

    printf("\033[1m%s\033[0m - %s\n", entry->name, entry->help);
    printf("Usage: %s\n", entry->usage);

    return 0;
}
