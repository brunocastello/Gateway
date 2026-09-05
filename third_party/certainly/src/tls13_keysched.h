/*
 * tls13_keysched.h — TLS 1.3 key schedule (RFC 8446 Section 7.1)
 *
 * Takes the raw Diffie-Hellman shared secret and derives all the
 * encryption keys needed for the TLS 1.3 handshake and application data.
 *
 * Uses BearSSL's HKDF implementation internally.
 */

#ifndef CERTAINLY_TLS13_KEYSCHED_H
#define CERTAINLY_TLS13_KEYSCHED_H

#include <bearssl.h>
#include <stddef.h>

/*
 * Key schedule context. Tracks the current secret as it progresses
 * through Early Secret → Handshake Secret → Master Secret.
 */
typedef struct {
    const br_hash_class *hash;       /* SHA-256 or SHA-384 */
    size_t              hash_len;    /* 32 or 48 */
    unsigned char       secret[64];  /* current secret (max hash len) */
    unsigned char       empty_hash[64]; /* precomputed Hash("") */
} tls13_keysched;

/* Initialize the key schedule for a given hash function.
 * Precomputes Hash("") which is used in the Derive-Secret("derived","")
 * steps between each Extract. */
void tls13_ks_init(tls13_keysched *ks, const br_hash_class *hash);

/* Step 1: Extract Early Secret from PSK (or zeros if no PSK).
 * For our implementation, always uses zeros (no PSK support). */
void tls13_ks_extract_early(tls13_keysched *ks);

/* Step 2: Derive-Secret(early, "derived", "") then
 * Extract Handshake Secret from the DH shared secret.
 * Call this after completing the X25519 key exchange. */
void tls13_ks_extract_handshake(tls13_keysched *ks,
                                const void *shared_secret, size_t len);

/* Derive client and server handshake traffic keys + IVs.
 * transcript_hash is Hash(ClientHello || ServerHello).
 * key_len is the AEAD key size (16 for AES-128, 32 for AES-256/ChaCha20).
 * If client_secret_out / server_secret_out are non-NULL, the raw traffic
 * secrets are copied there (needed later for Finished key derivation). */
void tls13_ks_derive_handshake_keys(tls13_keysched *ks,
                                    const void *transcript_hash,
                                    size_t key_len,
                                    void *client_key, void *client_iv,
                                    void *server_key, void *server_iv,
                                    void *client_secret_out,
                                    void *server_secret_out);

/* Step 3: Derive-Secret(handshake, "derived", "") then
 * Extract Master Secret (using zeros as input key material). */
void tls13_ks_extract_master(tls13_keysched *ks);

/* Derive client and server application traffic keys + IVs.
 * transcript_hash is Hash(ClientHello || ... || server Finished).
 * key_len is the AEAD key size (16 for AES-128, 32 for AES-256/ChaCha20). */
void tls13_ks_derive_app_keys(tls13_keysched *ks,
                              const void *transcript_hash,
                              size_t key_len,
                              void *client_key, void *client_iv,
                              void *server_key, void *server_iv);

/* Derive the Finished verification key from a traffic secret.
 * Used to compute/verify the Finished HMAC. */
void tls13_ks_derive_finished_key(tls13_keysched *ks,
                                  const void *base_key,
                                  void *finished_key);

#endif /* CERTAINLY_TLS13_KEYSCHED_H */
