<!-- This file is the release body: build-macos9.yml and build-win32.yml pass
     it to `gh release` as --notes-file. Keep it short — it is the page a
     person reads before downloading, not a changelog. Paragraphs stay on one
     line, because GitHub renders release bodies with newline-to-break. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers and the Internet Archive. Mac OS 9 (PowerPC), and Windows 95 through XP.

## What's new in 0.3.9

**A `CONNECT` to port 80 can now speak https instead.** `connect_upgrade` (off
by default) answers a plaintext `CONNECT` the way `connect_mitm` answers a
`443` one, but the other way round: there is no handshake to terminate, so
Gateway terminates the plaintext request inside the tunnel itself and
re-originates it over TLS to port 443, single-shot, then closes the tunnel.
No certificate is presented to anyone. Ported by hand from roytam1's `gw034`
fork.

**Gateway no longer dials itself.** A proxy-configured browser re-fetching
its own auto-configuration script sends the absolute form, and when that
request named Gateway's own address, Gateway used to answer it by opening a
connection back to itself — it worked, but spent a splice slot on the loop.
It is now answered locally instead.

**Gateway can say when it is out of date.** A little after the listeners are
up, it asks github.com once whether a newer release exists and logs the
direct download link for this platform when there is one — no JSON, no
rate limit, and nothing said at all when it is current or the check fails.
`check_updates = 0` turns it off.

## What's new in 0.3.8

**Secure pages load faster.** With `connect_mitm` on, every picture and script on an `https://` page is a new connection, and each one used to cost a full handshake — a slow RSA calculation on a vintage processor. Gateway now remembers the browser's secure session and lets it resume, so only the first connection to a site pays for the handshake. The log says `resumed` when it happens. Verified on Windows 95 with Internet Explorer 4 and Netscape 4.08. Should a browser turn a resumption down, Gateway forgets that session and the next connection simply does a full handshake again.

**Loose ends from 0.3.7, tidied.** The Tunnel's remote host can no longer be saved empty while the Tunnel is on — it would turn every client away. The Wayback checkbox once labelled "Strip charset from Content-Type" did the opposite of what it said, and now reads "Keep the charset in Content-Type"; the setting itself is unchanged. The "Find the nearest available snapshot" checkbox is gone: it was never connected to anything, and Gateway reaches the nearest snapshot regardless.

## Setting it up

Point the browser's HTTP proxy at the machine running Gateway, port `8765`. Classilla also needs `network.http.proxy.use-http-proxy-for-https` set to `true` in about:config.

`rewrite_https = 1` is on by default and needs nothing else: type addresses without a scheme and everything loads, though the address bar will not say `https`. For the real thing, set `connect_mitm = 1` and install Gateway's certificate authority in the browser from `http://<gateway-address>:8765/gateway-ca.crt`.

Mail, the Wayback proxy and the Tunnel are off until configured. Every setting is listed in [`docs/prefs.md`](https://github.com/brunocastello/Gateway/blob/main/docs/prefs.md).

## Upgrading from 0.3.7

Nothing to do. Your preferences file is kept as it is; a `wayback_api` line left in it is simply ignored. The certificate authority is kept, so a browser that already trusts it stays trusting it.

## Downloads

**Mac OS 9:** the `.sit` archive for real hardware, or the `.dsk` disk image to mount in an emulator.
**Windows:** the `.zip` to unpack anywhere, or the `.img` floppy image if the machine has no other way to receive a file.
