# Module 3 — Wayback proxy (design, not yet built)

Serve the old web to old browsers: requests go to the Internet Archive's
Wayback Machine at a configured date instead of to the live site, except for a
list of hosts that are allowed through as they are.

Nothing here is implemented. This document exists so a later session can pick
the work up without re-deriving the decisions, and it records what has already
been checked so those checks are not repeated.

---

## 1. Why this belongs in Gateway

The reference implementation, [WaybackProxy](https://github.com/richardg867/WaybackProxy),
runs on a **modern** machine for one reason: a vintage browser cannot speak TLS
to `web.archive.org`. Gateway already solves precisely that, on the vintage
machine itself. Folding this in collapses two machines into one and removes the
modern Mac from the daily path.

The trick that makes it tractable is the Wayback Machine's `id_` URL modifier,
which returns the **original archived bytes** — no toolbar, no injected
JavaScript. That means **no HTML rewriting**, which is the one part that would
be genuinely painful here. Everything else is URL and header string work, which
is what `src/portable/` is for.

## 2. How it is used today, and what must not change

The current workflow, which the port has to preserve:

1. Browsers on the OS 9 and classic Windows machines point at the proxy.
2. The user visits a settings URL on `web.archive.org`, which the proxy
   intercepts, and which both sets the date and jumps straight to a site:

   ```
   http://web.archive.org/?date=20011231&dateTolerance=730&targetUrl=frogfind.com&gcFix=on&quickImages=on&ctEncoding=on
   ```

3. Everything then resolves through the archive at that date, except the
   allow-list.

**Constraint: keep that URL and those parameter names exactly.** Bookmarks and
muscle memory carry over, and it costs nothing to be compatible. The parameters
are `date`, `dateTolerance`, `targetUrl`, `gcFix`, `quickImages`, `ctEncoding`
— checkboxes arrive as `=on` and are absent when off.

### A second listener, not a mode switch

Wayback gets **its own port** rather than a global on/off setting:

| Port | Setting | Serves |
|---|---|---|
| 8765 | `http_port` | the live web — Classilla, and anything else current |
| 8888 | `wayback_port` | the archive at `wayback_date` |

This is better than a toggle for three reasons. It reproduces the current
topology exactly — Classilla on one proxy, the older browsers on another —
inside a single application. Both are available at once, so switching a browser
between the live web and 1997 is a proxy setting rather than a Gateway restart.
And 8888 is what the browsers are already pointed at, so nothing on the client
side changes but the address.

Mechanically this is small: `GW_MAX_SESSIONS` sessions already carry per-session
state, so `GWProxy_Accept()` takes a flag saying which listener the connection
arrived on, and the session records it. `wayback_port = 0` disables the
listener and the module with it.

Gateway binds `INADDR_ANY` (CLAUDE.md rule 7), so other machines on the LAN can
use the OS 9 Mac as their proxy — which is how the classic Windows machines are
pointed at the modern Mac today. Note `GW_MAX_SESSIONS` is 4, so a second
machine will feel it; that cap may want raising if this becomes the household
proxy.

## 3. Already verified — do not re-check

TLS compatibility with what Gateway offers (ChaCha20-Poly1305 and AES-128-GCM,
X25519 and P-256):

| Host | TLS | X25519 | P-256 | AES-128-GCM | Chain roots at | Anchor shipped |
|---|---|---|---|---|---|---|
| `web.archive.org` | 1.3 | yes | yes | yes | Go Daddy Root CA - G2 | yes |
| `archive.org` | 1.3 | yes | yes | yes | Go Daddy Root CA - G2 | yes |
| `www.oocities.org` | 1.3 | yes | yes | yes | ISRG Root X1 | yes |

**No TLS or trust-anchor work is required.** The chain `web.archive.org` sends
includes the G2 root, which is among the 29 anchors in `ca_roots.c`, so
BearSSL's validator terminates there.

`QUICK_IMAGES` **does** work here, contrary to the advice in WaybackProxy's
README. That advice assumes the retro machine has no path to the internet
except the proxy. With Gateway the browser reaches everything *through*
Gateway, so a redirect to `https://web.archive.org/…` comes back as an
ordinary shape-2 request and Gateway does the TLS. Leaving it on saves Gateway
rewriting work.

## 4. Configuration

Everything goes in Gateway Prefs; no second file.

```
wayback_port         = 8888         # 0 disables the archive listener entirely
wayback_date         = 20011231     # YYYYMMDD, YYYYMM or YYYY
wayback_tolerance    = 730          # days after wayback_date to accept, 0 = no limit
wayback_api          = 1            # use the availability API to find the nearest snapshot
wayback_geocities    = 1            # send geocities.com to oocities.org
wayback_quick_images = 1            # let the browser fetch images from the archive directly
wayback_ct_encoding  = 1            # allow a charset in Content-Type
wayback_settings     = 1            # serve the settings page on web.archive.org

wayback_live = frogfind.com
wayback_live = *.frogfind.com
wayback_live = 68k.news
wayback_live = *.68k.news
```

### The allow-list

The reference implementation calls this a whitelist and keeps it in a separate
file. Gateway's prefs are one line per setting, so there are two options:

- **Repeated keys** (recommended). One pattern per line, which reads well for
  the ~33 patterns currently in use and matches how the file already looks.
  Needs `gw_prefs_get_nth(text, len, key, n, out, cap)` in
  `src/portable/gw_prefs.c` — about 30 lines, since the line walker is already
  factored out as `gw_prefs_line()`.
- **One space-separated value.** No new code: the current list is 547 bytes and
  `GW_CFG_VALUE` is 2048. Uglier to edit, but free.

Patterns are globs: `*` and `?`, matched against the hostname only. The
existing list is mostly `*.domain`, so wildcard support is not optional — the
reference implementation's own fork had to add it.

`gw_glob_match()` belongs in `src/portable/gw_util.c`. Roughly 40 lines
recursive, and it is the piece most worth testing hard: a wrong match sends a
live site to the archive or the reverse, and both look like the site being
broken.

## 5. Where the code goes

| Piece | File | Lines (est.) |
|---|---|---|
| Glob matching | `src/portable/gw_util.c` | 40 |
| Archive URL build / Location rewrite / timestamp parse | `src/portable/gw_wayback.[ch]` (new) | 300 |
| Query-string parse and URL-decode | `src/portable/gw_url.c` | 80 |
| Settings page HTML | `src/portable/gw_wayback.c` | 120 |
| Repeated-key prefs lookup | `src/portable/gw_prefs.c` | 30 |
| Hooks, plus the per-session mode flag | `src/proxy/gw_httpproxy.c` | 100 |
| Second listener | `src/gw_core.c` | 20 |

All of the portable work is string manipulation with no allocation and no
system calls, so it is testable in `tests/host` exactly like the rest of
`src/portable/`. That also means the planned Windows and Mac OS X builds
(`docs/porting.md`) inherit this module for free — only the listener needs
platform code, and that is one call into whatever `gw_transport.h` turns into. Write the tests first; the URL rewriting rules are fiddly and
a wrong one is hard to spot from the browser end.

### Rewriting rules

Request, when the session arrived on the Wayback listener and the host is not
in the allow-list:

```
GET http://www.example.com/page.html
  -> https://web.archive.org/web/<date>id_/http://www.example.com/page.html
```

`id_` is what suppresses the toolbar and the injected script. Without it this
becomes an HTML rewriting project.

Response: the archive answers a `/web/<timestamp>/…` request with a redirect to
the exact snapshot. Two things to do with it:

1. **Check the timestamp** against `wayback_date` and `wayback_tolerance`, and
   treat an out-of-range snapshot as a miss.
2. **Rewrite `Location:`** from `/web/<ts>/http://original/…` back to
   `http://original/…`, so the browser stays in vintage-URL space rather than
   filling its address bar and history with archive.org URLs.

Gateway already follows redirects up to five hops in `step_recv_head()`, and
that is where both belong.

### Two hooks, and only two

- `step_recv_request()` — after `gw_http_parse_request()` succeeds: if this
  session arrived on the Wayback listener and the host does not match the
  allow-list, replace `req.url` with the archive target and set `tls = 1`. The
  existing connect path does the rest.
- `step_recv_head()` — where redirects are already handled: timestamp check and
  `Location` rewrite.

`ctEncoding = off` additionally means stripping `; charset=…` from
`Content-Type` on the way back, since some period browsers choke on it.
`gw_http_filter_response()` already rewrites that header block.

## 6. The trap to design around

**The Wayback rewrite must apply to Module 1's proxy path only.** Gateway's own
outbound connections — the OAuth token refresh to `login.microsoftonline.com`,
and the IMAP, POP and SMTP upstreams — must never be rewritten.

Architecturally they already bypass it: they call `GWStream_ConnectTLS()`
directly and never pass through `step_recv_request()`. But the failure mode if
someone later routes them through a shared helper is a mail login that fails
because the token endpoint was served from 2001, which would be an
extraordinarily confusing afternoon. Leave a comment at both ends.

Note that `mail.hotmail.com` is in the current allow-list, which is a hint that
the boundary matters in practice.

## 7. Rate limiting — the actual risk

The archive refuses connections for a few seconds when hit too quickly, and a
period page with thirty images will burst. This, not the URL rewriting, is
where the engineering time will go.

`GW_MAX_SESSIONS = 4` already limits concurrency, which helps. Beyond that:
space consecutive archive requests by a minimum interval, and on a refused
connection back off and retry rather than surfacing a broken image. Gateway's
cooperative loop makes both straightforward — a "not before tick N" field on
the session and an early return from `session_step()`.

## 7a. The date is global, and that is deliberate

In the reference implementation the date is a module-level global: the settings
handler declares `global DATE, DATE_TOLERANCE, …` and every request reads that
same value. Gateway should keep this. It is what the existing habit depends on
— set the era from the settings URL, browse, switch browser, set it again —
and matching it means bookmarks and reflexes carry over untouched.

The consequence to be aware of: **one era at a time, for every client.** Two
browsers loading pages concurrently share whatever the last one set, and so do
two machines pointed at the same Gateway. Sequential use, which is how it is
actually used, never notices.

If simultaneous eras are ever wanted, the fix is already implied by the
listener design in §2 rather than by per-client state: give each Wayback
listener its own date.

```
wayback_port  = 8888
wayback_date  = 20011231

wayback_port2 = 8889
wayback_date2 = 19970822
```

A browser then chooses its era by proxy port, with no settings page visit at
all. Per-client state keyed on the peer address would *not* solve the case
described — IE4 and iCab on the same Mac share an address — so the port is the
right discriminator. Not worth building until it is asked for.

## 8. Decisions still open

- **Should the settings page persist to prefs?** The reference implementation
  changes the running configuration only; the file has to be edited for
  permanence. Gateway has `GWConfig_Set()` and already writes prefs for the
  rotated OAuth token, so it could persist. Recommendation: persist the date
  and tolerance, since otherwise they must be re-set after every launch on a
  machine that gets restarted often. Keep the checkboxes session-only.
- **Should a miss fall back to the live site?** Cleaner to return a clear error
  page than to silently serve a 2026 page to a 1997 browser.
- **`SETTINGS_PAGE` on `web.archive.org` means Gateway shadows a real host.**
  That is the reference behaviour and the bookmarks depend on it, but it should
  be switchable off. It should also only answer on the Wayback listener, so the
  live-web proxy on 8765 can still reach the real site.
- **Should the allow-list also apply on the live-web listener?** No: 8765 is
  already "everything live". The list is only meaningful as an exception to the
  archive.

## 9. Licensing

WaybackProxy is **GPL-3.0**; Gateway is MIT. The implementation must therefore
be independent — written from the Wayback Machine's public URL scheme and from
the observable behaviour described above, not from their source.

That is not a workaround. It is a different language, a different architecture
and a different concurrency model; nothing would be gained by copying anyway.
Credit WaybackProxy as prior art here and in the README, and do not paste from
it.

## 10. Suggested order

1. `gw_glob_match()` and its tests. Smallest piece, and everything else depends
   on the allow-list being right.
2. `gw_wayback.c` URL building and `Location` rewriting, with tests, entirely
   on the host.
3. Wire the two hooks; try it against one site with a fixed date and the
   availability API off.
4. Add the availability API, then the date tolerance.
5. Settings page last — it is the most code and the least essential, and by
   then the behaviour it configures will be settled.
6. Rate limiting once there is enough working to provoke it.
