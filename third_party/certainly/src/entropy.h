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

/*
 * Draw bytes from the pool.
 *
 * Added for RSA key generation, which needs a seeded PRNG of its own rather
 * than an SSL engine to inject into. The pool is hashed rather than handed
 * over, and stirred between blocks, so a caller asking for a long run does
 * not learn the pool state and does not get the same block twice.
 *
 * This is the pool, so it is exactly as good as entropy_init() managed to
 * make it -- see entropy_system_source() for whether that included an
 * operating-system generator. Do not treat it as one.
 */
void entropy_get(void *buf, size_t len);

/*
 * Which operating-system random generator, if any, the pool was able to
 * draw on — for the host application to log. NULL where the question does
 * not arise: Mac OS has never had one, so the pool is the whole story and a
 * line saying so every launch would be noise. On Windows it separates a
 * machine with a real PRNG from one running on timing alone.
 */
const char *entropy_system_source(void);

#endif /* CERTAINLY_ENTROPY_H */
