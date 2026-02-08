/* ============================================================================
 * vsh - Vanguard Shell
 * history.h - Command history with persistence
 * ============================================================================ */

#ifndef VSH_HISTORY_H
#define VSH_HISTORY_H

#include <stdbool.h>
#include <stddef.h>

#define HISTORY_MAX_SIZE 10000
#define HISTORY_FILE     ".vsh_history"

typedef struct HistoryEntry {
    char *line;
    int   index;     /* Global history index */
} HistoryEntry;

typedef struct History {
    HistoryEntry *entries;
    int           count;      /* Number of entries */
    int           capacity;   /* Max entries */
    int           pos;        /* Current navigation position (for up/down) */
    int           next_index; /* Next global index */
} History;

/* Create a new history */
History *history_create(int capacity);

/* Destroy history */
void history_destroy(History *hist);

/* Add a line to history (skips duplicates of previous entry, skips blank) */
void history_add(History *hist, const char *line);

/* Get entry at position (0 = oldest) */
const char *history_get(History *hist, int pos);

/* Get entry by global index (for !N expansion) */
const char *history_get_by_index(History *hist, int index);

/* Get the most recent entry */
const char *history_last(History *hist);

/* Navigate: move position up (older). Returns entry or NULL. */
const char *history_navigate_up(History *hist);

/* Navigate: move position down (newer). Returns entry or NULL at end. */
const char *history_navigate_down(History *hist);

/* Reset navigation position (call when a new prompt starts) */
void history_reset_nav(History *hist);

/* Search backwards for a line starting with prefix */
const char *history_search_prefix(History *hist, const char *prefix);

/* Search backwards for a line containing substring */
const char *history_search_substr(History *hist, const char *substr, int *out_pos);

/* Load history from file */
void history_load(History *hist, const char *path);

/* Save history to file */
void history_save(const History *hist, const char *path);

/* Clear all history */
void history_clear(History *hist);

/* Get the current count */
int history_count(const History *hist);

#endif /* VSH_HISTORY_H */
