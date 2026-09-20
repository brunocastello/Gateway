<!-- This file is the release body: build-macos9.yml and build-win32.yml pass
     it to `gh release` as --notes-file. Keep it short — it is the page a
     person reads before downloading, not a changelog. Paragraphs stay on one
     line, because GitHub renders release bodies with newline-to-break. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers and the Internet Archive. Mac OS 9 (PowerPC), and Windows 95 through XP.

## What's new in 0.3.6

**SSL 3.0 over RC2, and a certificate from the era.** 0.3.5 spoke SSL 3.0 over RC4 only, and turned away a browser that offered nothing but export-grade RC2. Gateway now has an RC2-CBC record layer, and the certificate it presents for each site is X.509 v1 — the form every server of that day sent, and the only one Netscape 3 accepts over RC2. Both from [roytam1](https://github.com/roytam1), verified on Netscape 3.04 Gold.

**The handshake log says what the browser is speaking.** Each `connect_mitm` handshake now logs the first bytes of the browser's hello and, if the browser walks away, how far it got — enough to tell an SSL 3.0 client from one speaking PCT, and a browser that refused the certificate from one that never sent a hello.

**One script for the mail token.** `tools/get-email-token.py`, run once on a modern computer with nothing but Python 3, signs in to Outlook.com or Gmail and prints the lines to paste into the preferences file. It replaces `extract-refresh-token.py`.

## Setting it up

Point the browser's HTTP proxy at the machine running Gateway, port `8765`. Classilla also needs `network.http.proxy.use-http-proxy-for-https` set to `true` in about:config.

`rewrite_https = 1` is on by default and needs nothing else: type addresses without a scheme and everything loads, though the address bar will not say `https`. For the real thing, set `connect_mitm = 1` and install the authority above.

Mail and the Wayback proxy are off until configured. Every setting is listed in [`docs/prefs.md`](https://github.com/brunocastello/Gateway/blob/main/docs/prefs.md).

## Upgrading from 0.3.5

Nothing to do. The certificate authority is kept, so a browser that already trusts it stays trusting it, and the preferences file is untouched.

## Downloads

**Mac OS 9:** the `.sit` archive for real hardware, or the `.dsk` disk image to mount in an emulator.
**Windows:** the `.zip` to unpack anywhere, or the `.img` floppy image if the machine has no other way to receive a file.
