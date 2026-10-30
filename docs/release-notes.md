<!-- This file is the release body: build-macos9.yml and build-win32.yml pass
     it to `gh release` as --notes-file. Keep it short — it is the page a
     person reads before downloading, not a changelog. Paragraphs stay on one
     line, because GitHub renders release bodies with newline-to-break. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers and the Internet Archive. 68K and PowerPC Macs, and Windows 95 through XP.

## What's new in 0.4.0

**68K support, from System 7.5.3 up to Mac OS 8.1.** The same Gateway, built for 68K Macs. It needs Open Transport 1.3 or later. Tested in QEMU's Quadra 800 on 7.5.3 with Netscape Communicator 4.04, and on 8.1 with Netscape 4.08 and Internet Explorer 4.01: plain web pages, `https://` through `connect_mitm`, and the Wayback proxy.

**On System 7, edit the prefs file by hand.** System 7 has no Appearance Manager, so the Settings window cannot open there. Drag the `Gateway Prefs` that comes with Gateway into the Preferences folder and edit it in SimpleText.

**Settings now save on Mac OS 8.0 and 8.1.** Two bugs, found on 68K but present on PowerPC too: the checkboxes did not toggle under 8.1's older Appearance Manager, and Save did nothing when there was no `Gateway Prefs` yet. Save now creates it.

## Setting it up

Point the browser's HTTP proxy at the machine running Gateway, port `8765`. Classilla also needs `network.http.proxy.use-http-proxy-for-https` set to `true` in about:config.

`rewrite_https = 1` is on by default and needs nothing else: type addresses without a scheme and everything loads, though the address bar will not say `https`. For the real thing, set `connect_mitm = 1` and install Gateway's certificate authority in the browser from `http://<gateway-address>:8765/gateway-ca.crt`.

Mail, the Wayback proxy and the Tunnel are off until configured. Every setting is listed in [`docs/prefs.md`](https://github.com/brunocastello/Gateway/blob/main/docs/prefs.md).

## Upgrading from 0.3.9

Nothing to do. Your preferences file and certificate authority are both kept as they are.

On System 7.5, installing a System 7.5 Update after Open Transport puts back Open Transport 1.1, on which Gateway cannot listen. Reinstall Open Transport 1.3 afterwards.

## Downloads

**PowerPC Mac:** the `.sit` archive for real hardware, or the `.dsk` disk image to mount in an emulator.
**68K Mac:** the `-68k.sit` archive for real hardware, or the `-68k.iso` CD image for QEMU. Attach it as a CD, not as a hard disk.
**Windows:** the `.zip` to unpack anywhere, or the `.img` floppy image if the machine has no other way to receive a file.
