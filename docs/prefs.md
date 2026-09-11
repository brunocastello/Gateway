# Gateway Prefs

One setting per line, `key = value` or `key: value`. `#` or `;` at the start of
a line makes it a comment; a `#` **after** a value is part of the value, so keep
comments on their own lines. Keys are case-insensitive, whitespace around them
is trimmed, and CR, LF or CRLF line endings all work.

The file lives in the System Preferences folder as **Gateway Prefs**. A
starting point is `docs/prefs-example.txt`. Gateway reads it once at launch,
and rewrites two lines itself: `refresh_token` when the provider rotates it,
and `wayback_date`/`wayback_tolerance` when the settings page is used.

The file may be up to 32 KB. Past that Gateway warns loudly in its log and
every setting after the cut silently reverts to its default — which is exactly
as confusing as it sounds, so heed the warning.

---

## Application

| Key | Default | Meaning |
|---|---|---|
| `show_window` | `1` | Open the log window at launch. `0` starts without one; the File menu is always present, so Gateway can be quit and the window brought back either way. Hiding the window records the choice here. |
| `max_sessions` | `12` | Concurrent proxy connections, clamped to 16. Each costs roughly 110 KB of the 8 MB partition. When they are all busy Gateway stops accepting, so surplus connections wait in the listen backlog rather than being refused. |
| `http_enabled` | `1` | Run Module 1, the HTTP/TLS proxy on `http_port`. |
| `mail_enabled` | `1` | Run Module 2, the IMAP, POP3 and SMTP splices. Also governs the OAuth token refresher, which exists only to serve them. |
| `wayback_enabled` | `1` | Run Module 3, the Internet Archive proxy on `wayback_port`. |
| `log_file` | `0` | Mirror the log window to a file. `1` (or `yes`/`on`) writes to **System Folder : Application Support : Gateway : Gateway Log.txt**, creating both folders if needed. Lines are appended across runs and written as they happen rather than buffered, so the tail survives a crash. The window keeps only the last 200 lines, which one slow page load can exceed, so this is the way to capture a whole session. |
| `max_connects` | `8` | How many upstream connections may be *opening* at once for ordinary live-web sessions, clamped to 8. |
| `wayback_connects` | `1` | The same cap for sessions served from the Internet Archive, counted separately so neither starves the other. It is low because a burst of new connections from one address is exactly what the archive's rate limiter refuses, logged as `connect failed [failed, OT 61, ...]`. Gateway runs a single cooperative thread, so concurrent TLS handshakes do not overlap anyway -- they take turns on one CPU -- and the connection pool recovers the throughput, since a reused connection skips the handshake entirely. |

## Web proxy

| Key | Default | Meaning |
|---|---|---|
| `http_port` | `8765` | Where browsers point for the live web. |
| `follow_redirects` | `auto` | `auto` follows every redirect whose destination is `https://`, because the client hop is always plaintext and a browser with no modern TLS could never follow one itself; a redirect to `http://` goes back to the browser, which follows it with its own cookies and knows where it ended up. `always` follows every redirect inside Gateway, which discards `Set-Cookie` and breaks media players. `never` passes all of them back. |
| `rewrite_https` | `1` | Turn `https://` into `http://` in HTML, CSS and JavaScript on the way to the browser, so a link the user clicks comes back to Gateway instead of becoming a `CONNECT` tunnel the browser cannot complete. Set to `0` for a client with its own modern TLS — RetroZilla, or `git` — where it buys nothing. See below for what it does not reach. |
| `max_body_mb` | `0` | Ceiling on a relayed response body, in MiB. `0` means none, which is the default: bodies stream through a 32 KB buffer and are never held, so a limit truncates downloads without saving memory. |

### What `rewrite_https` does not reach

A 1997 browser meeting an `https://` link does not ask Gateway for the page. It
opens a `CONNECT` tunnel and attempts its own handshake against a 2026 server,
which fails — Internet Explorer says *"an error occurred in the secure channel
support"*, Netscape 4 says *"no common encryption algorithm(s)"*. Neither
message mentions a proxy. Rewriting the links before the browser sees them is
what avoids that, and the plaintext hop it creates is loopback on the machine
Gateway is already running on.

Four things it cannot help with:

* **A URL typed by hand.** Type `https://…` in the address bar and the browser
  goes straight to `CONNECT`; there is no content to have rewritten. Type the
  `http://` form instead and Gateway takes it from there.
* **`https://` built up in JavaScript**, or percent- and backslash-escaped
  (`https%3A%2F%2F`, `https:\/\/`). Only the literal eight characters are
  matched.
* **Cookies marked `Secure`.** The browser now believes the connection is
  plain, so it will not send them. Sites that mark a session cookie `Secure`
  will not stay logged in.
* **The padlock.** There isn't one, and there shouldn't be — as far as the
  browser is concerned this is plain HTTP. The hop to the origin is still
  TLS 1.3; what is gone is the browser's own indication and enforcement of it.

## Wayback proxy

A second proxy port serving the web as it was, from the Internet Archive. The
live-web port keeps working at the same time, so a browser chooses its era by
which proxy it points at.

| Key | Default | Meaning |
|---|---|---|
| `wayback_port` | `8888` | Where browsers point for the archive. `0` disables the module. |
| `wayback_date` | `20011231` | The era: `YYYYMMDD`, `YYYYMM` or `YYYY`. |
| `wayback_tolerance` | `730` | How many days **newer** than `wayback_date` a snapshot may be. Older snapshots are always accepted — the archive returning 1999 for a 2001 request means that is the best it has. `0` accepts anything. |
| `wayback_geocities` | `1` | Send `geocities.com` to its successor, `oocities.org`. |
| `wayback_ct_encoding` | `1` | `0` strips `; charset=…` from `Content-Type`, which some period browsers choke on. |
| `wayback_settings` | `1` | Serve the settings page on `web.archive.org` and on `gateway`. |
| `wayback_quick_images` | `1` | Accepted for settings-page compatibility and does nothing. It tells the reference proxy to rewrite asset URLs in the HTML; Gateway fetches with the archive's `id_` modifier, which returns the original bytes with no HTML to rewrite. |
| `wayback_cache` | `1` | Replace the archive's half-hour freshness with a year, since a snapshot cannot change. `0` passes the origin's caching through unaltered. |
| `wayback_live` | — | Hosts to fetch live instead of from the archive. Repeat the key, one pattern per line. `*` and `?` are wildcards and matching is case-insensitive, so `*.frogfind.com` covers the subdomains. |

The era is changed from the browser, not from Gateway, by visiting the settings
page on the Wayback port:

```
http://web.archive.org/?date=20011231&dateTolerance=730&targetUrl=frogfind.com
```

`targetUrl` is optional: give it and Gateway saves the settings and sends you
straight there. The choice persists, so the next launch starts in the same era.

## Mail

Gateway listens in the clear on these ports and speaks TLS with the provider,
authenticating with OAuth. Set the mail client to use **no SSL** and, for SMTP,
authentication **on**.

| Key | Default | Meaning |
|---|---|---|
| `imap_port` | `1993` | Local IMAP port. |
| `pop_port` | `1995` | Local POP3 port. |
| `smtp_port` | `1587` | Local SMTP port. |
| `local_password` | — | The password typed into the mail client. Checked here and never sent anywhere. Unset refuses every login. |

### Provider

`provider` supplies the endpoints so they need not be listed individually.

| `provider` | IMAP | POP | SMTP | OAuth |
|---|---|---|---|---|
| `outlook` (default) | `outlook.office365.com` | `outlook.office365.com` | `smtp-mail.outlook.com` | `login.microsoftonline.com` |
| `gmail` | `imap.gmail.com` | `pop.gmail.com` | `smtp.gmail.com` | `oauth2.googleapis.com` |
| `custom` | — | — | — | — |

Anything set explicitly overrides the table, so a single different hostname does
not require `custom`. Use `custom` when none of the presets fit: it supplies
nothing, and the settings below stand on their own.

| Key | Default | Meaning |
|---|---|---|
| `imap_host`, `pop_host`, `smtp_host` | from `provider` | Upstream servers. |
| `imap_upstream_port` | `993` | Implicit TLS. |
| `pop_upstream_port` | `995` | Implicit TLS. |
| `smtp_upstream_port` | `587` | With `smtp_starttls = 1`. Use `465` with `smtp_starttls = 0` for implicit TLS. |
| `smtp_starttls` | `1` for 587, `0` for 465 | Whether to upgrade an initially plaintext connection. |
| `oauth_host`, `oauth_path` | from `provider` | Token endpoint. |
| `oauth_scope` | from `provider` | Must name every protocol in use — a token without the POP scope is refused by the POP server even though it is valid. |
| `oauth_user` | — | The account address. |
| `oauth_client_id` | — | The OAuth client the refresh token belongs to. |
| `oauth_client_secret` | empty | Needed by Google even for desktop clients; Microsoft public clients do not use one. |
| `refresh_token` | — | Obtained out of band. Gateway rewrites this line whenever the provider rotates it, which is what keeps an account working for months. |

Gateway never runs the OAuth consent flow. Get a refresh token on a modern
machine — running [email-oauth2-proxy](https://github.com/simonrob/email-oauth2-proxy)
against the account once is the easiest route — and extract it with
`tools/extract-refresh-token.py`, which also prints the matching `client_id`
and scope. Note that a token stored by that proxy is encrypted: a value
beginning `gAAAAA` is ciphertext, not a token.
