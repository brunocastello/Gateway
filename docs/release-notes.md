<!-- Paragraphs are deliberately unwrapped. GitHub renders release
     bodies with newline-to-break, so text wrapped for an editor arrives
     on the release page as a ragged column of short lines. Keep prose on
     one line per paragraph; headings, lists and tables wrap normally. -->

Gateway is a TLS 1.3 gateway and proxy that runs **on** the vintage machine rather than in front of it, so applications written before modern TLS existed can reach the current web, current mail servers, and the Internet Archive. It runs on Mac OS 9 (PowerPC) and on Windows 95 OSR2 through XP.

**0.3.1 is a follow-up to 0.3.0**, which added Windows. Everything here came out of using that release: a way to stop the gateway without quitting it, an installer, and the Windows build behaving as the Mac one does.

## Stop and start without quitting

Both builds gain a menu item that releases the ports and drops what is in flight, and another click binds them again. The application stays up either way, so the log stays readable and the settings can be corrected before starting again — which is the point of stopping rather than quitting. The item names what a click will do: **Stop Gateway** while it runs, **Start Gateway** while it does not.

On Windows the tray icon says which it is. The opening under the arch is **green while running and red while stopped**, and blue in Explorer, on the taskbar and in the window caption, where the icon means the application rather than its state. All three are the same drawing with four gradient steps changed, because at sixteen pixels square the opening is the only element with enough room to carry a state — greying the whole icon was tried first and read as a flat blob.

## A Windows installer

`Setup.exe` installs to **C:\Gateway** by default and lets you choose somewhere else, creates a **Gateway** group in the Start Menu, and registers a proper uninstaller with Add or Remove Programs.

Two things it deliberately will not do. It never overwrites an existing `Gateway.ini`, because that file holds the mail password and the OAuth refresh token, and replacing it on an upgrade would silently log you out of your own mail. And the uninstaller asks before removing it rather than assuming that uninstalling the program means discarding the credentials. It also closes a running Gateway before deleting it, and clears the registry entry that **Start with Windows** writes, which would otherwise have Windows complaining at every login about a program that is gone.

The Windows release ships as a zip and as a **1.44 MB floppy image**. Mount the image as drive A: in 86Box, or write it to a real diskette — Setup is a third of a floppy, so it fits with room to spare.

## Also fixed

* **One Gateway at a time on Windows.** A shortcut in the Startup group and the tray menu's Start with Windows are separate mechanisms that cannot see each other, so enabling both launched two copies at login, each trying to bind the same ports. A named mutex settles it; the second copy surfaces the first one's window and exits, which also covers double-clicking the executable while it is already in the tray.
* **The Windows About window** now carries the Mac's content and layout, in Windows' own typeface: the icon at the top, the same seven lines at the same offsets, with the names in bold.
* **The About box agrees with the version again**, on both platforms.

## Installing

**Mac OS 9:** unpack `Gateway.sit`, or mount `Gateway.dsk` in an emulator. Copy `docs/prefs-example.txt` into the System Preferences folder as **Gateway Prefs**. If the Finder shows a generic icon, rebuild the desktop by holding Command-Option through startup.

**Windows:** run `Setup.exe`, from the zip or from the floppy image. To install by hand instead, the zip also carries `Gateway.exe` on its own; put it anywhere with a `Gateway.ini` beside it.

Both need the configuration file to be writable: Gateway rewrites it when a mail provider rotates its refresh token, so a read-only copy works until the first rotation and then stops.

## Requirements

**Mac OS 9** with Open Transport, a PowerPC Mac, 8 MB of application memory (4 MB minimum). Real hardware and SheepShaver both work.

**Windows 95 OSR2, 98, Me, NT 4.0, 2000 or XP.** Verified on Windows Me under 86Box; the other versions are within the range the binary's imports allow but have not each been run.

## Known limits

Gmail is implemented as a provider but has not been tried against a live account. TLS 1.3 offers ChaCha20-Poly1305 and AES-128-GCM with X25519 and P-256; a server insisting on anything else will not connect. The compiled-in anchors — 129 of them, the whole system set — are all Gateway will ever trust, since neither target has a usable system trust store.

Archived pages can be slow: the Internet Archive rate-limits, so `wayback_connects` defaults to 1, and a missing image is usually `wayback_tolerance` refusing a snapshot too far from your date rather than a failure.

`docs/inventory.md` is the honest account of how this is put together, and `third_party/certainly/PATCHES.md` lists the twenty fixes the vendored TLS library has needed.

## Not warranted

A hobby project pointed at operating systems with no memory protection, no ASLR and no privilege separation. Research software, no warranty — do not put anything through it you would regret losing.
