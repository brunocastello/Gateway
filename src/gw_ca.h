/*
 * gw_ca.h - the local certificate authority.
 *
 * Internet Explorer 4 asked for https://host and will not take plaintext in
 * the tunnel, so to serve it at all Gateway has to present a certificate for
 * that host. No real authority will sign one for us, so Gateway signs it
 * itself, with a key and a certificate it generates on the machine it is
 * running on. Nothing outside that machine takes part.
 *
 * One key does everything: it is the authority's key, and it is also the key
 * in every leaf certificate. That is unusual and it is the point -- generating
 * an RSA key on a Pentium or a 604 takes long enough to notice, so it happens
 * once, and minting a certificate for a host afterwards is a DER encode and
 * one signature. Browsers do not object; nothing in the path checks that a
 * leaf's key differs from its issuer's.
 *
 * The browser will warn on the first connection unless the certificate from
 * GWCa_Cert() has been installed. That warning is a dialog with a Yes button
 * on the clients this exists for, so installing is worth doing and not
 * required -- except that subresources on other hosts tend to fail silently
 * rather than prompt, which is the real argument for installing it.
 */
#ifndef GW_CA_H
#define GW_CA_H

#include <stddef.h>

/*
 * Load the authority from disk, or generate one and save it. Returns 1 when
 * there is a usable key and certificate.
 *
 * The generating path is slow -- a thousand-bit RSA key means finding two
 * five-hundred-bit primes -- and it blocks the cooperative loop while it runs,
 * which is why it is called on first need rather than at startup: a machine
 * that never turns on the tunnel never pays for it. The log says how long it
 * took, because on this hardware that is worth knowing and cannot be guessed.
 */
int GWCa_Init(void);

/* Whether GWCa_Init() has already succeeded, without attempting it. */
int GWCa_Ready(void);

/*
 * The authority certificate, DER. This is the file to install in a browser.
 * NULL before GWCa_Init() succeeds.
 */
const unsigned char *GWCa_Cert(size_t *len);

/*
 * A certificate for one host, minted on demand and cached. The CN and the
 * dNSName are the host as given, so it must be the name the browser asked
 * for rather than anything resolved or canonicalised.
 *
 * Returns NULL if the authority is unavailable or the name will not fit.
 */
const unsigned char *GWCa_Leaf(const char *host, size_t *len);

/*
 * The private key, as a `const br_rsa_private_key *`.
 *
 * Deliberately void: this header is included by the proxy, which has no
 * business seeing BearSSL's types, and BearSSL's key struct is a typedef of an
 * anonymous struct so it cannot be forward declared. The one caller that
 * needs it hands it straight to Certainly, which casts it back.
 */
const void *GWCa_Key(void);

#endif /* GW_CA_H */
