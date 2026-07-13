/* ============================================================================
 * vsh - Vanguard Shell
 * test_sha256.c - SHA-256 tests (pinned against NIST/FIPS 180-4 vectors)
 * ============================================================================ */

#include "sha256.h"
#include "test.h"

#include <string.h>
#include <stdlib.h>

/* Compare two hex digests.  Wrapping the comparison in a helper (taking
 * pointers, not arrays) keeps -Waddress quiet inside the ASSERT macros. */
static int hex_eq(const char *got, const char *want)
{
    return got && want && strcmp(got, want) == 0;
}

/* Hash a C string and return its hex digest in `out` (65 bytes). */
static void hash_str(const char *s, char out[SHA256_HEX_SIZE])
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256(s, strlen(s), digest);
    sha256_hex(digest, out);
}

void test_sha256(void)
{
    printf("\n--- SHA-256 ---\n");
    char hex[SHA256_HEX_SIZE];

    /* Empty string (FIPS 180-4 well-known vector) */
    hash_str("", hex);
    ASSERT_TRUE(hex_eq(hex,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    /* "abc" (FIPS 180-4 Appendix B.1) */
    hash_str("abc", hex);
    ASSERT_TRUE(hex_eq(hex,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    /* 448-bit message (FIPS 180-4 Appendix B.2) */
    hash_str("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", hex);
    ASSERT_TRUE(hex_eq(hex,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    /* Standard "hello world" vector */
    hash_str("hello world", hex);
    ASSERT_TRUE(hex_eq(hex,
        "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"));

    /* Streaming update must equal the one-shot digest.  Feed "abc" in three
     * separate single-byte updates and compare. */
    {
        SHA256_Ctx ctx;
        uint8_t d1[SHA256_DIGEST_SIZE], d2[SHA256_DIGEST_SIZE];
        char h1[SHA256_HEX_SIZE], h2[SHA256_HEX_SIZE];

        sha256_init(&ctx);
        sha256_update(&ctx, "a", 1);
        sha256_update(&ctx, "b", 1);
        sha256_update(&ctx, "c", 1);
        sha256_final(&ctx, d1);
        sha256_hex(d1, h1);

        sha256("abc", 3, d2);
        sha256_hex(d2, h2);

        ASSERT_TRUE(hex_eq(h1, h2));
    }

    /* Multi-block input (one million 'a' characters, FIPS 180-4 Appendix B.3).
     * Exercises the block-boundary buffering logic across many transforms. */
    {
        SHA256_Ctx ctx;
        uint8_t digest[SHA256_DIGEST_SIZE];
        char *buf = malloc(1000);
        ASSERT_TRUE(buf != NULL);
        if (buf) {
            memset(buf, 'a', 1000);
            sha256_init(&ctx);
            for (int i = 0; i < 1000; i++)
                sha256_update(&ctx, buf, 1000);   /* 1,000,000 'a' total */
            sha256_final(&ctx, digest);
            sha256_hex(digest, hex);
            ASSERT_TRUE(hex_eq(hex,
                "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
            free(buf);
        }
    }

    /* Avalanche: a one-bit change must produce a completely different digest. */
    {
        char a[SHA256_HEX_SIZE], b[SHA256_HEX_SIZE];
        hash_str("The quick brown fox jumps over the lazy dog", a);
        hash_str("The quick brown fox jumps over the lazy dog.", b);
        ASSERT_TRUE(!hex_eq(a, b));
        ASSERT_TRUE(hex_eq(a,
            "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592"));
    }

    printf("  SHA-256 tests complete\n");
}
