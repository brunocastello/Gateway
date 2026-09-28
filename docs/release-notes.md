<!-- This file is the release body: build-macos9.yml and build-win32.yml pass
     it to `gh release` as --notes-file. Keep it short — it is the page a
     person reads before downloading, not a changelog. Paragraphs stay on one
     line, because GitHub renders release bodies with newline-to-break. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers and the Internet Archive. Mac OS 9 (PowerPC), and Windows 95 through XP.

## What's new in 0.3.7

**A tunnel, for SSH and anything else.** Module 4 relays one local port (`2222`) to a fixed far end over TLS, optionally through an HTTP `CONNECT` or SOCKS5 proxy — the job stunnel does, done on the vintage machine. Ported from [roytam1](https://github.com/roytam1)'s fork, with `tunnel_sni` rewritten so the certificate is always checked against the real host whatever the handshake sends. Off by default, and it has no login of its own: keep its port behind the machine's own boundary.

**A log a person can read.** Gateway's log now speaks in plain sentences, each with a short code that [`docs/log-codes.md`](https://github.com/brunocastello/Gateway/blob/main/docs/log-codes.md) explains. The byte counts, hello bytes and library error numbers are one tick away, under "Show engineering detail in the log" (`log_debug`).

**Preferences that follow your choices.** Settings that only matter under another one — the mail servers for a custom provider, the tunnel's TLS options, the proxy's login — are dimmed while they do not apply, showing what would be used, and commented out in the preferences file rather than left to be misread. Change the choice back and they return as they were. The tunnel's proxy is a pop-up.

**Six fixes from roytam1's fork.** An idle session no longer holds its slot for ever, a `CONNECT` can no longer send its `200 Connection Established` twice, and a site's `100 Continue` is no longer taken for the answer. On the TLS side, the 1.2 fallback no longer redials a connection it cannot, sends the record version old servers expect, and notices a server hanging up mid-handshake instead of waiting 30 seconds.

## Setting it up

Point the browser's HTTP proxy at the machine running Gateway, port `8765`. Classilla also needs `network.http.proxy.use-http-proxy-for-https` set to `true` in about:config.

`rewrite_https = 1` is on by default and needs nothing else: type addresses without a scheme and everything loads, though the address bar will not say `https`. For the real thing, set `connect_mitm = 1` and install the authority above.

Mail, the Wayback proxy and the Tunnel are off until configured. Every setting is listed in [`docs/prefs.md`](https://github.com/brunocastello/Gateway/blob/main/docs/prefs.md).

## Upgrading from 0.3.6

Your preferences file is kept as it is, and keeps working. One change: under `provider = outlook` or `gmail` the mail server settings in the file are now ignored in favour of the provider's own, so overriding a single host needs `provider = custom` with every server filled in — the Preferences window brings an existing override back when you choose Custom. The certificate authority is kept, so a browser that already trusts it stays trusting it.

## Downloads

**Mac OS 9:** the `.sit` archive for real hardware, or the `.dsk` disk image to mount in an emulator.
**Windows:** the `.zip` to unpack anywhere, or the `.img` floppy image if the machine has no other way to receive a file.
