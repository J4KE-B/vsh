# Architecture

This document describes the internal architecture of vsh (Vanguard Shell). It covers the
complete execution pipeline from raw input to process execution, with diagrams and tables
for every major subsystem.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Execution Pipeline](#2-execution-pipeline)
3. [Arena Allocator](#3-arena-allocator)
4. [Lexer](#4-lexer)
5. [Parser](#5-parser)
6. [Executor](#6-executor)
7. [Redirections](#7-redirections)
8. [Pipeline Wiring](#8-pipeline-wiring)
9. [Job Control & Signals](#9-job-control--signals)
10. [Memory Safety](#10-memory-safety)

---

## 1. Overview

vsh is a POSIX-compatible shell with modern extensions, written entirely in C11. The
design priorities are:

- **Zero external dependencies** — no readline, no ncurses, no third-party libraries.
  Only the C standard library and POSIX APIs.
- **Memory safety by convention** — an arena allocator eliminates parse-tree memory leaks,
  a bounds-checked string buffer prevents overflows, and the codebase compiles cleanly
  under `-Wall -Wextra -Werror -Wshadow` with AddressSanitizer/UBSan support.
- **Separation of concerns** — each stage of the execution pipeline (lex, parse, execute)
  is a standalone module with a clean interface. The shell state struct (`Shell`) is the
  only shared mutable context.

### Module Map

```
include/               src/                    tests/
  arena.h                arena.c                 test.h
  safe_string.h          safe_string.c           test_main.c
  lexer.h                lexer.c                 test_arena.c
  parser.h               parser.c                test_safe_string.c
  executor.h             executor.c              test_lexer.c
  pipeline.h             pipeline.c              test_parser.c
  env.h                  env.c
  history.h              history.c
  job_control.h          job_control.c
  shell.h                shell.c
  vsh_readline.h         vsh_readline.c
  builtins.h             builtins.c
  wildcard.h             wildcard.c
                         main.c
                         builtins/   (16 files)
```

---

## 2. Execution Pipeline

Every line of input passes through five stages, orchestrated by `shell_exec_line()`
in `src/shell.c`:

```
                          vsh Execution Pipeline
  ========================================================================

  User Input (raw string)
       |
       v
  +-----------------------------+
  | 1. HISTORY EXPANSION        |   !! / !N / !-N / !prefix
  |    expand_history()         |   Substitutes history references
  +-----------------------------+
       |
       v
       history_add()                Record line in history ring buffer
       |
       v
  +-----------------------------+
  | 2. ALIAS EXPANSION          |   First-word alias substitution
  |    expand_aliases()         |   Up to 10 levels of recursion
  +-----------------------------+
       |
       v
       arena_reset()                Wipe previous command's allocations
       |
       v
  +-----------------------------+
  | 3. LEXER                    |   String --> TokenList
  |    lexer_init()             |   All tokens arena-allocated
  |    lexer_tokenize()         |   30 token types recognized
  +-----------------------------+
       |
       v
  +-----------------------------+
  | 4. PARSER                   |   TokenList --> AST
  |    parser_init()            |   Recursive descent
  |    parser_parse()           |   13 node types, arena-allocated
  +-----------------------------+
       |
       v
  +-----------------------------+
  | 5. EXECUTOR                 |   AST --> Process execution
  |    executor_execute()       |   Expansion: $VAR, ~, globs
  |                             |   Dispatch: builtin or fork/exec
  +-----------------------------+
       |
       v
  shell->last_status                Exit code of the executed command
```

Key design decision: the arena is reset at the **start** of the next command, not at the
end of execution. This means the AST remains valid throughout execution, even though
expansion results are also arena-allocated.

---

## 3. Arena Allocator

The arena (`src/arena.c`, `include/arena.h`) is a page-based bump allocator. It enables
zero-leak parsing: every allocation during lex, parse, and execution is freed in a single
`arena_reset()` call.

### Lifecycle

```
  shell_init()
       |
       v
  arena_create()             Allocate arena + first 4KB page
       |
       v
  +--------------------------------------------------+
  |  REPL Loop (repeats per command)                  |
  |                                                   |
  |  arena_reset()  <-- wipe everything               |
  |       |                                           |
  |       v                                           |
  |  lexer_tokenize()   arena_alloc() x N             |
  |       |              arena_strdup() x M            |
  |       v                                           |
  |  parser_parse()     arena_calloc() x K             |
  |       |              arena_strdup() x J            |
  |       v                                           |
  |  executor_execute() arena_strdup() (expansions)    |
  |       |                                           |
  |       v                                           |
  |  (arena memory remains valid until next reset)    |
  +--------------------------------------------------+
       |
       v
  arena_destroy()            Free all pages + arena struct
```

### Page Structure

```
  ArenaPage (heap-allocated)
  +--------+--------+--------+---------------------------+
  | *next  | size   | used   |        data[]             |
  | (ptr)  | (4096) | (bump) | [alloc1][alloc2][  ...  ] |
  +--------+--------+--------+---------------------------+
                                ^                  ^
                                |                  |
                              start            used offset
                              (aligned to 8 bytes)
```

When `used + requested_size > size`, a new page is allocated and linked via `next`.
All allocations are 8-byte aligned (`ARENA_ALIGNMENT = 8`).

### API

| Function | Description |
|---|---|
| `arena_create()` | Create arena with one 4KB page |
| `arena_alloc(arena, size)` | Bump-allocate `size` bytes (8-byte aligned) |
| `arena_calloc(arena, count, size)` | Zero-filled allocation |
| `arena_strdup(arena, str)` | Copy string into arena |
| `arena_strndup(arena, str, n)` | Bounded string copy |
| `arena_reset(arena)` | Free all pages except first, reset first page |
| `arena_destroy(arena)` | Free everything |
| `arena_bytes_used(arena)` | Total bytes used (diagnostic) |

---

## 4. Lexer

The lexer (`src/lexer.c`, `include/lexer.h`) transforms a raw input string into an
arena-allocated `TokenList`. Each `Token` has a type, an optional string value, and
metadata for fd-prefixed redirections.

### Token Types (30)

| Token | Literal | Category |
|---|---|---|
| `TOK_WORD` | any word/string | Words |
| `TOK_PIPE` | `\|` | Operators |
| `TOK_AND` | `&&` | Operators |
| `TOK_OR` | `\|\|` | Operators |
| `TOK_SEMI` | `;` | Operators |
| `TOK_AMP` | `&` | Operators |
| `TOK_LPAREN` | `(` | Grouping |
| `TOK_RPAREN` | `)` | Grouping |
| `TOK_LBRACE` | `{` | Grouping |
| `TOK_RBRACE` | `}` | Grouping |
| `TOK_REDIR_IN` | `<` | Redirection |
| `TOK_REDIR_OUT` | `>` | Redirection |
| `TOK_REDIR_APPEND` | `>>` | Redirection |
| `TOK_REDIR_HEREDOC` | `<<` | Redirection |
| `TOK_REDIR_DUP` | `>&` / `<&` | Redirection |
| `TOK_IF` | `if` | Keywords |
| `TOK_THEN` | `then` | Keywords |
| `TOK_ELIF` | `elif` | Keywords |
| `TOK_ELSE` | `else` | Keywords |
| `TOK_FI` | `fi` | Keywords |
| `TOK_WHILE` | `while` | Keywords |
| `TOK_FOR` | `for` | Keywords |
| `TOK_DO` | `do` | Keywords |
| `TOK_DONE` | `done` | Keywords |
| `TOK_IN` | `in` | Keywords |
| `TOK_FUNCTION` | `function` | Keywords |
| `TOK_RETURN` | `return` | Keywords |
| `TOK_LOCAL` | `local` | Keywords |
| `TOK_BANG` | `!` | Prefix |
| `TOK_NEWLINE` | `\n` | Control |
| `TOK_EOF` | — | Control |

### Quoting Rules

| Syntax | Behavior |
|---|---|
| `'...'` | Literal — no expansion, no escape processing. Everything between single quotes is preserved verbatim. |
| `"..."` | Partial — backslash escapes recognized for `$`, `` ` ``, `"`, `\`, `\n`. Variable expansion markers preserved for the executor. |
| `\x` (unquoted) | Escapes the next character. `\ ` at end of line is a line continuation. |
| `#` (unquoted) | Comment — everything from `#` to end of line is discarded. |

### Fd-Prefixed Redirections

The lexer recognizes digit prefixes on redirection operators. When it sees a pattern like
`2>` or `0<`, it records the fd number in `token.redir_fd` and emits the corresponding
redirection token. This allows the parser to distinguish `> file` (fd 1 implied) from
`2> file` (fd 2 explicit).

### Tokenization Example

Input: `echo hello > out.txt 2>&1`

```
  Token 0:  TOK_WORD       "echo"
  Token 1:  TOK_WORD       "hello"
  Token 2:  TOK_REDIR_OUT  (fd=-1, default)
  Token 3:  TOK_WORD       "out.txt"
  Token 4:  TOK_REDIR_DUP  (fd=2)
  Token 5:  TOK_WORD       "1"
  Token 6:  TOK_EOF
```

---

## 5. Parser

The parser (`src/parser.c`, `include/parser.h`) is a recursive descent parser that
consumes a `TokenList` and produces an abstract syntax tree (AST). Every node and
every string in the tree is arena-allocated.

### Grammar

```
  program       ->  list EOF
  list          ->  pipeline ( (';' | '&' | '&&' | '||') pipeline )* [';' | '&']
  pipeline      ->  ['!'] command ( '|' command )*
  command       ->  simple_cmd | compound_cmd | function_def
  simple_cmd    ->  ( assignment | redirection | WORD )+
  compound_cmd  ->  if_cmd | while_cmd | for_cmd | '{' list '}' | '(' list ')'
  if_cmd        ->  'if' list 'then' list ('elif' list 'then' list)* ['else' list] 'fi'
  while_cmd     ->  'while' list 'do' list 'done'
  for_cmd       ->  'for' WORD ['in' WORD*] 'do' list 'done'
  function_def  ->  WORD '(' ')' '{' list '}'
                 |  'function' WORD ['(' ')'] '{' list '}'
```

### AST Node Types (13)

| Node Type | Union Member | Description |
|---|---|---|
| `NODE_COMMAND` | `cmd` | Simple command: `argv[]`, `argc`, redirections, assignments |
| `NODE_PIPELINE` | `pipeline` | Array of commands joined by `\|`, optional `negated` flag |
| `NODE_AND` | `binary` | `left && right` — short-circuit AND |
| `NODE_OR` | `binary` | `left \|\| right` — short-circuit OR |
| `NODE_SEQUENCE` | `binary` | `left ; right` — unconditional sequence |
| `NODE_BACKGROUND` | `child` | `command &` — wraps backgrounded subtree |
| `NODE_NEGATE` | `child` | `! command` — inverts exit status |
| `NODE_SUBSHELL` | `child` | `( list )` — executes in forked child |
| `NODE_IF` | `if_node` | `if/then/elif/else/fi` |
| `NODE_WHILE` | `while_node` | `while/do/done` |
| `NODE_FOR` | `for_node` | `for/in/do/done` |
| `NODE_FUNCTION` | `func` | Function definition (name + body) |
| `NODE_BLOCK` | `child` | `{ list }` — brace group, current shell |

### Supporting Structures

```c
/* Redirection (singly-linked list) */
typedef struct Redirection {
    RedirType type;        /* REDIR_INPUT, REDIR_OUTPUT, etc. */
    int       fd;          /* Source fd (-1 = default) */
    char     *target;      /* Filename or fd-as-string */
    struct Redirection *next;
} Redirection;

/* Simple command */
typedef struct CommandNode {
    char **argv;           /* Null-terminated argument array */
    int    argc;           /* Argument count */
    Redirection *redirs;   /* Linked list of redirections */
    char **assignments;    /* VAR=value prefixes (e.g. FOO=bar cmd) */
    int    nassign;        /* Number of assignments */
} CommandNode;

/* Pipeline */
typedef struct PipelineNode {
    ASTNode **commands;    /* Array of command nodes */
    int       count;       /* Number of commands */
    bool      negated;     /* ! prefix present */
} PipelineNode;

/* Binary operator (AND, OR, SEQUENCE) */
typedef struct BinaryNode {
    ASTNode *left;
    ASTNode *right;
} BinaryNode;
```

### AST Example

Input: `cat file | grep foo > out.txt && echo done`

```
                    NODE_AND
                   /        \
                  /          \
          NODE_PIPELINE     NODE_COMMAND
          (count=2)         argv=["echo","done"]
           /      \
          /        \
  NODE_COMMAND   NODE_COMMAND
  argv=["cat",   argv=["grep","foo"]
        "file"]  redirs -> REDIR_OUTPUT
                           fd=1
                           target="out.txt"
```

Input: `if test -f config; then source config; else echo missing; fi`

```
              NODE_IF
             /   |   \
            /    |    \
     condition  then   else
         |       |      |
   NODE_COMMAND  |   NODE_COMMAND
   argv=["test", |   argv=["echo",
         "-f",   |         "missing"]
         "config"]
                 |
           NODE_COMMAND
           argv=["source",
                 "config"]
```

---

## 6. Executor

The executor (`src/executor.c`) walks the AST and dispatches each node type. It also
handles word expansion (variables, tilde, globs) and the builtin-vs-external decision.

### Dispatch Table

| Node Type | Handler | Behavior |
|---|---|---|
| `NODE_COMMAND` | `exec_command()` | Expand words, run builtin or fork/exec |
| `NODE_PIPELINE` | `exec_pipeline()` | Delegate to `pipeline_execute()` |
| `NODE_AND` | `exec_and()` | Execute left; if `$? == 0`, execute right |
| `NODE_OR` | `exec_or()` | Execute left; if `$? != 0`, execute right |
| `NODE_SEQUENCE` | `exec_sequence()` | Execute left, then right unconditionally |
| `NODE_BACKGROUND` | `exec_background()` | Fork child, register as background job, return 0 |
| `NODE_NEGATE` | `exec_negate()` | Execute child, invert exit status |
| `NODE_SUBSHELL` | `exec_subshell()` | Fork child, execute subtree, wait |
| `NODE_IF` | `exec_if()` | Evaluate condition, branch to then/elif/else |
| `NODE_WHILE` | `exec_while()` | Loop: evaluate condition, execute body while `$? == 0` |
| `NODE_FOR` | `exec_for()` | For each word: expand, set var, execute body |
| `NODE_FUNCTION` | `exec_function()` | Store function body in environment |
| `NODE_BLOCK` | `exec_block()` | Execute child subtree in current shell |

### Word Expansion Pipeline

For each argument word in a `NODE_COMMAND`, three expansion phases run in order:

```
  Raw word from argv[]
       |
       v
  +------------------+
  | env_expand()     |   $VAR, ${VAR}, ${VAR:-default}, ${VAR:+alt},
  |                  |   ${#VAR}, $?, $$, $#, $@, $0..$9
  +------------------+
       |
       v
  +------------------+
  | env_expand_tilde()|  ~/path -> /home/user/path
  |                  |   ~user/path -> /home/user/path
  +------------------+   (only if word starts with ~)
       |
       v
  +------------------+
  | wildcard_expand()|   *, ?, [...] glob patterns
  |                  |   May produce multiple words
  +------------------+   (uses POSIX glob())
       |
       v
  Expanded argv[]         (may have grown due to glob matches)
```

### Builtin vs External Decision Tree

```
  After expansion, argv[0] is the command name:

  argv[0]
    |
    +---> builtins_is_builtin(argv[0])?
    |         |
    |        YES --> builtins_execute(shell, argc, argv)
    |         |      Runs in current process.
    |         |      Can modify shell state (cd, export, alias, exit, etc.)
    |         |
    |        NO  --> fork()
    |                  |
    |                CHILD:
    |                  setpgid(0, 0)          New process group
    |                  tcsetpgrp(0, getpid()) Take terminal
    |                  child_reset_signals()  Restore SIG_DFL
    |                  Apply assignments       (command-local env vars)
    |                  Apply redirections       (open/dup2)
    |                  execve() / execvp()     PATH lookup
    |                  _exit(127)              Not found
    |                  |
    |                PARENT:
    |                  setpgid(pid, pid)
    |                  job_add()              Register job
    |                  job_wait_foreground()  Block until done
    |                  |
    v                  v
  shell->last_status = exit code
```

**Exception**: When a builtin appears inside a pipeline (not as the only command),
it runs in a forked child and cannot affect the parent shell. This matches POSIX
behavior — `export FOO=bar | cat` does NOT set `FOO` in the parent.

---

## 7. Redirections

### Redirection Types (6)

| RedirType | Syntax | Action |
|---|---|---|
| `REDIR_INPUT` | `< file` | `open(file, O_RDONLY)` then `dup2(src_fd, target_fd)` |
| `REDIR_OUTPUT` | `> file` | `open(file, O_WRONLY\|O_CREAT\|O_TRUNC, 0644)` then `dup2()` |
| `REDIR_APPEND` | `>> file` | `open(file, O_WRONLY\|O_CREAT\|O_APPEND, 0644)` then `dup2()` |
| `REDIR_HEREDOC` | `<< DELIM` | Not yet implemented |
| `REDIR_DUP_OUT` | `2>&1` | `dup2(1, 2)` — duplicate fd 1 onto fd 2 |
| `REDIR_DUP_IN` | `0<&3` | `dup2(3, 0)` — duplicate fd 3 onto fd 0 |

### Storage

Redirections are stored as a singly-linked list on each `CommandNode`. The parser
prepends each new redirection to `cmd->redirs`. Example for `cmd > a.txt 2>&1 < b.txt`:

```
  cmd->redirs --> [REDIR_INPUT, fd=0, "b.txt"]
                    |
                    +--> [REDIR_DUP_OUT, fd=2, "1"]
                           |
                           +--> [REDIR_OUTPUT, fd=1, "a.txt"]
                                  |
                                  +--> NULL
```

### Application

Redirections are applied **only in child processes** (post-fork), via
`executor_apply_redirections()`. For each redirection in the linked list:

1. Open the target file (for file redirections) or parse the fd number (for dup)
2. Call `dup2(opened_fd, target_fd)` to wire the file descriptor
3. Close the temporary fd if it differs from the target

Since redirections only execute in children, the parent process never needs to save
or restore its own file descriptors.

---

## 8. Pipeline Wiring

The pipeline module (`src/pipeline.c`) handles multi-command pipes.

### Single-Command Optimization

If a pipeline has only one command, it is executed directly via `executor_execute()` in
the current process. This is critical because builtins like `cd` and `export` must modify
the parent shell's state.

### Multi-Command Pipeline

For N commands connected by `|`, the pipeline creates N-1 pipes and forks N children:

```
  Example: cmd1 | cmd2 | cmd3       (N=3, 2 pipes)

  pipe[0]           pipe[1]
  [read|write]      [read|write]

  +--------+  write  +--------+  write  +--------+
  |  cmd1  |-------->|  cmd2  |-------->|  cmd3  |
  | (child)| pipe[0] | (child)| pipe[1] | (child)|
  +--------+         +--------+         +--------+
   stdin              read               read       stdout
  (inherited)        pipe[0]            pipe[1]    (inherited)
```

### Fd Wiring per Child

```
  Child i (0-indexed) of N total:

  if (i > 0):
      dup2(pipes[i-1][READ_END], STDIN_FILENO)     <-- read from previous pipe
  if (i < N-1):
      dup2(pipes[i][WRITE_END], STDOUT_FILENO)      <-- write to next pipe
  close all pipe fds                                  <-- prevent fd leaks
```

### Process Group Management

```
  fork() child 0:
      pgid = child_0_pid                First child becomes group leader
      setpgid(0, 0)

  fork() child 1..N-1:
      setpgid(0, pgid)                  Join the leader's group

  Parent:
      setpgid(each_pid, pgid)           Ensure group assignment (race guard)
      tcsetpgrp(STDIN, pgid)            Give terminal to pipeline group
      close all pipe fds
      job_add(shell, pgid, pids, N)     Register as one job
      job_wait_foreground(shell, job)    Wait for all N children
      tcsetpgrp(STDIN, shell_pgid)      Reclaim terminal
```

### Pipeline Exit Status

The exit status of a pipeline is the exit status of the **last command** (rightmost).
If the `!` negation prefix was present, the status is inverted (0 becomes 1, non-zero
becomes 0). This matches POSIX behavior.

---

## 9. Job Control & Signals

### Signal Disposition

The shell configures signal handlers at startup in `shell_setup_signals()`:

| Signal | Shell Disposition | Child Disposition | Purpose |
|---|---|---|---|
| `SIGINT` | `SIG_IGN` | `SIG_DFL` | Ctrl+C kills child, not shell |
| `SIGQUIT` | `SIG_IGN` | `SIG_DFL` | Ctrl+\\ kills child, not shell |
| `SIGTSTP` | `SIG_IGN` | `SIG_DFL` | Ctrl+Z stops child, not shell |
| `SIGTTIN` | `SIG_IGN` | `SIG_DFL` | Background read from terminal |
| `SIGTTOU` | `SIG_IGN` | `SIG_DFL` | Background write to terminal |
| `SIGPIPE` | `SIG_IGN` | `SIG_DFL` | Broken pipe |
| `SIGCHLD` | `sigchld_handler` | `SIG_DFL` | Reap background children |

Children call `child_reset_signals()` after fork to restore all signals to `SIG_DFL`.

### Job Lifecycle

```
  Command with &:
      fork() --> child runs in background
      job_add(pgid, pids, n, foreground=false)
      Print "[1] 12345"
      Return immediately (status 0)

  Command without &:
      fork() --> child runs in foreground
      tcsetpgrp(STDIN, child_pgid)       Give terminal to child
      job_add(pgid, pids, n, foreground=true)
      job_wait_foreground()              Block until completion
      tcsetpgrp(STDIN, shell_pgid)       Reclaim terminal

  SIGCHLD arrives:
      sigchld_handler() sets a flag
      job_check_background() called at top of REPL loop
      Completed jobs printed: "[1]+ Done    sleep 10"

  Ctrl+Z (SIGTSTP) on foreground job:
      Child stops
      job->state = JOB_STOPPED
      Shell regains terminal
      Print "[1]+ Stopped    command"

  fg %1:
      tcsetpgrp(STDIN, job->pgid)
      kill(-job->pgid, SIGCONT)
      job_wait_foreground()

  bg %1:
      kill(-job->pgid, SIGCONT)
      job->state = JOB_RUNNING
```

### Job Table

The job table (`JobTable`) is a singly-linked list of `Job` structs. Each job tracks:

| Field | Purpose |
|---|---|
| `id` | Job number displayed as `[N]` |
| `pgid` | Process group ID (used for `kill(-pgid, sig)`) |
| `pids[]` | Array of all PIDs in the pipeline |
| `npids` | Number of processes |
| `state` | `JOB_RUNNING`, `JOB_STOPPED`, `JOB_DONE`, `JOB_KILLED` |
| `command` | Command string for display |
| `notified` | Whether the user has been told about completion |
| `foreground` | Whether this is/was a foreground job |

---

## 10. Memory Safety

### Arena Pattern

The arena allocator is the primary defense against memory leaks in the parse/execute
cycle. Every allocation during lexing, parsing, and word expansion goes through the arena.
A single `arena_reset()` at the start of each command frees everything at once — no need
to walk the AST and free individual nodes.

```
  Traditional approach:          Arena approach:
  ┌──────────────────────┐       ┌──────────────────────┐
  │ malloc(token_list)   │       │ arena_alloc(tokens)   │
  │ malloc(token.value)  │       │ arena_strdup(value)   │
  │ malloc(ast_node)     │       │ arena_calloc(node)    │
  │ malloc(argv_array)   │       │ arena_alloc(argv)     │
  │ malloc(redir_struct) │       │ arena_calloc(redir)   │
  │       ...            │       │       ...             │
  │ free(redir_struct)   │       │                       │
  │ free(argv_array)     │       │ arena_reset()         │
  │ free(ast_node)       │       │ (frees everything     │
  │ free(token.value)    │       │  in one call)         │
  │ free(token_list)     │       │                       │
  └──────────────────────┘       └──────────────────────┘
    Risk: miss a free()            Risk: none
    Cost: O(n) frees               Cost: O(1) reset
```

### SafeString

The `SafeString` library (`src/safe_string.c`) provides a bounds-checked dynamic string
buffer used throughout the shell for prompt building, line editing, and general string
manipulation. It auto-grows on append, tracks length and capacity separately, and
null-terminates at all times.

Key protections:
- All append operations check `len + new_data <= cap` before writing
- Growth uses a doubling strategy to amortize allocation cost
- All functions handle NULL input gracefully
- No raw `strcat()` or `sprintf()` — only `sstr_append()`, `sstr_appendf()`, etc.

### Compiler Flags

All builds use strict warning flags to catch bugs at compile time:

```
-std=c11                     C11 standard
-Wall                        All common warnings
-Wextra                      Extra warnings beyond -Wall
-Werror                      Treat all warnings as errors
-Wshadow                     Warn on variable shadowing
-Wstrict-prototypes          Require full prototypes
-Wmissing-prototypes         Warn on missing prototypes
-Wold-style-definition       Warn on K&R function definitions
-D_POSIX_C_SOURCE=200809L   POSIX.1-2008 features
-D_GNU_SOURCE                GNU extensions (getline, etc.)
```

The `make sanitize` target adds AddressSanitizer and UndefinedBehaviorSanitizer:

```
-fsanitize=address           Detect buffer overflows, use-after-free, leaks
-fsanitize=undefined         Detect signed overflow, null deref, alignment
-fno-omit-frame-pointer      Full stack traces in sanitizer reports
```
