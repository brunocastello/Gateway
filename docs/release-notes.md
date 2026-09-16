<!-- This file is the release body: build-macos9.yml and build-win32.yml pass
     it to `gh release` as --notes-file. Keep it short — it is the page a
     person reads before downloading, not a changelog. Paragraphs stay on one
     line, because GitHub renders release bodies with newline-to-break. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers and the Internet Archive. Mac OS 9 (PowerPC), and Windows 95 through XP.

## What's new in 0.3.5

**Browsers with no TLS at all now work.** Netscape 3 and 4, Internet Explorer 3 and 4, and IE 5 for Mac OS 9 speak SSL 3.0 and nothing newer, so until now they could only use link rewriting. Gateway implements SSL 3.0 itself — the key schedule, the record MAC and an RC4 record layer — and serves them a real `https://` address bar. Contributed by [roytam1](https://github.com/roytam1), verified on Netscape Communicator 4.75, 16-bit IE5 and IE 5.1.7 for Mac OS 9.

SSL 3.0 is spoken over RC4, which is what every browser of that age offers. A client that asks for SSL 3.0 with nothing but a CBC suite is told there is no cipher in common rather than being handed a broken handshake.

**Settings have a window.** Eight panes, on both platforms — Settings… in the Apple menu on Mac OS 9, File ▸ Preferences on Windows. The preferences file stays hand-editable and keeps its comments.

**The certificate authority can be installed.** Visit `http://<gateway-address>:8765/gateway-ca.crt` in the browser you are setting up and it will offer to install it, which stops the warning `connect_mitm` otherwise shows on every site. Clicking through the warning still works.

Also: the About box on Windows drew its text a third larger than the Mac's; a failed Open Transport connect reported the wrong call; and the SSLv2-compatible hello could translate two cipher specs onto one suite and send it twice.

## Setting it up

Point the browser's HTTP proxy at the machine running Gateway, port `8765`. Classilla also needs `network.http.proxy.use-http-proxy-for-https` set to `true` in about:config.

`rewrite_https = 1` is on by default and needs nothing else: type addresses without a scheme and everything loads, though the address bar will not say `https`. For the real thing, set `connect_mitm = 1` and install the authority above.

Mail and the Wayback proxy are off until configured. Every setting is listed in [`docs/prefs.md`](https://github.com/brunocastello/Gateway/blob/main/docs/prefs.md).

## Upgrading from 0.3.4

If you used `connect_mitm`, the certificate authority is regenerated once on first run and has to be trusted again. Nothing else changes, and the preferences file is untouched.

## Downloads

**Mac OS 9:** the `.sit` archive for real hardware, or the `.dsk` disk image to mount in an emulator.
**Windows:** the `.zip` to unpack anywhere, or the `.img` floppy image if the machine has no other way to receive a file.
