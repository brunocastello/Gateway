/*
 * gw_skexport.c - see gw_skexport.h.
 */

#include "gw_skexport.h"

#include <string.h>

size_t gw_ske_write_params(const unsigned char *mod, size_t mod_len,
                           const unsigned char *exp, size_t exp_len,
                           unsigned char *out, size_t cap)
{
    size_t total = 2 + mod_len + 2 + exp_len;

    if (cap < total) return 0;

    out[0] = (unsigned char)(mod_len >> 8);
    out[1] = (unsigned char)mod_len;
    memmove(out + 2, mod, mod_len);
    out[2 + mod_len]     = (unsigned char)(exp_len >> 8);
    out[2 + mod_len + 1] = (unsigned char)exp_len;
    memmove(out + 2 + mod_len + 2, exp, exp_len);

    return total;
}

size_t gw_ske_write_signed_input(const unsigned char client_random[32],
                                 const unsigned char server_random[32],
                                 const unsigned char *params,
                                 size_t params_len,
                                 unsigned char *out, size_t cap)
{
    size_t total = 64 + params_len;

    if (cap < total) return 0;

    memmove(out, client_random, 32);
    memmove(out + 32, server_random, 32);
    memmove(out + 64, params, params_len);

    return total;
}

size_t gw_ske_write_message(const unsigned char *params, size_t params_len,
                            const unsigned char *sig, size_t sig_len,
                            unsigned char *out, size_t cap)
{
    size_t body_len = params_len + 2 + sig_len;

    if (cap < 4 + body_len) return 0;

    out[0] = 12;
    out[1] = (unsigned char)(body_len >> 16);
    out[2] = (unsigned char)(body_len >> 8);
    out[3] = (unsigned char)body_len;

    /* params may already be sitting at out + 4 (the real call site
     * writes them there directly); memmove tolerates that overlap. */
    memmove(out + 4, params, params_len);
    out[4 + params_len]     = (unsigned char)(sig_len >> 8);
    out[4 + params_len + 1] = (unsigned char)sig_len;
    memmove(out + 4 + params_len + 2, sig, sig_len);

    return 4 + body_len;
}
