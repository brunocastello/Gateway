Gateway is a TLS 1.3 gateway and proxy that runs **on** Mac OS 9 rather than in
front of it: a native Classic Toolbox application for PowerPC that sits in the
background, terminates modern TLS, and lets applications written before it
existed reach the current web and current mail servers.

## What works

**HTTP proxy on `:8765`.** Point Classilla at it and set
`network.http.proxy.use-http-proxy-for-https` — `http://` and `https://` both
go through. Chunked responses are decoded on the way past, redirects followed
to five hops, bodies streamed with a 2 MiB ceiling. `CONNECT` is answered and
bounced raw, so anything doing its own TLS passes straight through untouched.

**Mail on `:1993` (IMAP), `:1995` (POP3) and `:1587` (SMTP).** Outlook Express 5
talks to Gateway in the clear with SSL off; Gateway authenticates outward with
`AUTHENTICATE XOAUTH2` over IMAPS, POP3S and SMTP with STARTTLS. Receiving and
sending are both verified against a live Outlook.com account. `provider =
outlook` or `gmail` sets the endpoints, scope and hostnames from one line.

The password typed into the mail client is checked against the local prefs file
and never leaves the machine. OAuth consent happens out of band; Gateway only
exchanges a refresh token for an access token, and saves the rotated refresh
token so the account keeps working.

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

`CONNECT` is implemented but has never been exercised — there is no git client
for Mac OS 9 to point at it. TLS 1.3 offers ChaCha20-Poly1305 and AES-128-GCM
with X25519 and P-256; a server that insists on anything outside that will not
connect. The 29 compiled-in trust anchors are all Gateway will ever trust,
since Mac OS 9 has no usable system trust store.

`docs/inventory.md` is the honest account of how this is put together and where
the edges are. `third_party/certainly/PATCHES.md` lists the fourteen fixes the
vendored TLS library needed along the way, including a missing
ChaCha20-Poly1305 authentication tag check and two asynchronous Open Transport
calls that were reading memory the caller had already freed.

## Not warranted

This is a hobby project pointed at a 27-year-old operating system with no
memory protection, no ASLR and no privilege separation. Research software, no
warranty — do not put anything through it you would regret losing.
