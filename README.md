# Gateway

A TLS 1.3 gateway and proxy that runs **on** Mac OS 9, not in front of it.

Gateway is a native Classic Toolbox (pre-Carbon) application for PowerPC Macs.
It sits in the background, listens on a few local ports, and terminates modern
TLS on behalf of applications that were written before it existed — Classilla,
Outlook Express 5, `git`. The vintage side of every connection stays plaintext
and stays on the same machine; only the modern side crosses the network.

* **Creator code:** `GT9A`
* **Built with:** [Retro68](https://github.com/autc04/Retro68) (GCC 12), C++17
  for the Toolbox shell and C99 for everything else
* **TLS:** [Certainly](https://github.com/minorbug/certainly) over
  [BearSSL](https://bearssl.org), vendored and patched
* **Builds in CI only** — see `.github/workflows/build.yml`

> This is a hobby project pointed at a 27-year-old operating system with no
> memory protection, no ASLR and no privilege separation. Certainly's own
> README puts it well: research software, no warranty. Do not put anything
> through it you would regret losing.

---

## Module 1 — HTTP proxy on `:8765`

Three request shapes:

| Shape | Client |
|---|---|
| `GET http://host/path HTTP/1.0` | any classic forward-proxy client |
| `GET https://host/path HTTP/1.0` | Classilla with `network.http.proxy.use-http-proxy-for-https = true` |
| `CONNECT host:443 HTTP/1.0` | `git`, or anything else that wants a raw tunnel |

For the first two Gateway terminates TLS itself: it resolves, connects,
handshakes with Certainly, rewrites the request into origin form, and streams
the response back over plaintext HTTP/1.0. Chunked responses are decoded on the
way through, redirects are followed up to five hops, and bodies are capped at
2 MiB. The client hop always carries `Connection: close`, so the body is
EOF-delimited and no length has to be re-advertised.

`CONNECT` is different on purpose: Gateway answers `200 Connection
Established` and then does nothing but bounce bytes through a 32 KB buffer. The
client's own TLS runs end to end, and Gateway never sees inside it.

**Classilla setup:** point the HTTP proxy at Gateway's address and port `8765`,
then set `network.http.proxy.use-http-proxy-for-https` to `true` in
about:config.

## Module 2 — mail splice on `:1993` and `:1587`

Outlook Express 5 talks to Gateway in the clear; Gateway talks IMAPS and SMTPS
outward with `AUTHENTICATE XOAUTH2`.

**Outlook Express 5 setup:** incoming IMAP on port `1993` with SSL **off**;
outgoing SMTP on port `1587` with SSL **off** and authentication **on**. The
password you type there is checked against `local_password` in Gateway's prefs
and never leaves the machine.

OAuth consent happens out of band on a modern computer. Gateway only ever
exchanges a refresh token — dropped into its prefs file — for a short-lived
access token, and reuses that token until it expires.

See `docs/prefs-example.txt` for the whole prefs file. It goes in the System
Preferences folder, named `Gateway Prefs`.

> **Known gap:** Certainly opens TLS at connect time and cannot upgrade an
> already-open socket, so Gateway cannot do `STARTTLS` on port 587.
> `smtp_upstream_port` defaults to 465 (implicit SMTPS). Gmail is fine with
> that; Office 365 documents 587 only. See `docs/inventory.md` §9.

---

## Building

You do not build this locally. Push, and GitHub Actions does it:

* `.github/workflows/build.yml` runs the Retro68 container, stages Apple's
  Universal Interfaces, builds, and uploads `Gateway.dsk`, `Gateway.APPL`,
  `Gateway.bin` and a StuffIt archive for real hardware.
* `.github/workflows/host-tests.yml` compiles the portable protocol code with a
  plain Linux gcc, runs its unit tests, and fails the build if anything in
  `src/portable/` starts reaching for a platform header.

To run the portable tests yourself:

```sh
make -C tests/host test
```

## Layout

```
src/main.cpp          Toolbox shell: window, menus, WaitNextEvent loop
src/gw_core.[ch]      the seam between the UI and the network core
src/gw_config.[ch]    the "Gateway Prefs" file
src/net/gw_net.[ch]   Open Transport listeners, connections, stream abstraction
src/proxy/            Module 1, Module 2, and the OAuth token refresher
src/portable/         protocol grammar with no system headers at all
src/ui/gateway.r      SIZE (8 MB / 4 MB) and vers, as raw data blocks
tests/host/           unit tests for src/portable, built with the host cc
third_party/certainly Certainly + BearSSL, vendored (see PATCHES.md)
third_party/InterfacesAndLibraries
                      Apple's Universal Interfaces, for the Open Transport files
docs/inventory.md     Phase 0: toolchain shape, memory budget, known gaps
```

The one structural rule worth knowing before editing anything:
`src/main.cpp` is compiled against the **Multiversal** Interfaces and must never
see Open Transport, while the network files are compiled against Apple's
**Universal** Interfaces. `src/gw_core.h` — plain C, no system headers — is the
only thing that crosses between them. `docs/inventory.md` §2 explains how the
build keeps that true.

## Licence

MIT for Gateway's own code; see `LICENSE` for the bundled third-party terms.
