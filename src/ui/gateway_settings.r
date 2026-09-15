/*
 * gateway_settings.r - the settings window's menus.
 *
 * Written as raw data blocks rather than through "Types.r", for the reason
 * given at the top of gateway.r: Rez runs against whichever RIncludes are
 * linked into the toolchain, and this file has to survive both the Universal
 * and the Multiversal arrangement.
 *
 * Pane geometry is stored in DITL resources below. Each disabled user-item
 * is a layout slot; gw_settings.cpp installs an Appearance control there.
 * The shell remains compiled against Multiversal; this window uses Universal.
 *
 * MENU layout, from Inside Macintosh: Macintosh Toolbox Essentials 3-142:
 *
 *   short  menu ID
 *   short  placeholder for the width  (0; CalcMenuSize fills it in)
 *   short  placeholder for the height (0)
 *   short  resource ID of the menu definition procedure (0 = the standard one)
 *   short  placeholder (0)
 *   long   enable flags, bit 0 for the menu and bit n for item n
 *   pstr   menu title (empty: a pop-up draws its own)
 *   then, per item: pstr text, then icon, key equivalent, mark and style bytes
 *   byte   0, ending the list
 */

/* Pane selector. Its order matches DITL 210-217 and kFields. */
data 'MENU' (200, "Preferences panes", purgeable) {
    $"00C8 0000 0000 0000 0000 FFFFFFFF 00"
    $"07" "Modules" $"00 00 00 00"
    $"09" "Web proxy" $"00 00 00 00"
    $"07" "Wayback" $"00 00 00 00"
    $"0D" "Wayback sites" $"00 00 00 00"
    $"04" "Mail" $"00 00 00 00"
    $"0D" "Mail upstream" $"00 00 00 00"
    $"05" "OAuth" $"00 00 00 00"
    $"03" "Log" $"00 00 00 00"
    $"00"
};

/* follow_redirects. The values written to the prefs file are "auto",
 * "always" and "never", in this order; src/main.cpp holds that list. */
data 'MENU' (201, "Follow redirects", purgeable) {
    $"00C9"                            /* menu ID 201                       */
    $"0000 0000"
    $"0000"
    $"0000"
    $"FFFFFFFF"
    $"00"
    $"09" "Automatic"      $"00 00 00 00"
    $"06" "Always"         $"00 00 00 00"
    $"05" "Never"          $"00 00 00 00"
    $"00"
};

/* provider: "outlook", "gmail", "custom", in this order. */
data 'MENU' (202, "Mail provider", purgeable) {
    $"00CA"                            /* menu ID 202                       */
    $"0000 0000"
    $"0000"
    $"0000"
    $"FFFFFFFF"
    $"00"
    $"07" "Outlook"        $"00 00 00 00"
    $"05" "Gmail"          $"00 00 00 00"
    $"06" "Custom"         $"00 00 00 00"
    $"00"
};

/* Empty item list for NewColorDialog (the dialog owns a copied handle). */
data 'DITL' (209, "Preferences chrome", purgeable) { $"FFFF" };

/* Explicit pane coordinates in window space: top, left, bottom, right.
 * Text slots are the OUTER 19-pixel field box. Appearance edit controls use
 * its inset text rectangle; their frame and focus ring are drawn by the CDEF.
 * Disabled user items provide geometry only, never a Dialog Manager editor.
 */
data 'DITL' (210, "Modules layout", purgeable) {
    $"0003"
    $"00000000 0044 001E 0055 01A4 8000" /* http_enabled */
    $"00000000 005D 001E 006E 01A4 8000" /* mail_enabled */
    $"00000000 0076 001E 0087 01A4 8000" /* wayback_enabled */
    $"00000000 009C 00D2 00AF 010A 8000" /* max_sessions */
};

data 'DITL' (211, "Web proxy layout", purgeable) {
    $"0005"
    $"00000000 0044 00D2 0057 010A 8000" /* http_port */
    $"00000000 006A 001E 007B 01A4 8000" /* rewrite_https */
    $"00000000 0091 001E 00A2 01A4 8000" /* connect_mitm */
    $"00000000 00BE 00D2 00CF 0140 8000" /* follow_redirects */
    $"00000000 00DE 00D2 00F1 010A 8000" /* max_body_mb */
    $"00000000 010A 00D2 011D 010A 8000" /* max_connects */
};

data 'DITL' (212, "Wayback layout", purgeable) {
    $"0004"
    $"00000000 0044 00D2 0057 010A 8000" /* wayback_port */
    $"00000000 0070 00D2 0083 0122 8000" /* wayback_date */
    $"00000000 009C 00D2 00AF 010A 8000" /* wayback_tolerance */
    $"00000000 00C8 00D2 00DB 010A 8000" /* wayback_connects */
    $"00000000 00F7 001E 0108 01A4 8000" /* wayback_api */
};

data 'DITL' (213, "Wayback sites layout", purgeable) {
    $"0005"
    $"00000000 0044 001E 0055 01A4 8000" /* wayback_geocities */
    $"00000000 005D 001E 006E 01A4 8000" /* wayback_cache */
    $"00000000 0076 001E 0087 01A4 8000" /* wayback_settings */
    $"00000000 008F 001E 00A0 01A4 8000" /* wayback_ct_encoding */
    $"00000000 00A8 001E 00B9 01A4 8000" /* wayback_quick_images */
    $"00000000 00DE 001E 0134 019D 8000" /* wayback_live */
};

data 'DITL' (214, "Mail layout", purgeable) {
    $"0005"
    $"00000000 0044 00D2 0055 0140 8000" /* provider */
    $"00000000 006A 00D2 007D 01AC 8000" /* oauth_user */
    $"00000000 0083 00D2 0096 01AC 8000" /* local_password */
    $"00000000 00B5 00D2 00C8 010A 8000" /* imap_port */
    $"00000000 00CE 00D2 00E1 010A 8000" /* pop_port */
    $"00000000 00E7 00D2 00FA 010A 8000" /* smtp_port */
};

data 'DITL' (215, "Mail upstream layout", purgeable) {
    $"0006"
    $"00000000 0044 00D2 0057 01AC 8000" /* imap_host */
    $"00000000 005D 00D2 0070 010A 8000" /* imap_upstream_port */
    $"00000000 0076 00D2 0089 01AC 8000" /* pop_host */
    $"00000000 008F 00D2 00A2 010A 8000" /* pop_upstream_port */
    $"00000000 00A8 00D2 00BB 01AC 8000" /* smtp_host */
    $"00000000 00C1 00D2 00D4 010A 8000" /* smtp_upstream_port */
    $"00000000 00EC 001E 00FD 01A4 8000" /* smtp_starttls */
};

data 'DITL' (216, "OAuth layout", purgeable) {
    $"0005"
    $"00000000 0044 00D2 0057 01AC 8000" /* oauth_host */
    $"00000000 005D 00D2 0070 01AC 8000" /* oauth_path */
    $"00000000 0076 00D2 0089 01AC 8000" /* oauth_scope */
    $"00000000 008F 00D2 00A2 01AC 8000" /* oauth_client_id */
    $"00000000 00A8 00D2 00BB 01AC 8000" /* oauth_client_secret */
    $"00000000 00C1 00D2 00D4 01AC 8000" /* refresh_token */
};

data 'DITL' (217, "Log layout", purgeable) {
    $"0001"
    $"00000000 0044 001E 0055 01A4 8000" /* show_window */
    $"00000000 0084 001E 0095 01A4 8000" /* log_file */
};
