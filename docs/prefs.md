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
| `max_sessions` | `12` | Concurrent proxy connections, clamped to 16. Each costs roughly 110 KB of the 8 MB partition. Too few shows up in the log as "proxy busy, dropped a connection", and the browser then retries, which makes it worse. |

## Web proxy

| Key | Default | Meaning |
|---|---|---|
| `http_port` | `8765` | Where browsers point for the live web. |
| `follow_redirects` | `auto` | `auto` follows only what the client cannot — a redirect from `http://` to `https://`, which a browser with no modern TLS could never follow itself — and passes everything else back so the browser follows it with its own cookies. `always` follows every redirect inside Gateway, which discards `Set-Cookie` and breaks media players. `never` passes all of them back. |
| `max_body_mb` | `0` | Ceiling on a relayed response body, in MiB. `0` means none, which is the default: bodies stream through a 32 KB buffer and are never held, so a limit truncates downloads without saving memory. |

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
