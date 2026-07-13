/* ============================================================================
 * vsh - Vanguard Shell
 * test_audit.c - Tamper-evident audit-log hash-chain tests
 * ============================================================================ */

#include "audit.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Read an entire file into a heap buffer (NUL-terminated); *len gets size. */
static char *slurp(const char *path, long *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, fp);
    buf[got] = '\0';
    fclose(fp);
    if (len) *len = (long)got;
    return buf;
}

static void spew(const char *path, const char *data, long len)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(data, 1, (size_t)len, fp);
    fclose(fp);
}

void test_audit(void)
{
    printf("\n--- Audit log ---\n");

    char path[256];
    snprintf(path, sizeof(path), "/tmp/vsh_audit_test_%d.log", (int)getpid());
    unlink(path);

    /* ---- Record a short chain ------------------------------------------- */
    AuditLog *log = audit_open(path);
    ASSERT_TRUE(log != NULL);
    ASSERT_EQ(log->count, 0L);
    ASSERT_EQ(audit_record(log, 1000, "echo alpha", 0), 0);
    ASSERT_EQ(audit_record(log, 1000, "ls -la", 0), 0);
    ASSERT_EQ(audit_record(log, 1000, "grep beta file", 1), 0);
    ASSERT_EQ(log->count, 3L);
    audit_close(log);

    /* ---- A pristine chain verifies clean -------------------------------- */
    {
        long n = 0, bad = -1;
        AuditStatus st = audit_verify(path, &n, &bad);
        ASSERT_EQ((int)st, (int)AUDIT_OK);
        ASSERT_EQ(n, 3L);
    }

    /* ---- Reopening recovers the head and count so appends continue ------ */
    {
        AuditLog *log2 = audit_open(path);
        ASSERT_TRUE(log2 != NULL);
        ASSERT_EQ(log2->count, 3L);
        ASSERT_EQ(audit_record(log2, 1000, "date", 0), 0);
        audit_close(log2);

        long n = 0, bad = -1;
        AuditStatus st = audit_verify(path, &n, &bad);
        ASSERT_EQ((int)st, (int)AUDIT_OK);
        ASSERT_EQ(n, 4L);
    }

    /* ---- Tampering with a past command is detected ---------------------- */
    {
        long len = 0;
        char *data = slurp(path, &len);
        ASSERT_TRUE(data != NULL);
        if (data) {
            /* Flip one character inside the second entry's command field. */
            char *hit = strstr(data, "ls -la");
            ASSERT_TRUE(hit != NULL);
            if (hit) hit[0] = 'X';       /* "ls -la" -> "Xs -la" */
            spew(path, data, len);
            free(data);

            long n = 0, bad = -1;
            AuditStatus st = audit_verify(path, &n, &bad);
            ASSERT_EQ((int)st, (int)AUDIT_TAMPERED);
            /* The break must be reported at the edited entry (line 2). */
            ASSERT_EQ(bad, 2L);
        }
    }

    /* ---- A structurally broken line is reported as malformed ------------ */
    {
        spew(path, "not-a-valid-record\n", 19);
        long n = 0, bad = -1;
        AuditStatus st = audit_verify(path, &n, &bad);
        ASSERT_EQ((int)st, (int)AUDIT_MALFORMED);
        ASSERT_EQ(bad, 1L);
    }

    /* ---- Chaining: a different first command yields a different head ----- *
     * Two independent logs whose first entries differ must diverge, proving
     * each hash actually commits to the command content. */
    {
        char p1[256], p2[256];
        snprintf(p1, sizeof(p1), "/tmp/vsh_audit_a_%d.log", (int)getpid());
        snprintf(p2, sizeof(p2), "/tmp/vsh_audit_b_%d.log", (int)getpid());
        unlink(p1); unlink(p2);

        AuditLog *a = audit_open(p1);
        AuditLog *b = audit_open(p2);
        ASSERT_TRUE(a && b);
        if (a && b) {
            audit_record(a, 0, "whoami", 0);
            audit_record(b, 0, "whoareyou", 0);
            ASSERT_TRUE(memcmp(a->head, b->head, sizeof(a->head)) != 0);
        }
        audit_close(a);
        audit_close(b);
        unlink(p1); unlink(p2);
    }

    unlink(path);
    printf("  Audit log tests complete\n");
}
