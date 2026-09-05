/*
 * entropy.h — Mac OS entropy pool for BearSSL
 *
 * Gathers entropy from multiple Mac OS sources, mixes them,
 * and provides a function to seed BearSSL's HMAC_DRBG.
 */

#ifndef CERTAINLY_ENTROPY_H
#define CERTAINLY_ENTROPY_H

#include <bearssl.h>
#include <stddef.h>

/*
 * Initialize the entropy pool. Call once during MacTLS_Init().
 * Gathers initial entropy from all available sources.
 */
void entropy_init(void);

/*
 * Gather fresh entropy and inject it into a BearSSL SSL engine's
 * internal PRNG. Call this before each TLS handshake to ensure
 * the PRNG is well-seeded.
 */
void entropy_seed_engine(br_ssl_engine_context *eng);

/*
 * Add external entropy bytes to the pool. Applications can call
 * this via MacTLS_AddEntropy() to contribute additional randomness
 * (e.g., user input timing).
 */
void entropy_add(const void *data, size_t len);

#endif /* CERTAINLY_ENTROPY_H */
