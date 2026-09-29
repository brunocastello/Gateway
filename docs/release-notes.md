<!-- This file is the release body: build-macos9.yml and build-win32.yml pass
     it to `gh release` as --notes-file. Keep it short — it is the page a
     person reads before downloading, not a changelog. Paragraphs stay on one
     line, because GitHub renders release bodies with newline-to-break. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers and the Internet Archive. Mac OS 9 (PowerPC), and Windows 95 through XP.

## What's new in 0.3.9

**A `CONNECT` to port 80 can now speak https instead.** Some clients open a plain `CONNECT host:80` rather than a `443` one, and Gateway used to just bounce those bytes. With `connect_upgrade` on (off by default), Gateway reads the plaintext request inside the tunnel and re-sends it over TLS to port 443, single-shot, then closes the tunnel — no certificate is shown to anyone, so this is protocol translation rather than MITM. Every other port is still a raw tunnel.

**Gateway no longer dials itself.** A proxy-configured browser re-fetching its own auto-configuration script sends the absolute form of the URL, and when that named Gateway's own address, Gateway used to answer it by opening a connection back to itself — it worked, but spent a splice slot on the loop. Gateway now remembers up to four addresses it has been asked for this way and answers them locally; the very first request for a given address still makes the one round trip that teaches Gateway the address.

**Gateway can say when it is out of date.** Once per launch, a little after the listeners are up, it asks github.com once whether a newer release exists. When one does, it logs a single line (code `G50`) with an `http://` link to download it for this platform; otherwise it says nothing, whether up to date or the check simply failed. `check_updates = 0` turns the request off entirely.

**40-bit export browsers, such as Internet Explorer 3.0, should now be able to complete a secure connection through `connect_mitm`** — expected pending a hardware re-test, since the previous handshake stopped one message short of it.

## Setting it up

Point the browser's HTTP proxy at the machine running Gateway, port `8765`. Classilla also needs `network.http.proxy.use-http-proxy-for-https` set to `true` in about:config.

`rewrite_https = 1` is on by default and needs nothing else: type addresses without a scheme and everything loads, though the address bar will not say `https`. For the real thing, set `connect_mitm = 1` and install Gateway's certificate authority in the browser from `http://<gateway-address>:8765/gateway-ca.crt`.

Mail, the Wayback proxy and the Tunnel are off until configured. Every setting is listed in [`docs/prefs.md`](https://github.com/brunocastello/Gateway/blob/main/docs/prefs.md).

## Upgrading from 0.3.8

Nothing to do. Your preferences file and certificate authority are both kept as they are. Gateway now checks github.com once at launch for a newer release; `check_updates = 0` turns that off.

The Wayback pane's "Quick images" checkbox is gone because it never did anything; an old `wayback_quick_images` line left in your prefs file is simply ignored.

## Downloads

**Mac OS 9:** the `.sit` archive for real hardware, or the `.dsk` disk image to mount in an emulator.
**Windows:** the `.zip` to unpack anywhere, or the `.img` floppy image if the machine has no other way to receive a file.
