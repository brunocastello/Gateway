# Gateway — Settings window specification

## Current visual refinements

The current implementation in `src/ui/gw_settings.cpp` and DITL 210–217
supersedes the older font, alignment, and session-scope guidance below:

- Checkbox titles, field captions, dropdown contents and Undo/Cancel/Save
  use Charcoal 12; descriptions and entry text remain Geneva 9.
- Row labels start at the checkbox square's left edge (x = 20). Single-line
  fields and dropdowns share x = 244 and a width of 110 pixels. Ordinary
  field rows have a 3-pixel gap, half the previous 6-pixel gap.
- The pane selector sits in the panel's top border, replacing its title;
  there is no separate “Settings for:” label. The Window Manager draws the
  native outer frame; no second outer frame is drawn in the content area.
- Descriptions beneath fields start at the field column and wrap. Module
  descriptions follow each checkbox, with the listener restart note above.
- Whitelist has a Charcoal heading and a scrollbar sharing the text area's
  right edge; the scrollbar extends one pixel higher to match its frame.
  The descriptions sit close to the area, with extra space below them.
- Mail upstream and OAuth instructions sit above their fields. OAuth secrets
  are displayed as stored; an empty secret is not invented or substituted.
- The Log panel's underlined link sends Finder an open-folder Apple event
  for Application Support : Gateway, the same directory used by logging.
- Concurrent sessions belongs to Web proxy: Web and Wayback share the HTTP
  session pool. Mail has a separate fixed pool of four sessions. This is not
  an application-wide session ceiling.
- File has a separator immediately before Settings and another before Quit.

## Original build sheet

This is a build sheet for the Mac OS 9 Settings window: what it contains, what
every control is bound to, and what the geometry has to be. It contains no code
and does not describe the current implementation; build it from here.

**It is not only a window.** Two of the settings need work below the interface
before a control over them can be honest. Section 0 says which, and in what
order — read it before starting, or you will build a window that looks finished
and silently does nothing for two of its controls.

The window has been attempted three times. Each attempt failed on the same
thing — the entry fields — so section 2 is measured from the reference rather
than described, and should be read next.

---

## 0. Scope, and the order to do it in

Three pieces of work. **A and B are not optional and come first**; both are
plain C with no Mac API in them, both are compiled by the Linux host tests, and
both can be finished and proven before a single Mac build.

| | Work | Where it is specified | How it is verified |
|---|---|---|---|
| **A** | The allow-list moves to one `;`-separated value: a splitter, a writer fix, and **two** consumers moved onto it | §4.4, *What has to change for the allow-list* | §7, then `make -C tests/host test` |
| **B** | `wayback_api` gets a reader, and the behaviour its default selects | §4.3, *wayback_api* | §7, then on the Mac |
| **C** | The window itself, all eight panes | §§2, 3, 4 | on the Mac |

### Why the order matters

Do **C** alone — build the window, add a `wayback_live` text area, add a
`wayback_api` checkbox — and all three of those things will appear to work and
none of them will:

* The text area saves a `;`-separated value that **neither consumer can read**.
  Both still split the list by line, so the whole list arrives as one
  nonsensical pattern and every host falls through to the archive.
* Deleting a host from the list **does not delete it**. The old repeated
  `wayback_live` lines are still in the file underneath the new value, and the
  merged reader hands them back on the next launch.
* The `wayback_api` checkbox writes a key **nothing reads**, so the setting
  reports a choice the program does not honour.

None of the three raises an error, a warning or a failed build. They present as
the Wayback module behaving oddly, weeks later.

### One thing to know before touching the preferences layer

`src/portable/gw_prefs.c` is shared. `Makefile.win32` takes all of
`src/portable/*.c`, so the writer change in **A** lands in the Windows build
too, and `tests/host/Makefile` compiles the same file, which is why the Linux
tests can prove it. That is a feature — it is the cheapest verification
available on this project — but it means the change is not Mac-only and its
tests are not optional.

---

## 1. What it is

One fixed-size window, titled **Gateway Preferences**, with a pop-up at the top
that switches between eight panes. Only one pane is visible at a time; the
window does not resize when the pane changes.

The model is the **Mac OS 9 Internet control panel**. That is the reference the
window is copied from, and where this document gives a number, the number was
measured off it.

Eight panes, in this order:

| # | Pane | What it holds |
|---|------|---------------|
| 1 | Modules | which listeners run, and the session ceiling |
| 2 | Web proxy | the `:8765` listener |
| 3 | Wayback | the `:8888` listener, and which snapshot it asks for |
| 4 | Wayback sites | which sites bypass the archive, and how their pages are handled |
| 5 | Mail | the client-facing half of the mail splice |
| 6 | Mail upstream | the provider-facing half |
| 7 | OAuth | token endpoint and credentials |
| 8 | Log | the log window and the log file |

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
* Bottom right, in this order left to right: **Undo**, **Cancel**, **Save**.
  Save is the default button and takes the ring.
* The window is as tall as the pane on show and resizes as panes change; the
  Mac build computes every pane's height in `src/ui/gw_settings.cpp` rather
  than storing coordinates. Place it for the tallest pane so resizing never
  walks it off the screen.

### 2.6 How big, and why Wayback is two panes

The window is fixed, so it has to be as tall as its tallest pane. Budget a pane
from the numbers in §2.1:

| Row | Height |
|---|---|
| Entry field row | 25 (19 box + 6) |
| Pop-up row | 25, so a mixed column lines up |
| Checkbox row | 20 |
| Hint line under either | 14 |
| Allow-list: title, six lines, hint | 14 + 86 + 14 |

Chrome around the pane comes to roughly 140: title bar, the `Settings for:`
row, the group box insets, the button row and the margins.

**Every Wayback setting on one pane comes to about 585 px tall.** That does not
fit a 640 × 480 screen, and it is wildly out of proportion to Gateway's own log
window, which is 520 × 340. Trimming hints does not save it — even with the
hints cut to the two that carry information the label cannot, it lands near
477, which still leaves no room for a menu bar.

So Module 3 takes two panes. Split by what the setting governs: **Wayback** is
the listener and which snapshot to ask for, **Wayback sites** is which sites
bypass the archive and what happens to the pages that do not. That gives:

| Pane | Content height |
|---|---|
| Wayback | ~190 |
| Wayback sites | ~256 ← the tallest |
| Web proxy | ~196 |
| Mail upstream | ~184 |
| OAuth | ~178 |

**About 460 × 400**, then — comfortably inside 640 × 480, and in scale with the
rest of the application.

Treat those figures as a budget to check rather than a measurement: they follow
from §2.1, which is measured, but the chrome and the checkbox pitch are
estimates. If a pane overruns, the fix is another split, not a taller window.

---

## 3. Control behaviour

* **Save** writes every field on every pane, closes the window, and makes the
  core re-read its settings. It is not "save this pane".
* **Undo** re-reads the preferences file and refills every field on every
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
  purpose — see §4.4.
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

The listener, and which snapshot it asks the archive for. The rest of Module 3
is on **Wayback sites** (§4.4); the settings did not fit on one pane, and §2.6
shows the arithmetic.

Module 3 is currently the most incomplete part of the window: **the allow-list
is missing**, along with three other settings.

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
| Nearest available snapshot | `wayback_api` | checkbox | 1 | Off asks the archive for the era exactly. |

#### wayback_api

Upstream's definition, which is the one to go by: *use the Wayback Machine
Availability API to find the closest available snapshot to the desired date,
instead of directly requesting that date.* Default true.

It is **not** the module's on/off switch. The Wayback proxy is turned off in
two places already in this specification: **`wayback_enabled`** on the Modules
pane, which stops the module being initialised at all, and
**`wayback_port = 0`** on this pane, which keeps the configured port and binds
no listener.

**What Gateway does today.** It asks the archive for `/web/<era>/<url>`; the
archive answers with a redirect to whichever capture is nearest; Gateway checks
that capture against `wayback_tolerance`, follows the hop itself, and rebuilds
the target as `/web/<stamp>id_/<url>`. That reaches the closest snapshot by
following a redirect, which is upstream's behaviour with `WAYBACK_API` **off**.
The API path — the default — is the one that does not exist.

##### Implementing it

**Off (0) is today's behaviour.** Leave that path exactly as it is.

**On (1)** asks first, then fetches:

1. `GET https://archive.org/wayback/available?url=<url>&timestamp=<era>`
2. Read `available` and `timestamp` out of the reply.
3. Build `/web/<timestamp>id_/<url>` on `web.archive.org` and fetch that — the
   same shape the redirect path already constructs, so the rest of the session
   is unchanged.
4. If `available` is false or missing, fail with the "no snapshot near the date
   Gateway is set to" 404 that the tolerance check already serves.

The parts exist:

* **The request.** `GWStream_ConnectTLS(&stream, host, 443)`, exactly as
  `GWToken_Request` in `src/proxy/gw_token.c` reaches the OAuth endpoint. That
  is the working pattern for a TLS fetch Gateway makes on its own account
  rather than on behalf of a client.
* **The reply.** `gw_json_string()` and `gw_json_number()` in
  `src/portable/gw_oauth.c`, which the token response already goes through.

##### Two traps

**Do not read `url` out of the response.** The body looks like this:

```json
{"url":"example.com",
 "archived_snapshots":{"closest":{"status":"200","available":true,
   "url":"http://web.archive.org/web/20011025.../http://example.com/",
   "timestamp":"20011025000000"}}}
```

There are **two** `url` members: the top-level echo of the query, and the real
one nested inside `closest`. `gw_json_string` scans flat for the first match of
a name, so it would return the echo — a value that looks plausible, is not a
snapshot, and would send the fetch to the wrong place. Read **`timestamp`**
instead, which appears only inside `closest`, and build the target yourself as
in step 3. `available` is likewise unique.

**It is an extra TLS handshake per page.** On this hardware that is the
expensive step, as the note at the top of `src/proxy/gw_httpproxy.c` says.
Cache the answer per host and era for the life of the session, or the archive
gets slower for a result the redirect was already producing.

##### Wiring

A flag on the Wayback settings struct, filled from `wayback_api` beside the
other four in `gw_core.c`, and tested in the archive branch of
`redirect_should_follow` in `src/proxy/gw_httpproxy.c`.

##### If the API call is more than you want in the first cut

Ship the **setting** mapped onto the mechanism that already exists: on follows
the archive's redirect to the nearest capture, off asks for `/web/<era>id_/<url>`
and refuses anything outside `wayback_tolerance`. The observable behaviour is
close, the preference round-trips through the settings page the way upstream's
does — which is what CLAUDE.md's compatibility requirement is about — and the
Availability API can replace the mechanism later without the setting changing
meaning. What is not acceptable is a checkbox wired to nothing: it would report
a choice the program does not honour.

### 4.4 Wayback sites

Which sites bypass the archive, and how the pages that do come from it are
handled. This pane exists because the Wayback settings do not fit on one —
see §2.6.

| Label | Key | Control | Default | Hint |
|---|---|---|---|---|
| GeoCities fix | `wayback_geocities` | checkbox | 1 | Sends geocities.com to oocities.org. |
| Let the browser keep snapshots | `wayback_cache` | checkbox | 1 | |
| Serve the settings page | `wayback_settings` | checkbox | 1 | |
| Charset in Content-Type | `wayback_ct_encoding` | checkbox | 1 | Off strips it; some period browsers choke. |
| Quick images | `wayback_quick_images` | checkbox | 1 | Accepted for settings-page compatibility; does nothing. |
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
stored value are the same string and nothing has to be assembled or taken apart
on the way through.

A continuation form — a second `+wayback_live` line appending to the first —
was considered and is not worth it. It would need a rule in the parser, a
writer to emit it, and a back-compatibility path for the repeated keys that
already exist, and it would buy only shorter lines in a file nobody has to
read. The single value gets the same result with code that is already written.

#### What has to change for the allow-list

Six changes. They are small, but **items 3 and 4 have to land together** — see
the warning under item 4.

**1. The writer drops stale duplicates.** `src/portable/gw_prefs.c`,
`gw_prefs_set`: it replaces the *first* occurrence of a key and copies every
later one through unchanged. Saving a `;`-separated list over a file that still
has thirty-odd repeated `wayback_live` lines would leave all of them in place
underneath it, and since the reader below merges both forms, a host the user
*deleted* would come straight back on the next launch. Make it skip the later
occurrences of the key it is setting — a few lines in the copy loop. No other
key in the codebase is ever read more than once, so nothing else changes.

**2. A portable indexed reader that understands both forms.** Also in
`gw_prefs.c`, beside `gw_prefs_get_nth`: given a key and an index, walk the
occurrences of that key and, within each one, split the value on `;`, returning
the nth entry across the whole thing. Trim spaces, skip empty entries. It is
pure string work over the prefs text, so it stays portable and the host tests
reach it without Retro68.

**3. A thin wrapper in `src/gw_config.c`** that supplies the loaded prefs text
to it, alongside the existing `GWConfig_GetNth`. Leave `GWConfig_GetNth` as it
is; other keys do not need splitting.

**4. Both consumers move to the wrapper.** `wayback_live` is read in two
places today, and both call `GWConfig_GetNth` directly:

* `GW_WaybackHostIsLive` in `src/gw_core.c` — what the proxy routes on.
* `pac_next_live_host` in `src/proxy/gw_httpproxy.c` — what the
  auto-configuration script is built from.

> **Change both or neither.** The comment already sitting on the second one
> says it plainly: a script that routed differently from the proxy it
> configures would be worse than no script. Convert only the first and the
> browser's PAC file keeps sending archive traffic for a host the proxy now
> fetches live, which presents as an intermittent routing bug with nothing
> wrong at either end.

Mind the limits already in `GW_WaybackHostIsLive` while you are there: it stops
at 128 entries and reads each pattern into a 256-byte buffer. Both are ample
for a `;`-separated value, but the splitter has to respect them rather than
assume one entry per line.

**5. The window saves with `GWConfig_Set`.** One call, one `;`-separated
string, no list writer involved. The repeated-key writer removed in `fa64f91`
stays removed — this design is the reason it is not needed.

**6. Tests.** `tests/host/run_tests.c` already has a *prefs lists* block that
checks `gw_prefs_get_nth` against a repeated key. Extend it: the `;` form, a
file mixing both forms, entries with spaces around them, an empty entry in the
middle, and — for change 1 — that setting a key which appears three times
leaves exactly one line behind. These run on Linux in the host-tests workflow,
so the whole allow-list path is verifiable without a Mac.

**Limits.** The parser puts no cap on a line's length, and the whole file may
be 32 KB. The real ceiling is the 2048-byte buffer `GWConfig_Str` hands back:
about 120 typical patterns, against the 547 bytes the current list occupies.
Past that it truncates silently, so the text area should stop accepting input
at 2000 bytes and say why rather than losing the tail.

**A format note.** A line whose first non-blank character is `;` is a comment
in this file, which is why the stored value must never start with one. Trimming
empty entries takes care of it; the writer should not emit a leading separator.

### 4.5 Mail

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

### 4.6 Mail upstream

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

### 4.7 OAuth

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

### 4.8 Log

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
               wayback_api
Wayback sites  wayback_geocities  wayback_cache  wayback_settings
               wayback_ct_encoding  wayback_quick_images  wayback_live
Mail           provider  oauth_user  local_password  imap_port  pop_port
               smtp_port
Mail upstream  imap_host  imap_upstream_port  pop_host  pop_upstream_port
               smtp_host  smtp_upstream_port  smtp_starttls
OAuth          oauth_host  oauth_path  oauth_scope  oauth_client_id
               oauth_client_secret  refresh_token
Log            show_window  log_file
```

`wayback_api` is the exception: forty-one of these have a reader today, and it
does not. It is on the pane anyway — upstream exposes it, CLAUDE.md requires
the settings URL to stay compatible with upstream, and Gateway already
implements the *off* half of it. §4.3 says what building the *on* half
involves, and gives a cheaper first cut that still makes the setting honest.

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

---

## 7. Host tests to add

`tests/host/run_tests.c`, run by `make -C tests/host test` and by the
*Host tests (portable code)* workflow. It compiles `src/portable/*.c` on Linux,
so everything in **A** and most of **B** is provable here before any Mac build.
Follow the existing style: a `printf` naming the group, then `check(...)` and
`check_str(...)` lines. The *prefs lists* group already covers the repeated-key
form against `gw_prefs_get_nth`; keep those as the regression floor and add to
them.

### The indexed reader, over both storage forms

| # | Given | Expect |
|---|---|---|
| 1 | four `wayback_live` lines, one pattern each | indices 0–3 return them in file order; index 4 returns 0 |
| 2 | `wayback_live = a.com;*.b.com;c.net` | indices 0–2 return the three; index 3 returns 0 |
| 3 | a `;` line **and** a later plain line | every entry, in file order, across both |
| 4 | `a.com ; *.b.com ;c.net` | values trimmed, no leading or trailing spaces |
| 5 | `a.com;;c.net` | the empty entry is skipped and **does not end the walk** — `c.net` is still reachable |
| 6 | a trailing `;` | no empty final entry |
| 7 | `# wayback_live = x` | not an entry; a commented line stays commented |
| 8 | `wayback_live =` with an empty value | contributes nothing, does not end the walk |
| 9 | `WAYBACK_LIVE = a.com` | found; key matching is case-insensitive, as the parser already is |
| 10 | an entry longer than `cap` | truncated, NUL-terminated, nothing written past `cap` |

Case 5 is the one worth writing first. The existing `gw_prefs_get_nth` carries a
comment explaining that an empty entry is not the end of a list, because
treating it as one silently lost half the list once already. A splitter can
reintroduce exactly that bug inside a single value.

### The writer dropping stale duplicates

| # | Given | Expect |
|---|---|---|
| 11 | a key present three times, set once | exactly one line for it, carrying the new value |
| 12 | the other keys around it | untouched, in their original order |
| 13 | a commented `# key = old` line | **left alone** — it is a comment, not a duplicate |
| 14 | a key present once, set | unchanged behaviour (regression) |
| 15 | a key absent, set | appended (regression) |
| 16 | after 11, read it back | index 0 is the new value, index 1 returns 0 |

### The two consumers agreeing

This is the failure §0 warns about, and it is catchable here rather than on a
Mac. `gw_pac_build` already takes a callback that yields the nth pattern, and
the *pac* group already exercises it.

| # | Given | Expect |
|---|---|---|
| 17 | one prefs text, read through the new accessor | the pattern sequence the proxy would match on and the sequence handed to `gw_pac_build` are identical |
| 18 | a `;`-separated list | the generated script names every entry, in order |
| 19 | the same list in repeated-key form | byte-identical script |

Case 19 is the point: the storage form must not be visible in the output.

### wayback_api

The JSON work is portable; the fetch is not, so test the parsing and the URL
building and leave the transport to the Mac.

| # | Given | Expect |
|---|---|---|
| 20 | a real availability body | reading `timestamp` returns the snapshot stamp |
| 21 | the same body | reading `url` returns the **top-level echo**, not the snapshot — pin the trap so nobody "simplifies" into it later |
| 22 | `"available":false` | treated as no snapshot |
| 23 | no `archived_snapshots` member | treated as no snapshot |
| 24 | a truncated body | no snapshot, no read past the end |
| 25 | a stamp and a URL | the built target is `/web/<stamp>id_/<url>` |

A body to test against:

```json
{"url":"example.com",
 "archived_snapshots":{"closest":{"status":"200","available":true,
   "url":"http://web.archive.org/web/20011025000000/http://example.com/",
   "timestamp":"20011025000000"}}}
```

Case 21 is not a normal test — it asserts behaviour that is *wrong for the
caller* — so say in the comment why it is there: `gw_json_string` scans flat, a
future reader will reach for `url`, and this is the line that stops them.

---

## 8. Definition of done

Work through this before calling it finished. The items that have historically
been missed are the ones below the interface, and they fail silently.

**A — the allow-list, below the interface**

- [ ] `gw_prefs_set` drops later occurrences of the key it sets; setting a key
      that appears three times leaves exactly one line.
- [ ] An indexed reader in `gw_prefs.c` returns the nth entry across both
      forms: repeated keys, one `;`-separated value, and a file mixing them.
- [ ] Spaces around entries are trimmed and empty entries are skipped.
- [ ] `GW_WaybackHostIsLive` (`gw_core.c`) uses it.
- [ ] `pac_next_live_host` (`gw_httpproxy.c`) uses it. **Both, or neither.**
- [ ] The PAC file and the proxy agree: a host on the list is fetched live by
      the proxy *and* routed direct by the generated script.
- [ ] The host tests in §7 are written and pass on Linux.

**B — wayback_api, below the interface**

- [ ] A flag on the Wayback settings struct, read from `wayback_api`.
- [ ] Something acts on it — either the Availability API path, or the mapping
      onto the existing mechanism that §4.3 offers as a first cut.
- [ ] Turning it off changes what Gateway fetches, demonstrably.

**C — the window**

- [ ] Eight panes, every key in §5 present exactly once.
- [ ] Entry fields are **19 px** tall and the 1 px frame *is* the box.
- [ ] Label baselines sit on their field's text baseline.
- [ ] Small system font everywhere except `Settings for:` and its pop-up.
- [ ] The `Settings for:` caption exists.
- [ ] **Undo** exists, beside Cancel and Save.
- [ ] A click into a field both focuses it and places the caret.
- [ ] Scope, Client ID and Refresh token do not wrap; the allow-list does.
- [ ] The allow-list scroll bar shares the text area's top and height, and
      scrolls.
- [ ] Save writes every pane, not the visible one.
- [ ] Undo refills every pane from the file.

**End to end**

- [ ] Add a host to the allow-list, Save, quit, relaunch: it is there, and the
      proxy fetches that host live.
- [ ] Remove it, Save, quit, relaunch: it is gone, and stays gone.
- [ ] `make -C tests/host test` passes.
- [ ] The Mac OS 9 workflow builds green.
