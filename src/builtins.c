/* ============================================================================
 * vsh - Vanguard Shell
 * builtins.c - Built-in command registry and dispatch
 * ============================================================================ */

#include "builtins.h"
#include "shell.h"
#include <string.h>
#include <stdio.h>

static const BuiltinEntry builtin_table[] = {
    {"cd",       builtin_cd,       "cd [dir]",           "Change the current directory"},
    {"exit",     builtin_exit,     "exit [N]",            "Exit the shell with status N"},
    {"help",     builtin_help,     "help [command]",      "Display help for builtins"},
    {"export",   builtin_export,   "export [VAR=value]",  "Set/display exported variables"},
    {"unset",    builtin_unset,    "unset VAR",           "Unset a variable"},
    {"alias",    builtin_alias,    "alias [name=value]",  "Define or display aliases"},
    {"unalias",  builtin_unalias,  "unalias name",        "Remove an alias"},
    {"history",  builtin_history,  "history [-c] [-n N]", "Display or manage command history"},
    {"jobs",     builtin_jobs,     "jobs",                "List active jobs"},
    {"fg",       builtin_fg,       "fg [%N]",             "Resume job in foreground"},
    {"bg",       builtin_bg,       "bg [%N]",             "Resume job in background"},
    {"source",   builtin_source,   "source FILE",         "Execute commands from FILE"},
    {".",        builtin_source,   ". FILE",              "Execute commands from FILE"},
    {"sysinfo",  builtin_sysinfo,  "sysinfo",             "Display system information dashboard"},
    {"httpfetch",builtin_httpfetch,"httpfetch URL",        "Fetch content from a URL via HTTP"},
    {"calc",     builtin_calc,     "calc EXPR",           "Evaluate a math expression"},
    {"watch",    builtin_watch,    "watch [-n SEC] CMD",  "Execute CMD repeatedly"},
    {"pushd",    builtin_pushd,    "pushd [dir]",         "Push directory onto stack"},
    {"popd",     builtin_popd,     "popd",                "Pop directory from stack"},
    {"dirs",     builtin_dirs,     "dirs",                "Display directory stack"},
    {"colors",   builtin_colors,   "colors",              "Display terminal color palette"},
    {"pwd",      builtin_pwd,      "pwd",                 "Print working directory"},
    {"echo",     builtin_echo,     "echo [args...]",      "Display text"},
    {"type",     builtin_type,     "type NAME",           "Describe a command"},
    {"return",   builtin_return_cmd,"return [N]",         "Return from a function"},
    {"local",    builtin_local,    "local VAR=value",     "Declare a local variable"},
    {NULL, NULL, NULL, NULL}
};

void builtins_init(void) {
    /* Table is statically initialized, nothing to do currently */
}

const BuiltinEntry *builtins_lookup(const char *name) {
    for (int i = 0; builtin_table[i].name; i++) {
        if (strcmp(builtin_table[i].name, name) == 0)
            return &builtin_table[i];
    }
    return NULL;
}

bool builtins_is_builtin(const char *name) {
    return builtins_lookup(name) != NULL;
}

int builtins_execute(Shell *shell, int argc, char **argv) {
    const BuiltinEntry *entry = builtins_lookup(argv[0]);
    if (!entry) return -1;
    return entry->handler(shell, argc, argv);
}

const BuiltinEntry *builtins_table(int *count) {
    int n = 0;
    while (builtin_table[n].name) n++;
    if (count) *count = n;
    return builtin_table;
}
