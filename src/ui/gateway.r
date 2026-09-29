/*
 * gateway.r - Gateway's resources.
 *
 * Written as raw data blocks rather than through "Processes.r" and "Types.r",
 * because Rez runs against whichever RIncludes are linked into the toolchain at
 * build time and this file has to survive both the Universal and the
 * Multiversal arrangement.
 */

/*
 * SIZE (-1): 8 MB preferred, 4 MB minimum (CLAUDE.md rule 5). Several
 * listeners, a 32 KB bounce buffer per splice and BearSSL's X.509 parsing all
 * want room, and Certainly's TLS 1.3 write path puts a 16 KB record buffer on
 * the stack.
 *
 * Flags word 0x58E0:
 *   0x4000  acceptSuspendResumeEvents
 *   0x1000  canBackground            - Gateway proxies while it is behind
 *   0x0800  doesActivateOnFGSwitch
 *   0x0080  is32BitCompatible
 *   0x0040  isHighLevelEventAware    - so the Quit Apple event arrives
 *   0x0020  localAndRemoteHLEvents
 *
 * 0x0400 (onlyBackground) is deliberately clear: Gateway ships as an ordinary
 * application with a window. Setting show_window = 0 in prefs makes it write
 * that bit into this resource, and from the next launch it runs faceless --
 * no window, no menu bar, and no entry in the Application menu. The Quit
 * Apple event is what stops it in that state.
 */
data 'SIZE' (-1, "Gateway", purgeable) {
    $"58E0"
    $"0080 0000"                        /* preferred: 8388608 bytes */
    $"0040 0000"                        /* minimum:   4194304 bytes */
};

/*
 * vers (1): 0.3.9 development. GW_VERSION_STRING in src/gw_version.h shows
 * the same number in the About window on both platforms.
 * Byte layout is major(BCD), minor(BCD), stage,
 * prerelease, region, then two Pascal strings.
 */
data 'vers' (1, purgeable) {
    $"00 39 20 00 0000"
    $"05" "0.3.9"
    $"1B" "0.3.9, Gateway for Mac OS 9"
};

data 'vers' (2, purgeable) {
    $"00 39 20 00 0000"
    $"05" "0.3.9"
    $"07" "Gateway"
};

/*
 * "Check for Updates..." result dialogs (main.cpp's kAlrtNewer/kAlrtCurrent/
 * kAlrtFailed). Written as raw 'ALRT'/'DITL' data for the same reason as
 * SIZE and vers above -- Rez's own built-in knowledge of these two types
 * needs no #include, so it is immune to which RIncludes set is linked in at
 * the moment Rez runs.
 *
 * 'ALRT': Rect boundsRect(8) + INTEGER itemsID(2) + StageList stages(2).
 * stages 0x7777 draws the box, bolds item 1, and beeps once, on every one of
 * the four stages the Alert Manager tracks -- this box is shown once and
 * dismissed, so the four stages never differ from each other.
 *
 * 'DITL': INTEGER (item count - 1), then per item: a 4-byte placeholder Rez
 * leaves zero, Rect(8), a type byte, a length byte, and that many bytes of
 * data -- Pascal-style but without the usual second length prefix -- padded
 * to an even offset. Confirmed against the empty DITL 209 in
 * gateway_settings.r, whose $"FFFF" is exactly this count field for zero
 * items. Item 1 is always the button Return/a click without moving the
 * mouse activates, so it is the row's primary action (Download, or the
 * dialog's only OK). Every DITL below also carries an iconItem: the Alert
 * Manager does not draw the system icon on its own, the DITL's own iconItem
 * does, referencing the System file's stop (0) or note (1) 'ICON' by id.
 *
 * Each statText item holds the single placeholder "^0" -- main.cpp calls
 * ParamText() with the one assembled sentence before Alert()/StopAlert(),
 * rather than splitting the message across several parameters.
 */

/* 300: a newer release is available. Download / Later. */
data 'ALRT' (300, "Update available", purgeable) {
    $"0046 005A 00D2 01D6 012C 7777"
};
data 'DITL' (300, "Update available", purgeable) {
    $"0003 0000 0000 006C 0126 0082 016C 0408"
    $"446F 776E 6C6F 6164 0000 0000 006C 00CE"
    $"0082 011E 0405 4C61 7465 7200 0000 0000"
    $"0010 0040 0064 016C 0802 5E30 0000 0000"
    $"0010 0010 0030 0030 2002 0001"
};

/* 301: the running version is current. OK only. */
data 'ALRT' (301, "Up to date", purgeable) {
    $"005A 0064 00C8 01A4 012D 7777"
};
data 'DITL' (301, "Up to date", purgeable) {
    $"0002 0000 0000 004E 00F0 0064 012C 0402"
    $"4F4B 0000 0000 0010 0040 0046 012C 0802"
    $"5E30 0000 0000 0010 0010 0030 0030 2002"
    $"0001"
};

/* 302: the check failed. OK only, the stop icon. */
data 'ALRT' (302, "Update check failed", purgeable) {
    $"0050 0050 00D2 01B8 012E 7777"
};
data 'DITL' (302, "Update check failed", purgeable) {
    $"0002 0000 0000 0060 0118 0076 0154 0402"
    $"4F4B 0000 0000 0010 0040 005A 0154 0802"
    $"5E30 0000 0000 0010 0010 0030 0030 2002"
    $"0000"
};
