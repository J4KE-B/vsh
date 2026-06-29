/* ============================================================================
 * vsh - Vanguard Shell
 * audit.c - Tamper-evident audit log (SHA-256 hash chain)
 *
 * On-disk format: one tab-separated record per line
 *
 *     seq <TAB> ts <TAB> uid <TAB> result <TAB> entry_hash_hex <TAB> command
 *
 * The command field comes last so it may itself contain tab or space
 * characters; only newlines are forbidden (they are stripped on write).  Each
 * entry_hash commits to the previous entry's hash, forming the chain.
 * ============================================================================ */

#include "audit.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

/* The genesis "previous hash" is 64 zero hex digits. */
static const char GENESIS_HEX[SHA256_HEX_SIZE] =
    "0000000000000000000000000000000000000000000000000000000000000000";

/* ---- Canonical entry hashing -------------------------------------------- *
 * digest = SHA256( prev_hex "\n" ts "\n" uid "\n" result "\n" command )     */
static void hash_entry(const char *prev_hex, long ts, unsigned long uid,
                       int result, const char *command,
                       uint8_t out[SHA256_DIGEST_SIZE])
{
    SHA256_Ctx ctx;
    char meta[96];
    int n;

    sha256_init(&ctx);
    sha256_update(&ctx, prev_hex, strlen(prev_hex));
    n = snprintf(meta, sizeof(meta), "\n%ld\n%lu\n%d\n", ts, uid, result);
    sha256_update(&ctx, meta, (size_t)n);
    sha256_update(&ctx, command, strlen(command));
    sha256_final(&ctx, out);
}

/* Copy `command` into `dst` (size `cap`), replacing newlines/CRs with spaces
 * so a single logical record never spans multiple physical lines. */
static void sanitize(char *dst, size_t cap, const char *command)
{
    size_t i = 0;
    for (; command[i] != '\0' && i + 1 < cap; i++) {
        char c = command[i];
        dst[i] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    dst[i] = '\0';
}

/* ---- Line parsing ------------------------------------------------------- *
 * Locate the six fields of a record.  Returns 0 on success and fills the out
 * pointers (into the same buffer, which is modified in place by writing NULs
 * at the field-separating tabs).  The command pointer covers the remainder. */
static int parse_line(char *line, long *ts, unsigned long *uid, int *result,
                      const char **hex, const char **command)
{
    char *fields[5];
    char *p = line;
    int i;

    for (i = 0; i < 5; i++) {
        char *tab = strchr(p, '\t');
        if (!tab) return -1;
        *tab = '\0';
        fields[i] = p;
        p = tab + 1;
    }

    *ts      = strtol(fields[1], NULL, 10);
    *uid     = strtoul(fields[2], NULL, 10);
    *result  = (int)strtol(fields[3], NULL, 10);
    *hex     = fields[4];
    *command = p;                 /* remainder of the line */
    return 0;
}

/* ---- Public API --------------------------------------------------------- */

AuditLog *audit_open(const char *path)
{
    if (!path) return NULL;

    AuditLog *log = calloc(1, sizeof(AuditLog));
    if (!log) return NULL;

    size_t plen = strlen(path);
    if (plen + 1 > sizeof(log->path)) {
        free(log);
        return NULL;
    }
    memcpy(log->path, path, plen + 1);
    log->count = 0;
    memset(log->head, 0, sizeof(log->head));

    /* Refuse anything that is not a regular file. Without this, a caller who controls
     * $VSH_AUDIT_LOG can point it at /dev/null (or any character device) and the sandbox runs with
     * every audit write silently discarded -- "an unauditable sandbox is not a sandbox". A symlink
     * or device here is treated as hostile. Non-existence is fine; we create it 0600 on first write. */
    {
        struct stat st;
        if (lstat(path, &st) == 0 && !S_ISREG(st.st_mode)) {
            free(log);
            return NULL;
        }
    }

    /* Recover the chain head and entry count from any existing log. */
    FILE *fp = fopen(path, "r");
    if (fp) {
        char   *line = NULL;
        size_t  cap  = 0;
        ssize_t len;
        while ((len = getline(&line, &cap, fp)) != -1) {
            if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
            if (line[0] == '\0') continue;

            long ts; unsigned long uid; int result;
            const char *hex, *command;
            if (parse_line(line, &ts, &uid, &result, &hex, &command) == 0 &&
                strlen(hex) == SHA256_DIGEST_SIZE * 2) {
                /* Decode the stored hex into the running head. */
                for (int b = 0; b < SHA256_DIGEST_SIZE; b++) {
                    unsigned v;
                    sscanf(hex + b * 2, "%2x", &v);
                    log->head[b] = (uint8_t)v;
                }
                log->count++;
            }
        }
        free(line);
        fclose(fp);
    }

    return log;
}

int audit_record(AuditLog *log, uid_t uid, const char *command, int result)
{
    if (!log || !command) return -1;

    char prev_hex[SHA256_HEX_SIZE];
    char curr_hex[SHA256_HEX_SIZE];
    char safe[4096];
    uint8_t digest[SHA256_DIGEST_SIZE];
    long ts = (long)time(NULL);

    /* Previous hash as hex (genesis when the chain is empty). */
    if (log->count == 0)
        memcpy(prev_hex, GENESIS_HEX, sizeof(prev_hex));
    else
        sha256_hex(log->head, prev_hex);

    sanitize(safe, sizeof(safe), command);
    hash_entry(prev_hex, ts, (unsigned long)uid, result, safe, digest);
    sha256_hex(digest, curr_hex);

    /* Open O_CREAT|O_APPEND with mode 0600 so the command log is never world-readable, and
     * O_NOFOLLOW so a pre-planted symlink at the path is not followed to clobber another file. */
    int fd = open(log->path, O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    FILE *fp = fdopen(fd, "a");
    if (!fp) { close(fd); return -1; }

    if (fprintf(fp, "%ld\t%ld\t%lu\t%d\t%s\t%s\n",
                log->count + 1, ts, (unsigned long)uid, result,
                curr_hex, safe) < 0) {
        fclose(fp);
        return -1;
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    memcpy(log->head, digest, sizeof(log->head));
    log->count++;
    return 0;
}

void audit_close(AuditLog *log)
{
    free(log);
}

AuditStatus audit_verify(const char *path, long *n_entries, long *bad_line)
{
    if (n_entries) *n_entries = 0;
    if (bad_line)  *bad_line  = 0;
    if (!path) return AUDIT_IO_ERROR;

    FILE *fp = fopen(path, "r");
    if (!fp) return AUDIT_IO_ERROR;

    char prev_hex[SHA256_HEX_SIZE];
    memcpy(prev_hex, GENESIS_HEX, sizeof(prev_hex));

    char   *line = NULL;
    size_t  cap  = 0;
    ssize_t len;
    long    lineno = 0;
    AuditStatus st = AUDIT_OK;

    while ((len = getline(&line, &cap, fp)) != -1) {
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (line[0] == '\0') continue;
        lineno++;

        long ts; unsigned long uid; int result;
        const char *hex, *command;
        if (parse_line(line, &ts, &uid, &result, &hex, &command) != 0) {
            st = AUDIT_MALFORMED;
            if (bad_line) *bad_line = lineno;
            break;
        }

        uint8_t digest[SHA256_DIGEST_SIZE];
        char    computed[SHA256_HEX_SIZE];
        hash_entry(prev_hex, ts, uid, result, command, digest);
        sha256_hex(digest, computed);

        if (strcmp(computed, hex) != 0) {
            st = AUDIT_TAMPERED;
            if (bad_line) *bad_line = lineno;
            break;
        }

        memcpy(prev_hex, computed, sizeof(prev_hex));
    }

    free(line);
    fclose(fp);

    if (n_entries) *n_entries = lineno;
    return st;
}

const char *audit_status_str(AuditStatus st)
{
    switch (st) {
    case AUDIT_OK:        return "intact";
    case AUDIT_TAMPERED:  return "TAMPERED";
    case AUDIT_MALFORMED: return "malformed";
    case AUDIT_IO_ERROR:  return "I/O error";
    }
    return "unknown";
}
