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

## 13. Offering TLS_AES_256_GCM_SHA384 guaranteed a failed handshake

The transcript hash has to be started before the server has chosen a cipher
suite, so Certainly starts it as SHA-256. `tls13_state_recv_server_hello()`
then compares the suite's hash against the one already running and gives up if
they differ:

```c
/* TODO: In a full implementation, we'd need to re-hash from the original
 * ClientHello bytes. For now, treat this as an error ... */
hs->error = BR_ERR_BAD_CIPHER_SUITE;
```

`TLS_AES_256_GCM_SHA384` was nevertheless in the offered list, so the failure
was reachable purely by a server taking us up on it — and Microsoft's
endpoints do exactly that, selecting AES-256-GCM-SHA384 whichever order the
client lists suites in:

```
-ciphersuites 'TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384'  ->  AES_256_GCM_SHA384
-ciphersuites 'TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256'  ->  AES_256_GCM_SHA384
```

So the handshake reached ServerHello and then rejected the server's choice of a
suite Certainly had itself proposed.

The suite is withdrawn from the ClientHello rather than the TODO being
implemented. RFC 8446 §9.1 makes `TLS_AES_128_GCM_SHA256` mandatory to
implement, so nothing is lost in interoperability, and the remaining suites all
use SHA-256 — which makes the mismatch branch unreachable instead of merely
unlikely. Restoring AES-256 needs a transcript that keeps the ClientHello bytes
so it can be re-hashed under SHA-384; the HRR path would need the same
treatment for its synthetic message_hash.

## 14. The trust anchor set was too small to reach ordinary sites

Certainly shipped ten roots: Amazon, DigiCert G2/G3, the four Google GTS roots,
both ISRG roots and Starfield. A Mac OS 9 machine has no usable system trust
store and no way to add a root by hand, so that list is the whole of what
Gateway will ever trust — and it does not include GlobalSign, which is what
`lite.cnn.com` chains to:

```
i:OU=GlobalSign ECC Root CA - R5, O=GlobalSign, CN=GlobalSign
```

The handshake reached the certificate and failed verification with
`BR_ERR_X509_NOT_TRUSTED` (62). It looked like the split-record bug from §4
because both surface at the same point in the handshake, but the two are
unrelated: `lite.cnn.com` supports X25519 and every suite offered, and its
chain is a perfectly ordinary 5.8 KB.

`ca_roots.c` is now generated by `tools/generate_ca_roots.py` from a PEM
bundle, emitting the same C that BearSSL's `brssl ta` does, and carries 29
anchors covering the CAs behind the major CDNs, the free-certificate issuers,
and the roots Microsoft and Google chain to. The generator's output was checked
against the previous file: Amazon Root CA 1's DN, modulus and exponent are
byte-for-byte identical, and the EC anchors round-trip.

## 15. Notifiers were left installed on providers being closed

`ot_transport_destroy()` closed both the endpoint and the internet services
provider without removing their notifiers first, then freed the `OTTransport`
they were installed with as their context:

```c
if (t->inetSvc != NULL) OTCloseProvider(t->inetSvc);
if (t->endpoint != NULL) OTCloseProvider(t->endpoint);
DisposePtr((Ptr)t);
```

A transport destroyed while a DNS lookup or a connect is still outstanding can
have `T_DNRSTRINGTOADDRCOMPLETE` or `T_DISCONNECT` delivered to it afterwards.
The notifier then writes its flags into memory that has gone back to the heap,
corrupting the Memory Manager's free list — and the crash lands somewhere
unrelated, some time later. It surfaced as an "error type 3" after pressing a
browser's Stop button, which tears down several in-flight connections at once.

`OTRemoveNotifier()` now precedes every `OTCloseProvider()`. Gateway's own
`GWConn_Destroy()` had the same omission on its DNS provider and is fixed
alongside.

## 16. Closing a connection mid-handshake encrypted with keys that did not exist

`MacTLS_Close()` sent a TLS 1.3 close_notify whenever the state was
`kMacTLS_Connected` **or** `kMacTLS_Handshaking`:

```c
if (ctx->state == kMacTLS_Connected ||
    ctx->state == kMacTLS_Handshaking) {
    if (ctx->tls13_active) {
        ... tls13_record_encrypt(&ctx->hs13.write_ctx, ...)
```

`tls13_active` is set the moment ServerHello selects TLS 1.3, which is well
before the key schedule has produced the write keys. Closing a connection
during its handshake therefore encrypted an alert with a zeroed record
context — `key_len` 0, `cipher_suite` 0. Zero selects the AES-GCM branch, and
`br_aes_ct_ctr_init()` derives its round count from the key length, so a
zero-length key produces a nonsense schedule and the cipher walks off the end
of it. On Mac OS 9 that surfaced as an "error type 3".

Reaching it required nothing unusual: closing the browser, or following a link,
while a page was still loading tears down connections whose handshakes are
still in flight.

The close_notify is now attempted only from `kMacTLS_Connected`, and only when
the write context actually holds a key. `tls13_record_encrypt()` and
`tls13_record_decrypt()` additionally refuse a context with `key_len == 0`
rather than trusting their callers, since the failure mode is memory
corruption rather than a wrong answer.

## §17 — the transport interface, split from Open Transport

*Gateway change, for the Windows port. Not a bug fix.*

`certainly.h` included `<OpenTransport.h>`, so every file that wanted to speak
TLS became a Mac file — including, transitively, Gateway's own portable
transport header. The one thing it needed from it was `EndpointRef`, for the
single `MacTLS_CreateOnEndpoint` parameter.

`ot_transport.h` is now `certainly_transport.h` and names no operating system:
`CTransport` is opaque, `CTSocket` is the platform's own connection handle, and
the eight functions become `ct_transport_*`. `ot_transport.c` becomes
`transport_ot.c`, one implementation of it; `transport_win32.c` is the other.

`certainly.c` read six fields of the transport struct directly —
`ordRelReceived`, `disconnectReceived`, `port`, `lastError`, `state`,
`hostInfo` — which is why the struct could not simply be hidden. Those reads
are now five accessors, one of which (`ct_transport_peer_closed`) replaces the
`ordRelReceived || disconnectReceived` pair that appeared six times. The TLS
core only ever cared that no more bytes were coming, not which of the two ways
the peer had gone.

`ct_socket_close()` exists because `ct_transport_adopt()` takes ownership
unconditionally: a caller that fails before reaching it still has to dispose of
the connection, and `certainly.c` should not have to know that Open Transport
spells that `OTCloseProvider`.

`CERTAINLY_OPEN_TRANSPORT`, set by CMake, selects the implementation and the
`EndpointRef` spelling of `CTSocket`.

## §18 — a TLS record could be sent in part and reported as sent in full

*Found by the Windows port; the bug was always there.*

The TLS 1.3 application-data write path sent the five-byte record header, then
looped over the ciphertext, and broke out of that loop if the transport went
flow controlled — after which it returned the full plaintext length as though
everything had gone. The rest of the record was never sent. The peer received
a truncated record and closed the connection without answering.

Worse, if the *header* send returned zero the function returned 0 and the
caller retried the same plaintext, which encrypted it again — with the next
sequence number. A number the peer never saw had been consumed, so even a
small request could kill the connection.

On Mac OS 9 neither half was reachable in practice: `OTSnd` on a non-blocking
endpoint either takes the whole buffer or returns `kOTFlowErr` having sent
nothing, so partial sends did not occur and a zero-length header send was rare
enough never to be hit. Winsock's `send()` returns a partial count as a matter
of course once the socket buffer fills. The first thing the Windows build did
was fail to refresh an OAuth token, and the Wayback proxy reported the same
thing as `upstream closed before sending a response`.

The record is now staged whole — header and ciphertext in one buffer, which is
why `tls13_enc_buf` grew by five bytes — and `MacTLS_Write` refuses to encrypt
another until the last one has left entirely, so a sequence number is never
consumed twice. `MacTLS_Pump` drains what is still owed, because the ordinary
shape of an exchange is a caller that writes a whole request and then only
reads.

## §19 — HKDF-Expand-Label wrote past the end of every key and IV buffer

*Found by the Windows port. A stack buffer overflow, present on Mac OS 9 too.*

`hkdf_expand_label()` finished with

```c
br_hmac_out(&mc, out);
```

and `br_hmac_out()` always writes the hash function's entire output — 32 bytes
for SHA-256. `out_len` is routinely smaller than that: a TLS 1.3 IV is 12
bytes, and an AES-128 key is 16. So every IV derivation wrote **20 bytes past
the end of the caller's buffer**, and every AES-128 key derivation wrote 16
past. In `tls13_state_recv_finished` the destination buffers are adjacent
locals:

```c
unsigned char client_key[32], client_iv[12];
unsigned char server_key[32], server_iv[12];
```

so the overflow landed on whichever local the compiler had placed next — and
which key it destroyed was therefore a property of the stack layout, not of the
source. That is why the same code was correct on PowerPC, corrupted the
*client* application traffic key on x86 at `-Os`, and corrupted the *server's*
at `-O0`. The handshake keys survived in every layout, which is what made this
so hard to see: the handshake completed, the Finished was accepted, and only
the first application record failed.

The comment left at that call said the caller "gets the first out_len bytes",
which describes what was intended rather than what the code did.

It now expands into a full-sized block and copies out only what was asked for.

Getting here took a known-answer test proving the cipher itself was correct in
both directions, byte counters proving the request reached the wire, key
fingerprints proving the stash was intact, and finally the peer's own alert —
`bad_record_mac` — which the library had been discarding (§18 area, fixed in
the same session). Each of those eliminated a layer that had otherwise been
guessed at.

## §20 — decrypted application data was discarded when the reader was behind

*Found by the Windows port's log reaching Mac OS 9. A data-loss bug on both.*

`tls13_recv_records()` appended each decrypted record to `tls13_app_buf` like
this:

```c
/* Append decrypted data (truncate if buffer full) */
size_t space = sizeof(ctx->tls13_app_buf) - ctx->tls13_app_len;
size_t copy = (dec_len < space) ? dec_len : space;
```

`tls13_app_buf` holds exactly one maximum-sized record. Whenever the
application read more slowly than the peer sent — which, on a 1999 Macintosh
behind a browser fetching a dozen resources, is the ordinary case — unread
bytes were still in the buffer when the next full record arrived, and the
overflow was thrown away without a word.

The result is a hole punched into the middle of the byte stream. A
Content-Length body arrives short and the session sits until its idle timeout.
A chunked body loses sync with its own framing a few kilobytes later, and the
proxy reports `malformed chunked body` — which is what led here, since the
chunked decoder was the obvious suspect and turned out to be innocent: the
exact failing response, captured from the live server, replayed through it
cleanly at every slice size and every drain rate.

The record is now left in `tls13_recv_buf` until there is room for its
plaintext. That costs nothing — the bytes are already buffered, and the next
pump finds them again once the reader has drained. The append can no longer
overflow, and a short copy is treated as an error rather than silently
tolerated.

This also explains the pauses: a body that never completes holds its session
until the 45-second idle timeout, and a page whose resources each do that loads
at the speed of the timeout rather than the network.

### §20 correction — the room test refused every full-sized record

The check added above compared the *ciphertext* length against the plaintext
buffer. A maximum-sized TLS 1.3 record is 16384 bytes of plaintext and
therefore 16401 on the wire — a content type byte and a 16-byte tag on top —
while `tls13_app_buf` holds exactly 16384. So `16384 - 0 < 16401` was true even
with a completely empty buffer, the record was never decrypted, and the
connection sat until its idle timeout.

It reached anything large enough to fill one record. A 24 KB image from the
Internet Archive does: nginx sends one maximum-sized record and one small one,
and the first could never be accepted.

The test now subtracts the overhead, so an empty buffer always has room for a
legal record. A record claiming more plaintext than the buffer can ever hold is
treated as an error rather than waited on, since no amount of draining would
make space for it.

---

## §21 — the CryptoAPI import kept the binary off Windows 95 RTM and NT 3.51

*Gateway change, for older Windows than the port first aimed at. Not a bug fix
on any system that already ran.*

Reported as [brunocastello/Gateway#1](https://github.com/brunocastello/Gateway/issues/1)
by roytam1, who had solved the same problem in RetroZilla.

`entropy_win32.c` called `CryptAcquireContextA`, `CryptGenRandom` and
`CryptReleaseContext` directly, and handled failure carefully — a machine whose
default key container had never been created simply lost that one source. What
it could not handle is the case where the functions are not there at all.
CryptoAPI arrived with Windows 95 OSR2 and NT 4.0; on 95 RTM, 95 OSR1 and NT
3.51 the ADVAPI32 that ships with the system exports none of it. Naming those
functions in C puts them in the PE import table, and the loader resolves every
import before the first instruction of the program runs. The result is not one
missing entropy source, it is a dialog about a missing entry point and a
process that never starts. No amount of in-function checking can reach that.

The three names are now looked up with `LoadLibraryA` and `GetProcAddress`, and
the source is skipped when any of them is absent. `<wincrypt.h>` is gone with
them and the handful of types and constants are declared locally, so that a
later edit cannot restore the import by reaching for the obvious spelling.
`SystemFunction036` — `RtlGenRandom` — is tried first, since on XP it is the
same generator without the key-container question; nothing older exports it,
which makes the lookup its own version test.

BearSSL carried a second copy of the same import. `src/inner.h` turns on
`BR_USE_WIN32_RAND` for any `_WIN32` target, which compiles the CryptoAPI
seeder in `src/rand/sysrng.c`. Gateway never used it — `certainly.c` injects
our own pool into every engine before the handshake, which sets
`rng_init_done` to 2 and stops `br_ssl_engine_init_rand` consulting
`br_prng_seeder_system` at all. It is now switched off explicitly in
`Makefile.win32`. The Mac OS 9 build has always run with no seeder compiled in,
so this is not a new configuration, only a newly deliberate one.

### What is left when there is no system PRNG

The pool, which on those systems is the whole story: `QueryPerformanceCounter`,
`GetTickCount`, a FILETIME, cursor position, process and thread IDs and
`GlobalMemoryStatus`, hashed to 32 bytes per handshake. `add_machine()` now
adds the cheap machine-specific sources NSS gathers for the same reason —
volume serial, filesystem and volume names, free and total clusters, computer
name, logical drive bitmap. None of it changes between two runs, so it does
nothing for the difference between one handshake and the next; what it does is
separate this machine from an identical one installed off the same disk.

NSS goes considerably further on this path, walking the temp directory and
shell folders and reading up to 250 KB of file contents into the pool. That is
the right call for a library that cannot yield, and the wrong one here: on a
95-era disk it would stall the cooperative loop for seconds at startup.

Which of the three states a machine is in is now reported through
`MacTLS_EntropySource()` and logged once at startup, so a screenshot of the log
answers the question rather than the Windows version being used to guess at it.
On Mac OS the function returns NULL and nothing is logged — there has never
been a system generator to fall back from, so there is no second state to
distinguish.

### Not fixed here

This does not by itself reach Windows 95 RTM. MinGW-w64 links `msvcrt.dll`,
which only ships with the operating system from 95 OSR2 onward; before that the
system runtime is `crtdll.dll`. That is the real floor, and the installer now
carries a copy for machines that lack one.

NT 3.51 needed a third thing, outside Certainly: its `Shell_NotifyIcon` is an
exported stub that fails with `ERROR_CALL_NOT_IMPLEMENTED`, so Gateway would
have run with no icon, no menu and no way to reach it. `main_win32.c` now looks
that function up rather than importing it, checks what `NIM_ADD` returns, and
falls back to a menu bar on the log window. The lesson of this patch generalised
further than the patch did.

None of it has been run on either system. The imports are checked on every
build; the behaviour is not, and cannot be from here.
