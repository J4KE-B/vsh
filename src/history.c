/* ============================================================================
 * vsh - Vanguard Shell
 * history.c - Command history with persistence
 *
 * Maintains a fixed-capacity array of history entries.  Navigation (up/down)
 * uses a separate position cursor that is reset whenever a new prompt starts.
 * History is persisted to ~/.vsh_history via simple line-per-entry text files.
 * ============================================================================ */

#include "history.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/* ---- Internal helpers --------------------------------------------------- */

/* Expand a leading ~ to $HOME.  Returns a malloc'd string that the caller
 * must free, or NULL on error. */
static char *expand_tilde(const char *path)
{
    if (!path)
        return NULL;

    if (path[0] != '~')
        return strdup(path);

    const char *home = getenv("HOME");
    if (!home)
        home = "/";

    size_t hlen = strlen(home);
    size_t plen = strlen(path + 1);   /* everything after ~ */
    char *out = malloc(hlen + plen + 1);
    if (!out)
        return NULL;

    memcpy(out, home, hlen);
    memcpy(out + hlen, path + 1, plen + 1);  /* includes '\0' */
    return out;
}

/* Return true if line is blank or whitespace-only */
static bool is_blank(const char *line)
{
    if (!line)
        return true;
    for (const char *p = line; *p; p++) {
        if (!isspace((unsigned char)*p))
            return false;
    }
    return true;
}

/* ---- Public API --------------------------------------------------------- */

History *history_create(int capacity)
{
    if (capacity <= 0)
        capacity = HISTORY_MAX_SIZE;

    History *hist = malloc(sizeof(History));
    if (!hist)
        return NULL;

    hist->entries = calloc((size_t)capacity, sizeof(HistoryEntry));
    if (!hist->entries) {
        free(hist);
        return NULL;
    }

    hist->count      = 0;
    hist->capacity   = capacity;
    hist->pos        = 0;
    hist->next_index = 1;
    return hist;
}

void history_destroy(History *hist)
{
    if (!hist)
        return;

    for (int i = 0; i < hist->count; i++)
        free(hist->entries[i].line);

    free(hist->entries);
    free(hist);
}

void history_add(History *hist, const char *line)
{
    if (!hist || !line)
        return;

    /* Skip blank lines */
    if (is_blank(line))
        return;

    /* Skip duplicate of the previous entry */
    if (hist->count > 0 &&
        strcmp(hist->entries[hist->count - 1].line, line) == 0)
        return;

    /* If at capacity, drop the oldest entry and shift left */
    if (hist->count == hist->capacity) {
        free(hist->entries[0].line);
        memmove(&hist->entries[0], &hist->entries[1],
                (size_t)(hist->capacity - 1) * sizeof(HistoryEntry));
        hist->count--;
    }

    /* Append the new entry */
    hist->entries[hist->count].line  = strdup(line);
    hist->entries[hist->count].index = hist->next_index++;
    hist->count++;

    /* Reset navigation */
    history_reset_nav(hist);
}

const char *history_get(History *hist, int pos)
{
    if (!hist || pos < 0 || pos >= hist->count)
        return NULL;
    return hist->entries[pos].line;
}

const char *history_get_by_index(History *hist, int index)
{
    if (!hist)
        return NULL;

    /* Scan from the end (most likely to be recent) */
    for (int i = hist->count - 1; i >= 0; i--) {
        if (hist->entries[i].index == index)
            return hist->entries[i].line;
    }
    return NULL;
}

const char *history_last(History *hist)
{
    if (!hist || hist->count == 0)
        return NULL;
    return hist->entries[hist->count - 1].line;
}

const char *history_navigate_up(History *hist)
{
    if (!hist || hist->count == 0)
        return NULL;

    if (hist->pos > 0)
        hist->pos--;

    return hist->entries[hist->pos].line;
}

const char *history_navigate_down(History *hist)
{
    if (!hist)
        return NULL;

    hist->pos++;

    if (hist->pos >= hist->count) {
        hist->pos = hist->count;  /* clamp to one-past-end */
        return NULL;               /* back to current input */
    }

    return hist->entries[hist->pos].line;
}

void history_reset_nav(History *hist)
{
    if (!hist)
        return;
    hist->pos = hist->count;  /* one past end = no history selected */
}

const char *history_search_prefix(History *hist, const char *prefix)
{
    if (!hist || !prefix)
        return NULL;

    size_t plen = strlen(prefix);
    for (int i = hist->count - 1; i >= 0; i--) {
        if (strncmp(hist->entries[i].line, prefix, plen) == 0)
            return hist->entries[i].line;
    }
    return NULL;
}

const char *history_search_substr(History *hist, const char *substr, int *out_pos)
{
    if (!hist || !substr)
        return NULL;

    /* Start searching backwards from one before the current nav position */
    int start = hist->pos - 1;
    if (start < 0 || start >= hist->count)
        start = hist->count - 1;

    for (int i = start; i >= 0; i--) {
        if (strstr(hist->entries[i].line, substr)) {
            if (out_pos)
                *out_pos = i;
            hist->pos = i;
            return hist->entries[i].line;
        }
    }
    return NULL;
}

void history_load(History *hist, const char *path)
{
    if (!hist || !path)
        return;

    char *expanded = expand_tilde(path);
    if (!expanded)
        return;

    FILE *fp = fopen(expanded, "r");
    free(expanded);
    if (!fp)
        return;

    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) {
        /* Strip trailing newline */
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';

        if (len > 0)
            history_add(hist, buf);
    }

    fclose(fp);
}

void history_save(const History *hist, const char *path)
{
    if (!hist || !path)
        return;

    char *expanded = expand_tilde(path);
    if (!expanded)
        return;

    FILE *fp = fopen(expanded, "w");
    free(expanded);
    if (!fp)
        return;

    for (int i = 0; i < hist->count; i++)
        fprintf(fp, "%s\n", hist->entries[i].line);

    fclose(fp);
}

void history_clear(History *hist)
{
    if (!hist)
        return;

    for (int i = 0; i < hist->count; i++) {
        free(hist->entries[i].line);
        hist->entries[i].line  = NULL;
        hist->entries[i].index = 0;
    }

    hist->count = 0;
    history_reset_nav(hist);
}

int history_count(const History *hist)
{
    if (!hist)
        return 0;
    return hist->count;
}
