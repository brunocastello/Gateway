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
