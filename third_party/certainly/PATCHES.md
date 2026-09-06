# Local patches to Certainly

Certainly is vendored from https://github.com/minorbug/certainly (MIT).
Gateway keeps a copy rather than a submodule because the sources below carry
local fixes. Re-apply them whenever the vendored tree is refreshed.

## 1. `src/tls13_record.c` — ChaCha20-Poly1305 authentication tag was never checked

Upstream `tls13_record_decrypt()` contained:

```c
ok = 1; /* poly1305_run handles verification internally */
```

That comment is wrong. BearSSL's `br_poly1305_*_run()` does not verify
anything on decryption — it *overwrites* the caller's tag buffer with the tag
it computed and leaves the comparison to the caller. As shipped, any forged or
corrupted record on a `TLS_CHACHA20_POLY1305_SHA256` connection decrypted
"successfully", which defeats the AEAD.

The patch stashes the received tag before the call and compares it against the
computed tag with `tls13_ct_equal()`, a constant-time compare ported from the
equivalent check in `mplsllc/macTLS`. The AES-GCM branch already did the right
thing via `br_gcm_check_tag()`.

## 2. `src/certainly.c` — missing `<Events.h>`

`MacTLS_Pump()` calls `TickCount()` to time out a stalled handshake but never
included `<Events.h>`. Upstream gets away with it because its build installs
Apple's Universal Interfaces over the whole toolchain, and something else in
the include chain happens to declare it. Gateway puts those headers on the
Open Transport translation units only, so the declaration has to be explicit.
Without it GCC 12 fails on `-Werror=implicit-function-declaration`, which is
the default in C99 mode.

## 3. Buffer overflows in the TLS 1.3 record and handshake paths

Three separate writes were driven by a length taken straight off the wire, with
no bound. Any site whose certificate chain or response records were large
enough crashed Gateway with a Mac OS "error of type 2" (address error), or —
when the overflow was small enough to land on the key schedule rather than past
the end of the block — failed the handshake with a bad MAC.

**`tls13_handshake.c`, `tls13_read_encrypted_hs()`.** The one that actually
bit. Every caller passes `hs->msg_buf` as the destination, which was
`unsigned char msg_buf[4096]`, and the function copied a whole handshake
message into it:

```c
hs_body_len = get_u24(hs->plain_buf + hs->plain_offset + 1);
total_hs = 4 + (size_t)hs_body_len;
...
memcpy(out_data, hs->plain_buf + hs->plain_offset, total_hs);
```

`total_hs` is bounded only by the decrypted record, so up to ~16 KB could be
written into 4 KB. A TLS 1.3 Certificate message carrying a leaf plus an
intermediate is routinely 3–6 KB, so this fired on any mainstream CDN. The
overflow ran through `plain_buf`, `read_ctx` and `write_ctx` and then off the
end of the heap block holding `MacTLS_Context`.

Fixed by giving the function an explicit `out_cap`, checking `total_hs` against
it, and sizing `msg_buf` to `TLS13_MAX_PLAINTEXT` — the largest message that
can arrive, since messages spanning several records are already rejected.

**`tls13_record_decrypt()`.** Decryption is in place: the function starts with
`memcpy(dec_buf, ciphertext, ct_len)`. It never checked `ct_len` against the
RFC 8446 §5.2 limit or against the caller's buffer. The two callers passed
buffers of exactly 16384 bytes, while a legal `TLSCiphertext` may be 2^14 + 256
= 16640, so a full-size record overran both by up to 256 bytes. The function
now takes `out_cap` and rejects anything that does not fit, before writing.

**`certainly.c`, `tls13_recv_records()` and `MacTLS_Write()`.** Both declared a
16 KB scratch buffer as a local, several frames deep inside the event loop.
They now use `tls13_dec_buf` and `tls13_enc_buf` in `MacTLS_Context`, sized
against the RFC limits, which fixes the 256-byte shortfall and takes 32 KB off
the Mac OS 9 stack at the same time.

**Hang on an over-long record.** Three loops waited for `recv_len >= 5 +
record_len` before doing anything else. A peer declaring a record longer than
the receive buffer could never satisfy that, so the connection spun forever
instead of failing. Each site now rejects `record_len > TLS13_MAX_CIPHERTEXT`.

`TLS13_MAX_PLAINTEXT` and `TLS13_MAX_CIPHERTEXT` are defined in
`tls13_record.h`, and `tls13_handshake.c` carries compile-time assertions so
the buffer sizes cannot drift back.

## 4. Handshake messages spanning several records were rejected

`tls13_read_encrypted_hs()` decrypted one record at a time and required each
handshake message to be complete within it:

```c
if (total_hs > remaining) {
    /* Handshake message spans multiple records — uncommon but legal. */
    hs->error = BR_ERR_BAD_PARAM;
    return kTLS13_Error;
}
```

It is legal and not especially uncommon: a server is free to fragment the
Certificate message across records, and CDNs with large chains do. The
handshake failed against those hosts with a bad-parameter error.

The buffered plaintext is now reassembled. When what is held is not yet a whole
message — including the case where even the 4-byte header is split — the
partial message is compacted to the front of `plain_buf` and the next record is
decrypted directly behind it. `plain_buf` grew to
`TLS13_MAX_PLAINTEXT + TLS13_MAX_CIPHERTEXT` so it can hold a partial message
plus the whole of the record that completes it.

## 5. No way to start TLS on an existing connection (STARTTLS)

`MacTLS_Create()` opens the socket itself, so there was no way to hand
Certainly a connection that had already carried plaintext. That ruled out
STARTTLS, and with it SMTP submission on port 587 — which is the only port
Microsoft offers for personal Outlook.com accounts.

Added `ot_transport_adopt()` and the public `MacTLS_CreateOnEndpoint()`. The
caller connects the endpoint and speaks the cleartext protocol up to the
server's "ready to start TLS" reply, then transfers the endpoint; the transport
replaces the caller's notifier with its own and starts in the Connected state,
so `MacTLS_Pump()` proceeds straight to the handshake. Ownership transfers
unconditionally, including on failure, so there is no path where both sides
think they should close it.

## 6. Asynchronous DNS held a pointer the caller was free to reuse

`ot_start_dns()` passed the caller's hostname straight to the resolver:

```c
err = OTInetStringToAddress(t->inetSvc, (char *)host, &t->hostInfo);
```

The provider is in asynchronous mode, so this call returns immediately and Open
Transport reads the name later, when the lookup actually runs — it does not
take a copy. The buffer must stay valid and unchanged until
`T_DNRSTRINGTOADDRCOMPLETE` arrives.

Upstream never noticed because its callers pass string literals. Gateway hit it
the moment a hostname came from anywhere else: connections whose host was a
literal or a long-lived struct member worked, while the OAuth token refresh —
whose host comes from the prefs cache, a small rotating set of buffers — got a
name that had been overwritten by the time the resolver looked, and failed with
a bare "connect failed".

`OTTransport` now carries `char host[256]` and resolves that, so the call is
safe whatever the caller does with its own buffer afterwards. Gateway also
keeps its own copies at both call sites, since relying on a library not to
retain a pointer is exactly the assumption that broke here.

## 7. The endpoint was bound after being switched to asynchronous mode

`ot_setup_endpoint()` installed the notifier, called `OTSetAsynchronous()` and
`OTSetNonBlocking()`, and only then called `OTBind()`. On an asynchronous
endpoint `OTBind` returns immediately and reports completion later as
`T_BINDCOMPLETE` — an event the notifier does not handle. Nothing therefore
guaranteed the endpoint was bound by the time `OTConnect()` ran after DNS
resolution, and `OTConnect` on an unbound endpoint fails with
`kOTOutStateErr`. The code worked whenever the DNS lookup happened to take
longer than the bind, which is most of the time and not something to depend on.

The bind now happens first, while the endpoint is still synchronous, so it
blocks until it has actually completed. This is the order Gateway's own
listener and connection code has always used.

## 8. Diagnostics for a connection that fails before the handshake

`MacTLS_GetPhase()` and `MacTLS_GetResolvedAddress()` report how far a
connection got and what the name resolved to. Combined with the existing
`MacTLS_GetOTError()`, a failure can be logged as

    connect failed [connecting TCP, OT -3259, 20.190.173.69]

rather than a bare "connect failed", which separates a name that will not
resolve from an address that will not accept a connection.

## 9. OTConnect was given a stack address it read after the frame was gone

The same class of bug as §6, and the one that actually stopped Gateway from
reaching the OAuth token endpoint. `ot_transport_pump()` built the connect
request in locals:

```c
InetAddress remoteAddr;
TCall       sndCall;

OTInitInetAddress(&remoteAddr, t->port, t->hostInfo.addrs[0]);
sndCall.addr.buf = (unsigned char *)&remoteAddr;
t->lastError = OTConnect(t->endpoint, &sndCall, NULL);
```

The endpoint is asynchronous, so `OTConnect` returns `kOTNoDataErr`
immediately and Open Transport reads the address later, when it actually sends
the SYN. By then `ot_transport_pump()` has returned and that stack frame has
been reused by whatever ran next, so OT connected to whatever happened to be
sitting there. Whether it worked came down to how much stack churn followed the
call, which is why it was survivable on some paths and reliably fatal on
others.

`remoteAddr` and `sndCall` now live in `OTTransport`, next to the hostname
fixed in §6. Both are the same mistake: an asynchronous Open Transport call
does not copy its arguments.

The symptom was a bare `connect failed` with an Open Transport error of 0,
because a SYN to a nonsense address produces `T_DISCONNECT` — see §10.

## 10. T_DISCONNECT was never consumed, and its reason was thrown away

The notifier recorded `t->lastError = result` for `T_DISCONNECT`, but that
argument is always 0 for this event: the reason lives in the `TDiscon` that
`OTRcvDisconnect()` fills in. Every connection refused or reset therefore
reported "no error", which is why the first round of diagnostics came back
empty-handed.

Worse, `OTRcvDisconnect()` is not optional. Until the event is consumed the
endpoint stays in a state where every subsequent call fails with
`kOTLookErr`. `ot_consume_disconnect()` now does both jobs wherever
`disconnectReceived` is handled.

## 11. Only the first resolved address was ever tried

`hostInfo.addrs[0]` was used and the rest ignored, so a single refused or
unreachable address failed the whole connection. Large services rotate through
many: `login.microsoftonline.com` returns eight. `ot_try_next_address()` now
walks the list on `T_DISCONNECT` before giving up.

## 12. Only X25519 was offered for TLS 1.3 key exchange

Certainly generated a single ephemeral key share, on X25519, and rejected any
ServerHello naming a different group. That is fine for most of the web and
fatal for Microsoft:

```
login.microsoftonline.com  -groups X25519  ->  Cipher is (NONE)
login.microsoftonline.com  -groups P-256   ->  TLS_AES_256_GCM_SHA384
```

`login.microsoftonline.com`, `outlook.office365.com` and
`smtp-mail.outlook.com` all refuse X25519, and Azure drops the connection
rather than answering with a `handshake_failure` alert — so the failure
surfaced as `ECONNRESET` from a TCP connection that had completed, which is
indistinguishable at the transport layer from a connect that never worked.
Every part of Gateway's mail module talks to one of those three hosts, so this
single gap blocked OAuth, IMAP and SMTP alike.

The ClientHello now advertises both X25519 and secp256r1 in
`supported_groups` and carries a key share for **both**, so the server can
finish the exchange from the ClientHello whichever it prefers, with no
HelloRetryRequest round trip. `negotiated_group` records the choice and the
ECDH dispatches on it; the P-256 secret is the X coordinate of the shared
point, per RFC 8446 §7.4.2. BearSSL supplies the curve as `br_ec_p256_m15`.
