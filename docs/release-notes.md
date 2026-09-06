Gateway is a TLS 1.3 gateway and proxy that runs **on** Mac OS 9 rather than in
front of it: a native Classic Toolbox application for PowerPC that sits in the
background, terminates modern TLS, and lets applications written before it
existed reach the current web, the current mail servers, and now the Internet
Archive.

## New in 0.2.0

**Module 3, the Wayback proxy, on `:8888`.** A second listener that serves the
web as it was on a chosen date. It is a separate port rather than a mode on
`:8765`, so the live web and the archive are both available at once and a
browser chooses between them by proxy setting alone. Settings can be changed
from the browser using the same query parameters as
[WaybackProxy](https://github.com/richardg867/WaybackProxy), so existing
bookmarks keep working — an independent implementation written from the
archive's public URL scheme, credited as prior art rather than borrowed from.
Snapshots are fetched with the `id_` modifier, so a 2001 page arrives as 2001
served it, without the archive's toolbar or rewritten links.

**Connection reuse.** Upstream connections are pooled and kept alive across
requests, including across the archive's redirects. On this hardware a TLS
handshake is most of the cost of fetching anything, so this is the difference
between an archived page loading and an archived page crawling.

**Each module can be switched off.** `http_enabled`, `mail_enabled` and
`wayback_enabled`. A disabled module is never initialised, so it costs no
listener and no session memory.

**Any mail provider.** `provider = custom` alongside `outlook` and `gmail`,
with the IMAP, POP3 and SMTP hosts and ports set directly.

**A usable log window.** Resizable, with a scroll bar and keyboard scrolling,
holding 200 lines and wrapping long ones. `log_file = 1` mirrors every line to
*System Folder : Application Support : Gateway : Gateway Log.txt*, written as it
happens so the tail survives a crash.

**Fixes that mattered.** Two crashes: notifiers left installed on providers
being closed, and a close_notify encrypted with keys that did not exist yet.
Gateway's own error pages are no longer cacheable — a browser was storing them
as the image and serving the failure from disk forever after. A full proxy now
lets connections wait in the listen backlog instead of accepting and resetting
them, which a browser reads as "this resource is gone".

## What works

**HTTP proxy on `:8765`.** Point Classilla at it and set
`network.http.proxy.use-http-proxy-for-https` — `http://` and `https://` both
go through. Chunked responses are decoded on the way past and redirects are
followed to five hops. Bodies are uncapped by default; `max_body_mb` sets a
ceiling if you want one.

**Mail on `:1993` (IMAP), `:1995` (POP3) and `:1587` (SMTP).** Outlook Express 5
talks to Gateway in the clear with SSL off; Gateway authenticates outward with
`AUTHENTICATE XOAUTH2` over IMAPS, POP3S and SMTP with STARTTLS. Receiving and
sending are both verified against a live Outlook.com account.

The password typed into the mail client is checked against the local prefs file
and never leaves the machine. OAuth consent happens out of band; Gateway only
exchanges a refresh token for an access token, and saves the rotated refresh
token so the account keeps working.

## Upgrading from 0.1.0

**Empty the browser cache once.** 0.1.0 had no cache headers on its own error
pages, so a browser may be holding failures from it that it will otherwise
never re-request. This is the single most likely reason an upgraded Gateway
still looks broken.

Existing prefs files keep working; every new setting has a default.

## Installing

Unpack `Gateway.sit` on the Mac, or mount `Gateway.dsk` in an emulator. Copy
`docs/prefs-example.txt` into the System Preferences folder as **Gateway
Prefs** and edit it — the whole configuration lives there. If the Finder shows
a generic icon, rebuild the desktop by holding Command-Option through startup.

`show_window = 0` starts with no window; the File menu is always present, so
Gateway can be quit and the window brought back at any time. For a Gateway that
starts with the Mac, put an alias in Startup Items.

## Requirements

Mac OS 9 with Open Transport, a PowerPC Mac, and 8 MB of application memory
(4 MB minimum). Real hardware and SheepShaver both work.

## Known limits

**Not exercised.** `CONNECT` is implemented but has never been used — there is
no git client for Mac OS 9 to point at it. Gmail is implemented as a provider
but has not been tried against a live account. Token expiry beyond the first
hour has not been observed directly.

**The archive is slow, and rate limits.** `wayback_connects` defaults to 1
because a burst of new connections from one address is what earns a refusal,
and Gateway is a single cooperative thread, so concurrent TLS handshakes take
turns on one CPU rather than overlapping. A missing image on an archived page
is usually `wayback_tolerance` refusing a snapshot too far from your date;
raise it, or set it to 0 to accept the nearest.

**TLS.** ChaCha20-Poly1305 and AES-128-GCM with X25519 and P-256; a server that
insists on anything outside that will not connect. The 29 compiled-in trust
anchors are all Gateway will ever trust, since Mac OS 9 has no usable system
trust store.

`docs/inventory.md` is the honest account of how this is put together and where
the edges are. `third_party/certainly/PATCHES.md` lists the sixteen fixes the
vendored TLS library needed along the way, including a missing
ChaCha20-Poly1305 authentication tag check and three asynchronous Open Transport
calls that were reading memory the caller had already freed.

## Not warranted

This is a hobby project pointed at a 27-year-old operating system with no
memory protection, no ASLR and no privilege separation. Research software, no
warranty — do not put anything through it you would regret losing.
