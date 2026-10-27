# Gateway

> [!CAUTION]
> ### 🤖 AI-Generated Code Ahead
> **This entire repository was crafted with the help of AI.**
>
> If you are allergic to neural networks, synthesized functions, or autocomplete on steroids, **this repository is not for you**.
> You are kindly invited to close this page and write your code line-by-line in `ed` or `vim`. Everyone else, enjoy!

Gateway lets old browsers and mail clients reach the modern internet.

It runs in the background on the vintage machine itself — a PowerPC Mac or a
Windows 95-era PC — and handles the modern TLS that Classilla, Internet
Explorer, Netscape, Outlook Express and `git` can't. Your old program talks
plain text to Gateway on the same machine; only Gateway's side of the
connection goes out over the network.

> This is a hobby project for operating systems with no memory protection.
> No warranty. Don't send anything through it you'd regret losing.

## What it does

| Port | What | |
|---|---|---|
| `8765` | **Web proxy** | `http://`, `https://` and `CONNECT`, fetched over TLS 1.3. |
| `1993` `1995` `1587` | **Mail** | IMAP, POP3 and SMTP for Outlook.com and Gmail, for clients with no TLS or OAuth. |
| `8888` | **Wayback proxy** | The web as the Internet Archive saw it on a date you choose. |
| `2222` | **Tunnel** | One local port relayed over TLS to a fixed server — SSH through stunnel, for example. Off by default, and with no password of its own. |

## Browsing

Set your browser's HTTP proxy to the machine running Gateway, port `8765`.
In Classilla, also set `network.http.proxy.use-http-proxy-for-https` to
`true` in about:config.

Old browsers can't speak modern TLS, so Gateway offers two ways around it.
Pick one:

- **`rewrite_https = 1`** (the default) changes `https://` links into
  `http://` ones before the browser sees them. Everything loads, but type
  addresses without `https://`.
- **`connect_mitm = 1`** lets a typed `https://` address work, padlock and
  all. Gateway makes its own certificate authority on your machine; install it
  from `http://<gateway-address>:8765/gateway-ca.crt`, or click through the
  warning. Browsers with no TLS at all — Netscape 3, IE 4, IE 5 for Mac — get
  SSL 3.0 (`allow_sslv3 = 0` turns that off). IE 3 can't complete SSL through
  Gateway and needs the default.

## Mail

1. **Get a token** on a modern computer: download `get-email-token.py` from
   the [releases page](https://github.com/brunocastello/Gateway/releases) and
   run `python3 get-email-token.py` (`py get-email-token.py` on Windows). It
   opens the provider's sign-in page and prints four lines.
2. **Give them to Gateway** in the Mail pane of the settings window, or in the
   prefs file. Choose a `local_password` for your mail client to use.
3. **Set up the mail client**: incoming IMAP `127.0.0.1` port `1993` (or POP3
   `1995`), outgoing SMTP `127.0.0.1` port `1587`, SSL **off** on both, SMTP
   authentication **on**, and your `local_password` as the password.

You only do this once: Gateway saves the new token itself whenever the
provider replaces it.

## Settings

Open **File ▸ Settings…** on the Mac or **File ▸ Preferences…** on Windows.
Every setting is also a line in a plain text file you can edit by hand —
`Gateway Prefs` in the Preferences folder, or `Gateway.ini` beside
`Gateway.exe`. [`docs/prefs.md`](docs/prefs.md) explains every setting.

## Supported systems

- **Mac OS 8.0 to 9.2.2** on PowerPC. Tested on 9.2; earlier versions have
  everything the app needs but haven't been tried.
- **Windows 95 to XP**. Tested on 95 OSR2, 98, Me and 2000.

## Building

Nothing is built locally. Push to GitHub and the Actions workflows build the
Mac app with [Retro68](https://github.com/autc04/Retro68) and the Windows app
with MinGW-w64, and run the unit tests.

## Thanks

[Certainly](https://github.com/minorbug/certainly) and
[BearSSL](https://bearssl.org) do the cryptography.
[email-oauth2-proxy](https://github.com/simonrob/email-oauth2-proxy) by Simon
Robinson designed the mail login and token refresh that Gateway carries over.
[WaybackProxy](https://github.com/richardg867/WaybackProxy) by
[richardg867](https://github.com/richardg867) is the prior art for the archive
proxy; Gateway keeps its settings page compatible but shares none of its code.
[roytam1](https://github.com/roytam1) contributed SSL 3.0 support and found
what kept Gateway off the first release of Windows 95.

## Licence

MIT. See [LICENSE](LICENSE).
