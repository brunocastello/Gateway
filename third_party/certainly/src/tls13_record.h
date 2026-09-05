/*
 * tls13_record.h — TLS 1.3 record encryption/decryption
 *
 * Handles the TLS 1.3 record format:
 * - Outer content type always 0x17 (application_data)
 * - Real content type hidden as last byte of encrypted payload
 * - Nonce = IV XOR sequence_number (no explicit nonce in record)
 * - AAD = outer header (type + version + ciphertext length)
 */

#ifndef CERTAINLY_TLS13_RECORD_H
#define CERTAINLY_TLS13_RECORD_H

#include <bearssl.h>
#include <stdint.h>
#include <stddef.h>

/* TLS record content types */
#define TLS13_CT_CHANGE_CIPHER_SPEC  20
#define TLS13_CT_ALERT               21
#define TLS13_CT_HANDSHAKE           22
#define TLS13_CT_APPLICATION_DATA    23

/* AEAD tag size (both AES-GCM and ChaCha20-Poly1305 use 16 bytes) */
#define TLS13_TAG_SIZE  16

/*
 * Record encryption/decryption context.
 * One per direction (read and write), each with its own key, IV,
 * and sequence counter.
 */
typedef struct {
    unsigned char key[32];     /* encryption key (16 or 32 bytes) */
    size_t        key_len;     /* 16 for AES-128, 32 for AES-256/ChaCha20 */
    unsigned char iv[12];      /* base IV */
    uint64_t      seq;         /* sequence number (starts at 0, increments) */
    uint16_t      cipher_suite; /* which cipher to use */
} tls13_record_ctx;

/* Initialize a record context with key, IV, and cipher suite */
void tls13_record_init(tls13_record_ctx *ctx,
                       const void *key, size_t key_len,
                       const void *iv,
                       uint16_t cipher_suite);

/*
 * Encrypt a record for sending.
 *
 * plaintext: the data to encrypt
 * pt_len:    length of plaintext
 * ct:        content type (e.g., TLS13_CT_HANDSHAKE or TLS13_CT_APPLICATION_DATA)
 * out:       output buffer (must be at least pt_len + 1 + TLS13_TAG_SIZE)
 * out_len:   receives the ciphertext length
 *
 * The content type byte is appended to the plaintext before encryption.
 * Returns 0 on success, -1 on error.
 */
int tls13_record_encrypt(tls13_record_ctx *ctx,
                         const void *plaintext, size_t pt_len,
                         uint8_t ct,
                         void *out, size_t *out_len);

/*
 * Decrypt a received record.
 *
 * ciphertext: the encrypted payload (after the 5-byte record header)
 * ct_len:     length of ciphertext (includes content type byte + tag)
 * out:        output buffer (must be at least ct_len)
 * out_len:    receives the plaintext length (excluding content type)
 * out_ct:     receives the real content type (extracted from decrypted payload)
 *
 * Returns 0 on success, -1 on decryption failure (bad MAC).
 */
int tls13_record_decrypt(tls13_record_ctx *ctx,
                         const void *ciphertext, size_t ct_len,
                         void *out, size_t *out_len,
                         uint8_t *out_ct);

#endif /* CERTAINLY_TLS13_RECORD_H */
