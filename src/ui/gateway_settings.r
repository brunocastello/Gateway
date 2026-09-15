/*
 * gateway_settings.r - the settings window's menus.
 *
 * Written as raw data blocks rather than through "Types.r", for the reason
 * given at the top of gateway.r: Rez runs against whichever RIncludes are
 * linked into the toolchain, and this file has to survive both the Universal
 * and the Multiversal arrangement.
 *
 * The window itself is built control by control in gw_settings.cpp; only the
 * menus and the dialog's (empty) item list live here.
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

/* Pane selector. Its order matches kPanes and the pane numbers in kFields. */
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
 * "always" and "never", in this order; gw_settings.cpp holds that list. */
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

/* Pane geometry is computed in gw_settings.cpp, not stored here: the window
 * is only as tall as the pane on show, so no fixed item list could describe
 * it. DITL 209 above stays because NewColorDialog insists on an item list. */
