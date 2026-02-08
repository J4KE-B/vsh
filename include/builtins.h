/* ============================================================================
 * vsh - Vanguard Shell
 * builtins.h - Built-in command registry and dispatch
 * ============================================================================ */

#ifndef VSH_BUILTINS_H
#define VSH_BUILTINS_H

#include <stdbool.h>

typedef struct Shell Shell;

/* Builtin handler function signature */
typedef int (*BuiltinHandler)(Shell *shell, int argc, char **argv);

typedef struct BuiltinEntry {
    const char     *name;
    BuiltinHandler  handler;
    const char     *usage;
    const char     *help;
} BuiltinEntry;

/* Initialize the builtins table */
void builtins_init(void);

/* Look up a builtin by name. Returns NULL if not found. */
const BuiltinEntry *builtins_lookup(const char *name);

/* Check if a command name is a builtin */
bool builtins_is_builtin(const char *name);

/* Execute a builtin command. Returns exit status. */
int builtins_execute(Shell *shell, int argc, char **argv);

/* Get the full builtin table (for help/completion) */
const BuiltinEntry *builtins_table(int *count);

/* ---- Individual builtin handlers ---------------------------------------- */
int builtin_cd(Shell *shell, int argc, char **argv);
int builtin_exit(Shell *shell, int argc, char **argv);
int builtin_help(Shell *shell, int argc, char **argv);
int builtin_export(Shell *shell, int argc, char **argv);
int builtin_unset(Shell *shell, int argc, char **argv);
int builtin_alias(Shell *shell, int argc, char **argv);
int builtin_unalias(Shell *shell, int argc, char **argv);
int builtin_history(Shell *shell, int argc, char **argv);
int builtin_jobs(Shell *shell, int argc, char **argv);
int builtin_fg(Shell *shell, int argc, char **argv);
int builtin_bg(Shell *shell, int argc, char **argv);
int builtin_source(Shell *shell, int argc, char **argv);
int builtin_sysinfo(Shell *shell, int argc, char **argv);
int builtin_httpfetch(Shell *shell, int argc, char **argv);
int builtin_calc(Shell *shell, int argc, char **argv);
int builtin_watch(Shell *shell, int argc, char **argv);
int builtin_pushd(Shell *shell, int argc, char **argv);
int builtin_popd(Shell *shell, int argc, char **argv);
int builtin_dirs(Shell *shell, int argc, char **argv);
int builtin_colors(Shell *shell, int argc, char **argv);
int builtin_pwd(Shell *shell, int argc, char **argv);
int builtin_echo(Shell *shell, int argc, char **argv);
int builtin_type(Shell *shell, int argc, char **argv);
int builtin_return_cmd(Shell *shell, int argc, char **argv);
int builtin_local(Shell *shell, int argc, char **argv);

#endif /* VSH_BUILTINS_H */
