/*
 * gateway_settings.r - the settings window's menus.
 *
 * Written as raw data blocks rather than through "Types.r", for the reason
 * given at the top of gateway.r: Rez runs against whichever RIncludes are
 * linked into the toolchain, and this file has to survive both the Universal
 * and the Multiversal arrangement.
 *
 * These are the menus behind the pop-ups in the Settings window. They are the
 * only resources it needs: the window, its controls and its entry fields are
 * all built at run time in src/main.cpp. The IDs are 200 and up, clear of the
 * menu bar's 128 and 129, which stay in the menu list the whole time a pop-up
 * is inserted.
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

/* Section selector: the four panes of the Settings window. */
data 'MENU' (200, "Settings sections", purgeable) {
    $"00C8"                            /* menu ID 200                       */
    $"0000 0000"                       /* width and height, computed later  */
    $"0000"                            /* standard menu definition procedure*/
    $"0000"                            /* placeholder                       */
    $"FFFFFFFF"                        /* every item enabled                */
    $"00"                              /* no title                          */
    $"0B" "Application"    $"00 00 00 00"
    $"09" "Web Proxy"      $"00 00 00 00"
    $"0D" "Wayback Proxy"  $"00 00 00 00"
    $"04" "Mail"           $"00 00 00 00"
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
