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

---

## §22 — BearSSL accepts the SSLv2-compatible ClientHello framing

*BearSSL patch, in `bearssl/src/ssl/ssl_engine.c`, `bearssl/inc/bearssl_ssl.h`
and `bearssl/src/ssl/ssl_hs_server.c`. The first BearSSL files Gateway
modifies; everything above is Certainly. Contributed by roytam1 as
`roytam1/Gateway@65f2c9f`, taken with its second change dropped — see the end
of this section.*

A vintage browser with "Use SSL 2.0" checked wraps an otherwise TLS-capable
CLIENT-HELLO in the 2-byte SSLv2 record header (high bit set) instead of the
TLS 5-byte header. BearSSL rejected that before any field was read, in
`recvrec_ack()`:

```c
/* Note: right now, we reject clients that try to send
 * a ClientHello in a format compatible with SSL-2.0. ... */
```

so a browser that could have negotiated TLS 1.0 failed with
`BR_ERR_UNSUPPORTED_VERSION` (3) over framing alone.

The engine now detects a high-bit first byte on the very first record (only
then: `version_in` must still be 0 and encryption inactive, otherwise
`BR_ERR_UNEXPECTED`), gathers the whole SSLv2 message — up to `SSL2_MAX_MSG`
(2048) bytes, past which `BR_ERR_TOO_LARGE` — and rewrites it in place to a
plain TLS ClientHello record:

* `CLIENT-HELLO` (msg type 1) only; anything else is `BR_ERR_UNEXPECTED`.
  A version below 3.0 is still `BR_ERR_UNSUPPORTED_VERSION`: pure SSLv2 and
  its crypto are not implemented and never will be.
* TLS suites arrive as `0x00 xx xx` and are kept; the SSLv2 3DES spec
  `0x07 0x00 0xC0` maps to `TLS_RSA_WITH_3DES_EDE_CBC_SHA` (`0x00 0x0A`),
  the only suite these browsers share with BearSSL. Other SSLv2-specific
  specs (RC4, single DES) are dropped, and an empty remainder is
  `BR_ERR_BAD_CIPHER_SUITE`. The challenge (required 16–32 bytes) becomes
  `client_random`, left-padded with zeros per RFC 6101 Appendix E. A session
  id over 32 bytes is `BR_ERR_OVERSIZED_ID`; one of a legal length is
  dropped rather than echoed, because RFC 5246 Appendix E.2 requires the
  field empty in a converted hello — a V2 session id can only name an
  SSLv2 session, which is not resumable here.
* The rewritten hello carries no extensions — SSLv2 has none — so there is
  no SNI or secure-renegotiation signalling; the handshake already handles
  their absence with defaults. Gateway does not need SNI: the leaf is minted
  from the host in the `CONNECT` line, not from the hello.

`record_type_in = 0x80` (`SSL2_MARKER`, never a real TLS content type) marks
a message being gathered. Split delivery works: the marker is set once the
5-byte read completes and conversion runs when the remainder arrives.

### The transcript hash

The Finished messages cover every handshake byte, and the client hashes the
hello it *sent*, not the one we rewrote. RFC 5246 Appendix E.2 excludes only
the V2 length header, so the conversion feeds `msg_type` onward straight into
the multihasher and sets `hash_skip` (new field, `bearssl_ssl.h`) to the
length of the re-encoded message. The two read paths in `ssl_hs_server.c`
count that down instead of hashing, so the rewritten bytes are never hashed
twice. Without this both sides compute different verify data and every
handshake fails at the last message.

The pre-feed is safe because the multihasher is pristine at that point:
`ssl_hs_server.t0` runs `multihash-init` inside `do-handshake` immediately
before `read-ClientHello`, and the V2 framing is only accepted when
`version_in` is still 0, so a converted hello is always the first thing in
the transcript.

`ssl_hs_server.c` is generated from `ssl_hs_server.t0` by the T0 compiler,
which Gateway does not run. The two hash gates live in the generated file
only; regenerating it drops them, and the Finished failure that follows will
not look like a missing patch. A comment at the top of the file says so.

### Not taken

The original commit carried a second, undocumented change in
`jump_handshake()`: a ClientHello whose body version was `0x0300` had the
byte rewritten to `0x0301` so BearSSL's `version_min` would accept it. That
is left out. It rewrites the byte before the handshake code hashes it, and
`hash_skip` does not cover that path, so an ordinary SSL 3.0 hello would
break its own Finished; the guard is three bytes of pattern against a buffer
that is frequently positioned mid-message, so it can fire by chance; and a
client that offered `0x0300` as its maximum is entitled to reject the
`0x0301` ServerHello it would get back. A browser that can do TLS 1.0 puts
`0x0301` in the body; one that cannot is asking for SSL 3.0, which BearSSL
does not implement. §23 names the offered version in the log instead.

### Verification

Not verified here. roytam1 reports testing the conversion on the host with
mingw-gcc against the vendored tree: an IE-style hello (version `0x0301`,
ciphers `0700C0`/`00000A`/`010080`, 16-byte challenge) fed byte-at-a-time
converts to record type 22 with the padded random and the two 3DES suites; a
normal TLS header is still accepted; a `0x0002` version still fails
`UNSUPPORTED_VERSION`; a second SSLv2 header fails `UNEXPECTED`. What remains
untested on both sides is the part that only a real browser can exercise: a
complete handshake through a converted hello, Finished included.

---

## §23 — the TLS 1.2 fallback could not revive a failed engine

*Certainly patch, in `src/certainly.c`.*

Certainly attempts TLS 1.3 with its own state machine and falls back to
BearSSL's T0 engine when the server will not do 1.3 — by closing the
connection, reconnecting, and calling `br_ssl_client_reset()`. That reset is
not enough on its own, and the fallback discarded what it returned.

`br_ssl_engine_fail()` sets two things:

```c
if (rc->iomode != BR_IO_FAILED) {
        rc->iomode = BR_IO_FAILED;
        rc->err = err;
}
```

`br_ssl_client_reset()` calls `br_ssl_engine_hs_reset()`, which clears the
handshake state, the T0 stacks, `alert` and `shutdown_recv` — and neither
`iomode` nor `err`. It then ends with

```c
return br_ssl_engine_last_error(&cc->eng) == BR_ERR_OK;
```

so against an engine that has ever failed it returns 0 and leaves it failed.
The only entry point that puts `iomode` back to `BR_IO_INOUT` and `err` back
to `BR_ERR_OK` is `br_ssl_engine_set_buffers_bidi()`, reached through
`br_ssl_engine_set_buffer()`. The fallback never called it, so a client
context that failed once reported the same stale number for the rest of its
life, and the caller — which ignored the return — went on driving it.

The fallback now re-arms the buffer before resetting the client and treats a
failed reset as fatal rather than looping on a context that can no longer
handshake. Suites, versions, trust anchors and the seeded RNG all survive
`set_buffer`; only the record state is reset, which is exactly what a
reconnect wants.

### Telling the two handshakes apart

`MacTLS_GetBearSSLError()` reports whichever leg failed, and both use
`BR_ERR_*` numbering, so `TLS 1` in the log could be the 1.3 parser rejecting
a ServerHello field or the 1.2 engine refusing to start — opposite faults with
the same number. `MacTLS_GetTls13Error()` now says which, and `gw_stream.c`
puts it in the line: `TLS 1 1.3` against `TLS 1 1.2`.

### What this does and does not explain

It was found looking for why `www.floodgap.com` fails. That host has no TLS
1.3 at all — a 1.3 ClientHello draws a `handshake_failure` alert — so it
always takes this path, where almost nothing else goes, and the log shows four
identical reconnects each ending in `TLS 1`. A sticky error reproduces exactly
like that, where a genuine protocol failure would tend to vary.

That is a motive, not a proof. Nothing here establishes that `err` was
actually non-zero at the moment of the fallback; the engine's handshake is
deliberately never started before then (`certainly.c`, the note above
`MacTLS_Create`), so it should be pristine. The defect is real and worth
fixing on its own terms either way, and the new log marker is what will
identify the leg next time rather than leaving it to inference.

---

## §24 — a failed ServerHello could not say which field it disliked

*Certainly patch, in `src/tls13_handshake.c`.*

The TLS 1.3 ServerHello path had seventeen separate ways to answer
`BR_ERR_BAD_PARAM`. All of them arrive in the log as `TLS 1`, and none of them
says which field was wrong or even which check ran. Diagnosing one cost three
rounds of inference about a server nobody here can packet-capture.

Those sites now answer `0x3000 | __LINE__`. Subtract `0x3000` from the number
in the log and the remainder is the line in `tls13_handshake.c` — for the
build the log came from, which the `GW_BUILD_ID` stamp identifies, since the
line numbers move whenever the file does. The rest of the file still answers
`BR_ERR_BAD_PARAM`; only the path that has needed finding was changed.

`gw_stream.c` prints any code at or above `0x1000` in hex, so the encoded
families read straight off: `0x1LLDD` is an alert the peer sent, `0x2000|type`
a record that was not one, `0x3000|line` a rejected field. Plain `BR_ERR_*`
numbers stay decimal, which is how they are quoted everywhere else.

### The ordering fault behind it

The record header was read in the wrong order:

```c
record_type = recv_buf[0];
record_len = get_u16(recv_buf + 3);

if (record_len > TLS13_MAX_CIPHERTEXT) {   /* ran first */
        ...BAD_PARAM
}
...
if (record_type != TLS13_CT_HANDSHAKE) {   /* could not be reached */
        hs->error = 0x2000 | (int)record_type;
}
```

The length only means anything once the first byte says this is a TLS record
at all. A server answering 443 with plain text sends `HTTP/`, whose bytes 3
and 4 read as a 20527-byte record, so the length check rejected it as a bad
parameter without ever looking at the byte that would have explained it — and
the unexpected-type branch below was unreachable for any type whose header
happened to encode an absurd length, which is most of them. The type is now
validated first.

### Not a fix for anything yet

This diagnoses; it does not repair. It was written because
`www.floodgap.com` fails with `TLS 1 1.3` — the 1.3 state machine, not the
1.2 engine that §23 addressed — and none of the seventeen sites could be
ruled in or out from here. The next log from that host names the line.

---

## §25 — a TLS 1.2 ServerHello with no extensions was rejected as malformed

*Certainly patch, in `src/tls13_handshake.c`. This is the one that makes
`www.floodgap.com` reachable.*

`tls13_parse_server_hello()` opened with

```c
/*
 * Minimum ServerHello size:
 * 4 (hs header) + 2 (version) + 32 (random) + 1 (session_id_len) +
 * 2 (cipher suite) + 1 (compression) + 2 (extensions length) = 44
 */
if (msg_len < 44) {
```

The extensions length is not mandatory. RFC 5246 7.4.1.3 makes the whole
extensions block optional in a TLS 1.2 ServerHello — its presence is detected
by whether any bytes follow `compression_method` — so a server with no
extensions to send stops after 42 bytes. TLS 1.3 does require extensions, but
a 1.3 server is not who sends a short hello; recognising a 1.2 one and handing
over to BearSSL is the entire reason this function parses a hello it cannot
use.

Forty lines further down the function already knew that:

```c
if (pos + 2 > msg_len) {
        /* No extensions at all — this is a TLS 1.2 ServerHello */
        return kTLS13_Fallback12;
}
```

which was unreachable for exactly the servers it was written for. The minimum
is now 42. Every field between is bounds-checked individually, so nothing else
had to change: a 42-byte hello walks version, random, an empty session id,
the suite and the compression byte, finds no extensions, and falls back.

### Why floodgap and almost nothing else

`www.floodgap.com` runs HTTPi on AIX and has no TLS 1.3 at all — a 1.3
ClientHello draws a `handshake_failure` — so it always takes the fallback
path, where almost nothing else goes. The size then comes down to what
Certainly asks for. Against OpenSSL's ClientHello the same server answers with
57 bytes, because OpenSSL offers `ec_point_formats` and `renegotiation_info`
and it echoes both:

```
02 00 00 35 03 03 <32-byte random> 00 c0 2f 00
00 0d ff 01 00 01 00 00 0b 00 04 03 00 01 02
```

That is 42 bytes of hello, a 2-byte extensions length and 13 bytes of
extensions. Certainly's ClientHello offers neither extension, so the server
has nothing to put in the block and omits it: 57 − 15 = 42, one byte under the
gate. A server that echoes anything at all clears 44 and was never affected,
which is why this survived every other host.

### How it was found

Not by reading. `TLS 1` in the log was one of seventeen `BR_ERR_BAD_PARAM`
sites and three rounds of inference had not narrowed it. §24 made those sites
answer `0x3000 | __LINE__`; the next log said `TLS 0x3319 1.3`, and
`0x319` is line 793. The line-numbered codes stay, because the next one of
these should cost one build rather than four.

---

## §26 — a connected TLS 1.2 session was put back into handshaking by its own writes

*Certainly patch, in `src/certainly.c`. With §25, this is what makes a
TLS 1.2-only origin work at all.*

The pump classified the engine's state after every cycle:

```c
if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) {
        ctx->state = kMacTLS_Connected;
} else if (st & (BR_SSL_SENDREC | BR_SSL_RECVREC)) {
        /* Only record-level I/O — still handshaking */
        ctx->state = kMacTLS_Handshaking;
}
```

The engine runs on one buffer for both directions —
`br_ssl_engine_set_buffer(&ctx->sc.eng, ctx->iobuf, sizeof(ctx->iobuf), 0)`,
where the trailing 0 is `bidi` — so it works one direction at a time. While an
outgoing record is being pushed out, the engine offers neither `SENDAPP` nor
`RECVAPP` and `br_ssl_engine_current_state()` is `BR_SSL_SENDREC` alone.

That is not a handshake in progress. It is the ordinary condition of a
connected session that has just been written to, which is every session in the
instant after its request goes out. The context went backwards to
`kMacTLS_Handshaking`, and

```c
if (ctx->state != kMacTLS_Connected &&
    ctx->state != kMacTLS_Closing &&
    ctx->state != kMacTLS_Closed) return -1;
```

at the top of `MacTLS_Read()` then answered -1 — a read failure reported on a
connection in perfect health, with no error set anywhere in it. Once
`Connected`, only `BR_SSL_CLOSED` leaves it now.

### What it looked like

```
#8 www.floodgap.com read failed: ok [connected, OT 0, TLS 0, 76.79.210.35]
```

Every part of that line is a consequence. `read failed` is the -1. `ok` is
`GWStream_ErrorText`, because nothing had failed. `TLS 0` is the engine's
error, because there was not one. And the version is missing because
`MacTLS_GetVersion()` also answers only in `Connected` — the same backwards
state, showing up twice in one line and saying so in neither.

### Why nothing caught it sooner

Nothing had ever reached the TLS 1.2 path. Certainly always opens with a TLS
1.3 ClientHello, and §25 was rejecting the ServerHello of every server that
answered it in 1.2, so the fallback ended in an error before any application
data was read. §25 made the path reachable and this was the next thing in it.
Both are needed and neither is sufficient: a TLS 1.2-only origin could not be
fetched from before today.

Most of the web hides this, since a host with TLS 1.3 never goes near either
bug. `www.floodgap.com` is a small hand-written server on AIX with no 1.3 at
all, which is why it was the site that found both.

---

## §27 — the tail of every MITM'd response was discarded at close

*Certainly patch, in `src/server.c`, with the matching change in
`src/proxy/gw_httpproxy.c`. This is why images did not appear on a page
fetched over `https` with `connect_mitm`.*

`MacTLS_ServerWrite()` stages plaintext in the engine and flushes it into a
record. The record reaches the socket in `MacTLS_ServerPump()`, which is a
separate call. `MacTLS_ServerClose()` never pumped:

```c
if (s->state == kMacTLS_Connected || s->state == kMacTLS_Handshaking)
        br_ssl_engine_close(&s->sc.eng);

if (s->transport != NULL) {
        ct_transport_close(s->transport);
        ct_transport_destroy(s->transport);
```

and the proxy closed the moment the last byte had been *written*:

```c
int r = session_flush(s);
if (r != 0) s->state = kHPDone;      /* -> GWStream_Close(&s->cli) */
```

So whatever had not yet been pumped was thrown away with the transport. On a
plaintext hop this is invisible, because the bytes are in the socket by the
time the write returns and the operating system flushes on close. On a TLS hop
they are in a buffer Gateway owns.

`GWStream_SendPending()` now answers whether the engine still holds records,
and `kHPFlushAndClose` waits for it. It cannot hang: pending output counts as
waiting on the client in the idle check, so a browser that stops reading ends
the session on the ordinary timeout.

### Why it looked like an image problem

Size decided it. A small body fits one write, so the whole response was staged
and then discarded — nothing arrived. A large one crosses many
`session_step()` passes, each of which pumps, so all but the last records were
already gone. Google's page is chunked and long and rendered; its logo is 2478
bytes and did not.

The same page over plain `http` was whole, because Google serves its
subresources under the scheme of the document: from an `http://` page the
images are fetched over `http` and never touch this path at all. That is what
made it look like a difference between two schemes rather than between two
transports, and it is why the upstream side was searched first — the logs show
every resource fetched identically in both cases, which was true and was not
the question.

### Also here

`MacTLS_ServerPump()` carried the same state fault as §26 — record-level I/O
alone read as "still handshaking" — and it is fixed the same way. It was
latent: `MacTLS_ServerRead()` and `MacTLS_ServerWrite()` consult the engine
directly rather than `s->state`, and `GWStream_Pump()` will not move a stream
back out of Ready, so nothing acted on it. Left in place it would have been
waiting for the first caller that did.

### §22 addendum — two SSLv2 specs can mean one TLS suite

Internet Explorer 5's SSLv2-compatible hello lists both SSLv2's own 3DES
(`07 00 C0`) and `TLS_RSA_WITH_3DES_EDE_CBC_SHA` (`00 00 0A`). The converter
maps the first onto the second, so the rewritten hello carried `0x000A` twice
— a list no client would have sent. Found in the log as
`4 suite(s) [0004,000a,000a,0003]` on the connection IE opened and abandoned,
against `3 suite(s) [0004,000a,0003]` on the one it kept. Translated suites
are now checked against the ones already emitted.

## §28 — SSL 3.0, for browsers that have no TLS at all

Contributed by [roytam1](https://github.com/roytam1/Gateway/tree/myfix2), who
verified it against Netscape 3.04 Gold (Export) and 16-bit Internet Explorer 5.

BearSSL implements TLS 1.0 and up. `BR_SSL30` existed as a header constant with
no implementation behind it, which is why `connect_mitm` could not serve
Netscape 3, IE 3 or IE 4 — the only common ground with those browsers is
SSL 3.0, and there was none to offer.

The constraint that shapes the whole patch is that BearSSL's handshake logic is
**not C**. `ssl_hs_client.c` and `ssl_hs_server.c` are generated from
`ssl_hs_*.t0` by a Forth-like compiler that is not part of this tree, so the
bytecode cannot be extended to parse a second wire format. Everything below
therefore either lives in the `.t0` sources (regenerated) or reconciles the two
formats at the record layer, before the bytecode looks at the bytes.

**Key schedule and MAC.** SSL 3.0 derives its master secret and key block with
an `A`/`BB`/`CCC` letter-pad construction (RFC 6101 §6), not a PRF, and its
record MAC nests the secret with `0x36`/`0x5C` pads rather than XOR-ing it into
an HMAC key (§5.2.3.1). Both are in `src/ssl3/`, selected by
`session.version == BR_SSL30` at the `compute_master` and `compute_key_block`
call sites. The TLS PRFs stay installed: TLS 1.0 and 1.1 still need them.

**Record layer.** `ssl_rec_rc4.c` adds RC4 with the SSL 3.0 MAC, and cipher
type 10 in the suite table carries `SSL3_RSA_EXPORT_RC4_40_MD5` (0x0003),
`SSL3_RSA_RC4_128_MD5` (0x0004) and `SSL3_RSA_RC4_128_SHA` (0x0005). The
export suite's 40-bit key is expanded to a 16-byte RC4 key the way §6.2.2
specifies. `ssl_scert_single_rsa.c` refuses all three above SSL 3.0, so no
TLS connection can land on RC4.

**The two wire differences**, both bridged in `ssl_engine.c` and both fired at
most once per connection, under conditions tight enough to be provable — a
plaintext or encrypted handshake record as appropriate, SSL 3.0 negotiated with
one of the three suites, the buffer positioned exactly at a record start
holding exactly one complete message of the expected shape. Anything else is
left untouched to fail as it did before:

* **ClientKeyExchange has no length prefix.** SSL 3.0 sends the RSA-encrypted
  pre-master secret raw (§5.2.2); the bytecode reads a TLS-style 16-bit length
  and dies on the first two ciphertext bytes. `ssl3_rewrite_cke` inserts the
  missing length in place and grows the record by two.
* **Finished is 36 bytes, not 12** — an MD5 and a SHA-1 over the handshake
  transcript with a `CLNT`/`SRVR` sender (§5.6.9). `ssl3_rewrite_finished`
  verifies the real 36 bytes in constant time and only then shrinks the message
  to the 12 zero bytes the bytecode expects; `compute-Finished-inner` is
  patched to produce zeros for SSL 3.0 so the comparison it makes is
  zeros against zeros. **A Finished that does not verify is left alone and
  fails the handshake** — the check is not bypassed, it is moved.
  `br_ssl_engine_flush_record` expands our own Finished the same way on the way
  out.

That verification needs the raw transcript, which BearSSL does not keep — the
multihash retains digests only, and the record-layer rewrites here and in §22
mean the parsed bytes are not the hashed ones. `hs_transcript` collects exactly
what is fed to the transcript hash, and `hs_transcript_full` latches on
overflow, which disables the bridge and so fails closed.

**Two things found along the way**, both independent of SSL 3.0:

* §22's `hash_skip` had been applied to the generated `ssl_hs_server.c` and to
  no `.t0` file, so regenerating the bytecode would have silently dropped it.
  It is in `ssl_hs_common.t0` now, where it survives a regeneration.
* `BR_SSE2` was on for any i386 target. The Windows build's floor is Windows 95
  and NT 3.51, which run on 486 and original Pentium hardware. BearSSL
  dispatches on CPUID at run time, so this was belt and braces — but the belt
  is free here, where throughput is bounded by a 1997 browser.

**Reached only through `connect_mitm`, and only while `allow_sslv3` is 1**
(the default). `ssl3_server_init` widens the server engine's floor to 0x0300;
with the preference off it returns before doing so and nothing else in the
patch can be entered. BearSSL's server picks the highest version in common, so
a browser that can manage TLS 1.0 still gets TLS 1.0.

**Cost.** `hs_transcript` is 4 KB in every `br_ssl_engine_context`, including
the client engines that talk TLS 1.2 and 1.3 upstream and will never use it.
At the default twelve concurrent sessions that is under 100 KB against an 8 MB
partition, which is why it is a fixed buffer rather than an allocation.
