/*
 * gateway_settings.r - Settings window resources for Gateway.
 *
 * Written as raw data blocks (no RIncludes) so it survives both the Universal
 * and Multiversal toolchain arrangements.  Follows Mac OS 9 Platinum HIG:
 *   - Window procID = movableDBoxProc (4) with zoom box
 *   - Pop-up button for section navigation
 *   - Standard controls: checkboxes, edit fields, pop-ups, text area with scroll bar
 *   - Geneva 9 for labels, Monaco 9 for the wayback live list text area
 *   - OK / Cancel buttons at bottom right
 *
 * Resource IDs:
 *   Window: 129
 *   Strings: 128 (shared with main app)
 *   Icons: 128 (shared)
 */

/* --------------------------------------------------------------- Window */

/*
 * Settings window: movable dialog box with zoom (procID 4).
 * Width 380, height 290 — big enough for all sections.
 */
data 'WIND' (129, "Gateway Settings", purgeable) {
    $"00 04"                          /* procID: movableDBoxProc + zoom */
    $"00 64 00 A8"                   /* bounds: (100,168) to (480,452) */
    $"FFFF"                           /* window is in background initially*/
    $"00 00"                          /* no refCon */
};

/* --------------------------------------------------------------- Strings */

data 'STR#' (128, purgeable) {
    $"06"                              /* item count */
    /* 1: Settings title */
    $"0D" "Settings for Gateway..."
    /* 2: Section labels (used in pop-up) */
    $"0B" "Application"
    $"09" "Web Proxy"
    $"0A" "Wayback Proxy"
    $"04" "Mail"
    /* 3: OK / Cancel */
    $"02" "OK"
    $"06" "Cancel"
};

/* --------------------------------------------------------------- Icon */

data 'ICN#' (128, "Gateway Settings", purgeable) {
	$"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
	$"00 0F F0 00 00 38 1C 00 00 C0 03 00 01 80 01 80"
	$"03 07 E0 C0 02 1F F8 40 04 3F FC 20 04 7F FE 20"
	$"08 FF FF 10 08 FF FF 10 08 FF FF 10 08 FF FF 10"
	$"11 FC 3F 88 11 F9 9F 88 11 FB DF 88 11 FF FF 88"
	$"11 F0 0F 88 11 F1 8F 88 11 F1 8F 88 11 F1 8F 88"
	$"11 F0 0F 88 11 FF FF 88 11 FF FF 88 11 FF FF 88"
	$"40 00 00 02 40 00 00 02 7F FF FF FE 00 00 00 00"
	$"00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
	$"00 0F F0 00 00 3F FC 00 00 FF FF 00 01 FF FF 80"
	$"03 FF FF C0 03 FF FF C0 07 FF FF E0 07 FF FF E0"
	$"0F FF FF F0 0F FF FF F0 0F FF FF F0 0F FF FF F0"
	$"1F FF FF F8 1F FF FF F8 1F FF FF F8 1F FF FF F8"
	$"1F FF FF F8 1F FF FF F8 1F FF FF F8 1F FF FF F8"
	$"1F FF FF F8 1F FF FF F8 1F FF FF F8 1F FF FF F8"
	$"7F FF FF FE 7F FF FF FE 7F FF FF FE 00 00 00 00"
};

/* --------------------------------------------------------------- Menu */

data 'MENU' (129, purgeable) {
    $"03"                              /* 3 items: Application, Web Proxy, Wayback */
    /* Item 1: Application (selected by default) */
    $"0B" "Application"
    /* Item 2: Web Proxy */
    $"09" "Web Proxy"
    /* Item 3: Wayback Proxy */
    $"0A" "Wayback Proxy"
};

data 'MENU' (130, purgeable) {
    $"04"                              /* 4 items: + Mail */
    $"0B" "Application"
    $"09" "Web Proxy"
    $"0A" "Wayback Proxy"
    $"04" "Mail"
};
