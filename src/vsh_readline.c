/* ============================================================================
 * vsh - Vanguard Shell
 * vsh_readline.c - Line editor with tab completion (no libreadline dependency)
 *
 * Implements a full interactive line editor supporting cursor movement,
 * kill/yank, history navigation, reverse incremental search, and tab
 * completion for commands, builtins, and file paths.
 *
 * All terminal output uses write(STDOUT_FILENO, ...) for signal safety.
 * Assumes the terminal is already in raw mode when vsh_readline() is called.
 * ============================================================================ */

#include "vsh_readline.h"
#include "shell.h"
#include "history.h"
#include "builtins.h"
#include "safe_string.h"

#include <unistd.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* --------------------------------------------------------------------------
 * LineEditor - internal state for a single readline invocation
 * -------------------------------------------------------------------------- */

typedef struct LineEditor {
    SafeString *buf;          /* Current line buffer                       */
    int         cursor;       /* Cursor position (byte offset)             */
    int         prompt_len;   /* Displayed prompt width (for cursor math)  */
    SafeString *yank_buf;     /* Kill ring (last killed text)              */
    Shell      *shell;
    /* Reverse search state */
    bool        searching;
    SafeString *search_buf;
    int         search_pos;   /* Position in history for search            */
} LineEditor;

/* Persistent yank buffer across invocations (static) */
static SafeString *s_yank_buf = NULL;

/* Saved line when navigating history */
static SafeString *s_saved_line = NULL;

/* --------------------------------------------------------------------------
 * Low-level I/O helpers
 * -------------------------------------------------------------------------- */

/* Signal-safe write wrapper: retries on EINTR. */
static void term_write(const char *s, size_t len)
{
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, s, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        s   += n;
        len -= (size_t)n;
    }
}

/* Convenience: write a C string. */
static void term_puts(const char *s)
{
    term_write(s, strlen(s));
}

/* Signal-safe single-byte read; retries on EINTR. Returns 1 on success, 0
 * on EOF, -1 on error. */
static int term_read_char(char *c)
{
    for (;;) {
        ssize_t n = read(STDIN_FILENO, c, 1);
        if (n == 1) return 1;
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* Get terminal width. Falls back to 80. */
static int term_cols(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

/* --------------------------------------------------------------------------
 * Display / refresh
 * -------------------------------------------------------------------------- */

static void refresh_line(LineEditor *ed, const char *prompt)
{
    const char *buf = sstr_cstr(ed->buf);
    size_t      len = ed->buf->len;

    /* Build output in one batch to reduce flicker. */
    SafeString *out = sstr_new(256);

    /* \r - move to column 0 */
    sstr_append(out, "\r");

    /* Write prompt */
    sstr_append(out, prompt);

    /* Write buffer content */
    sstr_append_n(out, buf, len);

    /* Clear to end of line */
    sstr_append(out, "\x1b[0K");

    /* Move cursor to correct position:
     * \r brings us to col 0, then advance prompt_len + cursor positions. */
    sstr_append(out, "\r");
    int pos = ed->prompt_len + ed->cursor;
    if (pos > 0) {
        char seq[32];
        snprintf(seq, sizeof(seq), "\x1b[%dC", pos);
        sstr_append(out, seq);
    }

    term_write(sstr_cstr(out), out->len);
    sstr_free(out);
}

/* --------------------------------------------------------------------------
 * Completions helpers
 * -------------------------------------------------------------------------- */

static Completions *completions_new(void)
{
    Completions *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->capacity = 16;
    c->entries = calloc((size_t)c->capacity, sizeof(char *));
    if (!c->entries) { free(c); return NULL; }
    return c;
}

static void completions_add(Completions *c, const char *entry)
{
    if (!c || !entry) return;
    if (c->count >= c->capacity) {
        int newcap = c->capacity * 2;
        char **tmp = realloc(c->entries, (size_t)newcap * sizeof(char *));
        if (!tmp) return;
        c->entries  = tmp;
        c->capacity = newcap;
    }
    c->entries[c->count++] = strdup(entry);
}

void completions_free(Completions *comp)
{
    if (!comp) return;
    for (int i = 0; i < comp->count; i++)
        free(comp->entries[i]);
    free(comp->entries);
    free(comp);
}

/* --------------------------------------------------------------------------
 * Tab completion - PATH / builtins / files
 * -------------------------------------------------------------------------- */

/* Find start of the word being completed. */
static int word_start(const char *line, int cursor)
{
    int i = cursor;
    while (i > 0 && line[i - 1] != ' ')
        i--;
    return i;
}

/* Is the word at position the first word (command position)? */
static bool is_command_position(const char *line, int ws)
{
    /* Everything before ws should be whitespace (or empty). */
    for (int i = 0; i < ws; i++) {
        if (line[i] != ' ' && line[i] != '\t')
            return false;
    }
    return true;
}

/* Add executable matches from a single directory. */
static void complete_from_dir(Completions *comp, const char *dir_path,
                              const char *prefix, size_t prefix_len)
{
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && prefix_len == 0)
            continue; /* skip hidden unless prefix starts with '.' */
        if (strncmp(ent->d_name, prefix, prefix_len) != 0)
            continue;

        /* Check if executable */
        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, ent->d_name);
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
            completions_add(comp, ent->d_name);
        }
    }
    closedir(d);
}

/* Complete command names (PATH + builtins). */
static void complete_commands(Completions *comp, Shell *shell __attribute__((unused)),
                              const char *prefix, size_t prefix_len)
{
    /* Builtins */
    int bcount = 0;
    const BuiltinEntry *btable = builtins_table(&bcount);
    for (int i = 0; i < bcount; i++) {
        if (strncmp(btable[i].name, prefix, prefix_len) == 0)
            completions_add(comp, btable[i].name);
    }

    /* PATH directories */
    const char *path_var = getenv("PATH");
    if (!path_var) return;

    char *path_copy = strdup(path_var);
    if (!path_copy) return;

    char *saveptr = NULL;
    for (char *dir = strtok_r(path_copy, ":", &saveptr);
         dir != NULL;
         dir = strtok_r(NULL, ":", &saveptr)) {
        complete_from_dir(comp, dir, prefix, prefix_len);
    }
    free(path_copy);
}

/* Complete file/directory names. */
static void complete_files(Completions *comp, const char *prefix, size_t prefix_len)
{
    const char *dir_part  = ".";
    const char *base_part = prefix;
    size_t base_len = prefix_len;

    /* Split into directory and basename parts. */
    char dir_buf[4096];
    const char *last_slash = NULL;
    for (size_t i = 0; i < prefix_len; i++) {
        if (prefix[i] == '/')
            last_slash = prefix + i;
    }

    if (last_slash) {
        size_t dlen = (size_t)(last_slash - prefix);
        if (dlen == 0) {
            dir_part = "/";
        } else {
            if (dlen >= sizeof(dir_buf)) dlen = sizeof(dir_buf) - 1;
            memcpy(dir_buf, prefix, dlen);
            dir_buf[dlen] = '\0';
            dir_part = dir_buf;
        }
        base_part = last_slash + 1;
        base_len  = prefix_len - (size_t)(base_part - prefix);
    }

    DIR *d = opendir(dir_part);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip . and .. unless explicitly typed. */
        if (ent->d_name[0] == '.' && base_len == 0)
            continue;
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (strncmp(ent->d_name, base_part, base_len) != 0)
            continue;

        /* Build the full completion string (re-include dir_part if needed). */
        char full[4096];
        if (last_slash) {
            size_t dlen = (size_t)(last_slash - prefix) + 1; /* include slash */
            if (dlen >= sizeof(full)) continue;
            memcpy(full, prefix, dlen);
            full[dlen] = '\0';
            strncat(full, ent->d_name, sizeof(full) - dlen - 1);
        } else {
            snprintf(full, sizeof(full), "%s", ent->d_name);
        }

        /* Append '/' for directories. */
        char check_path[PATH_MAX];
        int cp_len = snprintf(check_path, sizeof(check_path), "%s/%s", dir_part, ent->d_name);
        if (cp_len < 0 || (size_t)cp_len >= sizeof(check_path)) continue;
        struct stat st;
        if (stat(check_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            size_t flen = strlen(full);
            if (flen + 1 < sizeof(full)) {
                full[flen]     = '/';
                full[flen + 1] = '\0';
            }
        }

        completions_add(comp, full);
    }
    closedir(d);
}

Completions *vsh_complete(Shell *shell, const char *line, int cursor_pos)
{
    Completions *comp = completions_new();
    if (!comp) return NULL;

    int ws = word_start(line, cursor_pos);
    size_t prefix_len = (size_t)(cursor_pos - ws);
    char prefix[4096];
    if (prefix_len >= sizeof(prefix))
        prefix_len = sizeof(prefix) - 1;
    memcpy(prefix, line + ws, prefix_len);
    prefix[prefix_len] = '\0';

    if (is_command_position(line, ws)) {
        /* If prefix contains a slash, complete as file path (e.g. ./foo). */
        if (strchr(prefix, '/'))
            complete_files(comp, prefix, prefix_len);
        else
            complete_commands(comp, shell, prefix, prefix_len);
    } else {
        complete_files(comp, prefix, prefix_len);
    }

    return comp;
}

/* Find the longest common prefix among completions. */
static size_t common_prefix_len(Completions *comp)
{
    if (comp->count == 0) return 0;
    if (comp->count == 1) return strlen(comp->entries[0]);

    size_t cpl = strlen(comp->entries[0]);
    for (int i = 1; i < comp->count; i++) {
        size_t j = 0;
        while (j < cpl && comp->entries[i][j] == comp->entries[0][j])
            j++;
        cpl = j;
    }
    return cpl;
}

/* Handle tab key. */
static void handle_tab(LineEditor *ed, const char *prompt)
{
    const char *line = sstr_cstr(ed->buf);
    Completions *comp = vsh_complete(ed->shell, line, ed->cursor);
    if (!comp || comp->count == 0) {
        completions_free(comp);
        return;
    }

    int ws = word_start(line, ed->cursor);
    size_t prefix_len = (size_t)(ed->cursor - ws);

    if (comp->count == 1) {
        /* Single match: insert the remaining part + space. */
        const char *match = comp->entries[0];
        size_t mlen = strlen(match);
        if (mlen > prefix_len) {
            const char *suffix = match + prefix_len;
            size_t slen = mlen - prefix_len;
            /* Insert suffix into buffer at cursor. */
            sstr_ensure(ed->buf, slen + 1);
            for (size_t i = 0; i < slen; i++) {
                sstr_insert_char(ed->buf, (size_t)ed->cursor, suffix[i]);
                ed->cursor++;
            }
        }
        /* Append a space if the completion doesn't end with '/'. */
        size_t blen = ed->buf->len;
        if (blen == 0 || ed->buf->data[ed->cursor - 1] != '/') {
            sstr_insert_char(ed->buf, (size_t)ed->cursor, ' ');
            ed->cursor++;
        }
        refresh_line(ed, prompt);
    } else {
        /* Multiple matches: complete to longest common prefix. */
        size_t cpl = common_prefix_len(comp);
        if (cpl > prefix_len) {
            const char *suffix = comp->entries[0] + prefix_len;
            size_t slen = cpl - prefix_len;
            sstr_ensure(ed->buf, slen);
            for (size_t i = 0; i < slen; i++) {
                sstr_insert_char(ed->buf, (size_t)ed->cursor, suffix[i]);
                ed->cursor++;
            }
            refresh_line(ed, prompt);
        }
        /* Display all matches. */
        term_puts("\r\n");
        int cols = term_cols();
        /* Find max entry length for columnar display. */
        int maxlen = 0;
        for (int i = 0; i < comp->count; i++) {
            int l = (int)strlen(comp->entries[i]);
            if (l > maxlen) maxlen = l;
        }
        int colw = maxlen + 2;
        int ncols = cols / colw;
        if (ncols < 1) ncols = 1;

        for (int i = 0; i < comp->count; i++) {
            if (i > 0 && (i % ncols) == 0)
                term_puts("\r\n");
            char fmt_buf[4096];
            snprintf(fmt_buf, sizeof(fmt_buf), "%-*s", colw, comp->entries[i]);
            term_puts(fmt_buf);
        }
        term_puts("\r\n");
        /* Re-display prompt and line. */
        refresh_line(ed, prompt);
    }

    completions_free(comp);
}

/* --------------------------------------------------------------------------
 * Kill / yank helpers
 * -------------------------------------------------------------------------- */

static void yank_save(LineEditor *ed, const char *text, size_t len)
{
    if (!s_yank_buf)
        s_yank_buf = sstr_new(64);
    sstr_clear(s_yank_buf);
    sstr_append_n(s_yank_buf, text, len);
    ed->yank_buf = s_yank_buf;
}

/* Ctrl+K: kill from cursor to end of line. */
static void kill_to_end(LineEditor *ed)
{
    const char *data = sstr_cstr(ed->buf);
    size_t len = ed->buf->len;
    size_t cur = (size_t)ed->cursor;

    if (cur < len) {
        yank_save(ed, data + cur, len - cur);
        sstr_truncate(ed->buf, cur);
    }
}

/* Ctrl+U: kill from beginning to cursor. */
static void kill_to_start(LineEditor *ed)
{
    if (ed->cursor > 0) {
        yank_save(ed, sstr_cstr(ed->buf), (size_t)ed->cursor);
        sstr_delete(ed->buf, 0, (size_t)ed->cursor);
        ed->cursor = 0;
    }
}

/* Ctrl+W: kill previous word. */
static void kill_prev_word(LineEditor *ed)
{
    int i = ed->cursor;
    /* Skip trailing whitespace. */
    while (i > 0 && ed->buf->data[i - 1] == ' ')
        i--;
    /* Skip word chars. */
    while (i > 0 && ed->buf->data[i - 1] != ' ')
        i--;

    if (i < ed->cursor) {
        size_t count = (size_t)(ed->cursor - i);
        yank_save(ed, ed->buf->data + i, count);
        sstr_delete(ed->buf, (size_t)i, count);
        ed->cursor = i;
    }
}

/* Alt+D: kill word forward. */
static void kill_word_forward(LineEditor *ed)
{
    int i = ed->cursor;
    int len = (int)ed->buf->len;

    /* Skip whitespace. */
    while (i < len && ed->buf->data[i] == ' ')
        i++;
    /* Skip word chars. */
    while (i < len && ed->buf->data[i] != ' ')
        i++;

    if (i > ed->cursor) {
        size_t count = (size_t)(i - ed->cursor);
        yank_save(ed, ed->buf->data + ed->cursor, count);
        sstr_delete(ed->buf, (size_t)ed->cursor, count);
    }
}

/* Ctrl+Y: yank (paste). */
static void yank(LineEditor *ed)
{
    if (!s_yank_buf || sstr_empty(s_yank_buf))
        return;
    const char *text = sstr_cstr(s_yank_buf);
    size_t tlen = s_yank_buf->len;
    sstr_ensure(ed->buf, tlen);
    for (size_t i = 0; i < tlen; i++) {
        sstr_insert_char(ed->buf, (size_t)ed->cursor, text[i]);
        ed->cursor++;
    }
}

/* --------------------------------------------------------------------------
 * Word movement helpers
 * -------------------------------------------------------------------------- */

/* Alt+B: move back one word. */
static void move_word_back(LineEditor *ed)
{
    int i = ed->cursor;
    while (i > 0 && ed->buf->data[i - 1] == ' ')
        i--;
    while (i > 0 && ed->buf->data[i - 1] != ' ')
        i--;
    ed->cursor = i;
}

/* Alt+F: move forward one word. */
static void move_word_forward(LineEditor *ed)
{
    int i = ed->cursor;
    int len = (int)ed->buf->len;
    while (i < len && ed->buf->data[i] == ' ')
        i++;
    while (i < len && ed->buf->data[i] != ' ')
        i++;
    ed->cursor = i;
}

/* --------------------------------------------------------------------------
 * History navigation
 * -------------------------------------------------------------------------- */

static void history_nav_up(LineEditor *ed, const char *prompt)
{
    History *hist = ed->shell->history;
    if (!hist) return;

    /* Save current line if we are at the bottom (first up press). */
    if (hist->pos >= hist->count) {
        if (!s_saved_line)
            s_saved_line = sstr_new(64);
        sstr_set(s_saved_line, sstr_cstr(ed->buf));
    }

    const char *entry = history_navigate_up(hist);
    if (entry) {
        sstr_set(ed->buf, entry);
        ed->cursor = (int)ed->buf->len;
        refresh_line(ed, prompt);
    }
}

static void history_nav_down(LineEditor *ed, const char *prompt)
{
    History *hist = ed->shell->history;
    if (!hist) return;

    const char *entry = history_navigate_down(hist);
    if (entry) {
        sstr_set(ed->buf, entry);
        ed->cursor = (int)ed->buf->len;
    } else {
        /* Restore saved line at the bottom of history. */
        if (s_saved_line && !sstr_empty(s_saved_line))
            sstr_set(ed->buf, sstr_cstr(s_saved_line));
        else
            sstr_clear(ed->buf);
        ed->cursor = (int)ed->buf->len;
    }
    refresh_line(ed, prompt);
}

/* --------------------------------------------------------------------------
 * Reverse incremental search (Ctrl+R)
 * -------------------------------------------------------------------------- */

static void reverse_search(LineEditor *ed, const char *prompt)
{
    History *hist = ed->shell->history;
    if (!hist) return;

    SafeString *search_buf = sstr_new(64);
    int search_pos = hist->count; /* Start past the end (search from newest). */

    for (;;) {
        /* Build the search prompt. */
        const char *match = "";
        if (search_buf->len > 0) {
            int out_pos = search_pos;
            const char *found = history_search_substr(hist, sstr_cstr(search_buf), &out_pos);
            if (found) {
                match = found;
                search_pos = out_pos;
            }
        }

        /* Display: (reverse-i-search)`query': matched_line */
        char search_prompt[512];
        snprintf(search_prompt, sizeof(search_prompt),
                 "\r\x1b[0K(reverse-i-search)`%s': %s",
                 sstr_cstr(search_buf), match);
        term_puts(search_prompt);

        char c;
        int rc = term_read_char(&c);
        if (rc <= 0)
            break;

        if (c == 18) {
            /* Ctrl+R again: search further back. */
            if (search_pos > 0)
                search_pos--;
            continue;
        } else if (c == 10 || c == 13) {
            /* Enter: accept the match. */
            if (match[0] != '\0') {
                sstr_set(ed->buf, match);
                ed->cursor = (int)ed->buf->len;
            }
            sstr_free(search_buf);
            /* Clear search line and refresh normal prompt. */
            term_puts("\r\x1b[0K");
            refresh_line(ed, prompt);
            return;
        } else if (c == 7 || c == 27) {
            /* Ctrl+G or Escape: cancel search, restore original line. */
            sstr_free(search_buf);
            term_puts("\r\x1b[0K");
            refresh_line(ed, prompt);
            return;
        } else if (c == 127 || c == 8) {
            /* Backspace: remove last char from search query. */
            if (search_buf->len > 0) {
                sstr_truncate(search_buf, search_buf->len - 1);
                search_pos = hist->count; /* Reset search position. */
            }
        } else if (c >= 32) {
            /* Printable character: add to search query. */
            sstr_append_char(search_buf, c);
            /* Don't reset search_pos so we search from current position. */
        } else {
            /* Any other control char: cancel search and inject the char back
             * by accepting match and processing the character in the main loop.
             * For simplicity, just accept the match. */
            if (match[0] != '\0') {
                sstr_set(ed->buf, match);
                ed->cursor = (int)ed->buf->len;
            }
            sstr_free(search_buf);
            term_puts("\r\x1b[0K");
            refresh_line(ed, prompt);
            return;
        }
    }

    sstr_free(search_buf);
    term_puts("\r\x1b[0K");
    refresh_line(ed, prompt);
}

/* --------------------------------------------------------------------------
 * Escape sequence handling
 * -------------------------------------------------------------------------- */

static void handle_escape(LineEditor *ed, const char *prompt)
{
    char seq[4];

    /* Read the next character after ESC. */
    if (term_read_char(&seq[0]) <= 0)
        return;

    if (seq[0] == '[') {
        /* CSI sequence: ESC [ ... */
        if (term_read_char(&seq[1]) <= 0)
            return;

        if (seq[1] >= '0' && seq[1] <= '9') {
            /* Extended sequence like ESC [ 3 ~ */
            if (term_read_char(&seq[2]) <= 0)
                return;
            if (seq[2] == '~') {
                switch (seq[1]) {
                case '1': /* Home */
                    ed->cursor = 0;
                    break;
                case '3': /* Delete */
                    if (ed->cursor < (int)ed->buf->len)
                        sstr_delete(ed->buf, (size_t)ed->cursor, 1);
                    break;
                case '4': /* End */
                    ed->cursor = (int)ed->buf->len;
                    break;
                }
            }
        } else {
            switch (seq[1]) {
            case 'A': /* Up arrow */
                history_nav_up(ed, prompt);
                return; /* refresh already done */
            case 'B': /* Down arrow */
                history_nav_down(ed, prompt);
                return; /* refresh already done */
            case 'C': /* Right arrow */
                if (ed->cursor < (int)ed->buf->len)
                    ed->cursor++;
                break;
            case 'D': /* Left arrow */
                if (ed->cursor > 0)
                    ed->cursor--;
                break;
            case 'H': /* Home */
                ed->cursor = 0;
                break;
            case 'F': /* End */
                ed->cursor = (int)ed->buf->len;
                break;
            }
        }
    } else if (seq[0] == 'O') {
        /* SS3 sequence: ESC O ... (some terminals use this for Home/End) */
        if (term_read_char(&seq[1]) <= 0)
            return;
        switch (seq[1]) {
        case 'H': /* Home */
            ed->cursor = 0;
            break;
        case 'F': /* End */
            ed->cursor = (int)ed->buf->len;
            break;
        }
    } else if (seq[0] == 'b') {
        /* Alt+B: move back one word */
        move_word_back(ed);
    } else if (seq[0] == 'f') {
        /* Alt+F: move forward one word */
        move_word_forward(ed);
    } else if (seq[0] == 'd') {
        /* Alt+D: kill word forward */
        kill_word_forward(ed);
    }
    /* Other ESC sequences are silently ignored. */

    refresh_line(ed, prompt);
}

/* --------------------------------------------------------------------------
 * Main readline function
 * -------------------------------------------------------------------------- */

char *vsh_readline(Shell *shell, const char *prompt)
{
    /* Initialize the editor state. */
    LineEditor ed;
    memset(&ed, 0, sizeof(ed));
    ed.buf        = sstr_new(256);
    ed.cursor     = 0;
    ed.prompt_len = (int)strlen(prompt); /* Assumes no escape sequences in prompt width calc - may need refinement */
    ed.yank_buf   = s_yank_buf;
    ed.shell      = shell;
    ed.searching  = false;
    ed.search_buf = NULL;
    ed.search_pos = 0;

    /* Reset history navigation position for this new prompt. */
    if (shell->history)
        history_reset_nav(shell->history);

    /* Clear the saved-line buffer for fresh history navigation. */
    if (s_saved_line)
        sstr_clear(s_saved_line);
    else
        s_saved_line = sstr_new(64);

    /* Write prompt to terminal. */
    term_puts(prompt);

    /* Main input loop. */
    for (;;) {
        char c;
        int rc = term_read_char(&c);

        if (rc <= 0) {
            /* EOF or error. */
            sstr_free(ed.buf);
            return NULL;
        }

        switch (c) {

        /* ---- Enter ---- */
        case 13:
        case 10: {
            term_puts("\r\n");
            char *result = NULL;
            if (ed.buf->len > 0) {
                result = strdup(sstr_cstr(ed.buf));
                /* Add to history if non-empty. */
                if (shell->history)
                    history_add(shell->history, result);
            } else {
                result = strdup("");
            }
            sstr_free(ed.buf);
            return result;
        }

        /* ---- Ctrl+A: beginning of line ---- */
        case 1:
            ed.cursor = 0;
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+B: move left ---- */
        case 2:
            if (ed.cursor > 0)
                ed.cursor--;
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+C: cancel line ---- */
        case 3:
            term_puts("^C\r\n");
            sstr_clear(ed.buf);
            ed.cursor = 0;
            /* Print fresh prompt. */
            term_puts(prompt);
            break;

        /* ---- Ctrl+D: EOF or delete ---- */
        case 4:
            if (ed.buf->len == 0) {
                /* EOF on empty line. */
                sstr_free(ed.buf);
                return NULL;
            }
            /* Delete char at cursor. */
            if (ed.cursor < (int)ed.buf->len)
                sstr_delete(ed.buf, (size_t)ed.cursor, 1);
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+E: end of line ---- */
        case 5:
            ed.cursor = (int)ed.buf->len;
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+F: move right ---- */
        case 6:
            if (ed.cursor < (int)ed.buf->len)
                ed.cursor++;
            refresh_line(&ed, prompt);
            break;

        /* ---- Tab ---- */
        case 9:
            handle_tab(&ed, prompt);
            break;

        /* ---- Ctrl+K: kill to end ---- */
        case 11:
            kill_to_end(&ed);
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+L: clear screen ---- */
        case 12:
            term_puts("\x1b[H\x1b[2J");
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+R: reverse search ---- */
        case 18:
            reverse_search(&ed, prompt);
            break;

        /* ---- Ctrl+U: kill to start ---- */
        case 21:
            kill_to_start(&ed);
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+W: kill previous word ---- */
        case 23:
            kill_prev_word(&ed);
            refresh_line(&ed, prompt);
            break;

        /* ---- Ctrl+Y: yank ---- */
        case 25:
            yank(&ed);
            refresh_line(&ed, prompt);
            break;

        /* ---- Escape: start of escape sequence ---- */
        case 27:
            handle_escape(&ed, prompt);
            break;

        /* ---- Backspace ---- */
        case 127:
        case 8:
            if (ed.cursor > 0) {
                ed.cursor--;
                sstr_delete(ed.buf, (size_t)ed.cursor, 1);
            }
            refresh_line(&ed, prompt);
            break;

        /* ---- Printable characters ---- */
        default:
            if ((unsigned char)c >= 32) {
                sstr_insert_char(ed.buf, (size_t)ed.cursor, c);
                ed.cursor++;
                refresh_line(&ed, prompt);
            }
            /* Ignore other control characters. */
            break;
        }
    }
}
