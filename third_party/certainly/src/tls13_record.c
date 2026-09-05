/*
 * tls13_record.c — TLS 1.3 record encryption/decryption
 *
 * The nonce for each record is computed as:
 *   nonce = base_IV XOR padded_sequence_number
 *
 * Where padded_sequence_number is the 64-bit sequence number
 * left-padded with zeros to 12 bytes.
 *
 * The AAD (additional authenticated data) is the 5-byte record header:
 *   content_type (0x17) + version (0x0303) + ciphertext_length
 *
 * The ciphertext_length includes the content type byte and the
 * 16-byte AEAD tag.
 */

#include "tls13_record.h"
#include <string.h>

/* TLS 1.3 cipher suite IDs */
#define TLS_AES_128_GCM_SHA256       0x1301
#define TLS_AES_256_GCM_SHA384       0x1302
#define TLS_CHACHA20_POLY1305_SHA256 0x1303

/*
 * Compute the per-record nonce by XORing the IV with the sequence number.
 *
 * The sequence number is a 64-bit counter placed in the low 8 bytes
 * of the 12-byte nonce. The high 4 bytes of IV are XORed with zeros
 * (i.e., unchanged).
 */
static void compute_nonce(const unsigned char *iv, uint64_t seq,
                          unsigned char *nonce)
{
    memcpy(nonce, iv, 12);
    /* XOR sequence number into the low 8 bytes of the nonce */
    nonce[4]  ^= (unsigned char)(seq >> 56);
    nonce[5]  ^= (unsigned char)(seq >> 48);
    nonce[6]  ^= (unsigned char)(seq >> 40);
    nonce[7]  ^= (unsigned char)(seq >> 32);
    nonce[8]  ^= (unsigned char)(seq >> 24);
    nonce[9]  ^= (unsigned char)(seq >> 16);
    nonce[10] ^= (unsigned char)(seq >> 8);
    nonce[11] ^= (unsigned char)(seq);
}

/*
 * Build the AAD (Additional Authenticated Data) for a TLS 1.3 record.
 *
 * AAD = content_type (0x17) || version (0x0303) || ciphertext_length
 *
 * The ciphertext_length is the total encrypted payload size:
 *   plaintext + 1 (content type byte) + 16 (AEAD tag)
 */
static void build_aad(size_t ciphertext_len, unsigned char *aad)
{
    aad[0] = 0x17;  /* application_data */
    aad[1] = 0x03;  /* TLS 1.2 (legacy) */
    aad[2] = 0x03;
    aad[3] = (unsigned char)(ciphertext_len >> 8);
    aad[4] = (unsigned char)(ciphertext_len);
}

void tls13_record_init(tls13_record_ctx *ctx,
                       const void *key, size_t key_len,
                       const void *iv,
                       uint16_t cipher_suite)
{
    memcpy(ctx->key, key, key_len);
    ctx->key_len = key_len;
    memcpy(ctx->iv, iv, 12);
    ctx->seq = 0;
    ctx->cipher_suite = cipher_suite;
}

int tls13_record_encrypt(tls13_record_ctx *ctx,
                         const void *plaintext, size_t pt_len,
                         uint8_t ct,
                         void *out, size_t *out_len)
{
    unsigned char nonce[12];
    unsigned char aad[5];
    size_t total_ct_len;
    unsigned char *out_buf = (unsigned char *)out;

    /* Total ciphertext = plaintext + content type byte + AEAD tag */
    total_ct_len = pt_len + 1 + TLS13_TAG_SIZE;

    compute_nonce(ctx->iv, ctx->seq, nonce);
    build_aad(total_ct_len, aad);

    /* Copy plaintext + append content type byte */
    memcpy(out_buf, plaintext, pt_len);
    out_buf[pt_len] = ct;

    /* Encrypt in-place */
    if (ctx->cipher_suite == TLS_CHACHA20_POLY1305_SHA256) {
        br_poly1305_ctmul_run(ctx->key, nonce,
                              out_buf, pt_len + 1,
                              aad, sizeof(aad),
                              out_buf + pt_len + 1, /* tag output */
                              br_chacha20_ct_run,
                              1 /* encrypt */);
    } else {
        /* AES-GCM */
        br_aes_ct_ctr_keys aes_ctx;
        br_gcm_context gcm;

        br_aes_ct_ctr_init(&aes_ctx, ctx->key, ctx->key_len);
        br_gcm_init(&gcm, &aes_ctx.vtable, br_ghash_ctmul32);
        br_gcm_reset(&gcm, nonce, 12);
        br_gcm_aad_inject(&gcm, aad, sizeof(aad));
        br_gcm_flip(&gcm);
        br_gcm_run(&gcm, 1 /* encrypt */, out_buf, pt_len + 1);
        br_gcm_get_tag(&gcm, out_buf + pt_len + 1);
    }

    *out_len = total_ct_len;
    ctx->seq++;
    return 0;
}

/*
 * Constant-time buffer compare. Returns 1 when the two buffers are equal,
 * 0 otherwise, without leaking where the first difference is. Ported from
 * the equivalent check in mplsllc/macTLS.
 */
static int tls13_ct_equal(const unsigned char *a, const unsigned char *b,
                          size_t len)
{
    unsigned int diff = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        diff |= (unsigned int)(a[i] ^ b[i]);
    }
    return (int)(((diff - 1) >> 8) & 1u);
}

int tls13_record_decrypt(tls13_record_ctx *ctx,
                         const void *ciphertext, size_t ct_len,
                         void *out, size_t *out_len,
                         uint8_t *out_ct)
{
    unsigned char nonce[12];
    unsigned char aad[5];
    unsigned char *dec_buf = (unsigned char *)out;
    size_t payload_len;
    int ok;

    if (ct_len < 1 + TLS13_TAG_SIZE) return -1;

    payload_len = ct_len - TLS13_TAG_SIZE;

    compute_nonce(ctx->iv, ctx->seq, nonce);
    build_aad(ct_len, aad);

    /* Copy ciphertext to output buffer for in-place decryption */
    memcpy(dec_buf, ciphertext, ct_len);

    if (ctx->cipher_suite == TLS_CHACHA20_POLY1305_SHA256) {
        unsigned char recv_tag[TLS13_TAG_SIZE];

        /* br_poly1305_*_run() does NOT verify the tag: on decryption it
         * overwrites the tag buffer with the tag it computed. Stash the
         * received tag first, then compare it ourselves in constant time.
         * Gateway patch - see third_party/certainly/PATCHES.md. */
        memcpy(recv_tag, dec_buf + payload_len, TLS13_TAG_SIZE);

        br_poly1305_ctmul_run(ctx->key, nonce,
                              dec_buf, payload_len,
                              aad, sizeof(aad),
                              dec_buf + payload_len, /* tag out */
                              br_chacha20_ct_run,
                              0 /* decrypt */);

        ok = tls13_ct_equal(recv_tag, dec_buf + payload_len, TLS13_TAG_SIZE);
    } else {
        /* AES-GCM */
        br_aes_ct_ctr_keys aes_ctx;
        br_gcm_context gcm;

        br_aes_ct_ctr_init(&aes_ctx, ctx->key, ctx->key_len);
        br_gcm_init(&gcm, &aes_ctx.vtable, br_ghash_ctmul32);
        br_gcm_reset(&gcm, nonce, 12);
        br_gcm_aad_inject(&gcm, aad, sizeof(aad));
        br_gcm_flip(&gcm);
        br_gcm_run(&gcm, 0 /* decrypt */, dec_buf, payload_len);
        ok = br_gcm_check_tag(&gcm, dec_buf + payload_len);
    }

    if (!ok) return -1;

    /* Extract real content type: last non-zero byte of decrypted payload.
     * RFC 8446: "The content type is the real content type of the record.
     * The padding is all zeros." */
    {
        size_t i = payload_len;
        while (i > 0 && dec_buf[i - 1] == 0) i--;
        if (i == 0) return -1; /* all zeros — invalid */
        *out_ct = dec_buf[i - 1];
        *out_len = i - 1;
    }

    ctx->seq++;
    return 0;
}
