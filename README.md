# Gateway

A TLS 1.3 gateway and proxy that runs **on** the vintage machine, not in front
of it.

Gateway is a native Classic Toolbox (pre-Carbon) application for PowerPC Macs,
and a native Win32 application for Windows 95 and later. It sits in the
background, listens on a few local ports, and terminates modern TLS on behalf
of applications written before it existed — Classilla, Internet Explorer,
Outlook Express 5, `git`. The vintage side of every connection stays plaintext
and stays on the same machine; only the modern side crosses the network.

> A hobby project pointed at a 27-year-old operating system with no memory
> protection, no ASLR and no privilege separation. Research software, no
> warranty. Do not put anything through it you would regret losing.

## What it does

| Port | Module | |
|---|---|---|
| `8765` | **Web proxy** | `http://`, `https://` and `CONNECT`. Fetches over TLS 1.3 and hands the result back in plaintext. |
| `1993`, `1995`, `1587` | **Mail splice** | IMAP, POP3 and SMTP for a client that cannot do TLS or OAuth. Outlook.com and Gmail. |
| `8888` | **Wayback proxy** | Serves pages from the Internet Archive at a date you choose, with an allow-list of hosts that pass through live. |

Point the browser's HTTP proxy at the machine running Gateway, port `8765`,
and that is the whole setup. Classilla also needs
`network.http.proxy.use-http-proxy-for-https` set to `true` in about:config.

## Getting an old browser onto https://

Two ways, and they are alternatives rather than companions:

**`rewrite_https = 1`** (the default) rewrites `https://` links to `http://`
so the browser never tries a handshake it cannot finish. Type addresses
without a scheme. Everything loads; the address bar and `Secure` cookies do
not follow.

**`connect_mitm = 1`** terminates TLS on the browser's side of a `CONNECT`,
so a typed `https://` URL works with the padlock and the real URL. Gateway
generates a certificate authority on your machine and signs a certificate for
each site, so the browser warns until that authority is trusted — fetch
`http://<gateway-address>:8765/gateway-ca.crt` in the browser to install it,
or click through the warning.

Since 0.3.5 this reaches every browser in range, including ones with no TLS at
all: Netscape 3, Internet Explorer 3 and 4, and IE 5 for Mac OS 9 are served
over SSL 3.0. `allow_sslv3 = 0` refuses it if you would rather not.

## Settings

A preferences window, from **Settings…** in the Apple menu on Mac OS 9 and
**File ▸ Preferences** on Windows. Everything is also a line in a plain text
file — `Gateway Prefs` beside the System Preferences folder, or `Gateway.ini`
beside `Gateway.exe` — which stays hand-editable. Every key is listed in
[`docs/prefs.md`](docs/prefs.md), with an example in
[`docs/prefs-example.txt`](docs/prefs-example.txt).

Turn whole modules off with `http_enabled`, `mail_enabled` and
`wayback_enabled`. `show_window = 0` starts without a window; on Mac OS 9 that
also means no menu bar and no Application menu entry, so stop it with a Quit
Apple event.

Gateway serves its own proxy auto-configuration script at
`http://<gateway-address>:8765/proxy.pac`, which is the only way to say that
some hosts belong on the archive listener and some on the live one.

## Supported systems

**Mac OS 9** (PowerPC), and **Windows 95 through XP**. Verified on Windows Me,
98, 2000 and 95 OSR2 under 86Box, and on Mac OS 9.2. Windows 95 RTM and NT
3.51 are within what the binary's imports allow but have never been run.

## Building

You do not build this locally. Push, and GitHub Actions does it:
`build-macos9.yml` runs the Retro68 container and uploads a disk image and
application; `build-win32.yml` cross-compiles with MinGW-w64 and builds an
installer. `host-tests.yml` compiles the portable parts for Linux and runs the
unit tests, which is the only part that runs anywhere else.

## Layout

```
src/portable/   HTTP, prefs, URL, Base64, chunking, rewriting — no platform headers
src/proxy/      the three modules
src/net/        sockets: Open Transport on Mac OS 9, Winsock on Windows
src/ssl3/       SSL 3.0 key schedule and MAC, for browsers with no TLS
src/ui/         the Mac shell and its preferences window
src/win32/      the Windows shell and its preferences window
third_party/certainly/   Certainly over BearSSL, vendored — see its PATCHES.md
docs/           prefs reference, release notes, porting notes, design notes
```

## Thanks

[Certainly](https://github.com/minorbug/certainly) and
[BearSSL](https://bearssl.org) do the cryptography.
[email-oauth2-proxy](https://github.com/simonrob/email-oauth2-proxy) by Simon
Robinson is where the mail splice comes from: the local-password login, the
XOAUTH2 upstream and the token refresh are his design, carried over to C, and
`get-email-token.py` stands in for its authorisation step.
[Retro68](https://github.com/autc04/Retro68) makes a Mac OS 9 binary from a
modern toolchain. [roytam1](https://github.com/roytam1) contributed the SSL 3.0
implementation that reaches browsers older than TLS, and found the import that
kept Gateway off Windows 95 RTM.

## Licence

MIT. See [LICENSE](LICENSE).
