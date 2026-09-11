# CLAUDE.md

## Project Overview
**Gateway** is a native Classic Toolbox (pre-Carbon) application that runs in the background on Mac OS 9 (PPC), acting as a modern TLS 1.3 gateway and proxy for vintage OS 9 clients.

* **Creator Code:** `GT9A`
* **Toolchain:** Retro68 (`retroppc.toolchain.cmake`), C++17, Multiversal for UI, GitHub Actions (`ghcr.io/autc04/retro68:latest`).
* **Host Environment (Development):** 14-inch MacBook Pro M4 Pro.

---

## Architectural Constraints & Rules

1. **No Carbon:** Classic Toolbox only. Carbon CFM cannot bind an OT endpoint to a chosen address. Gateway **must listen**, so it requires Classic Toolbox.
2. **C++ Global Constructors:** Retro68 PPC `crt0` skips C++ global constructors. Always heap-allocate with `new` inside `main()`. C file-scope statics in Certainly are permitted.
3. **Open Transport Isolation:** Multiversal Interfaces have **no Open Transport**. Vendor `third_party/InterfacesAndLibraries` from `brunocastello/iWordle` and supply that `-I` **only** to Certainly / OT translation units. UI files stay on Multiversal. Do not run `interfaces-and-libraries.sh` against the toolchain prefix compiling `src/main.cpp`.
4. **TLS Library:** Use **Certainly** (MIT). Check `tls13_record.c` for any missing ChaCha20-Poly1305 auth-tag compares; port the constant-time check from `mplsllc/macTLS` if absent. Do not invent a TLS stack or use OpenSSL/macTLS.
5. **Partition Size:** `SIZE` resource starts at preferred **8 MB** / minimum 4 MB to accommodate multiple listeners, buffers, and X.509 parsing.
6. **Cooperative Loop:** Single `WaitNextEvent` loop. Cap concurrent splices (start at 4). Every OT/TLS step yields. Never block handshakes in a tight loop.
7. **Networking & Binding:** Bind `INADDR_ANY` or the interface IP (do not restrict only to `127.0.0.1` because OT loopback can be flaky). Support both `127.0.0.1` and the TCP/IP control-panel address.
8. **Portability:** Portable files (HTTP request parser, OAuth form body, IMAP LOGIN extract) **must not** include `<OpenTransport.h>` or `<Windows.h>` so host tests compile cleanly on Ubuntu/Linux gcc without Retro68.

---

## Modules & Scope

### Module 1 — HTTP Proxy (`:8765`)
* Supports:
  1. `GET http://host/path HTTP/1.0` (classic forward proxy)
  2. `GET https://host/path HTTP/1.0` + `Host:` header (Classilla with `network.http.proxy.use-http-proxy-for-https = true`)
  3. `CONNECT host:443 HTTP/1.0` (for git or any CONNECT client, via `200 Connection Established` + raw 32 KB bounce buffer splice)
* Always `Connection: close` on the client hop. Decode chunked from origin.
* Redirects: follow every hop whose destination is `https://`, pass the rest back. `follow_redirects = auto|always|never`. The test is what the *client* can do, not where Gateway currently is — the client hop is always plaintext, so a second `https → https` hop is still ours. Following them all internally discards `Set-Cookie` and hides the final URL, which breaks media.
* Rewrite `https://` → `http://` in `text/*`, XHTML and JavaScript bodies (`rewrite_https`, default 1). A 1997 browser meeting an `https://` link opens a `CONNECT` tunnel and fails its own handshake, so the link is changed before it is seen. Never rewrite binary — a JPEG containing those bytes would be corrupted by one. Hold back up to 7 bytes at a chunk boundary so a split `https://` still matches.
* Body ceiling is `max_body_mb`, default 0 (none). The original 2 MiB cap protected nothing — bodies stream through a 32 KB buffer — and made video impossible.
* Forward `Content-Length` to the client whenever the body is not de-chunked **and not rewritten** — rewriting shortens the body by a byte per link, so the length would strand the client. Media keeps its length because media is never rewritten; players will not start without it.

### Module 2 — Mail Splice (`:1993` / `:1587`)
* Outlook Express 5 setup (Incoming IMAP `:1993` SSL off; Outgoing SMTP `:1587` SSL off, auth on).
* Local password checked against Gateway prefs; refresh tokens read from a local file (out-of-band OAuth bootstrap). Gate9 only handles token refresh via Certainly and upstream IMAPS (`outlook.office365.com:993` with `AUTHENTICATE XOAUTH2`) / SMTPS (`smtp.office365.com:587` STARTTLS or 465).

### Module 3 — Wayback Proxy (`:8888`, designed, not built)
* Serve archived pages from the Internet Archive at a configured date, with a
  glob allow-list of hosts that pass through to the live web.
* Its own listener (`wayback_port`), not a mode on `:8765`, so the live web and
  the archive are both available at once and a browser chooses between them by
  proxy setting alone.
* Design, decisions and prior verification: **`docs/module3-wayback.md`**.
* Compatible by design with the settings URL of `richardg867/WaybackProxy`, so
  existing bookmarks keep working. That project is GPL-3 and Gateway is MIT, so
  the implementation must be independent of its source.

---

## Non-Goals
* Newsstand / NewsProxy / Google News / trafilatura
* SMBv1/v2/v3, AFP, file-system plugins
* Transparent intercept (no pf on OS 9)
* Carbon, OpenSSL, writing TLS 1.3 from scratch
* Completing OAuth consent inside Classilla
* HTTP/2, HTTP/3, CONNECT tunnels speaking TLS on the *client* side (Gateway terminates TLS)

---

## Development Phases

* **Phase 0:** `docs/inventory.md` confirmation of toolchain shape, SIZE resource, crt0 rules, and OT header isolation.
* **Phase 1:** Listen on `:8765`, integrate Certainly, verify Classilla loads an `https://` URL. Debug line in UI showing negotiated TLS version and HTTP status.
* **Phase 2:** IMAP/SMTP splice + refresh-token file for Outlook.com. Outlook Express 5 send/receive verification.
* **Phase 3:** Provider expansion (Gmail) and raw `CONNECT` verification for git.
