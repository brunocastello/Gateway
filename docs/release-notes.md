<!-- Paragraphs are deliberately unwrapped. GitHub renders release
     bodies with newline-to-break, so text wrapped for an editor arrives
     on the release page as a ragged column of short lines. Keep prose on
     one line per paragraph; headings, lists and tables wrap normally. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers, and the Internet Archive. It runs on Mac OS 9 (PowerPC) and on Windows 95 through XP.

**0.3.3 makes a typed `https://` URL work in a browser that has no modern TLS of its own, and takes Gateway back to Windows 95 RTM and, on paper, NT 3.51.** Nothing in 0.3.2 is regressed; everything new is off by default except the link rewriting.

## Which settings you need, and when

This is the part worth reading before anything else, because the right answer depends on your browser and one of the steps is a checkbox nobody would guess at.

There are two ways to get an old browser onto an `https://` site, and they are **alternatives rather than companions**. Turning both on works but is worse than either alone: the browser fetches a page over real https and then finds every link in it rewritten to `http://`, which IE 6 complains about.

| Prefs | For | What you get |
|---|---|---|
| `connect_mitm = 1`, `rewrite_https = 0` | Internet Explorer 5 and 6, Classilla, RetroZilla | Type `https://` and it works. Real https as far as the browser is concerned: the padlock, `Secure` cookies, the URL it asked for. |
| `rewrite_https = 1`, `connect_mitm = 0` | Internet Explorer 4, Netscape 4.x, IE 5.1 on Mac OS 9 | Type `http://`, or no scheme at all. Links, redirects and resources from other hosts all work. The address bar and `Secure` cookies do not. |

`rewrite_https = 1` is the default, so a fresh install behaves as the second row without being configured.

### If you use `connect_mitm`, untick "Use SSL 2.0"

In Internet Explorer: **Tools → Internet Options → Advanced**, down in the Security group. It is **on by default**, and it has to be off. Leave "Use SSL 3.0" and "Use TLS 1.0" ticked. Netscape 4.7 has the same switch under **Security → Navigator → Configure SSL**.

A browser with SSL 2.0 enabled sends its very first message in SSL 2.0 framing, which has no TLS record header at all. BearSSL rejects that format before reading a single field, so the handshake fails **with no certificate warning** — which makes it look like anything except a checkbox. The log names it:

```
#3 handshake with the browser failed for lite.duckduckgo.com
   (BearSSL 3: it sent an SSL 2.0-style hello -- untick "Use SSL 2.0" ...)
```

### Internet Explorer 4 and Netscape 4 cannot use `connect_mitm`

Not a setting and not a bug. Those browsers have SSL 3.0 and no TLS; BearSSL has TLS 1.0 and no SSL. There is no version in common and nothing that creates one. Use `rewrite_https` with them and type addresses without a scheme — which works well, and is how the feature came to exist.

## Typing an https:// URL

With `connect_mitm = 1`, a `CONNECT` is no longer a pipe Gateway stays out of. Gateway answers it, presents a certificate it made for that host, and speaks TLS 1.0 to the browser while speaking TLS 1.3 to the site.

The certificates are Gateway's own and are made **on the vintage machine**. Nothing outside it takes part and nothing is downloaded. The first time it is needed, Gateway generates a 1024-bit RSA key and a self-signed authority, and keeps them beside the preferences as `Gateway CA`. That generation blocks Gateway while it runs — it is looking for two 512-bit primes on period hardware — and happens once; the log reports how long it took. Certificates for individual hosts are minted from that key as they are asked for, which is fast, and signed with SHA-1 because the browsers this exists for cannot verify anything newer.

Your browser will warn that it does not recognise the issuer, with a Yes button to continue. Installing `Gateway CA` stops the warning and is worth doing, because resources on *other* hosts tend to fail quietly rather than prompt.

It is off by default for a reason: the only cipher these browsers and BearSSL share is 3DES, so every byte of every page gets encrypted three times over on a connection that never leaves the machine.

## Links that used to become dead ends

`rewrite_https = 1`, on by default, turns `https://` into `http://` in HTML, CSS and JavaScript on the way to the browser. A 1997 browser meeting an `https://` link does not ask Gateway for the page — it opens a tunnel and tries its own handshake, which fails with *"an error occurred in the secure channel support"* or *"no common encryption algorithm(s)"*, neither of which mentions a proxy. Changing the link before the browser sees it avoids the whole situation, and the plaintext hop it creates is loopback on the machine Gateway is already running on.

Binary bodies are never touched — a JPEG that happens to contain those eight bytes would be corrupted by one — and a `https://` split across a read boundary is still matched, which is the case that would otherwise leave one broken link per few hundred and no way to explain it.

**A redirect fix came with it, and it is the more important half.** The rule for which redirects Gateway follows itself asked where *Gateway* was rather than what the *client* could do, so after following one `http → https` hop Gateway was itself on TLS and handed the next `https → https` hop back to the browser as a `Location` it could not fetch. Two redirects was all it took, and `lite.duckduckgo.com` sends exactly two. The client hop is always plaintext, so every hop ending in TLS is Gateway's to follow.

## Windows 95 RTM, and NT 3.51 on paper

Reported by [roytam1](https://github.com/brunocastello/Gateway/issues/1), who had solved the same problem in RetroZilla.

Gateway named `CryptAcquireContextA` in its entropy pool, which made it an import, and an import the loader cannot resolve rejects the whole executable before any of Gateway's own version handling runs. CryptoAPI arrived with Windows 95 OSR2, so on 95 RTM and NT 3.51 Gateway did not fail to gather entropy — it failed to start. Those functions are now looked up at run time, and their absence costs one entropy source instead of the program. BearSSL carried a second copy of the same import, which is compiled out.

`msvcrt.dll` turned out to be the real floor rather than CryptoAPI: MinGW links it and it only ships with the system from OSR2 onward. The installer now carries a copy and drops it beside `Gateway.exe`, but only on a machine whose SYSTEM directory has none, so nothing from OSR2 forward is touched.

NT 3.51 needed one more thing: it is Program Manager, it has no notification area, and its `Shell_NotifyIcon` is a stub that fails. Gateway now checks, and where there is no tray the log window keeps a menu bar — Start/Stop, Start with Windows, Exit, About — and closing it minimises to a desktop icon instead of hiding.

**Neither has been run.** Every claim here is an argument about the binary's imports, which CI now checks on each build; the behaviour is unverified and there is no substitute for hardware. If you have either, a report would be welcome.

## Also

The Mac `.sit` and `.dsk` now carry a starting **Gateway Prefs** beside the application, so a first run has something to read.

## Large responses could stall (fixed in 0.3.2)

A TLS 1.3 record carries at most 16384 bytes of plaintext, and 16401 on the wire once its content type byte and 16-byte authentication tag are counted. The buffer receiving that plaintext is exactly 16384 bytes, and a check added in 0.3.0 compared the *wire* size against it. That comparison was true even for a completely empty buffer, so a maximum-sized record could never be decrypted: Gateway waited for room that was already there, and the connection sat until its idle timeout.

Any response big enough to fill one record reached it. A 24 KB image from the Internet Archive is sent as one maximum-sized record followed by a small one, so the first stalled and the rest never arrived. Smaller assets were unaffected, which is why pages mostly loaded while individual larger images and scripts did not.

The comparison now accounts for the record's overhead. An empty buffer always has room for a legal record, while a buffer still holding unread bytes continues to apply backpressure, which is what the check was added for. A record claiming more plaintext than the buffer could ever hold is now reported as an error rather than waited on, since no amount of draining would make space for it.

Both platforms were affected and both are fixed. `third_party/certainly/PATCHES.md` §20 has the detail.

## Installing

**Mac OS 9:** unpack `Gateway.sit`, or mount `Gateway.dsk` in an emulator. Copy `docs/prefs-example.txt` into the System Preferences folder as **Gateway Prefs**. If the Finder shows a generic icon, rebuild the desktop by holding Command-Option through startup.

**Windows:** run `Setup.exe`, from the zip or from the floppy image. It installs to **C:\Gateway** by default, lets you choose elsewhere, adds a **Gateway** group to the Start Menu and registers an uninstaller. Upgrading over an existing installation keeps your `Gateway.ini`, which holds your mail password and sign-in token. To install by hand instead, the zip also carries `Gateway.exe` on its own.

Both need the configuration file to be writable: Gateway rewrites it when a mail provider rotates its refresh token, so a read-only copy works until the first rotation and then stops.

## Requirements

**Mac OS 9** with Open Transport, a PowerPC Mac, 8 MB of application memory (4 MB minimum). Real hardware and SheepShaver both work.

**Windows 95, 98, Me, NT 4.0, 2000 or XP.** Verified on Windows Me, Windows 98, Windows 2000 and Windows 95 OSR2 under 86Box. Windows 95 RTM and NT 3.51 are now within the range the binary's imports allow — the installer carries an `msvcrt.dll` for machines without one — but neither has been run.

## Known limits

Gmail is implemented as a provider but has not been tried against a live account. TLS 1.3 offers ChaCha20-Poly1305 and AES-128-GCM with X25519 and P-256; a server insisting on anything else will not connect. The compiled-in anchors — 129 of them, the whole system set — are all Gateway will ever trust, since neither target has a usable system trust store.

Two upstream certificate failures are known and not yet diagnosed, both of them Gateway rejecting what a *site* sent rather than anything to do with `connect_mitm`. `certificate is for another host [TLS 56]` means the name Gateway asked for was not in the chain; `certificate chain out of order [TLS 55]` means the site sent its chain in an order BearSSL's validator will not walk, where a browser would have rebuilt it. Both predate this release as far as we can tell. The log now names the host in each case.

Archived pages can be slow: the Internet Archive rate-limits, so `wayback_connects` defaults to 1, and a missing image is usually `wayback_tolerance` refusing a snapshot too far from your date rather than a failure.

## Not warranted

A hobby project pointed at operating systems with no memory protection, no ASLR and no privilege separation. Research software, no warranty — do not put anything through it you would regret losing.
