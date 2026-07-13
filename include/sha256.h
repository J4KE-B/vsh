/* ============================================================================
 * vsh - Vanguard Shell
 * sha256.h - SHA-256 cryptographic hash (FIPS 180-4)
 *
 * A from-scratch, dependency-free implementation of SHA-256.  Used by the
 * tamper-evident audit log to build a hash chain over executed commands.
 * No OpenSSL, no libcrypto -- vsh links zero third-party libraries.
 * ============================================================================ */

#ifndef VSH_SHA256_H
#define VSH_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32   /* Raw digest length in bytes           */
#define SHA256_HEX_SIZE    65   /* Hex string length incl. NUL terminator */
#define SHA256_BLOCK_SIZE  64   /* Compression block size in bytes      */

/* Streaming hash state */
typedef struct SHA256_Ctx {
    uint32_t state[8];               /* Intermediate hash values (H0..H7) */
    uint64_t bitlen;                 /* Total message length in bits      */
    uint8_t  buffer[SHA256_BLOCK_SIZE];
    size_t   buflen;                 /* Bytes currently buffered          */
} SHA256_Ctx;

/* Initialize a streaming context */
void sha256_init(SHA256_Ctx *ctx);

/* Feed `len` bytes of `data` into the context */
void sha256_update(SHA256_Ctx *ctx, const void *data, size_t len);

/* Finalize and write the 32-byte digest to `out` (ctx is consumed) */
void sha256_final(SHA256_Ctx *ctx, uint8_t out[SHA256_DIGEST_SIZE]);

/* One-shot convenience: hash `len` bytes of `data` into `out` */
void sha256(const void *data, size_t len, uint8_t out[SHA256_DIGEST_SIZE]);

/* Format a raw digest as a lowercase hex string (writes 65 bytes incl. NUL) */
void sha256_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                char out[SHA256_HEX_SIZE]);

#endif /* VSH_SHA256_H */
