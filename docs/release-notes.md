<!-- Paragraphs are deliberately unwrapped. GitHub renders release
     bodies with newline-to-break, so text wrapped for an editor arrives
     on the release page as a ragged column of short lines. Keep prose on
     one line per paragraph; headings, lists and tables wrap normally. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers, and the Internet Archive. It runs on Mac OS 9 (PowerPC) and on Windows 95 through XP.

**0.3.4 removes the checkbox.** 0.3.3 made a typed `https://` URL work in a browser with no modern TLS of its own, but only after unticking "Use SSL 2.0" — a setting every Internet Explorer ships with on, and one whose failure looked like anything except a setting. Gateway now understands the message those browsers send and `connect_mitm` works out of the box. Everything 0.3.3 did it still does; nothing else changed.

0.3.3, still current in everything below, makes a typed `https://` URL work in a browser that has no modern TLS of its own, and takes Gateway back to Windows 95 RTM and, on paper, NT 3.51. Nothing from 0.3.2 is regressed; everything new is off by default except the link rewriting.

## Which settings you need, and when

This is the part worth reading before anything else, because the right answer depends on your browser and one of the steps is a checkbox nobody would guess at.

There are two ways to get an old browser onto an `https://` site, and they are **alternatives rather than companions**. Turning both on works but is worse than either alone: the browser fetches a page over real https and then finds every link in it rewritten to `http://`, which IE 6 complains about.

| Prefs | For | What you get |
|---|---|---|
| `connect_mitm = 1`, `rewrite_https = 0` | Internet Explorer 5 and 6, Classilla, RetroZilla | Type `https://` and it works. Real https as far as the browser is concerned: the padlock, `Secure` cookies, the URL it asked for. |
| `rewrite_https = 1`, `connect_mitm = 0` | Internet Explorer 4, Netscape 4.x, IE 5.1 on Mac OS 9 | Type `http://`, or no scheme at all. Links, redirects and resources from other hosts all work. The address bar and `Secure` cookies do not. |

`rewrite_https = 1` is the default, so a fresh install behaves as the second row without being configured.

### If you use `connect_mitm`, check that "Use TLS 1.0" is ticked

In Internet Explorer: **Tools → Internet Options → Advanced**, down in the Security group. Netscape 4.7 has the equivalent under **Security → Navigator → Configure SSL**. TLS 1.0 is the oldest protocol Gateway can speak to a browser, so a browser with it switched off has nothing in common with Gateway however capable it otherwise is.

"Use SSL 2.0" can be left alone, which is new in 0.3.4 and is the next section. In 0.3.3 it had to be unticked.

### Internet Explorer 4 and Netscape 4 cannot use `connect_mitm`

Not a setting and not a bug. Those browsers have SSL 3.0 and no TLS; BearSSL has TLS 1.0 and no SSL. There is no version in common and nothing that creates one. Use `rewrite_https` with them and type addresses without a scheme — which works well, and is how the feature came to exist. What changed in 0.3.4 is the framing, not the version, so this limit is where it was.

## The SSL 2.0 hello is converted rather than refused (new in 0.3.4)

Contributed by [roytam1](https://github.com/roytam1), who wrote the patch.

A browser with "Use SSL 2.0" enabled sends its very first message in SSL 2.0 framing: no TLS record header at all, just a length with its high bit set. What the message *asks for* can still be TLS 1.0 — the framing is a compatibility wrapper from 1996, meant to let a server that had moved on recognise the hello anyway. BearSSL never took that concession up; it rejected the format before reading a single field, and its own comment said the case might one day be handled. So the handshake failed **with no certificate warning**, because there was no certificate yet to warn about, and the log said `BearSSL 3`.

Gateway now reads that wrapper and rewrites what is inside it as an ordinary TLS ClientHello — the challenge becomes the client random, the SSL 2.0 spelling of 3DES becomes the TLS one — and the handshake goes on as though the browser had used TLS framing to begin with. Internet Explorer can be left exactly as it shipped.

A hello that genuinely asks for SSL 2.0 still fails, and so does one asking for SSL 3.0. Nothing below TLS 1.0 is implemented here and nothing below it will be: SSL 3.0 shares none of TLS's key derivation, has its own MAC construction, and is broken by POODLE besides. `BearSSL 3` therefore still appears, meaning something narrower than it used to — the browser has SSL 3.0 and TLS 1.0 both switched off. When the version is the problem, the log now names the version the browser offered and says which side objected.

**This has not been run against a real browser.** The conversion is tested on the host and reasoned through against RFC 5246 Appendix E.2, including the transcript hashing that decides whether the last message of the handshake succeeds. Whether a period Internet Explorer completes a handshake through it is exactly what 0.3.4 is for. `third_party/certainly/PATCHES.md` §22 has the detail, including the one change from the original patch that was left out and why.

## One URL instead of two ports (new in 0.3.4)

Both listeners now serve a proxy auto-configuration script at `/proxy.pac` — `http://192.168.1.5:8765/proxy.pac`, or the same path on `:8888`. In Internet Explorer it goes under **Tools → Internet Options → Connections → LAN Settings → Use automatic configuration script**, and Netscape 4 has the same thing under **Edit → Preferences → Advanced → Proxies**. Every browser this program targets supports it.

**Which port you fetch it from is how you choose.** From `:8765` the script routes every host to the live proxy. From `:8888` it routes every host to the archive *except* the ones on the `wayback_live` allow-list — so the browser stays pointed at one place and a whitelisted site genuinely never reaches `:8888` at all. That is the one thing two port numbers cannot say on their own. Each script names which of the two it is in its first lines, so two bookmarks are tellable apart.

A host on `wayback_live` goes `DIRECT` — not the archive, and not Gateway either. The list names the sites you want left alone, so the script leaves them alone. The consequence is worth knowing for an `https` host on that list: the browser then does its own handshake, which is the thing Gateway normally spares it, so put one there only if the browser can manage modern TLS by itself.

The proxy addresses in the script are taken from the `Host:` header of the request that fetched it, so they are addresses that browser has just demonstrated it can reach — two interfaces, a name from the hosts file, or `127.0.0.1` from the same machine all produce a working script with nothing to fill in. Gateway's own address and anything undotted return `DIRECT`, so re-fetching the script cannot go through the proxy the script describes.

This is automatic *configuration*, not automatic *detection*. WPAD needs DHCP option 252 or a `wpad` DNS record and Gateway can provide neither, so the URL is pasted once. `/wpad.dat` serves the same script for anyone whose network already points WPAD at this machine.

## Sites without TLS 1.3 now work at all (new in 0.3.4)

Gateway always opens a connection to a site with a TLS 1.3 hello, and falls back to TLS 1.2 when the site has no 1.3. That fallback had never once completed, so a site serving only TLS 1.2 could not be fetched — it failed as a handshake error, a certificate error, or a read failure depending on where it got to, and none of those named the real cause.

Two faults, both needed: Gateway rejected any TLS 1.2 ServerHello that carried no extensions, which is legal and is what a server with nothing to add sends; and once past that, it treated an ordinary outgoing record as evidence that the handshake had restarted, so the first read after the request always failed on a connection with nothing wrong with it. `third_party/certainly/PATCHES.md` §25 and §26 have the detail.

Most of the web hides this, because a host with TLS 1.3 goes near neither. The sites this matters for are the small, hand-run, old-web ones — which are the sites this program exists for. `www.floodgap.com` is the worked example and the one that found both.

## Typing an https:// URL

With `connect_mitm = 1`, a `CONNECT` **to port 443** is no longer a pipe Gateway stays out of. Any other port stays a raw tunnel, because nothing obliges a `CONNECT` to be a TLS one — `git`, ssh through a proxy and anything else that only wants bytes moved keep working exactly as before. Gateway answers it, presents a certificate it made for that host, and speaks TLS 1.0 to the browser while speaking TLS 1.3 to the site.

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

Some sites cannot be reached over TLS at all, and `connect_mitm` makes that visible where it was not before — not because it causes the failure, but because it is the first thing that makes Gateway open a TLS connection to a site the browser used to reach over plain HTTP. `www.macintoshgarden.org` is the worked example: it sends its certificate chain out of order, with a stray self-signed root between the leaf and the intermediate that actually issued it. BearSSL's validator walks the chain in the order received and wants each certificate's issuer to be the next one's subject, so it stops there and reports `certificate chain out of order [TLS 55]`; a browser rebuilds the chain itself and never notices. Behind that sits a second problem: the chain leads to a Sectigo root that is not among Gateway's compiled-in anchors, so even a reordered chain would be refused. Both want fixing and neither is fixed here.

`certificate is for another host [TLS 56]` is the related case where the name Gateway asked for is not in the chain at all. The log now names the host in both, which is what tells you whether the site is at fault or Gateway asked for the wrong thing.

Archived pages can be slow: the Internet Archive rate-limits, so `wayback_connects` defaults to 1, and a missing image is usually `wayback_tolerance` refusing a snapshot too far from your date rather than a failure.

## Not warranted

A hobby project pointed at operating systems with no memory protection, no ASLR and no privilege separation. Research software, no warranty — do not put anything through it you would regret losing.
