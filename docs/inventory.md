# Phase 0 — inventory

What the toolchain actually looks like, what the build depends on, and where
the sharp edges are. Everything below was confirmed against the vendored trees
in this repository and against the sources of the tools that consume them; the
PowerPC build itself runs in CI (`.github/workflows/build.yml`), never locally.

---

## 1. Toolchain shape

| Piece | Value |
|---|---|
| Cross toolchain | Retro68, `ghcr.io/autc04/retro68:latest` |
| Prefix inside the image | `/Retro68-build/toolchain` |
| CMake toolchain file | `$PREFIX/powerpc-apple-macos/cmake/retroppc.toolchain.cmake` |
| Compiler | GCC 12 cross-compiling to PowerPC |
| Languages | C99 for the core, C++17 for `src/main.cpp` |
| Output | `Gateway.dsk`, `Gateway.APPL`, `Gateway.bin`, plus a `.sit` for real hardware |

`retroppc` is the Classic (pre-Carbon) PowerPC target. It is deliberately not
`retrocarbon`: Carbon CFM cannot bind an Open Transport endpoint to a chosen
address, and Gateway's whole job is to **listen** (CLAUDE.md rule 1).

## 2. Interfaces: two sets, kept apart

Retro68 ships the open-source **Multiversal Interfaces**, which cover the
Toolbox but contain no Open Transport at all. Apple's **Universal Interfaces**
do, so both are needed — in different translation units.

`third_party/InterfacesAndLibraries` is Apple's Interfaces & Libraries folder
(MPW 3.5 "Golden Master" layout), vendored from `brunocastello/iWordle`.
Resource forks are stored as AppleDouble sidecars (`._InterfaceLib` and
friends), which is one of the formats Retro68's `ResInfo` and `MakeImport`
recognise on Linux. `.gitattributes` marks the whole tree binary so Git never
rewrites a line ending inside one.

The CI build runs the staging script twice:

```sh
interfaces-and-libraries.sh $PREFIX third_party/InterfacesAndLibraries false true false
interfaces-and-libraries.sh $PREFIX --multiversal                       false true false
```

The trailing `false true false` is `BUILD_68K BUILD_PPC BUILD_CARBON`: PowerPC
only, no 68K conversion pass, no Carbon checks.

* The **first** call builds `$PREFIX/universal/{CIncludes,RIncludes,libppc}`.
  `MakeImport` turns every shared library in `Libraries/SharedLibraries` into a
  linkable import archive, and the static Open Transport client glue from
  `Libraries/PPCLibraries` is copied and wrapped in `.a` archives.
* The **second** call puts the toolchain's own include and lib directories back
  on Multiversal. It only re-points symlinks; `$PREFIX/universal` survives
  untouched (only `--remove` deletes it).

CMake is then told where the staged copy lives:

```sh
cmake .. -DGW_UNIVERSAL_DIR=$PREFIX/universal
```

and hands `$GW_UNIVERSAL_DIR/CIncludes` to the `certainly` and `gw_net` targets
as a **PRIVATE** include directory. PRIVATE is the load-bearing word: a PUBLIC
one would propagate along the link graph into `src/main.cpp` and quietly put
Apple's headers in front of Multiversal there. This is CLAUDE.md rule 3, and it
is enforced by the build files rather than by convention.

The libraries linked from the staged copy, by full path:

```
libppc/libOpenTransportLib.a       CFM import stub
libppc/libOpenTptInternetLib.a     CFM import stub
libppc/libOpenTransportAppPPC.a    static client glue
libppc/libOpenTptInetPPC.a         static client glue
```

Everything else (InterfaceLib and the rest of the Toolbox) resolves through
Multiversal's own import libraries, which the toolchain links by default.

### Which file gets which headers

| Translation unit | Interfaces | Why |
|---|---|---|
| `src/main.cpp` | Multiversal | UI only; never sees Open Transport |
| `src/net/gw_net.c` | Universal | endpoints, listeners, `OTSnd`/`OTRcv` |
| `src/gw_core.c`, `src/gw_config.c` | Universal | reach `gw_net.h` / Toolbox file calls |
| `src/proxy/*.c` | Universal | build on `gw_net.h` |
| `third_party/certainly/src/*.c` | Universal | Certainly is an Open Transport client |
| `src/portable/*.c` | none at all | no system headers, host-testable |
| `third_party/certainly/bearssl/**` | none at all | plain C99 |

`src/gw_core.h` is the seam. It declares nothing but plain C functions on
`int`, `long` and `const char *`, so `main.cpp` can drive the whole proxy
without ever including a header that reaches Open Transport.

## 3. C++ global constructors

Retro68's PowerPC `crt0` does not run C++ static initialisers, so a
constructor at file scope simply never fires and the object is left zeroed
(CLAUDE.md rule 2). Gateway's rules:

* `src/main.cpp` has **no** objects at file scope. The single `GatewayApp` is
  `new`-ed inside `main()` and deleted before returning.
* Everything else in `src/` is C. C file-scope statics are zero-initialised by
  the loader, which is fine and is how the log ring, the session tables and the
  config cache all work.
* The same applies to the vendored C in Certainly and BearSSL.

Session tables and per-session buffers come from `NewPtrClear` / `NewPtr` at
run time, never from large static arrays, so the application heap absorbs them
rather than the data segment.

## 4. SIZE resource and the memory budget

`src/ui/gateway.r` sets **8 MB preferred / 4 MB minimum** (CLAUDE.md rule 5),
written as a raw `data 'SIZE' (-1)` block rather than through `Processes.r`.
That matters: Rez runs against whichever `RIncludes` are symlinked into the
toolchain at the moment it runs, and after the `--multiversal` call that is the
Multiversal set. A resource file with no `#include` at all is immune to which
set won.

Flags word `0x5880` = `acceptSuspendResumeEvents | canBackground |
doesActivateOnFGSwitch | is32BitCompatible`. `canBackground` is the one that
matters for a gateway: Gateway keeps proxying while another application is in
front.

Where the 8 MB goes:

| Consumer | Size |
|---|---|
| HTTP session: request head, response head, rewritten request | 3 × 16 KB |
| HTTP session: read scratch + pending client output | 16 KB + 32 KB |
| HTTP sessions, 8 concurrent (`max_sessions`, clamped to 16) | ~880 KB |
| Mail session: four line/queue buffers | 4 × 4 KB |
| Mail sessions, 4 concurrent | 64 KB |
| Token refresh: request + response | 16 KB |
| BearSSL X.509 chain validation | hundreds of KB, transient |
| Certainly's TLS 1.3 write path | a 16 KB record buffer **on the stack** |

That last row is the reason the minimum is 4 MB rather than something smaller:
`MacTLS_Write` declares `unsigned char ciphertext[16384 + 17]` as a local, so
every TLS write puts 16 KB on the stack.

Response bodies **stream**. The 2 MiB cap from CLAUDE.md is enforced with a
counter, not a 2 MiB buffer — four buffered 2 MiB bodies would not fit in the
partition at all. Chunked bodies are decoded incrementally on the way through.

## 5. Cooperative loop

One `WaitNextEvent` loop in `src/main.cpp`. Every pass calls `GW_Poll()` once,
which polls three listeners, the token refresher, four HTTP sessions and four
mail sessions, and returns. Nothing blocks:

* Open Transport endpoints are asynchronous and non-blocking. Notifiers run at
  interrupt time and do nothing but set `volatile` flags on the owning struct;
  all real work happens in the pump functions at application time.
* `OTSnd` returning `kOTFlowErr` and `OTRcv` returning `kOTNoDataErr` are
  ordinary "try again next slice" answers, not errors.
* Certainly's `MacTLS_Pump` follows the same contract, so a TLS handshake makes
  a little progress per pass and yields.
* The sleep value passed to `WaitNextEvent` is 1 tick while any splice is live
  and 10 ticks when idle, so an idle Gateway costs almost nothing.

Concurrency starts at 4 splices (`GW_MAX_SESSIONS`), plus 4 mail sessions.
Connections that arrive with every slot busy are refused rather than queued.

## 6. Networking and binding

Listeners bind `kOTAnyInetAddress` — Open Transport's `INADDR_ANY` — with a
`tilisten,tcp` configuration. Two consequences, both deliberate:

* Both `127.0.0.1` and the address from the TCP/IP control panel reach the
  same listener, which is CLAUDE.md rule 7. OT's loopback is not dependable
  enough to rely on alone.
* `tilisten` serialises connection indications so exactly one `T_LISTEN` is
  outstanding at a time. Without it a server has to track several pending
  `TCall`s by sequence number, which is more state than a single-threaded
  cooperative proxy wants.

The accept path is: `T_LISTEN` → `OTListen` → open and bind a fresh endpoint
with `qlen = 0` → `OTAccept` → `T_PASSCON` on the new endpoint.

## 7. TLS

Certainly (MIT) is vendored in `third_party/certainly` rather than referenced
as a submodule, because it carries a local fix.

**`tls13_record.c` was not checking the ChaCha20-Poly1305 authentication tag.**
Upstream `tls13_record_decrypt()` had:

```c
ok = 1; /* poly1305_run handles verification internally */
```

BearSSL's `br_poly1305_*_run()` does no such thing: on decryption it overwrites
the caller's tag buffer with the tag it computed and leaves the comparison to
the caller. As shipped, a forged record on a `TLS_CHACHA20_POLY1305_SHA256`
connection would decrypt "successfully". The patch stashes the received tag
first and compares it with a constant-time `tls13_ct_equal()` ported from the
equivalent check in `mplsllc/macTLS`. The AES-GCM branch was already correct
via `br_gcm_check_tag()`. Details in `third_party/certainly/PATCHES.md`.

## 7a. TLS buffer sizing

Records are decrypted **in place**, so every buffer a record passes through has
to be sized against the *ciphertext* limit, not the plaintext one. RFC 8446
§5.2: `TLSInnerPlaintext` is at most 2^14 = 16384 bytes, and
`TLSCiphertext.length` is at most 2^14 + 256 = 16640 to cover the content-type
byte, padding and the AEAD tag. Certainly had all of these sized at 16384, and
its handshake message buffer at 4096, which is smaller than a routine
certificate chain. See `third_party/certainly/PATCHES.md` §3.

Per TLS connection, after the fix:

| Buffer | Size |
|---|---|
| `iobuf` (BearSSL, TLS 1.2 path) | 16709 |
| `tls13_recv_buf` | 16645 |
| `tls13_app_buf` | 16384 |
| `tls13_dec_buf` | 16640 |
| `tls13_enc_buf` | 16401 |
| `hs13.msg_buf` | 16384 |
| `hs13.plain_buf` | 16640 |
| BearSSL client + X.509 contexts, key schedule | ~10 KB |

That is roughly **126 KB per `MacTLS_Context`**. Gateway opens at most nine at
once — four HTTP upstreams, four mail upstreams and the token refresher — so
about 1.1 MB, on top of ~400 KB of session buffers. Comfortable at the 8 MB
preferred size; the 4 MB minimum is the real floor.

## 8. Portability guard

CLAUDE.md rule 8: the HTTP request parser, the OAuth form body and the IMAP
LOGIN extraction must compile with a plain Linux gcc. They live in
`src/portable/`, include no system header beyond `<string.h>`, `<stdio.h>` and
`<stddef.h>`, and are built by `tests/host/Makefile` with the developer's own
compiler. `.github/workflows/host-tests.yml` runs those tests **and** greps the
directory for platform includes, so the rule fails the build rather than
drifting.

## 8a. Process visibility

`Processes.h` defines exactly one flag that hides a running application:

```
modeOnlyBackground = 0x00000400
```

It comes from the `SIZE` resource, the Process Manager reads it at launch, and
it removes the application from the Application menu. A dock has nothing else
to go on, so it takes the entry away there too. There is no way for an
application to appear in one and not the other, and no runtime call to change
any of it.

Gateway therefore does not try. `show_window` controls the window and nothing
else, and the menu bar is always present so the application can always be
quit.

An earlier version set `modeOnlyBackground` the moment the window was hidden.
That left a running application with no window, no menu bar and no Application
menu entry — nothing to quit it with short of restarting the machine. Hiding a
window must never be the thing that makes an application unreachable.
`ClearFacelessFlag()` is what remains: it only ever *clears* the bit, so a file
left carrying it from that version repairs itself on the next launch. The edit
goes through the resource map the Process Manager already opened and closes
nothing — reopening the file with `FSpOpenResFile` was a separate bug, since
the Resource Manager returns the **existing** refNum and the matching
`CloseResFile` closed the application's own resources.

## 9. Known gaps

* ~~**SMTP STARTTLS on port 587.**~~ Closed. Certainly gained
  `MacTLS_CreateOnEndpoint()`, so Gateway connects in the clear, speaks
  EHLO/STARTTLS itself, and hands the endpoint over for the handshake. The
  defaults are now `smtp-mail.outlook.com:587` with `smtp_starttls = 1`, which
  is what personal Outlook.com and Hotmail accounts require; set
  `smtp_starttls = 0` with port 465 for a provider offering implicit TLS.
  See `third_party/certainly/PATCHES.md` §5.
* **Trust anchors are compiled in and are all Gateway will ever trust.** Mac OS
  9 has no usable system trust store, so a site whose chain ends outside
  `third_party/certainly/src/ca_roots.c` fails with
  `BR_ERR_X509_NOT_TRUSTED` (62) and there is nothing the user can do about it
  on the machine. The list is generated by `tools/generate_ca_roots.py` and
  currently holds 29 roots. Regenerate it when a site fails verification that
  a modern browser accepts.
* **TLS 1.3 offers ChaCha20-Poly1305 and AES-128-GCM only.**
  `TLS_AES_256_GCM_SHA384` is withheld because it is the one suite needing a
  SHA-384 transcript, and the transcript must be started before the server has
  picked a suite. Microsoft's endpoints select it whenever it is offered,
  regardless of client order, so offering it guaranteed a failed handshake.
  See `third_party/certainly/PATCHES.md` §13.
* **TLS 1.3 key exchange offers X25519 and P-256.** Certainly shipped with
  X25519 only, which no Microsoft endpoint accepts — see
  `third_party/certainly/PATCHES.md` §12. Any host requiring a group outside
  those two will still fail; the ClientHello carries a share for both so no
  HelloRetryRequest is needed for either.
* ~~**Streaming media does not work.**~~ Fixed. `Content-Length` is forwarded
  when the body is untouched, redirects are passed to the client unless it
  could not follow them itself, and the 2 MiB body cap is gone. See
  `docs/issue-flash-video.md` for the sequence, including the way the two old
  bugs together left a truncated copy in the browser cache that survived the
  fix.
* **HTTP/1.1 keep-alive to the client** is not implemented and will not be:
  the client hop is always `Connection: close`, which is what makes an
  EOF-delimited body legal and keeps the state machine small.
* **Content codings.** Gateway sends `Accept-Encoding: identity` upstream. An
  origin that ignores that and gzips anyway will have its bytes passed through
  undecoded.
* **The `CONNECT` tunnel is unverified.** Module 1 shape 3 is implemented and
  reachable, but there is no git client for Mac OS 9 to point at it, so
  nothing has ever opened a tunnel through it in anger. Phase 3's "raw
  CONNECT verification for git" stays open for that reason rather than for
  want of code.
* **Asynchronous OT calls do not copy their arguments.** This bit twice, and it
  is the single most important thing to know when touching this code.
  `OTInetStringToAddress` reads the hostname when the resolver runs, and
  `OTConnect` reads the address when it sends the SYN — both long after the
  call returns. Certainly passed the caller's hostname pointer and, worse, a
  stack-local `InetAddress` to `OTConnect`, so it dialled whatever had reused
  that frame. Anything handed to an async OT call must live at least as long as
  the transport. See `third_party/certainly/PATCHES.md` §6 and §9.
* **Remove a notifier before closing its provider.** The notifier's context is
  usually the struct about to be freed, and Open Transport can still deliver an
  event to a provider that is being closed with an operation outstanding. The
  resulting write into freed memory corrupts the heap and crashes somewhere
  else entirely. Both Gateway and Certainly had this; see
  `third_party/certainly/PATCHES.md` §15.
* **`T_DISCONNECT` carries no reason in the notifier.** Its `result` argument is
  always 0; the reason is in the `TDiscon` from `OTRcvDisconnect()`, which must
  be called anyway or the endpoint fails every later call with `kOTLookErr`.
* **email-oauth2-proxy stores its tokens encrypted** (Fernet, keyed from the
  account password with PBKDF2-HMAC-SHA256 over `token_salt` /
  `token_iterations`). A `refresh_token` beginning `gAAAAA` is ciphertext, and
  Gateway has no way to use it. `tools/extract-refresh-token.py` decrypts it on
  the host side; doing it on the Mac would mean 1.2 million PBKDF2 iterations
  on a PowerPC, for a value that only has to be extracted once.
* **Prefs line endings** were a real trap: the parser split on LF only, so a
  file typed on the Mac (CR endings) parsed as a single comment line and every
  setting silently fell back to its default — including `local_password`, which
  made every mail login fail as a bad password. All three conventions are
  accepted now, `tests/host` covers them, and `GWConfig_Load()` logs whether
  `local_password` actually parsed.
* The build has never been compiled locally. Every check in this document is
  either a source-level fact about the vendored trees or an assertion the CI
  job makes (`test -f` on the staged Open Transport libraries before CMake
  runs).
