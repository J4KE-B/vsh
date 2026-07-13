/* ============================================================================
 * vsh - Vanguard Shell
 * audit.h - Tamper-evident audit log (SHA-256 hash chain)
 *
 * Every command executed under restricted mode is appended to an audit log as
 * a link in a hash chain:
 *
 *     entry_hash = SHA256( prev_hash_hex || ts || uid || result || command )
 *
 * Because each entry commits to the hash of the entry before it, editing,
 * reordering, or deleting any past entry invalidates every hash from that
 * point forward -- audit_verify() detects the break and names the first bad
 * line.  This is tamper-EVIDENT, not tamper-proof: an attacker with write
 * access can recompute the whole chain.  See vsh-security-defense.md.
 * ============================================================================ */

#ifndef VSH_AUDIT_H
#define VSH_AUDIT_H

#include "sha256.h"

#include <sys/types.h>

typedef struct AuditLog {
    char    path[4096];
    uint8_t head[SHA256_DIGEST_SIZE];  /* Hash of the most recent entry     */
    long    count;                     /* Number of entries in the chain    */
} AuditLog;

/* Verification outcome. */
typedef enum AuditStatus {
    AUDIT_OK = 0,       /* Chain is intact                                   */
    AUDIT_TAMPERED,     /* A stored hash disagrees with recomputation        */
    AUDIT_MALFORMED,    /* A line could not be parsed                        */
    AUDIT_IO_ERROR      /* The log file could not be read                    */
} AuditStatus;

/* Open (creating if absent) the audit log at `path`.  Existing entries are
 * scanned to recover the current chain head and entry count so appends
 * continue the chain.  Returns NULL on error. */
AuditLog *audit_open(const char *path);

/* Append a tamper-evident entry recording that `command` executed with exit
 * status `result` under `uid`.  Returns 0 on success, -1 on I/O error. */
int audit_record(AuditLog *log, uid_t uid, const char *command, int result);

/* Flush, close, and free the log handle. */
void audit_close(AuditLog *log);

/* Recompute the entire chain from genesis and compare against stored hashes.
 * `*n_entries` (if non-NULL) receives the number of entries scanned, and on a
 * non-OK result `*bad_line` (if non-NULL) receives the 1-based offending line.
 */
AuditStatus audit_verify(const char *path, long *n_entries, long *bad_line);

/* Human-readable name for an AuditStatus. */
const char *audit_status_str(AuditStatus st);

#endif /* VSH_AUDIT_H */
