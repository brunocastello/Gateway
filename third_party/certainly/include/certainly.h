/*
 * certainly.h — Public API for the Certainly TLS library
 *
 * Certainly provides TLS 1.2 and TLS 1.3 connectivity for classic Mac
 * OS 9 apps. It handles the handshake, record-layer encryption, and
 * certificate verification on top of Open Transport. Callers supply
 * plaintext (HTTP requests, etc.); Certainly handles the crypto.
 *
 * Usage pattern:
 *
 *   MacTLS_Init();
 *   ctx = MacTLS_Create("api.example.com", 443);
 *
 *   // In your event loop:
 *   while (running) {
 *       WaitNextEvent(everyEvent, &event, 1, NULL);
 *       HandleEvent(&event);
 *
 *       state = MacTLS_Pump(ctx);
 *       if (state == kMacTLS_Connected) {
 *           MacTLS_Write(ctx, request, strlen(request));
 *           n = MacTLS_Read(ctx, buf, sizeof(buf));
 *       }
 *   }
 *
 *   MacTLS_Close(ctx);
 *   MacTLS_Shutdown();
 */

#ifndef CERTAINLY_H
#define CERTAINLY_H

#include <stdint.h>
#include <stddef.h>

/*
 * For CTSocket, the handle type of whatever transport is linked. This header
 * used to include <OpenTransport.h> for the one EndpointRef below, which made
 * every file that wanted to speak TLS a Mac file.
 */
#include "../src/certainly_transport.h"

/* ── Opaque types ── */
typedef struct MacTLS_Context MacTLS_Context;
typedef struct MacTLS_Config  MacTLS_Config;

/* ── Connection state ── */
typedef enum {
    kMacTLS_Idle,
    kMacTLS_Connecting,
    kMacTLS_Handshaking,
    kMacTLS_Connected,
    kMacTLS_Closing,
    kMacTLS_Closed,
    kMacTLS_Error
} MacTLS_State;

/* ── Error codes ── */
typedef enum {
    kMacTLS_OK = 0,
    kMacTLS_ErrMemory,
    kMacTLS_ErrDNS,
    kMacTLS_ErrConnect,
    kMacTLS_ErrHandshake,
    kMacTLS_ErrCertificate,
    kMacTLS_ErrRead,
    kMacTLS_ErrWrite,
    kMacTLS_ErrClosed,
    kMacTLS_ErrOT
} MacTLS_Error;

typedef enum {
    kMacTLS_VersionUnknown = 0,    /* Handshake not yet complete. */
    kMacTLS_Version12,
    kMacTLS_Version13
} MacTLS_Version;

/* ── Library lifecycle ── */
MacTLS_Error MacTLS_Init(void);
void         MacTLS_Shutdown(void);

/* ── Connection lifecycle ── */

/*
 * Create a TLS connection context and begin connecting.
 *
 * Returns NULL only on allocation failure. On other errors (invalid hostname,
 * transport failure), returns a non-NULL context in kMacTLS_Error state.
 * ALWAYS check MacTLS_GetState() after Create — a NULL check alone is not
 * sufficient:
 *
 *   ctx = MacTLS_Create("example.com", 443);
 *   if (ctx == NULL || MacTLS_GetState(ctx) == kMacTLS_Error) {
 *       // handle error (call MacTLS_Close(ctx) if non-NULL)
 *   }
 */
MacTLS_Context *MacTLS_Create(const char *host, uint16_t port);
MacTLS_Context *MacTLS_CreateWithConfig(const char *host, uint16_t port,
                                        MacTLS_Config *cfg);

/*
 * Start TLS on a connection that is already open and has already carried
 * plaintext -- the STARTTLS / STLS / STARTTLS-on-587 pattern.
 *
 * The caller opens the endpoint, connects it, and speaks the cleartext part of
 * the protocol up to and including the server's "ready to start TLS" reply.
 * It must have consumed that reply completely: anything left unread on the
 * wire is the first bytes of the TLS handshake.
 *
 * Ownership of ep transfers to the returned context unconditionally, including
 * on failure, so the caller must not close it afterwards. host is used for SNI
 * and certificate validation exactly as in MacTLS_Create.
 *
 * Same return contract as MacTLS_Create: check MacTLS_GetState() as well as
 * the NULL case.
 */
MacTLS_Context *MacTLS_CreateOnEndpoint(const char *host, CTSocket sock);
MacTLS_State    MacTLS_Pump(MacTLS_Context *ctx);
void            MacTLS_Close(MacTLS_Context *ctx);

/* ── Data transfer ── */
int    MacTLS_Write(MacTLS_Context *ctx, const void *data, size_t len);
int    MacTLS_Read(MacTLS_Context *ctx, void *buf, size_t len);
size_t MacTLS_Available(const MacTLS_Context *ctx);

/* ── Status ── */
MacTLS_State MacTLS_GetState(const MacTLS_Context *ctx);
MacTLS_Error MacTLS_GetError(const MacTLS_Context *ctx);
/*
 * The transport's own error number -- an Open Transport OSStatus on Mac OS 9,
 * a Winsock error on Windows. It was called GetOTError and returned OSStatus
 * until there was a second transport for that to be wrong about.
 */
long         MacTLS_GetTransportError(const MacTLS_Context *ctx);

/*
 * Raw bytes across the socket in each direction, ciphertext included. What the
 * peer actually saw, as opposed to what the TLS layer thinks it handed over --
 * a request the record layer accepted but that never reached the wire is
 * indistinguishable, from above, from one the peer ignored.
 */
void         MacTLS_GetCounters(const MacTLS_Context *ctx,
                                unsigned long *sent, unsigned long *received);

/*
 * Ciphertext received but not yet turned into anything: the number of bytes
 * sitting in the record buffer, and the first few of them, which for a TLS 1.3
 * record is its header -- content type, legacy version, and the length the
 * peer says the record is.
 *
 * That header answers the question no counter can: whether bytes that arrived
 * and produced nothing were an unparsed record, a record still short of its
 * declared length, or an alert.
 */
/*
 * The last alert the peer sent, as (level << 8) | description, or 0 if none.
 * Level 1 is a warning and 2 is fatal; description 20 is bad_record_mac, 40 a
 * handshake failure, 51 decrypt_error. RFC 8446 section 6 has the rest.
 */
unsigned int MacTLS_GetAlert(const MacTLS_Context *ctx);

size_t       MacTLS_GetPending(const MacTLS_Context *ctx,
                               unsigned char *head, size_t headcap);
int          MacTLS_GetBearSSLError(const MacTLS_Context *ctx);

/* Returns the negotiated protocol version, or kMacTLS_VersionUnknown
 * before the handshake completes (state != kMacTLS_Connected). */
MacTLS_Version MacTLS_GetVersion(const MacTLS_Context *ctx);

/*
 * How far the connection got before it stopped. A failure reported only as
 * "connect failed" cannot be acted on; knowing whether the name resolved, and
 * to what, separates a DNS problem from a routing or firewall one.
 */
typedef enum {
    kMacTLS_PhaseIdle = 0,
    kMacTLS_PhaseResolving,
    kMacTLS_PhaseConnecting,
    kMacTLS_PhaseConnected,
    kMacTLS_PhaseClosing,
    kMacTLS_PhaseClosed,
    kMacTLS_PhaseFailed
} MacTLS_Phase;

MacTLS_Phase MacTLS_GetPhase(const MacTLS_Context *ctx);

/* The first address DNS returned, in host byte order, or 0 if the lookup has
 * not completed. */
uint32_t MacTLS_GetResolvedAddress(const MacTLS_Context *ctx);

/* ── Configuration ── */
MacTLS_Config *MacTLS_ConfigCreate(void);
void           MacTLS_ConfigFree(MacTLS_Config *cfg);
MacTLS_Error   MacTLS_ConfigAddCA(MacTLS_Config *cfg,
                                  const void *der, size_t len);

/* ── Entropy ── */
void MacTLS_AddEntropy(const void *data, size_t len);

#endif /* CERTAINLY_H */
