/* ============================================================================
 * vsh - Vanguard Shell
 * shell.h - Main shell state and types
 * ============================================================================ */

#ifndef VSH_SHELL_H
#define VSH_SHELL_H

#include <stdbool.h>
#include <termios.h>
#include <sys/types.h>

/* Forward declarations */
typedef struct Arena Arena;
typedef struct History History;
typedef struct SafeString SafeString;

/* ---- Environment Table -------------------------------------------------- */
#define ENV_HASH_SIZE 256

typedef struct EnvEntry {
    char *key;
    char *value;
    bool  exported;       /* Should be passed to child processes */
    struct EnvEntry *next;
} EnvEntry;

typedef struct EnvTable {
    EnvEntry *buckets[ENV_HASH_SIZE];
    int       count;
} EnvTable;

/* ---- Alias Table -------------------------------------------------------- */
#define ALIAS_HASH_SIZE 128

typedef struct AliasEntry {
    char *name;
    char *value;
    struct AliasEntry *next;
} AliasEntry;

typedef struct AliasTable {
    AliasEntry *buckets[ALIAS_HASH_SIZE];
    int         count;
} AliasTable;

/* Alias table operations */
void alias_set(AliasTable *table, const char *name, const char *value);
const char *alias_get(AliasTable *table, const char *name);
bool alias_remove(AliasTable *table, const char *name);

/* ---- Job Control -------------------------------------------------------- */
typedef enum JobState {
    JOB_RUNNING,
    JOB_STOPPED,
    JOB_DONE,
    JOB_KILLED
} JobState;

typedef struct Job {
    int         id;           /* Job number [1], [2], ... */
    pid_t       pgid;         /* Process group ID */
    pid_t      *pids;         /* Array of PIDs in pipeline */
    int         npids;        /* Number of processes */
    JobState    state;        /* Current state */
    char       *command;      /* Command string for display */
    bool        notified;     /* Has user been notified of completion? */
    bool        foreground;   /* Is this a foreground job? */
    struct Job *next;
} Job;

typedef struct JobTable {
    Job *head;
    int  next_id;
} JobTable;

/* ---- Directory Stack ---------------------------------------------------- */
#define DIRSTACK_MAX 64

typedef struct DirStack {
    char *dirs[DIRSTACK_MAX];
    int   top;
} DirStack;

/* ---- Shell State -------------------------------------------------------- */
typedef struct Shell {
    Arena       *parse_arena;   /* Per-command arena (reset each iteration) */
    EnvTable    *env;           /* Environment variables */
    JobTable    *jobs;          /* Background jobs */
    History     *history;       /* Command history */
    AliasTable  *aliases;       /* Alias table */
    DirStack    *dirstack;      /* pushd/popd stack */

    int          last_status;   /* $? - exit status of last command */
    pid_t        shell_pid;     /* $$ - PID of the shell */
    bool         interactive;   /* Is this connected to a TTY? */
    bool         running;       /* Main loop flag */
    bool         login_shell;   /* Is this a login shell? */

    struct termios orig_termios; /* Original terminal settings */
    bool         raw_mode;       /* Are we in raw terminal mode? */

    pid_t        fg_pgid;       /* Foreground process group */

    /* Positional parameters (for scripts/functions) */
    char       **pos_params;
    int          pos_count;

    /* Script execution state */
    int          script_depth;  /* Nesting depth for source/scripts */
    bool         in_function;   /* Currently executing a function? */
} Shell;

/* Initialize the shell */
Shell *shell_init(int argc, char **argv);

/* Destroy the shell and free all resources */
void shell_destroy(Shell *shell);

/* Main REPL loop */
int shell_run(Shell *shell);

/* Execute a single line of input */
int shell_exec_line(Shell *shell, const char *line);

/* Enable raw terminal mode */
void shell_enable_raw_mode(Shell *shell);

/* Disable raw terminal mode (restore original) */
void shell_disable_raw_mode(Shell *shell);

/* Build the prompt string */
char *shell_build_prompt(Shell *shell);

/* Signal setup for the shell process */
void shell_setup_signals(Shell *shell);

#endif /* VSH_SHELL_H */
