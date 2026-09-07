# Gateway

A TLS 1.3 gateway and proxy that runs **on** the vintage machine, not in front
of it.

Gateway is a native Classic Toolbox (pre-Carbon) application for PowerPC Macs,
and a native Win32 application for Windows 95 OSR2 and later.
It sits in the background, listens on a few local ports, and terminates modern
TLS on behalf of applications that were written before it existed — Classilla,
Outlook Express 5, `git`. The vintage side of every connection stays plaintext
and stays on the same machine; only the modern side crosses the network.

* **Creator code:** `GT9A` (Mac OS 9)
* **Built with:** [Retro68](https://github.com/autc04/Retro68) (GCC 12), C++17
  for the Toolbox shell and C99 for everything else
* **TLS:** [Certainly](https://github.com/minorbug/certainly) over
  [BearSSL](https://bearssl.org), vendored and patched
* **Windows:** MinGW-w64, Winsock 1.1, nothing newer than Windows 95 OSR2
* **Builds in CI only** — `build-macos9.yml` and `build-win32.yml`

> This is a hobby project pointed at a 27-year-old operating system with no
> memory protection, no ASLR and no privilege separation. Certainly's own
> README puts it well: research software, no warranty. Do not put anything
> through it you would regret losing.

---

## Status

| | |
|---|---|
| HTTP proxy, `http://` and `https://` | working, in daily use |
| `CONNECT` tunnel | working — verified with RetroZilla on Windows Me |
| IMAP, POP3 and SMTP splices | working, verified end to end against Outlook.com |
| OAuth refresh, including rotated tokens | working |
| Gmail as a provider | implemented, not yet tried against a live account |
| Windows 95 OSR2 – XP | working — all three modules, verified on Windows Me |
| Wayback proxy (Module 3) on `:8888` | working — archived pages load in IE 5 and iCab |
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
way through and redirects are followed up to five hops. Bodies are not capped
by default — `max_body_mb` sets a ceiling if you want one, but the original
2 MiB limit protected nothing, since bodies stream through a 32 KB buffer, and
it made video impossible. The client hop always carries `Connection: close`.
Upstream, connections are kept alive and pooled, which on this hardware matters
more than anything else: a reused connection skips a TLS handshake, and the
handshake is most of the cost of a request.

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

## Module 3 — Wayback proxy on `:8888`

A second listener that serves the web as it was on a chosen date, from the
Internet Archive. It is a separate port rather than a mode on `:8765` so that
the live web and the archive are both available at once, and a browser picks
between them by proxy setting alone.

```
wayback_date       = 20011231
wayback_tolerance  = 730
```

`wayback_tolerance` is how many days either side of that date a snapshot may
be and still be served; a snapshot outside it is refused, which is what a
missing image on an archived page usually means. Set it to `0` to accept
whatever the archive has nearest. Hosts listed in `wayback_live` pass through
to the present-day web instead, matched with `*` and `?` globs.

Settings can also be changed from the browser, at
`http://web.archive.org/` through the proxy, using the same query parameters
as [WaybackProxy](https://github.com/richardg867/WaybackProxy) so existing
bookmarks keep working. That project is GPL-3 and Gateway is MIT, so this is
an independent implementation written from the archive's public URL scheme;
it is credited as prior art, not borrowed from.

Gateway asks the archive for `id_` snapshots, which are the original bytes
without the archive's own toolbar and link rewriting — a 2001 page arrives as
2001 served it.

---

## Turning modules on and off

Each of the three modules is independent, and a module that is switched off is
never initialised — no listener, and no memory for its sessions.

```
http_enabled    = 1     # Module 1, the HTTP/TLS proxy on :8765
mail_enabled    = 1     # Module 2, the IMAP, POP3 and SMTP splices
wayback_enabled = 1     # Module 3, the Internet Archive proxy on :8888
```

`mail_enabled` also governs the OAuth token refresher, which exists only to
serve the mail splices. Turning every module off is allowed: Gateway says so in
the log and keeps running, so the window can be read and the prefs corrected.

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

The log window is resizable, and scrolls with the scroll bar or from the
keyboard (arrows, Page Up/Down, Home/End). It keeps the last 200 lines. For
more than that, `log_file = 1` mirrors every line to **System Folder :
Application Support : Gateway : Gateway Log.txt**, written as it happens so the
tail survives a crash.

To start Gateway with the Mac, put an alias to it in **Startup Items** inside
the System Folder.

If the Finder shows a generic application icon, the desktop database has not
picked Gateway up yet: hold Command-Option through startup to rebuild it.

## Windows

The same program, the same settings, the same three modules. It runs as a tray
icon: right-click for **Show/Hide Window**, **Start with Windows**, **About
Gateway** and **Quit**; double-clicking shows or hides the log.

Settings live in **Gateway.ini** beside `Gateway.exe` rather than in a profile
directory — 95 and 98 have no user profiles by default and NT puts them
somewhere else again, and keeping them next to the program means the whole
thing moves on a floppy. Every setting is the one documented in
`docs/prefs.md`, and line endings do not matter, so a file written on either
platform works on the other. `log_file = 1` writes `Gateway.log` beside the
executable.

**Windows 95 needs OSR2 or later.** That is where `msvcrt.dll` starts shipping
with the system, so nothing has to be installed alongside Gateway. The binary
imports nothing newer; `docs/porting.md` §3 lists every entry point it uses.

## Building

You do not build this locally. Push, and GitHub Actions does it:

* `.github/workflows/build-macos9.yml` runs the Retro68 container, stages
  Apple's Universal Interfaces, builds, and uploads `Gateway.dsk`,
  `Gateway.APPL`, `Gateway.bin` and a StuffIt archive for real hardware.
* `.github/workflows/build-win32.yml` cross-compiles `Gateway.exe` with
  MinGW-w64 and checks that the result imports nothing the target cannot
  provide.
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
src/win32/            Win32 shell: tray icon, log window, About, resources
src/gw_core.[ch]      the seam between the UI and the network core
src/gw_config.[ch]    the "Gateway Prefs" file
src/net/gw_transport.h    what a platform must provide; names no OS
src/net/gw_net.[ch]       the Open Transport implementation of it
src/net/gw_net_win32.c    the Winsock implementation of it
src/net/gw_stream.c       TCP-or-TLS stream, shared by both
src/gw_plat_{mac,win32}.c preferences and the log file, per platform
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
docs/porting.md       what the Windows port took, and what OS X would take
docs/module3-wayback.md
                      design and decisions for the Internet Archive proxy
```

The one structural rule worth knowing before editing anything:
`src/main.cpp` is compiled against the **Multiversal** Interfaces and must never
see Open Transport, while the network files are compiled against Apple's
**Universal** Interfaces. `src/gw_core.h` — plain C, no system headers — is the
only thing that crosses between them. `docs/inventory.md` §2 explains how the
build keeps that true.

## Thanks

Gateway is a small amount of original work resting on other people's:

* **[BearSSL](https://bearssl.org)** — Thomas Pornin. The reason a TLS 1.3
  client on a 1999 machine is possible at all: self-contained, constant-time,
  and correct unmodified on a big-endian PowerPC and a little-endian x86 alike.
  Every cryptographic bug found in this project was in the code around it.
* **[Certainly](https://github.com/minorbug/certainly)** — minorbug. A TLS 1.3
  client written for Mac OS 9, which is the part nobody else had done. Vendored
  and patched twenty times; `PATCHES.md` records each one, and several are
  fixes that belong upstream rather than adaptations.
* **[Retro68](https://github.com/autc04/Retro68)** — Wolfgang Thaller. The
  cross toolchain and the Multiversal Interfaces. Without it there is no way to
  build a modern C++ Classic application at all.
* **[WaybackProxy](https://github.com/richardg867/WaybackProxy)** —
  richardg867. The prior art for Module 3, and the source of its settings URL,
  so bookmarks made for it keep working here. That project is GPL-3 and Gateway
  is MIT, so no code was taken: Module 3 was written from the archive's public
  URL scheme and observable behaviour. The idea is theirs; only the
  implementation is ours.
* **[email-oauth2-proxy](https://github.com/simonrob/email-oauth2-proxy)** —
  simonrob. Where the out-of-band OAuth consent that produces Gateway's refresh
  token actually happens, and whose config format
  `tools/extract-refresh-token.py` reads.
* **[FrogFind](https://frogfind.com)** — Action Retro. A search engine and page
  simplifier for vintage machines, and a good deal of this project's testing.
* **The Internet Archive**, for keeping the web Module 3 serves.

## Licence

MIT for Gateway's own code; see `LICENSE` for the bundled third-party terms.
