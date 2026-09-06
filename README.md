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

## Status

| | |
|---|---|
| HTTP proxy, `http://` and `https://` | working, in daily use |
| `CONNECT` tunnel | implemented, never exercised — no git client for OS 9 |
| IMAP, POP3 and SMTP splices | working, verified end to end against Outlook.com |
| OAuth refresh, including rotated tokens | working |
| Gmail as a provider | implemented, not yet tried against a live account |
| Wayback proxy (Module 3) | designed only — see `docs/module3-wayback.md` |
| Streaming media (Flash video) | working — clear the browser cache once after upgrading |

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

## Module 2 — mail splice on `:1993`, `:1995` and `:1587`

Outlook Express 5 talks to Gateway in the clear; Gateway talks IMAPS, POP3S and
SMTPS outward with `AUTHENTICATE XOAUTH2`.

| Gateway port | Protocol | Upstream |
|---|---|---|
| `1993` | IMAP | `outlook.office365.com:993`, implicit TLS |
| `1995` | POP3 | `outlook.office365.com:995`, implicit TLS |
| `1587` | SMTP | `smtp-mail.outlook.com:587`, STARTTLS |

**Outlook Express 5 setup:** create the account as a plain **IMAP** or **POP**
account — *not* the "Hotmail" account type, which speaks Microsoft's long-dead
HTTPMail protocol. SSL stays **off** on every port; Gateway is the one that
speaks TLS. Authentication is **on** for SMTP. The password you type is checked
against `local_password` in Gateway's prefs and never leaves the machine.

`provider = outlook` (the default) or `provider = gmail` in the prefs file
supplies the OAuth endpoint, the scope and all three mail hostnames; anything
set explicitly still overrides it. Gmail additionally needs
`oauth_client_secret` — Google issues one even for desktop clients — and IMAP
or POP enabled on the account under Forwarding and POP/IMAP.

OAuth consent happens out of band on a modern computer. Gateway only ever
exchanges a refresh token — dropped into its prefs file — for a short-lived
access token, and reuses that token until it expires.

If your token comes from [email-oauth2-proxy](https://github.com/simonrob/email-oauth2-proxy),
its config stores tokens **encrypted** (Fernet, keyed off your account password
via PBKDF2). A `refresh_token` beginning `gAAAAA` is ciphertext and will not
work. Decrypt it first, on the modern machine:

```sh
python3 tools/extract-refresh-token.py /path/to/emailproxy.config
```

`docs/prefs-example.txt` is a starting point for the prefs file, which goes in
the System Preferences folder named `Gateway Prefs`. Every setting is
documented in `docs/prefs.md`.

---

## Running with or without a window

`show_window = 1` (the default) opens the log window at launch; `show_window =
0` starts with no window and just runs. The File menu is present either way,
with **Show/Hide Window** and **Quit**, so the window can always be brought
back and Gateway can always be quit. Hiding the window records the choice, so
the next launch starts the same way.

Gateway stays in the Application menu regardless of the setting. The Process
Manager offers exactly one flag for hiding an application —
`modeOnlyBackground` — and it removes it from the Application menu and from
any dock *together*; there is no way to ask for one without the other. A dock
that should not list Gateway has to be told so in the dock's own settings.

To start Gateway with the Mac, put an alias to it in **Startup Items** inside
the System Folder.

If the Finder shows a generic application icon, the desktop database has not
picked Gateway up yet: hold Command-Option through startup to rebuild it.

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
tools/                host-side helpers: OAuth refresh-token extraction,
                      and regenerating the compiled-in CA trust anchors
src/portable/         protocol grammar with no system headers at all
src/ui/gateway.r      SIZE (8 MB / 4 MB) and vers, as raw data blocks
tests/host/           unit tests for src/portable, built with the host cc
third_party/certainly Certainly + BearSSL, vendored (see PATCHES.md)
third_party/InterfacesAndLibraries
                      Apple's Universal Interfaces, for the Open Transport files
docs/prefs.md         reference for every setting
docs/inventory.md     Phase 0: toolchain shape, memory budget, known gaps
docs/porting.md       what it would take to run this on Windows or Mac OS X
docs/module3-wayback.md
                      design for the Internet Archive proxy, not yet built
```

The one structural rule worth knowing before editing anything:
`src/main.cpp` is compiled against the **Multiversal** Interfaces and must never
see Open Transport, while the network files are compiled against Apple's
**Universal** Interfaces. `src/gw_core.h` — plain C, no system headers — is the
only thing that crosses between them. `docs/inventory.md` §2 explains how the
build keeps that true.

## Licence

MIT for Gateway's own code; see `LICENSE` for the bundled third-party terms.
