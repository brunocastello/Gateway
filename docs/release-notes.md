<!-- Paragraphs are deliberately unwrapped. GitHub renders release
     bodies with newline-to-break, so text wrapped for an editor arrives
     on the release page as a ragged column of short lines. Keep prose on
     one line per paragraph; headings, lists and tables wrap normally. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers, and the Internet Archive.

**0.3.0 adds Windows.** The same program, the same settings file, the same three modules, on Windows 95 OSR2 through XP — and it fixes two memory bugs that were live in the Mac builds all along.

## Windows 95 OSR2 and later

`Gateway.exe` is a single native Win32 binary with no installer and no dependencies. It runs as a tray icon: right-click for **Show/Hide Window**, **Start with Windows**, **About Gateway** and **Quit**.

Settings live in **Gateway.ini** beside the executable rather than in a profile directory — 95 and 98 have no user profiles by default and NT puts them somewhere else again — and the file is the same one Mac OS 9 uses. Line endings do not matter to Gateway, so a configuration written on either platform works on the other.

95 needs **OSR2 or later**, which is where `msvcrt.dll` starts shipping with the system. Beyond that the binary imports nothing newer than Windows 95: every entry point it uses is listed in `docs/porting.md` §3, along with how that was established rather than assumed.

Roughly 9,500 of Gateway's 12,000 lines are shared between the two platforms unchanged — the protocol grammar, both proxy modules, the TLS library and the stream layer. What is per-platform is the transport, the preferences and log files, the entropy source, and the shell.

## Two fixes that matter on Mac OS 9 too

**HKDF wrote past the end of every key and IV buffer** (`PATCHES.md` §19). `br_hmac_out` always writes the hash's whole output — 32 bytes for SHA-256 — and a TLS 1.3 IV is 12. Every IV derivation overran its buffer by 20 bytes, four times per handshake, onto whichever local the compiler had placed next. Which key that destroyed depended on the stack layout, so the same source was correct on PowerPC, corrupted the client's traffic key on x86 at `-Os`, and corrupted the server's at `-O0`.

**Decrypted data was discarded when the reader fell behind** (`PATCHES.md` §20). Application data was appended to a buffer holding exactly one maximum record, and the overflow was dropped with a comment saying "truncate if buffer full". Whenever a client read more slowly than a server sent — the ordinary case on this hardware — that punched a hole in the byte stream: chunked bodies were reported malformed, plain ones arrived short and sat until their idle timeout, and pages loaded at the speed of the timeout rather than the network.

Both were present in 0.2.0 and earlier. **Upgrading is worthwhile on Mac OS 9 whether or not you care about Windows.**

## Also in this release

* **`CONNECT` is verified.** It shipped in 0.1.0 and had never been exercised,
because there is no git client for Mac OS 9 to point at it. RetroZilla on Windows Me does exercise it.
* **129 trust anchors, up from 29.** The curated list was a guess about which
authorities the vintage web needs, and it kept being wrong — GlobalSign for `lite.cnn.com`, then Sectigo for `code.jquery.com`. A BearSSL anchor is a name and a public key rather than a certificate, so the whole system set costs about 53 KB and retires the problem.
* **A transfer waiting on a slow client is no longer timed out.** A browser
busy parsing a large script stops reading; that is backpressure, not an idle connection, and cutting it off cost exactly one timeout of stall.
* **A cipher self-test at startup**, silent unless it fails.
* **The About box agrees with the version.** 0.2.0 shipped with resources
reading 0.2 and an About box still saying 0.1; the number now lives in one header that both platforms read.

## Installing

**Mac OS 9:** unpack `Gateway.sit`, or mount `Gateway.dsk` in an emulator. Copy `docs/prefs-example.txt` into the System Preferences folder as **Gateway Prefs**. If the Finder shows a generic icon, rebuild the desktop by holding Command-Option through startup.

**Windows:** unpack `Gateway.zip` anywhere and run `Gateway.exe`. Copy `docs/prefs-example.txt` beside it as **Gateway.ini**. Use **Start with Windows** in the tray menu to run it at login.

Both need the configuration file to be writable: Gateway rewrites it when a mail provider rotates its refresh token, so a read-only copy works until the first rotation and then stops.

## Requirements

**Mac OS 9** with Open Transport, a PowerPC Mac, 8 MB of application memory (4 MB minimum). Real hardware and SheepShaver both work.

**Windows 95 OSR2, 98, Me, NT 4.0, 2000 or XP.** Verified on Windows Me under 86Box; the other versions are within the range the binary's imports allow but have not each been run.

## Known limits

Gmail is implemented as a provider but has not been tried against a live account. TLS 1.3 offers ChaCha20-Poly1305 and AES-128-GCM with X25519 and P-256; a server insisting on anything else will not connect. The compiled-in anchors are all Gateway will ever trust, since neither target has a usable system trust store.

Archived pages can be slow: the Internet Archive rate-limits, so `wayback_connects` defaults to 1, and a missing image is usually `wayback_tolerance` refusing a snapshot too far from your date rather than a failure.

`docs/inventory.md` is the honest account of how this is put together. `third_party/certainly/PATCHES.md` lists the twenty fixes the vendored TLS library has needed, and §19 and §20 record how they were found — a known-answer test, byte counters, and the peer's own alert, each eliminating a layer that had otherwise been guessed at.

## Not warranted

A hobby project pointed at operating systems with no memory protection, no ASLR and no privilege separation. Research software, no warranty — do not put anything through it you would regret losing.
