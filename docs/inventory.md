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
| HTTP sessions, 4 concurrent | ~448 KB |
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

## 8. Portability guard

CLAUDE.md rule 8: the HTTP request parser, the OAuth form body and the IMAP
LOGIN extraction must compile with a plain Linux gcc. They live in
`src/portable/`, include no system header beyond `<string.h>`, `<stdio.h>` and
`<stddef.h>`, and are built by `tests/host/Makefile` with the developer's own
compiler. `.github/workflows/host-tests.yml` runs those tests **and** greps the
directory for platform includes, so the rule fails the build rather than
drifting.

## 9. Known gaps

* **SMTP STARTTLS on port 587.** Certainly's only entry point is
  `MacTLS_Create(host, port)`, which opens the socket itself; there is no way
  to hand it an endpoint that is already connected and speaking plaintext. So
  Gateway cannot perform the `STARTTLS` upgrade, and `smtp_upstream_port`
  defaults to **465** (implicit SMTPS). Gmail supports 465. Office 365
  officially documents 587/STARTTLS only, so an Outlook.com account may need
  `MacTLS_CreateFromEndpoint()` added to Certainly before SMTP send works.
  IMAP is unaffected — 993 is implicit TLS already. This is the one part of
  Phase 2 that is blocked on a library change rather than on Gateway.
* **HTTP/1.1 keep-alive to the client** is not implemented and will not be:
  the client hop is always `Connection: close`, which is what makes an
  EOF-delimited body legal and keeps the state machine small.
* **Content codings.** Gateway sends `Accept-Encoding: identity` upstream. An
  origin that ignores that and gzips anyway will have its bytes passed through
  undecoded.
* The build has never been compiled locally. Every check in this document is
  either a source-level fact about the vendored trees or an assertion the CI
  job makes (`test -f` on the staged Open Transport libraries before CMake
  runs).
