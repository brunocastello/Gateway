# Gateway — Settings window specification

This is a build sheet for the Mac OS 9 Settings window. It says what the window
contains, what every control is bound to, and what the geometry has to be. It
does not contain code and does not describe the current implementation; build
it from here.

The window has been attempted three times. Each attempt failed on the same
thing — the entry fields — so section 2 is measured from the reference rather
than described, and should be read before anything else.

---

## 1. What it is

One fixed-size window, titled **Gateway Preferences**, with a pop-up at the top
that switches between seven panes. Only one pane is visible at a time; the
window does not resize when the pane changes.

The model is the **Mac OS 9 Internet control panel**. That is the reference the
window is copied from, and where this document gives a number, the number was
measured off it.

Seven panes, in this order:

| # | Pane | What it holds |
|---|------|---------------|
| 1 | Modules | which listeners run, and the session ceiling |
| 2 | Web proxy | the `:8765` listener |
| 3 | Wayback | the `:8888` archive listener, including the live-host allow-list |
| 4 | Mail | the client-facing half of the mail splice |
| 5 | Mail upstream | the provider-facing half |
| 6 | OAuth | token endpoint and credentials |
| 7 | Log | the log window and the log file |

---

## 2. Geometry

### 2.1 Measured from the reference

These come from `Internet example.png` at 1:1. Coordinates are the pixel rows
and columns of that image, so heights are inclusive of both frame lines.

| Thing | Measurement |
|---|---|
| Entry field box, total height | **19 px** |
| Entry field frame | 1 px black rectangle — this rectangle **is** the box |
| Vertical pitch, one field row to the next | **25 px** (19 box + 6 gap) |
| Text baseline inside a field | box top **+ 12** |
| First text pixel, from the frame's left edge | **+ 5** |
| Label, right-aligned: last ink to the frame's left edge | **6 px** |
| Label baseline | **identical to the field's text baseline** |
| Pop-up button, total height | **17 px** |
| Focus ring | ~2 px, drawn *outside* the frame, not in place of it |

### 2.2 The failure to avoid

The previous attempt drew a 1 px rectangle **three pixels outside** the text
item. With a 22 px item that produced a **28 px** visible box against the
reference's 19 px, and every panel read as a coarse enlargement of the
original. Two rounds of "make the font smaller" did not fix it because the
font was not the problem.

**The frame and the field are one box, 19 px tall.** Nothing is drawn outside
it except the focus ring.

### 2.3 Font

**Small system font (Geneva 9) everywhere**, with two exceptions:

* the `Settings for:` caption and its pop-up, which take the large system font;
* group box titles, which take the small system font **bold**.

Hint lines under a field are the small system font, in a lighter grey if that
is available, left-aligned with the **field's** left edge — not the pane's.

### 2.4 Columns

* Labels are right-aligned in a fixed column. The column is wide enough for the
  longest label in **any** pane, so a column does not move when the pane does.
* Fields start immediately after it and run to the pane's right margin, except
  where a width is given below. A fixed narrow width hung off the right edge
  bunches every pane to one side — do not.
* Checkboxes start at the pane's left margin, not at the field column.
* A pop-up in a field row is the same width and left edge as a field in that
  row, so a column of mixed controls lines up.

### 2.5 Window chrome

* Title: **Gateway Preferences**.
* Top left: the caption **`Settings for:`** and, to its right, the pane pop-up.
  *The caption is currently missing and must be added.*
* Each pane sits inside a group box whose title is the pane's name.
* Bottom right, in this order left to right: **Revert**, **Cancel**, **Save**.
  Save is the default button and takes the ring. *Revert is currently missing
  and must be added.*
* Fixed size. Pick it from the tallest pane (Wayback) and leave the others with
  space at the bottom; the reference does the same.

---

## 3. Control behaviour

* **Save** writes every field on every pane, closes the window, and makes the
  core re-read its settings. It is not "save this pane".
* **Revert** re-reads the preferences file and refills every field on every
  pane. The window stays open.
* **Cancel** closes without writing.
* **Return / Enter** is Save. **Escape** and **Command-.** are Cancel.
* **Tab** moves to the next entry field in the visible pane and wraps.
* A click in an entry field must both take the keyboard focus and place the
  caret. Taking one without the other has broken this twice: once a field that
  could not be clicked into, once a focus ring on a field nobody had touched.
* Long single-line values (**Scope**, **Refresh token**, **Client ID**) must
  **not** wrap. They are one line in a one-line box; a wrapping field hides its
  own content. The Wayback allow-list is the one exception and wraps on
  purpose — see §4.3.
* Numeric fields accept digits only.

### Validation, applied on Save

| Field kind | Rule | On failure |
|---|---|---|
| Any port | 1–65535, or 0 where 0 is documented as "off" | beep, focus the field, do not close |
| `wayback_date` | 8, 6 or 4 digits (`YYYYMMDD`, `YYYYMM`, `YYYY`) | as above |
| `wayback_tolerance` | 0 or more; 0 means no limit | as above |
| `max_body_mb` | 0 or more; 0 means no ceiling | as above |
| Session and connection counts | 1 or more | as above |

### What takes effect when

Changing a port, or turning a module off, requires the listener to be rebound.
Stop and start Gateway from the File menu after saving, or say so in a hint.
Everything else takes effect on the next request.

---

## 4. The panes

Every row below is one setting. **Key** is the name in the preferences file.
Checkboxes write `1` or `0`. Pop-ups write the value in the Values column, not
the label shown in the menu.

### 4.1 Modules

| Label | Key | Control | Default | Hint |
|---|---|---|---|---|
| Web proxy | `http_enabled` | checkbox | 1 | |
| Mail | `mail_enabled` | checkbox | 1 | |
| Wayback | `wayback_enabled` | checkbox | 1 | |
| Concurrent sessions | `max_sessions` | number, 56 px | 12 | Each costs about 110 KB. |

`max_sessions` is the ceiling across **all** modules. It is not the same as the
per-listener figures on the Web proxy and Wayback panes.

### 4.2 Web proxy

| Label | Key | Control | Default | Hint |
|---|---|---|---|---|
| Port | `http_port` | number, 56 px | 8765 | |
| Rewrite https:// to http:// | `rewrite_https` | checkbox | 1 | For a browser with no modern TLS of its own. |
| Terminate TLS for a typed https:// URL | `connect_mitm` | checkbox | 0 | Needs the Gateway CA installed in the browser. |
| Follow redirects | `follow_redirects` | pop-up, 100 px | `auto` | auto follows the hops the browser could not. |
| Largest response (MB) | `max_body_mb` | number, 56 px | 0 | 0 for no limit. |
| Connections opening at once | `max_connects` | number, 56 px | 8 | |

`follow_redirects` values: **`auto`**, `always`, `never`.
Menu labels: *Automatic*, *Always*, *Never*.

`rewrite_https` and `connect_mitm` are alternatives rather than companions —
with both on, a page fetched over real https has its links rewritten for
nothing. Consider a hint to that effect rather than disabling either.

### 4.3 Wayback

This is the pane that is currently incomplete: **the allow-list is missing**,
along with three other settings.

Gateway's Module 3 is a port of `richardg867/WaybackProxy`, and CLAUDE.md
requires the settings URL to stay compatible with it, so every parameter that
project exposes needs a home here. The mapping:

| Upstream parameter | Upstream default | Gateway key |
|---|---|---|
| `LISTEN_PORT` | 8888 | `wayback_port` |
| `DATE` | 20011025 | `wayback_date` (Gateway defaults to 20011231) |
| `DATE_TOLERANCE` | 365 | `wayback_tolerance` (Gateway defaults to 730; 0, not `null`, disables) |
| `GEOCITIES_FIX` | true | `wayback_geocities` |
| `QUICK_IMAGES` | true | `wayback_quick_images` |
| `WAYBACK_API` | true | `wayback_api` |
| `CONTENT_TYPE_ENCODING` | true | `wayback_ct_encoding` |
| `SETTINGS_PAGE` | true | `wayback_settings` |
| `SILENT` | true | no Wayback key — Gateway logs through the **Log** pane |
| `HOST` | blank | no key — CLAUDE.md rule 7 settles binding for every listener |

Gateway adds four of its own that upstream has no equivalent for:
`wayback_enabled`, `wayback_connects`, `wayback_cache`, and `wayback_live`,
which upstream keeps in a separate whitelist file rather than a parameter.

| Label | Key | Control | Default | Hint |
|---|---|---|---|---|
| Port | `wayback_port` | number, 56 px | 8888 | 0 disables the archive listener. |
| Era (YYYYMMDD) | `wayback_date` | number, 80 px | 20011231 | Also settable from the browser. |
| Days newer allowed | `wayback_tolerance` | number, 56 px | 730 | 0 accepts any date. |
| Connections opening at once | `wayback_connects` | number, 56 px | 1 | The archive refuses bursts. |
| GeoCities fix | `wayback_geocities` | checkbox | 1 | Sends geocities.com to oocities.org. |
| Let the browser keep snapshots | `wayback_cache` | checkbox | 1 | |
| Serve the settings page | `wayback_settings` | checkbox | 1 | |
| Charset in Content-Type | `wayback_ct_encoding` | checkbox | 1 | Off strips it; some period browsers choke. |
| Quick images | `wayback_quick_images` | checkbox | 1 | Accepted for settings-page compatibility; does nothing. |
| Nearest available snapshot | `wayback_api` | checkbox | 1 | Off asks the archive for the era exactly. See below. |
| **Fetched live, not archived** | `wayback_live` | **text area**, see below | see below | Separate with `;`. A plain name covers its subdomains. |

#### The allow-list

`wayback_live` is the list of hosts that go to the live web instead of the
archive. It is the most-edited setting in this module and the reason the pane
needs to exist.

**Control.** A multi-line text area with a scroll bar, about six lines tall and
the full width of the field column, holding the patterns **separated by `;`** —
the way a period browser's "no proxy for" box worked:

```
frogfind.com;*.frogfind.com;68k.news;*.68k.news;floodgap.com
```

* This is the one field in the window that **wraps**. Every other field is one
  line and must not wrap; this one soft-wraps at the box's right edge so a long
  list stays visible.
* The scroll bar sits immediately to the right of the text area, with the
  **same top and the same height**. It has been misaligned, drawn over the
  area's own title, and non-functional in three separate rounds. It has to
  actually scroll.
* Accept newlines as separators on input as well, so a pasted one-per-line list
  works, and normalise them to `;` on save.
* Drop empty entries and surrounding spaces on save. The value must never
  begin with `;` — see the format note below.
* Patterns are globs. `frogfind.com` covers the host and its subdomains;
  `*.frogfind.com` covers subdomains explicitly.

**Storage.** One line, one value:

```
wayback_live = frogfind.com;*.frogfind.com;68k.news;*.68k.news
```

This needs no new preferences syntax. `gw_prefs_set` already writes a single
value and `gw_prefs_get` already reads one, so the text area's contents and the
stored value are the same string and nothing has to be assembled or taken
apart on the way through.

A continuation form — a second `+wayback_live` line appending to the first —
was considered and is not worth it. It would need a new rule in the parser, a
new writer to emit it, and a back-compatibility path for the repeated keys that
already exist, and it would buy only shorter lines in a file nobody has to
read. The single value gets the same result with code that is already written
and already tested.

**Reading, and old files.** Existing preferences files write the list as a
repeated key, one `wayback_live` line per pattern, and the indexed reader for
that already exists. Keep it: read occurrence 0 and split it on `;`, then keep
walking the later occurrences and take each whole line as one more entry.
A file written either way then works, and saving once collapses an old file
into the single-line form.

**One change to the preferences writer is required.** `gw_prefs_set` replaces
the *first* occurrence of a key and copies every later one through unchanged.
Saving a `;`-separated list over a file that still has thirty-odd repeated
`wayback_live` lines would therefore leave all of them in place underneath it —
and because the reader merges both forms, a host the user *deleted* would come
straight back from a stale line. Make `gw_prefs_set` drop the later
occurrences of the key it is setting. It is a few lines in the copy loop, it is
portable C that the host tests already cover, and no other key in the codebase
is ever read more than once, so nothing else changes behaviour.

**Limits.** The parser puts no cap on a line's length, and the whole file may
be 32 KB. The real ceiling is the 2048-byte buffer `GWConfig_Str` hands back:
about 120 typical patterns, against the 547 bytes the current list occupies.
Past that it truncates silently, so the text area should stop accepting input
at 2000 bytes and say why rather than losing the tail.

**A format note.** A line whose first non-blank character is `;` is a comment
in this file, which is why the stored value must never start with one. Trimming
empty entries takes care of it; the writer should not emit a leading separator.

#### wayback_api

Upstream's definition, which is the one to go by: *use the Wayback Machine
Availability API to find the closest available snapshot to the desired date,
instead of directly requesting that date.* Default true.

It is **not** the module's on/off switch. The Wayback proxy is turned off in
two places already in this specification: **`wayback_enabled`** on the Modules
pane, which stops the module being initialised at all, and
**`wayback_port = 0`** on this pane, which keeps the configured port and binds
no listener.

**The behaviour exists already and is currently unconditional.** Gateway asks
the archive for `/web/<era>/<url>`; the archive answers with a redirect to
whichever capture is nearest; Gateway follows that hop itself and rebuilds the
target with the `id_` modifier. That is the closest-snapshot result, reached by
following a redirect rather than by calling the Availability API endpoint. So
what is missing is the switch, not the feature — with `wayback_api` off,
Gateway should ask for the era exactly and take what comes back rather than
following the archive on to a neighbouring capture.

The checkbox therefore belongs on the pane, and the reader goes in with it. It
is small: a flag on the Wayback settings struct, filled from
`wayback_api` beside the other four in `gw_core.c`, and a test on that flag in
the archive branch of `redirect_should_follow` in `src/proxy/gw_httpproxy.c`.
Ship the control and the reader in the same change — a checkbox that writes a
key no code consults reports a choice the program will not honour, which is
worse than leaving it out.

### 4.4 Mail

| Label | Key | Control | Default | Hint |
|---|---|---|---|---|
| Provider | `provider` | pop-up, 100 px | `outlook` | Supplies the hosts and OAuth endpoint below. |
| Address | `oauth_user` | text | *(empty)* | |
| Password for the mail client | `local_password` | text | *(empty)* | Checked here; never leaves the machine. |
| IMAP port | `imap_port` | number, 56 px | 1993 | |
| POP port | `pop_port` | number, 56 px | 1995 | |
| SMTP port | `smtp_port` | number, 56 px | 1587 | |

`provider` values: **`outlook`**, `gmail`, `custom`.
Menu labels: *Outlook*, *Gmail*, *Custom*.

`local_password` is shown **as typed, not as asterisks**. An earlier attempt
masked it and then saved the asterisks back over the real password. If masking
is wanted later, it has to keep the real value separately.

Changing `provider` fills in the empty fields on the **Mail upstream** and
**OAuth** panes from a built-in table; `custom` supplies nothing and leaves
those panes as the user set them. An explicit value on those panes always wins
over the provider default. Consider refreshing the two panes' placeholder text
when the pop-up changes, so the effect is visible.

### 4.5 Mail upstream

| Label | Key | Control | Default (Outlook / Gmail) | Hint |
|---|---|---|---|---|
| IMAP host | `imap_host` | text | outlook.office365.com / imap.gmail.com | |
| IMAP port | `imap_upstream_port` | number, 56 px | 993 | |
| POP host | `pop_host` | text | outlook.office365.com / pop.gmail.com | |
| POP port | `pop_upstream_port` | number, 56 px | 995 | |
| SMTP host | `smtp_host` | text | smtp-mail.outlook.com / smtp.gmail.com | |
| SMTP port | `smtp_upstream_port` | number, 56 px | 587 | |
| STARTTLS on the SMTP port | `smtp_starttls` | checkbox | follows the port | Off for port 465, which is TLS from the first byte. |

`smtp_starttls` has no fixed default: when the key is absent it is **on** for
any port except 465, where it is **off**. The checkbox should show that
computed state when the key is unset, and write an explicit `1` or `0` once the
user touches it.

### 4.6 OAuth

| Label | Key | Control | Default | Hint |
|---|---|---|---|---|
| Token host | `oauth_host` | text | from `provider` | |
| Token path | `oauth_path` | text | from `provider` | |
| Scope | `oauth_scope` | text, no wrap | from `provider` | |
| Client ID | `oauth_client_id` | text, no wrap | *(empty)* | |
| Client secret | `oauth_client_secret` | text | *(empty)* | Google issues one even for desktop clients. |
| Refresh token | `refresh_token` | text, no wrap | *(empty)* | Rewritten by Gateway when the provider rotates it. |

Outlook defaults: host `login.microsoftonline.com`, path
`/common/oauth2/v2.0/token`, scope `offline_access` plus the three
`https://outlook.office.com/…` scopes.
Gmail defaults: host `oauth2.googleapis.com`, path `/token`, scope
`https://mail.google.com/`.

Scope and Refresh token are far longer than their boxes. They stay one line and
scroll horizontally; they must not grow a second line.

### 4.7 Log

| Label | Key | Control | Default | Hint |
|---|---|---|---|---|
| Show the log window at launch | `show_window` | checkbox | 1 | |
| Also write the log to a file | `log_file` | checkbox | 0 | System Folder : Application Support : Gateway : Gateway Log.txt |

The window keeps only the last 200 lines; the file keeps everything. Say so
under the second checkbox.

`show_window` has a side effect worth a hint: with it off, the next launch is
faceless — no window, no menu bar, no entry in the Application menu — and the
Quit Apple event is the only way to stop it.

`log_file` is a flag on Mac OS 9; the file always goes to the path above. On
Windows the same key may also hold a path, so do not narrow the stored value to
`1`/`0` in shared code.

---

## 5. Complete key list

Forty-two keys, all of them above. Checked against the codebase in both
directions: nothing readable from preferences is left off a pane, and nothing
on a pane is absent from the code — with one deliberate exception, noted below.

```
Modules        http_enabled  mail_enabled  wayback_enabled  max_sessions
Web proxy      http_port  rewrite_https  connect_mitm  follow_redirects
               max_body_mb  max_connects
Wayback        wayback_port  wayback_date  wayback_tolerance  wayback_connects
               wayback_geocities  wayback_cache  wayback_settings
               wayback_ct_encoding  wayback_quick_images  wayback_api
               wayback_live
Mail           provider  oauth_user  local_password  imap_port  pop_port
               smtp_port
Mail upstream  imap_host  imap_upstream_port  pop_host  pop_upstream_port
               smtp_host  smtp_upstream_port  smtp_starttls
OAuth          oauth_host  oauth_path  oauth_scope  oauth_client_id
               oauth_client_secret  refresh_token
Log            show_window  log_file
```

`wayback_api` is the exception: forty-one of these have a reader today, and it
does not. It is on the Wayback pane anyway because the behaviour it governs is
already there and hardwired on, and because upstream exposes it — so the reader
is a small addition rather than a new feature. See §4.3.

---

## 6. Constraints, and the dead ends already paid for

**The Appearance Manager is what draws the reference.** The Mac OS 8.5+ look
being copied — the field frame, the focus ring, the pop-up, the group box, the
dialog ground — is drawn by that manager. The Multiversal Interfaces the Mac
shell compiles against do not declare any of it, so anything built only against
Multiversal is a System 7 rendering and cannot be made to match by adjusting
geometry. That is the ceiling three attempts have run into.

The Appearance Manager **is already vendored in this repository**, under
`third_party/InterfacesAndLibraries` — headers and `AppearanceLib` both. Follow
the precedent in CLAUDE.md rule 3 that already exists for Open Transport:
give the Universal `CIncludes` to the settings window's translation unit alone
and link `AppearanceLib`, leaving the rest of the Mac shell on Multiversal.

**Lay the panes out as `DLOG`/`DITL` resources, one `DITL` per pane, with
explicit item coordinates.** That is how the panels being copied were built.
Layout computed at run time from constants is why the previous attempt never
converged: the geometry was guessed from screenshots at one CI round per
attempt, on a platform that cannot be run on the development machine.

Do not repeat these:

* `NewWindow` and `NewDialog` make monochrome GrafPorts, so `RGBForeColor` does
  nothing and the Platinum ground is silently discarded. Use `NewColorDialog`.
  This was hit twice.
* CDEF 63, the old pop-up, **draws** correctly and **will not track** a click,
  on a colour port or a monochrome one. Let it draw and open the menu yourself.
  The Appearance pop-up, CDEF 400, does track.
* `NewControl`'s arguments are `(value, min, max, procID, refCon)`. Passing the
  procedure ID where the value goes makes every control a push button and
  raises no diagnostic.
* The Dialog Manager draws an `editText` item in its own font and a `statText`
  item in the port's, so labels and field text come out different sizes unless
  the dialog font is set.
* Mac OS 9 draws no border around an `editText` item. The window draws the box
  itself — see §2.2 for the height, and do not draw it outside the item.
* Never size a control from `StringWidth` measured in the port's font when the
  Control Manager will draw that title in the system font. That cut "Web proxy"
  to "Web pro" in every pane.
* Multiversal has no `Controls.h` and no `Scrap.h`; both live in `Multiverse.h`.
  There is a `Dialogs.h`.
