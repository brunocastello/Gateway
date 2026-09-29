# Gateway Prefs

One setting per line, `key = value` or `key: value`. `#` or `;` at the start of
a line makes it a comment; a `#` **after** a value is part of the value, so keep
comments on their own lines. Keys are case-insensitive, whitespace around them
is trimmed, and CR, LF or CRLF line endings all work.

The file lives in the System Preferences folder as **Gateway Prefs**. A
starting point is `docs/prefs-example.txt`. Gateway reads it once at launch,
and rewrites two lines itself: `refresh_token` when the provider rotates it,
and `wayback_date`/`wayback_tolerance` when the settings page is used. The
Preferences window also comments out or restores the settings that do not
apply under the current choices (see [Provider](#provider) and
[Tunnel](#tunnel)).

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
| `tunnel_enabled` | `0` | Run Module 4, the generic TLS tunnel on `tunnel_local_port`. Off by default: unlike the other modules it forwards whatever bytes the local port receives, with no local authentication of its own, so switch it on deliberately. |
| `log_file` | `0` | Mirror the log window to a file. `1` (or `yes`/`on`) writes to **System Folder : Application Support : Gateway : Gateway Log.txt**, creating both folders if needed. Lines are appended across runs and written as they happen rather than buffered, so the tail survives a crash. The window keeps only the last 200 lines, which one slow page load can exceed, so this is the way to capture a whole session. |
| `log_debug` | `0` | Add the engineering detail under each log line: hello bytes, suite numbers, byte counts and library error numbers. Off, the log is plain sentences, each ending in a short code such as `(H12)` that says exactly which branch wrote it -- see [log-codes.md](log-codes.md). Turn it on when reporting a problem, and send the log with it. Read at launch. |
| `check_updates` | `1` | A little after the listeners are up, ask `github.com` once whether a newer release exists: an HTTP/1.0 `GET` of `/brunocastello/Gateway/releases/latest` over Gateway's own TLS stack, sending nothing but that request line and a `User-Agent`. The `302` it answers with is read for its `Location` header and never followed. A newer release logs one line with the direct download link for this platform; up to date, or the request fails for any reason -- no network, a TLS failure, no `Location` -- and nothing appears in the plain log, so a failed check never looks like a Gateway fault. `0` skips the request entirely. |
| `max_connects` | `8` | How many upstream connections may be *opening* at once for ordinary live-web sessions, clamped to 8. |
| `wayback_connects` | `1` | The same cap for sessions served from the Internet Archive, counted separately so neither starves the other. It is low because a burst of new connections from one address is exactly what the archive's rate limiter refuses, logged as `connect failed [failed, OT 61, ...]`. Gateway runs a single cooperative thread, so concurrent TLS handshakes do not overlap anyway -- they take turns on one CPU -- and the connection pool recovers the throughput, since a reused connection skips the handshake entirely. |

## Web proxy

| Key | Default | Meaning |
|---|---|---|
| `http_port` | `8765` | Where browsers point for the live web. |
| `follow_redirects` | `auto` | `auto` follows every redirect whose destination is `https://`, because the client hop is always plaintext and a browser with no modern TLS could never follow one itself; a redirect to `http://` goes back to the browser, which follows it with its own cookies and knows where it ended up. `always` follows every redirect inside Gateway, which discards `Set-Cookie` and breaks media players. `never` passes all of them back. |
| `rewrite_https` | `1` | Turn `https://` into `http://` in HTML, CSS and JavaScript on the way to the browser, so a link the user clicks comes back to Gateway instead of becoming a `CONNECT` tunnel the browser cannot complete. Set to `0` for a client with its own modern TLS — RetroZilla, or `git` — where it buys nothing. See below for what it does not reach. |
| `connect_mitm` | `0` | Terminate TLS on the browser's side of a `CONNECT` **to port 443** instead of tunnelling, so a typed `https://` URL works. A `CONNECT` to any other port stays a raw tunnel, since nothing obliges it to be TLS — `git`, ssh through a proxy and anything else that just wants bytes moved are unaffected. Gateway generates its own certificate authority on first use and presents a certificate it signed for each host — so the browser warns until that authority is installed. The browser hop is encrypted with what the browser can do: 3DES for IE 4 and later over TLS 1.0, RC4 or RC2 for the SSL 3.0 browsers (see `allow_sslv3`). The first connection to a host costs a full handshake; later ones can resume the session, skipping the slow RSA step, and the log then says `resumed` (a browser sending the SSL 2.0-framed hello cannot resume). Leave it off unless you need the address bar or `Secure` cookies. |
| `allow_sslv3` | `1` | Inside `connect_mitm`, offer SSL 3.0 to a browser that offers nothing better. Netscape 3 and IE 4 have no TLS at all and could previously reach nothing but `rewrite_https`; with this they get a real `https://` address bar, over RC4 (or export-grade RC2, for a browser that offers nothing else). The 16-bit (Windows 3.1) build of IE 3 closes the tunnel without sending a byte (`S01`) and still needs `rewrite_https`; the 32-bit build is untested. BearSSL's server takes the highest version in common, so a browser that can manage TLS 1.0 still gets TLS 1.0 and never sees SSL 3.0. Set to `0` to refuse it — the handshake then fails as it did before, and `rewrite_https` still serves those browsers. |
| `connect_upgrade` | `0` | Answer a `CONNECT` **to port 80** by terminating the plaintext HTTP inside the tunnel and re-originating it over TLS to port 443 — single-shot, one inner request per `CONNECT`, then the tunnel closes. No certificate is presented to anyone, so this is protocol translation rather than MITM: it answers the opposite case from `connect_mitm`, a client that never starts a handshake of its own. Off by default and opt-in, because a `CONNECT` may carry anything — `git`, ssh, a custom protocol — and parsing those bytes as HTTP would break them; with it off, `CONNECT host:80` stays a raw tunnel as today. |
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

### What `connect_mitm` needs from the browser

**TLS 1.0, which BearSSL's floor requires.** BearSSL implements TLS 1.0, 1.1
and 1.2 and no SSL at all — `BR_SSL30` is a header constant with nothing behind
it. Tick **Use TLS 1.0** under **Tools → Internet Options → Advanced →
Security**; Netscape 4.7 has the equivalent under **Security → Navigator →
Configure SSL**. So IE 5.5 and IE 6 work; IE 4, which has SSL 3.0 and no TLS,
is out of reach whatever its settings say.

When the version is what failed, the log gives the number the browser offered
and says which side objected:

```
#3 handshake with the browser failed for lite.duckduckgo.com
   (BearSSL 582: it offered SSL 3.0 at best and TLS 1.0 is the floor
    (we refused its hello))
```

**"Use SSL 2.0" no longer has to be off.** It used to be the setting that
caught everybody. A browser with SSL 2.0 enabled sends its ClientHello in SSL
2.0 framing — no TLS record header, just a length with its high bit set
followed by message type `01` — and BearSSL rejected that format outright,
because the byte it reads as a protocol version is a length byte. The
handshake died before a single field was parsed, which is why **no certificate
warning appeared**: there was no certificate yet to warn about. The log said
`BearSSL 3`.

Since 0.3.4 Gateway converts that framing into the TLS hello it stands for
(`third_party/certainly/PATCHES.md` §22), so the box can stay as the browser
shipped it. `BearSSL 3` now means something narrower — the hello inside asked
for SSL 2.0 itself, which happens only when SSL 3.0 and TLS 1.0 are both off.

A browser with its own modern TLS — Classilla, RetroZilla — needs none of this
and should leave `connect_mitm` off: `CONNECT` is a raw tunnel then, and the
browser does its own handshake with the origin.

With a browser that cannot be brought to TLS 1.0, use `rewrite_https` and type
addresses without a scheme. Links, redirects and subresources all work; only
the address bar and `Secure` cookies do not.

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
| `wayback_ct_encoding` | `1` | Keep `; charset=…` in `Content-Type`. `0` strips it, for period browsers that choke on it. The Preferences window calls it "Keep the charset in Content-Type". |
| `wayback_settings` | `1` | Serve the settings page on `web.archive.org` and on `gateway`. |
| `wayback_cache` | `1` | Replace the archive's half-hour freshness with a year, since a snapshot cannot change. `0` passes the origin's caching through unaltered. |
| `wayback_live` | — | Hosts to fetch live instead of from the archive. Repeat the key, one pattern per line. A plain host name covers the site and everything under it, so `frogfind.com` also matches `www.frogfind.com`; a pattern containing `*` or `?` is a glob and matches only what it says. Case-insensitive either way. A blank entry is skipped rather than ending the list. |

An old `wayback_quick_images` line left in the prefs file is ignored, like any
unknown key — the setting it controlled never did anything and was removed.

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

`provider` supplies the upstream servers and the token endpoint, so under
Outlook or Gmail they are not written at all.

| `provider` | IMAP | POP | SMTP | OAuth |
|---|---|---|---|---|
| `outlook` (default) | `outlook.office365.com` | `outlook.office365.com` | `smtp-mail.outlook.com` | `login.microsoftonline.com` |
| `gmail` | `imap.gmail.com` | `pop.gmail.com` | `smtp.gmail.com` | `oauth2.googleapis.com` |
| `custom` | — | — | — | — |

The settings below, down to `oauth_scope`, are read **only** under `custom`.
Under `outlook` or `gmail` the provider's own values stand, and a copy left in
the file is ignored, so a host kept from an earlier custom setup cannot quietly
redirect the mail. A single different hostname therefore needs `custom` with
all of them filled in. The example file keeps them commented out; the
Preferences window shows them dimmed with the provider's values, and on Save
comments them out under Outlook or Gmail and writes them back in place under
Custom, so switching away and back loses nothing.

| Key | Default | Meaning |
|---|---|---|
| `imap_host`, `pop_host`, `smtp_host` | from `provider` | Upstream servers. Required under `custom`. |
| `imap_upstream_port` | `993` | Implicit TLS. |
| `pop_upstream_port` | `995` | Implicit TLS. |
| `smtp_upstream_port` | `587` | With `smtp_starttls = 1`. Use `465` with `smtp_starttls = 0` for implicit TLS. |
| `smtp_starttls` | `1` for 587, `0` for 465 | Whether to upgrade an initially plaintext connection. |
| `oauth_host`, `oauth_path` | from `provider` | Token endpoint. Required under `custom`. |
| `oauth_scope` | from `provider` | Must name every protocol in use — a token without the POP scope is refused by the POP server even though it is valid. |

These are read for every provider, and are what `get-email-token.py` prints:

| Key | Default | Meaning |
|---|---|---|
| `oauth_user` | — | The account address. |
| `oauth_client_id` | — | The OAuth client the refresh token belongs to. |
| `oauth_client_secret` | empty | Needed by Google even for desktop clients; Microsoft public clients do not use one. |
| `refresh_token` | — | Obtained out of band. Gateway rewrites this line whenever the provider rotates it, which is what keeps an account working for months. |

Gateway never runs the OAuth consent flow: the providers' sign-in pages need a
modern browser, and no browser Gateway serves can render them. Get the token
on a modern computer with `get-email-token.py`, which ships with every release
and lives in `tools/` — macOS, Windows or Linux, any Python 3, nothing to
install. It opens the sign-in in your browser, then prints the `provider`,
`oauth_client_id`, `oauth_client_secret` and `refresh_token` lines to put
here. It uses Thunderbird's public clients, so there is no application to
register.

The splice itself follows [email-oauth2-proxy](https://github.com/simonrob/email-oauth2-proxy),
which does the same job on a modern machine. A token from its config file does
not paste across: that proxy stores tokens encrypted, and a value beginning
`gAAAAA` is ciphertext. Run `get-email-token.py` instead.

## Tunnel

Module 4, a generic relay for one TCP stream:

```
local_app --plain--> :tunnel_local_port --TLS--> [proxy] --> remote:port
```

The local application speaks plaintext to Gateway; Gateway opens the far leg
— directly, or through a forward proxy — upgrades it to TLS when `tunnel_tls`
is set, and splices bytes both ways until either side closes. The far side
unwraps TLS (stunnel and friends) and hands the stream onward. SSH is the
motivating use, not the protocol: for it, the SSH client points at
`tunnel_local_port` and authenticates to `sshd` exactly as if the tunnel were
not there.

Some of these only mean something under another one's value, and the
example file keeps them commented out in a group of their own:

| Only read when | Settings |
|---|---|
| `tunnel_tls = 1` | `tunnel_tls12`, `tunnel_insecure`, `tunnel_sni` |
| `tunnel_proxy` is `http` or `socks5` | `tunnel_proxy_host`, `tunnel_proxy_port`, `tunnel_settle_ms` |
| `tunnel_proxy = http` | `tunnel_proxy_user`, `tunnel_proxy_pass`, `tunnel_host_header` |

The Preferences window dims those rows while they do not apply and, on Save,
comments their lines out, keeping the values; they come back in place when
the row applies again. The commented values in the example are the defaults,
so turning TLS on does not quietly bring back `tunnel_insecure = 1`.

| Key | Default | Meaning |
|---|---|---|
| `tunnel_local_port` | `2222` | Where the local application connects. |
| `tunnel_remote_host` | — | The far endpoint. Required; without it clients are dropped and the log says so. This is also the certificate identity checked on the far leg, always, whatever `tunnel_sni` sends — see below. |
| `tunnel_remote_port` | `443` | The far port. |
| `tunnel_tls` | `1` | Wrap the far leg in TLS. `0` relays plaintext — only for a far leg that is already safe. |
| `tunnel_tls12` | `0` | Speak only TLS 1.2 on the far leg, for far ends with no TLS 1.3 (an old stunnel). The default negotiates 1.3 first, but falling back from it needs a fresh connection the tunnel cannot re-open past a proxy — so a 1.2-only far end fails unless this is set. When set, the log reports `TLS 1.2` on success like any other 1.2 negotiation. |
| `tunnel_insecure` | `0` | Testing only: accept any certificate on the far leg — wrong name, private CA, expired — without checking. The tunnel then proves only that bytes flow, not who they flow to, so turn it back off afterwards. Never applies to mail, the web proxy, or the archive. |
| `tunnel_sni` | empty | Diagnosis only: what the handshake sends as the SNI extension. Empty (the default) sends `tunnel_remote_host`. `none` omits SNI entirely; anything else is sent instead. One corporate proxy answered a nameless ClientHello and stalled one carrying the hostname, with no other difference on the wire. **This changes only what is sent — the certificate is always checked against `tunnel_remote_host`, never against an override, and never skipped just because SNI was omitted**, unless `tunnel_insecure` is also set. |
| `tunnel_proxy` | `none` | `none`, `http` (CONNECT, with `Proxy-Authorization` when a user is set) or `socks5` (no-auth only). Anything else drops the client and logs the valid values. |
| `tunnel_proxy_host` | — | The proxy. Required unless `tunnel_proxy` is `none`. |
| `tunnel_proxy_port` | `8080` / `1080` | The proxy port: `8080` for `http`, `1080` for `socks5`. Set explicitly to override. |
| `tunnel_settle_ms` | `0` | Diagnosis: milliseconds to wait after the proxy accepts before starting TLS. One proxy answered 200 before its own upstream splice was ready, so the first flight sent inside a millisecond fell into the void with no RST and no reply; delaying past that race (try `2000`) tells a setup race apart from a byte-level block. Clamped to 30000. |
| `tunnel_proxy_user`, `tunnel_proxy_pass` | empty | HTTP proxy credentials (Basic). A SOCKS5 login is not implemented: setting one refuses the connection loudly rather than connecting anonymously. |
| `tunnel_host_header` | `1` | Send a `Host:` line in the proxy CONNECT request. `0` omits it (request line, optional auth, blank line — byte-for-byte what `socat` sends). One proxy answered the `Host` form with 200 and then stalled the tunnel past the handshake timeout, while passing the bare form. HTTP/1.0 does not require `Host`: the authority is already in the request line. |

Up to four sessions run at once; surplus clients wait in the listen backlog.
A session stuck opening the far leg, talking to a proxy, or negotiating TLS
times out after 45 seconds like any other module. Once the splice is up
there is no idle timeout at all — an SSH session sits quiet for hours — so
that slot is held until EOF or an error on either side. The tunnel performs
no local authentication of its own: anything that can reach
`tunnel_local_port` can use it, so keep that port behind the machine's own
boundary.
