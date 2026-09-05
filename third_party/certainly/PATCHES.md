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
