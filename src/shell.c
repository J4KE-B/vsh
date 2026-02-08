/* ============================================================================
 * vsh - Vanguard Shell
 * shell.c - Core shell initialization, REPL loop, and supporting routines
 *
 * This file implements the main shell lifecycle: initialization of all
 * subsystems, the interactive read-eval-print loop, history/alias expansion,
 * terminal mode management, prompt building, and signal configuration.
 * ============================================================================ */

#include "shell.h"
#include "arena.h"
#include "env.h"
#include "history.h"
#include "builtins.h"
#include "job_control.h"
#include "lexer.h"
#include "parser.h"
#include "executor.h"
#include "vsh_readline.h"
#include "safe_string.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>

/* ---- Forward declarations of static helpers ----------------------------- */
static char *expand_history(Shell *shell, const char *line);
static char *expand_aliases(Shell *shell, const char *line);
static char *find_git_branch(void);
static char *shorten_path(const char *cwd, const char *home);
static char *build_history_path(void);

/* ============================================================================
 * shell_init - Allocate and initialize the shell and all subsystems
 * ============================================================================ */
Shell *shell_init(int argc, char **argv) {
    Shell *shell = calloc(1, sizeof(Shell));
    if (!shell) {
        fprintf(stderr, "vsh: fatal: out of memory\n");
        exit(1);
    }

    shell->shell_pid  = getpid();
    shell->interactive = isatty(STDIN_FILENO);
    shell->running     = true;

    /* Subsystem creation */
    shell->parse_arena = arena_create();
    shell->env         = env_create();
    shell->jobs        = calloc(1, sizeof(JobTable));
    if (shell->jobs) {
        shell->jobs->head    = NULL;
        shell->jobs->next_id = 1;
    }
    shell->history  = history_create(HISTORY_MAX_SIZE);
    shell->aliases  = calloc(1, sizeof(AliasTable));
    shell->dirstack = calloc(1, sizeof(DirStack));
    if (shell->dirstack) {
        shell->dirstack->top = -1;
    }

    /* Register built-in commands */
    builtins_init();

    /* Interactive-only initialization */
    if (shell->interactive) {
        tcgetattr(STDIN_FILENO, &shell->orig_termios);
        job_control_init(shell);
        job_set_shell(shell);

        /* Load persistent history */
        char *hist_path = build_history_path();
        if (hist_path) {
            history_load(shell->history, hist_path);
            free(hist_path);
        }

        shell_setup_signals(shell);
    }

    /* Source RC file (~/.vshrc) for interactive shells */
    if (shell->interactive) {
        const char *home = getenv("HOME");
        if (home) {
            char rc_path[PATH_MAX];
            int n = snprintf(rc_path, sizeof(rc_path), "%s/.vshrc", home);
            if (n > 0 && (size_t)n < sizeof(rc_path)) {
                struct stat st;
                if (stat(rc_path, &st) == 0 && S_ISREG(st.st_mode)) {
                    char *source_argv[] = { "source", rc_path, NULL };
                    builtin_source(shell, 2, source_argv);
                }
            }
        }
    }

    /* Standard environment variables */
    env_set(shell->env, "VSH_VERSION", "1.0.0", true);

    /* Store positional parameters ($0 .. $N) */
    if (argc > 0 && argv) {
        shell->pos_count  = argc;
        shell->pos_params = calloc((size_t)argc, sizeof(char *));
        if (shell->pos_params) {
            for (int i = 0; i < argc; i++) {
                shell->pos_params[i] = strdup(argv[i]);
            }
        }
    }

    return shell;
}

/* ============================================================================
 * shell_destroy - Tear down the shell and release all resources
 * ============================================================================ */
void shell_destroy(Shell *shell) {
    if (!shell) return;

    if (shell->interactive) {
        /* Persist history */
        char *hist_path = build_history_path();
        if (hist_path) {
            history_save(shell->history, hist_path);
            free(hist_path);
        }
        /* Restore original terminal attributes */
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &shell->orig_termios);
    }

    /* Subsystem destruction */
    if (shell->parse_arena)  arena_destroy(shell->parse_arena);
    if (shell->env)          env_destroy(shell->env);
    if (shell->jobs)         { job_table_destroy(shell); free(shell->jobs); }
    if (shell->history)      history_destroy(shell->history);

    /* Free alias table entries */
    if (shell->aliases) {
        for (int i = 0; i < ALIAS_HASH_SIZE; i++) {
            AliasEntry *e = shell->aliases->buckets[i];
            while (e) {
                AliasEntry *next = e->next;
                free(e->name);
                free(e->value);
                free(e);
                e = next;
            }
        }
        free(shell->aliases);
    }

    /* Free directory stack strings */
    if (shell->dirstack) {
        for (int i = 0; i <= shell->dirstack->top; i++) {
            free(shell->dirstack->dirs[i]);
        }
        free(shell->dirstack);
    }

    /* Free positional parameters */
    if (shell->pos_params) {
        for (int i = 0; i < shell->pos_count; i++) {
            free(shell->pos_params[i]);
        }
        free(shell->pos_params);
    }

    free(shell);
}

/* ============================================================================
 * shell_run - Main REPL loop (interactive) or batch reader (non-interactive)
 * ============================================================================ */
int shell_run(Shell *shell) {
    if (shell->interactive) {
        while (shell->running) {
            job_check_background(shell);

            char *prompt = shell_build_prompt(shell);
            char *line   = vsh_readline(shell, prompt);
            free(prompt);

            if (!line) {
                /* EOF (Ctrl+D) */
                printf("\n");
                break;
            }

            if (line[0] != '\0') {
                shell_exec_line(shell, line);
            }

            free(line);
        }
    } else {
        /* Non-interactive: read from stdin line by line */
        char buf[4096];
        while (fgets(buf, sizeof(buf), stdin)) {
            size_t len = strlen(buf);
            if (len > 0 && buf[len - 1] == '\n') {
                buf[len - 1] = '\0';
            }
            if (buf[0] == '#' || buf[0] == '\0') continue;
            shell_exec_line(shell, buf);
        }
    }

    return shell->last_status;
}

/* ============================================================================
 * shell_exec_line - Execute a single line of input
 *
 * Pipeline: history expansion -> alias expansion -> lex -> parse -> execute
 * ============================================================================ */
int shell_exec_line(Shell *shell, const char *line) {
    if (!line || line[0] == '\0') return shell->last_status;

    /* ---- History expansion (!! / !N / !-N / !prefix) -------------------- */
    char *expanded = expand_history(shell, line);
    if (!expanded) return shell->last_status;

    /* Add the (possibly expanded) line to history */
    history_add(shell->history, expanded);

    /* ---- Alias expansion ------------------------------------------------ */
    char *aliased = expand_aliases(shell, expanded);
    if (expanded != line) free(expanded);
    if (!aliased) return shell->last_status;

    /* ---- Lex ------------------------------------------------------------ */
    arena_reset(shell->parse_arena);

    Lexer lex;
    lexer_init(&lex, aliased, shell->parse_arena);
    TokenList *tokens = lexer_tokenize(&lex);

    if (aliased != expanded && aliased != line) free(aliased);

    if (!tokens || lex.error) {
        fprintf(stderr, "vsh: syntax error: %s\n",
                lex.error ? lex.error : "tokenization failed");
        shell->last_status = 2;
        return shell->last_status;
    }

    /* ---- Parse ---------------------------------------------------------- */
    Parser parser;
    parser_init(&parser, tokens, shell->parse_arena);
    ASTNode *ast = parser_parse(&parser);

    if (parser.had_error || !ast) {
        const char *msg = parser_error(&parser);
        fprintf(stderr, "vsh: parse error: %s\n",
                msg ? msg : "unexpected token");
        shell->last_status = 2;
        return shell->last_status;
    }

    /* ---- Execute -------------------------------------------------------- */
    shell->last_status = executor_execute(shell, ast);
    return shell->last_status;
}

/* ============================================================================
 * shell_enable_raw_mode - Switch terminal to cbreak mode for line editing
 * ============================================================================ */
void shell_enable_raw_mode(Shell *shell) {
    if (!shell->interactive || shell->raw_mode) return;

    struct termios raw = shell->orig_termios;

    /* Disable Ctrl+S/Q flow control, fix Ctrl+M so it reads as 13 */
    raw.c_iflag &= ~((unsigned)IXON | (unsigned)ICRNL);

    /* No echo, no canonical buffering, no Ctrl+V literal-next */
    /* Keep ISIG so Ctrl+C generates SIGINT (handled by our signal setup) */
    raw.c_lflag &= ~((unsigned)ECHO | (unsigned)ICANON | (unsigned)IEXTEN);

    raw.c_cc[VMIN]  = 1;   /* Read returns after 1 byte */
    raw.c_cc[VTIME] = 0;   /* No timeout */

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    shell->raw_mode = true;
}

/* ============================================================================
 * shell_disable_raw_mode - Restore original terminal settings
 * ============================================================================ */
void shell_disable_raw_mode(Shell *shell) {
    if (!shell->interactive || !shell->raw_mode) return;

    tcsetattr(STDIN_FILENO, TCSAFLUSH, &shell->orig_termios);
    shell->raw_mode = false;
}

/* ============================================================================
 * shell_build_prompt - Build a coloured, informative prompt string
 *
 * Format:
 *   [HH:MM:SS] user@host:~/path (git-branch)
 *   $ _                         (green if $?==0, red otherwise)
 * ============================================================================ */
char *shell_build_prompt(Shell *shell) {
    /* Colour codes */
    static const char *COL_RESET   = "\x1b[0m";
    static const char *COL_DIM     = "\x1b[90m";
    static const char *COL_GREEN_B = "\x1b[1;32m";
    static const char *COL_BLUE_B  = "\x1b[1;34m";
    static const char *COL_MAG_B   = "\x1b[1;35m";
    static const char *COL_RED_B   = "\x1b[1;31m";

    /* Allocate a SafeString to build the prompt incrementally */
    SafeString *ps = sstr_new(256);
    if (!ps) return strdup("$ ");

    /* -- Time component --------------------------------------------------- */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char timebuf[16];
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);
    sstr_appendf(ps, "%s[%s]%s ", COL_DIM, timebuf, COL_RESET);

    /* -- User@Host -------------------------------------------------------- */
    const char *user = env_get(shell->env, "USER");
    if (!user) user = getenv("USER");
    if (!user) user = "user";

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        strncpy(hostname, "localhost", sizeof(hostname) - 1);
        hostname[sizeof(hostname) - 1] = '\0';
    }
    /* Trim domain part for readability */
    char *dot = strchr(hostname, '.');
    if (dot) *dot = '\0';

    sstr_appendf(ps, "%s%s@%s%s:", COL_GREEN_B, user, hostname, COL_RESET);

    /* -- Working directory (with ~ substitution) -------------------------- */
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        strncpy(cwd, "?", sizeof(cwd));
    }

    const char *home = env_get(shell->env, "HOME");
    if (!home) home = getenv("HOME");

    char *display_path = shorten_path(cwd, home);
    sstr_appendf(ps, "%s%s%s", COL_BLUE_B, display_path, COL_RESET);
    free(display_path);

    /* -- Git branch (if in a repo) ---------------------------------------- */
    char *branch = find_git_branch();
    if (branch) {
        sstr_appendf(ps, " %s(%s)%s", COL_MAG_B, branch, COL_RESET);
        free(branch);
    }

    /* -- Newline + status indicator --------------------------------------- */
    sstr_append(ps, "\n");
    if (shell->last_status == 0) {
        sstr_appendf(ps, "%s$%s ", COL_GREEN_B, COL_RESET);
    } else {
        sstr_appendf(ps, "%s[%d]$%s ", COL_RED_B, shell->last_status, COL_RESET);
    }

    /* Convert SafeString to a plain malloc'd string */
    char *result = strdup(sstr_cstr(ps));
    sstr_free(ps);
    return result ? result : strdup("$ ");
}

/* ============================================================================
 * shell_setup_signals - Install signal handlers for the interactive shell
 * ============================================================================ */
void shell_setup_signals(Shell *shell) {
    (void)shell;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    /* Ignore signals that the shell itself should not respond to */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags   = 0;

    sigaction(SIGINT,  &sa, NULL);   /* Ctrl+C handled in readline */
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);   /* Ctrl+Z handled in job control */
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    /* SIGCHLD: reap background children */
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
}

/* ============================================================================
 * Static helpers
 * ============================================================================ */

/* ---- History expansion -------------------------------------------------- */
static char *expand_history(Shell *shell, const char *line) {
    if (!line || line[0] != '!') {
        return (char *)line;   /* No expansion needed; caller won't free */
    }

    /* "!!" - repeat last command */
    if (line[1] == '!') {
        const char *last = history_last(shell->history);
        if (!last) {
            fprintf(stderr, "vsh: !!: event not found\n");
            return NULL;
        }
        /* Concatenate replacement + rest of line after "!!" */
        size_t rest_len = strlen(line + 2);
        size_t last_len = strlen(last);
        char *result = malloc(last_len + rest_len + 1);
        if (!result) return NULL;
        memcpy(result, last, last_len);
        memcpy(result + last_len, line + 2, rest_len + 1);
        fprintf(stderr, "%s\n", result);
        return result;
    }

    /* "!-N" - Nth from last */
    if (line[1] == '-' && isdigit((unsigned char)line[2])) {
        int n = atoi(line + 2);
        int count = history_count(shell->history);
        int target = count - n;
        if (target < 0) {
            fprintf(stderr, "vsh: !-%d: event not found\n", n);
            return NULL;
        }
        const char *entry = history_get(shell->history, target);
        if (!entry) {
            fprintf(stderr, "vsh: !-%d: event not found\n", n);
            return NULL;
        }
        /* Find end of the !-N specifier */
        const char *rest = line + 2;
        while (*rest && isdigit((unsigned char)*rest)) rest++;
        size_t entry_len = strlen(entry);
        size_t rest_len  = strlen(rest);
        char *result = malloc(entry_len + rest_len + 1);
        if (!result) return NULL;
        memcpy(result, entry, entry_len);
        memcpy(result + entry_len, rest, rest_len + 1);
        fprintf(stderr, "%s\n", result);
        return result;
    }

    /* "!N" - history entry by index */
    if (isdigit((unsigned char)line[1])) {
        int n = atoi(line + 1);
        const char *entry = history_get_by_index(shell->history, n);
        if (!entry) {
            fprintf(stderr, "vsh: !%d: event not found\n", n);
            return NULL;
        }
        /* Find end of the number */
        const char *rest = line + 1;
        while (*rest && isdigit((unsigned char)*rest)) rest++;
        size_t entry_len = strlen(entry);
        size_t rest_len  = strlen(rest);
        char *result = malloc(entry_len + rest_len + 1);
        if (!result) return NULL;
        memcpy(result, entry, entry_len);
        memcpy(result + entry_len, rest, rest_len + 1);
        fprintf(stderr, "%s\n", result);
        return result;
    }

    /* "!prefix" - most recent command starting with prefix */
    if (isalpha((unsigned char)line[1]) || line[1] == '_') {
        /* Extract the prefix (everything after ! until whitespace or end) */
        const char *pstart = line + 1;
        const char *pend   = pstart;
        while (*pend && !isspace((unsigned char)*pend)) pend++;

        size_t plen = (size_t)(pend - pstart);
        char *prefix = malloc(plen + 1);
        if (!prefix) return NULL;
        memcpy(prefix, pstart, plen);
        prefix[plen] = '\0';

        const char *entry = history_search_prefix(shell->history, prefix);
        free(prefix);

        if (!entry) {
            fprintf(stderr, "vsh: !%.*s: event not found\n", (int)plen, pstart);
            return NULL;
        }

        size_t entry_len = strlen(entry);
        size_t rest_len  = strlen(pend);
        char *result = malloc(entry_len + rest_len + 1);
        if (!result) return NULL;
        memcpy(result, entry, entry_len);
        memcpy(result + entry_len, pend, rest_len + 1);
        fprintf(stderr, "%s\n", result);
        return result;
    }

    /* No recognised expansion pattern */
    return (char *)line;
}

/* ---- Alias expansion ---------------------------------------------------- */

/* Simple hash for alias lookup (must match whatever builtins/alias uses) */
static unsigned alias_hash(const char *key) {
    unsigned h = 5381;
    while (*key) {
        h = ((h << 5) + h) + (unsigned char)*key++;
    }
    return h % ALIAS_HASH_SIZE;
}

static const char *alias_lookup(AliasTable *tbl, const char *name) {
    if (!tbl) return NULL;
    unsigned h = alias_hash(name);
    for (AliasEntry *e = tbl->buckets[h]; e; e = e->next) {
        if (strcmp(e->name, name) == 0) return e->value;
    }
    return NULL;
}

static char *expand_aliases(Shell *shell, const char *line) {
    if (!shell->aliases || !line || line[0] == '\0') {
        return (char *)line;
    }

    /* Work on a mutable copy */
    char *current = strdup(line);
    if (!current) return (char *)line;

    for (int depth = 0; depth < 10; depth++) {
        /* Skip leading whitespace */
        const char *p = current;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        /* Extract the first word */
        const char *word_start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t word_len = (size_t)(p - word_start);

        char word[256];
        if (word_len >= sizeof(word)) break;
        memcpy(word, word_start, word_len);
        word[word_len] = '\0';

        const char *replacement = alias_lookup(shell->aliases, word);
        if (!replacement) break;

        /* Build new line: leading_ws + replacement + rest */
        size_t leading = (size_t)(word_start - current);
        size_t rep_len = strlen(replacement);
        size_t rest_len = strlen(p);

        char *newline = malloc(leading + rep_len + rest_len + 1);
        if (!newline) break;

        memcpy(newline, current, leading);
        memcpy(newline + leading, replacement, rep_len);
        memcpy(newline + leading + rep_len, p, rest_len + 1);

        free(current);
        current = newline;

        /* If the replacement doesn't end with a space, stop expanding
         * (bash convention: trailing space means expand next word too,
         *  but we keep it simple and just re-check the first word) */
        if (rep_len == 0 || replacement[rep_len - 1] != ' ') break;
    }

    return current;
}

/* ---- Git branch detection ----------------------------------------------- */
static char *find_git_branch(void) {
    char path[PATH_MAX];

    if (!getcwd(path, sizeof(path))) return NULL;

    /* Walk up directory tree looking for .git */
    while (1) {
        char git_head[PATH_MAX];
        int n = snprintf(git_head, sizeof(git_head), "%s/.git/HEAD", path);
        if (n < 0 || (size_t)n >= sizeof(git_head)) break;

        FILE *fp = fopen(git_head, "r");
        if (fp) {
            char buf[256];
            if (fgets(buf, sizeof(buf), fp)) {
                fclose(fp);
                size_t len = strlen(buf);
                if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';

                /* Symbolic ref: "ref: refs/heads/BRANCH" */
                const char *prefix = "ref: refs/heads/";
                if (strncmp(buf, prefix, strlen(prefix)) == 0) {
                    return strdup(buf + strlen(prefix));
                }
                /* Detached HEAD: return short hash */
                if (len >= 7) {
                    buf[7] = '\0';
                    return strdup(buf);
                }
            }
            fclose(fp); /* in case fgets failed */
            return NULL;
        }

        /* Move to parent directory */
        char *slash = strrchr(path, '/');
        if (!slash || slash == path) break;
        *slash = '\0';
    }

    return NULL;
}

/* ---- Path shortening (HOME -> ~) ---------------------------------------- */
static char *shorten_path(const char *cwd, const char *home) {
    if (home && *home) {
        size_t home_len = strlen(home);
        if (strncmp(cwd, home, home_len) == 0 &&
            (cwd[home_len] == '/' || cwd[home_len] == '\0')) {
            size_t rest_len = strlen(cwd + home_len);
            char *result = malloc(1 + rest_len + 1);
            if (result) {
                result[0] = '~';
                memcpy(result + 1, cwd + home_len, rest_len + 1);
                return result;
            }
        }
    }
    return strdup(cwd);
}

/* ---- Build path to ~/.vsh_history --------------------------------------- */
static char *build_history_path(void) {
    const char *home = getenv("HOME");
    if (!home) return NULL;

    size_t len = strlen(home) + 1 + strlen(HISTORY_FILE) + 1;
    char *path = malloc(len);
    if (path) {
        snprintf(path, len, "%s/%s", home, HISTORY_FILE);
    }
    return path;
}
