<!-- Paragraphs are deliberately unwrapped. GitHub renders release
     bodies with newline-to-break, so text wrapped for an editor arrives
     on the release page as a ragged column of short lines. Keep prose on
     one line per paragraph; headings, lists and tables wrap normally. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers, and the Internet Archive. It runs on Mac OS 9 (PowerPC) and on Windows 95 OSR2 through XP.

**0.3.2 is a single fix, and an important one: anything larger than about 16 KB fetched over TLS could hang.** If you are running 0.3.0 or 0.3.1, upgrade.

## Large responses could stall

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

**Windows 95 OSR2, 98, Me, NT 4.0, 2000 or XP.** Verified on Windows Me, Windows 2000 and Windows 95 OSR2 under 86Box; the remaining versions are within the range the binary's imports allow but have not each been run.

## Known limits

Gmail is implemented as a provider but has not been tried against a live account. TLS 1.3 offers ChaCha20-Poly1305 and AES-128-GCM with X25519 and P-256; a server insisting on anything else will not connect. The compiled-in anchors — 129 of them, the whole system set — are all Gateway will ever trust, since neither target has a usable system trust store.

Archived pages can be slow: the Internet Archive rate-limits, so `wayback_connects` defaults to 1, and a missing image is usually `wayback_tolerance` refusing a snapshot too far from your date rather than a failure.

## Not warranted

A hobby project pointed at operating systems with no memory protection, no ASLR and no privilege separation. Research software, no warranty — do not put anything through it you would regret losing.
