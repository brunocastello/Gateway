/*
 * tls13_keysched.c — TLS 1.3 key schedule (RFC 8446 Section 7.1)
 *
 * The key schedule processes the DH shared secret through three stages:
 *
 *   1. Early Secret     (from PSK, or zeros if no PSK)
 *   2. Handshake Secret (from DH shared secret)
 *   3. Master Secret    (from zeros — just advances the chain)
 *
 * Between each stage, we do Derive-Secret(current, "derived", Hash(""))
 * then HKDF-Extract with the next input. This "derived" step with an
 * empty transcript hash provides domain separation — it ensures that
 * the Early, Handshake, and Master secrets are cryptographically
 * independent even though they're chained together.
 *
 * At each stage, we can derive traffic keys using Derive-Secret with
 * the actual transcript hash. The transcript hash binds the keys to
 * the exact handshake messages exchanged, so any tampering changes
 * the keys and breaks the connection.
 */

#include "tls13_keysched.h"
#include <string.h>

/*
 * HKDF-Expand-Label (RFC 8446 Section 7.1)
 *
 * This is a wrapper around HKDF-Expand that adds TLS 1.3-specific
 * labeling. The label is prefixed with "tls13 " and combined with
 * a context (usually a transcript hash) into a structured info field:
 *
 *   struct {
 *       uint16 length;          // output length
 *       opaque label<7..255>;   // "tls13 " + label
 *       opaque context<0..255>; // usually transcript hash
 *   } HkdfLabel;
 *
 * This structured labeling ensures that keys derived for different
 * purposes (different labels or different transcript states) are
 * always different, even from the same base secret.
 */
static void hkdf_expand_label(
    const br_hash_class *hash,
    const void *secret, size_t secret_len,
    const char *label, size_t label_len,
    const void *context, size_t context_len,
    void *out, size_t out_len)
{
    unsigned char info[512];
    size_t info_len;
    size_t tls_label_len;

    /*
     * Build the HkdfLabel structure:
     *   uint16 length
     *   uint8  label_length
     *   "tls13 " + label
     *   uint8  context_length
     *   context
     */
    tls_label_len = 6 + label_len; /* "tls13 " prefix */

    info[0] = (unsigned char)(out_len >> 8);
    info[1] = (unsigned char)(out_len);
    info[2] = (unsigned char)(tls_label_len);
    memcpy(info + 3, "tls13 ", 6);
    memcpy(info + 9, label, label_len);
    info[9 + label_len] = (unsigned char)(context_len);
    if (context_len > 0) {
        memcpy(info + 10 + label_len, context, context_len);
    }
    info_len = 10 + label_len + context_len;

    /*
     * HKDF-Expand: use the secret as the PRK (pseudorandom key),
     * and our HkdfLabel as the info parameter.
     *
     * BearSSL's HKDF API:
     *   br_hkdf_init — set hash + salt (we use the secret as PRK directly)
     *   br_hkdf_inject — feed the IKM
     *   br_hkdf_flip — switch from Extract to Expand mode
     *   br_hkdf_produce — get output bytes
     *
     * But for Expand-only, we need to set up the PRK directly.
     * BearSSL doesn't have a separate Expand function — its HKDF
     * combines Extract+Expand. We work around this by using the
     * HMAC interface directly for Expand.
     */
    {
        /*
         * HKDF-Expand is defined as:
         *   T(0) = empty
         *   T(i) = HMAC(PRK, T(i-1) || info || i)
         *   output = T(1) || T(2) || ...
         *
         * For TLS 1.3, output is always <= hash_len, so we only need T(1):
         *   T(1) = HMAC(PRK, info || 0x01)
         */
        br_hmac_key_context kc;
        br_hmac_context mc;
        unsigned char one = 0x01;

        br_hmac_key_init(&kc, hash, secret, secret_len);
        br_hmac_init(&mc, &kc, 0);
        br_hmac_update(&mc, info, info_len);
        br_hmac_update(&mc, &one, 1);
        br_hmac_out(&mc, out);

        /* If out_len < hash output, caller gets the first out_len bytes.
         * br_hmac_out always writes hash_len bytes, so we may produce
         * more than needed. For TLS 1.3, out_len is always either
         * hash_len (for secrets) or key_len/iv_len (for traffic keys). */
    }
}

/*
 * Derive-Secret (RFC 8446 Section 7.1)
 *
 * Derive-Secret(Secret, Label, Messages) =
 *     HKDF-Expand-Label(Secret, Label, Hash(Messages), Hash.length)
 *
 * "Messages" is the concatenation of handshake messages — but we pass
 * in the pre-computed hash of those messages (the transcript hash).
 */
static void derive_secret(
    const br_hash_class *hash, size_t hash_len,
    const void *secret,
    const char *label, size_t label_len,
    const void *transcript_hash,
    void *out)
{
    hkdf_expand_label(hash, secret, hash_len,
                      label, label_len,
                      transcript_hash, hash_len,
                      out, hash_len);
}

/*
 * HKDF-Extract wrapper using BearSSL's HMAC directly.
 *
 * HKDF-Extract(salt, IKM) = HMAC(salt, IKM)
 *
 * The salt is the previous secret (or zeros for the first Extract).
 * The IKM (input key material) is the new secret material.
 */
static void hkdf_extract(
    const br_hash_class *hash, size_t hash_len,
    const void *salt, size_t salt_len,
    const void *ikm, size_t ikm_len,
    void *out)
{
    br_hmac_key_context kc;
    br_hmac_context mc;

    (void)hash_len;

    br_hmac_key_init(&kc, hash, salt, salt_len);
    br_hmac_init(&mc, &kc, 0);
    br_hmac_update(&mc, ikm, ikm_len);
    br_hmac_out(&mc, out);
}

void tls13_ks_init(tls13_keysched *ks, const br_hash_class *hash)
{
    br_sha256_context sha256;
    br_sha384_context sha384;

    ks->hash = hash;
    ks->hash_len = (hash == &br_sha256_vtable) ? 32 : 48;

    memset(ks->secret, 0, sizeof(ks->secret));

    /*
     * Precompute Hash("") — the hash of an empty string.
     * This is used in the Derive-Secret("derived", "") steps.
     *
     * SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
     * SHA-384("") = 38b060a751ac9638...
     */
    if (hash == &br_sha256_vtable) {
        br_sha256_init(&sha256);
        br_sha256_out(&sha256, ks->empty_hash);
    } else {
        br_sha384_init(&sha384);
        br_sha384_out(&sha384, ks->empty_hash);
    }
}

void tls13_ks_extract_early(tls13_keysched *ks)
{
    unsigned char zeros[64];
    memset(zeros, 0, ks->hash_len);

    /* Early Secret = HKDF-Extract(salt=0, IKM=0) */
    hkdf_extract(ks->hash, ks->hash_len,
                 zeros, ks->hash_len,  /* salt: zeros */
                 zeros, ks->hash_len,  /* IKM: zeros (no PSK) */
                 ks->secret);
}

void tls13_ks_extract_handshake(tls13_keysched *ks,
                                const void *shared_secret, size_t len)
{
    unsigned char derived[64];

    /* Derive-Secret(Early Secret, "derived", "") */
    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "derived", 7,
                  ks->empty_hash,
                  derived);

    /* Handshake Secret = HKDF-Extract(salt=derived, IKM=shared_secret) */
    hkdf_extract(ks->hash, ks->hash_len,
                 derived, ks->hash_len,
                 shared_secret, len,
                 ks->secret);

    /* Zero intermediate value */
    memset(derived, 0, sizeof(derived));
}

void tls13_ks_derive_handshake_keys(tls13_keysched *ks,
                                    const void *transcript_hash,
                                    size_t key_len,
                                    void *client_key, void *client_iv,
                                    void *server_key, void *server_iv,
                                    void *client_secret_out,
                                    void *server_secret_out)
{
    unsigned char client_secret[64];
    unsigned char server_secret[64];

    /* Derive client/server handshake traffic secrets */
    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "c hs traffic", 12,
                  transcript_hash,
                  client_secret);

    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "s hs traffic", 12,
                  transcript_hash,
                  server_secret);

    /* Copy raw traffic secrets if requested (needed for Finished keys) */
    if (client_secret_out != NULL) {
        memcpy(client_secret_out, client_secret, ks->hash_len);
    }
    if (server_secret_out != NULL) {
        memcpy(server_secret_out, server_secret, ks->hash_len);
    }

    /* Expand traffic secrets into keys and IVs */
    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      client_key, key_len);
    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      client_iv, 12);

    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      server_key, key_len);
    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      server_iv, 12);

    /* Zero intermediate secrets */
    memset(client_secret, 0, sizeof(client_secret));
    memset(server_secret, 0, sizeof(server_secret));
}

void tls13_ks_extract_master(tls13_keysched *ks)
{
    unsigned char derived[64];
    unsigned char zeros[64];

    memset(zeros, 0, ks->hash_len);

    /* Derive-Secret(Handshake Secret, "derived", "") */
    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "derived", 7,
                  ks->empty_hash,
                  derived);

    /* Master Secret = HKDF-Extract(salt=derived, IKM=0) */
    hkdf_extract(ks->hash, ks->hash_len,
                 derived, ks->hash_len,
                 zeros, ks->hash_len,
                 ks->secret);

    memset(derived, 0, sizeof(derived));
}

void tls13_ks_derive_app_keys(tls13_keysched *ks,
                              const void *transcript_hash,
                              size_t key_len,
                              void *client_key, void *client_iv,
                              void *server_key, void *server_iv)
{
    unsigned char client_secret[64];
    unsigned char server_secret[64];

    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "c ap traffic", 12,
                  transcript_hash,
                  client_secret);

    derive_secret(ks->hash, ks->hash_len,
                  ks->secret,
                  "s ap traffic", 12,
                  transcript_hash,
                  server_secret);

    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      client_key, key_len);
    hkdf_expand_label(ks->hash, client_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      client_iv, 12);

    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "key", 3, NULL, 0,
                      server_key, key_len);
    hkdf_expand_label(ks->hash, server_secret, ks->hash_len,
                      "iv", 2, NULL, 0,
                      server_iv, 12);

    memset(client_secret, 0, sizeof(client_secret));
    memset(server_secret, 0, sizeof(server_secret));
}

void tls13_ks_derive_finished_key(tls13_keysched *ks,
                                  const void *base_key,
                                  void *finished_key)
{
    /* finished_key = HKDF-Expand-Label(BaseKey, "finished", "", Hash.length) */
    hkdf_expand_label(ks->hash, base_key, ks->hash_len,
                      "finished", 8, NULL, 0,
                      finished_key, ks->hash_len);
}
