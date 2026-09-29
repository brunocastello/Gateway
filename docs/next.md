# After 0.3.8 — what is worth doing next

## Built for 0.3.9

- **`connect_upgrade`**, re-applied by hand from roytam1's `gw034` fork:
  answer a `CONNECT` to port 80 by terminating the inner plaintext request
  and re-originating it over TLS to port 443, single-shot. Off by default.
  Still wants one hardware/curl check before it can be called verified: a
  client that really sends `CONNECT host:80` and reads the answer back --
  `curl -p -x http://<gateway>:8765 http://example.com/` sends exactly that.
  (Was item 3 of *Ideas for 0.3.9*.)
- **A request for Gateway's own address, through Gateway**, `#47` from the
  2026-09-20 session: Classilla, on manual settings, asked the proxy for
  `http://proxyweb.com:8765/proxy.pac`, so Gateway opened a connection to
  itself and served its own request -- it worked, but spent a splice slot
  on the loop. An absolute-form request whose host:port names one of
  Gateway's own listeners is now answered locally
  (`gw_pac_is_self()`, `src/portable/gw_pac.c`) instead. Conservative:
  nothing counts as "us" beyond 127.0.0.1, localhost and a small remembered
  set of up to `GW_MAX_SELF_HOSTS` authorities a script fetch was last
  served for (`GW_SelfHostAt()`, `src/gw_core.c`) -- a set rather than one
  slot, so another client on the LAN fetching `/proxy.pac` with its own
  `Host` header cannot evict the address the real browser is using and
  bring the self-loop back for it. The first request for a given address
  still loops once, which is also what teaches Gateway that host.
- **Check for updates**, from the premise probed below: a little after the
  listeners are up, Gateway makes one HTTP/1.0 request of
  `https://github.com/brunocastello/Gateway/releases/latest` with its own
  TLS stack and reads only the `Location` header of the `302` it gets back --
  no JSON, no `api.github.com`, no rate limit. The tag is parsed and compared
  numerically against `GW_VERSION_STRING`; a newer release logs one line with
  the direct asset link for this platform, given as `http://` because the
  browsers Gateway serves reach it through Gateway's own `:8765` proxy
  (`follow_redirects=auto` follows both `https` hops on the way to
  `objects.githubusercontent.com`). Up to date, or the request fails for any
  reason, is silent in the plain log -- a failed check must never look like a
  Gateway fault. `check_updates` (default `1`) turns it off. The version
  arithmetic (tag parsing, numeric comparison, asset name and link) is
  portable and host-tested, `src/portable/gw_update.c`; the request itself is
  a small state machine on a `GWStream`, driven from the cooperative loop the
  way `gw_token.c` drives a token refresh -- `src/proxy/gw_updater.c`. New
  log code `G50`.
- **An RSA_EXPORT ServerKeyExchange**, for the SSL 3.0 / TLS 1.0 40-bit
  suites (0x0003, 0x0006): 2026-09-29 hardware evidence (item 2 below) found
  32-bit IE 3.0 offering suite 0003 and nothing else, then hanging up on our
  Certificate with no key it was allowed to encrypt a pre-master secret to.
  RFC 6101 §5.6.3/§5.6.7 and RFC 2246 §7.4.3 both call for a temporary
  512-bit key, signed with the certificate's key, whenever the certificate
  key is longer -- Gateway's 1024-bit leaf always is. Built in full: a
  lazily generated, process-lifetime temporary key
  (`third_party/certainly/src/gw_export_key.c`), portable and host-tested
  wire framing (`src/portable/gw_skexport.c`), and the matching
  ClientKeyExchange decrypt against the temporary key rather than the
  certificate's. See PATCHES.md §38 for the full account, including why the
  same message was tried once and removed (5e13663 -- a missing 2-byte
  signature length prefix, not a client that rejected the idea). **Pending:**
  a re-run against the IE 3.0 hardware that reported this, and a Netscape
  3.04 regression check -- it negotiates export suites today and completes
  *without* this message, and PATCHES.md §36 records that the same browser
  rejects the message outright when it is sent, from the previous attempt.
  Neither has been re-tested since this was built.

0.3.8 shipped on 2026-09-28. The upstream side is TLS 1.3 and finished; the
browser side serves everything from Netscape 3 to Classilla over SSL 3.0 or
TLS 1.0; mail works through `get-email-token.py`; a generic tunnel carries
SSH or anything else over TLS. What remains is making the machine Gateway
runs on feel less punished for it. This document records the candidates in
order of value, with the evidence each rests on, so a later session can pick
one up without re-deriving it — and records what was tried and is not worth
trying again.

---

## Ideas for 0.3.9

Picked on 2026-09-28 for later sessions; each is written up further down.
Item 3 is done (see *Built for 0.3.9* at the top); items 1 and 2 are closed.

1. **Keep-alive inside the terminated tunnel** — closed 2026-09-29 as not
   worth building. (Item 1 below.)
2. **IE 3.0's hello bytes** — the 16-bit build logs S01 on 0.3.8. The 32-bit
   build was reopened 2026-09-29 with a usable image and answered: it
   offers only a 40-bit suite and hangs up on our Certificate with nothing
   it can encrypt to. See *An RSA_EXPORT ServerKeyExchange* above, now
   built and pending a hardware re-run. (Item 2 below.)
3. **roytam1's `connect_upgrade`** — built. See *Built for 0.3.9* at the top;
   the hardware/curl check is still outstanding.

Settled the same night, after 0.3.8 shipped:

- *Resumption on the remaining browsers* (IE 5.1.7 Mac, 16-bit IE 5): marked
  completed by the user.
- *Prefs writer polish* from the 0.3.7 settings review: uncommenting now
  removes only the `# ` a comment added, so an indented line comes back
  indented. Setting-shaped prose (`# provider = custom is accepted`) still
  reads as a commented setting — it cannot be told from a value with spaces,
  as `oauth_scope`'s has — and a host test pins that as accepted. "Rewriting
  a line drops what follows its value" was not a defect: in this format a `#`
  after a value is part of the value.
- *Separate proxies for IE and Outlook Express on Windows*: discarded.

## Shipped in 0.3.8

- **TLS session resumption** on the browser side of `connect_mitm`: one LRU
  cache, `, resumed` on the handshake line, S22 and a forgotten session when
  a browser rejects a resumption (`PATCHES.md` §36). Verified on Windows 95
  with IE 4 and Netscape 4.08; IE 5.1.7 Mac and 16-bit IE 5 marked completed
  afterwards.
- **The post-0.3.7 audit**: `wayback_api` removed (below), the Tunnel's
  remote host required while the tunnel is on, the inverted "Keep the
  charset in Content-Type" label corrected, stale docs brought up to date.

## Shipped in 0.3.7

For the record, and for where to look when something in them misbehaves:

- **Six fixes from roytam1's `gw034` fork**, re-applied by hand: the idle
  slot leak, the doubled `200 Connection Established`, interim 1xx
  responses, and on the Certainly side the fallback redial, the `03 01`
  record version and mid-handshake close detection (`PATCHES.md` §29–§31).
- **Module 4, the tunnel** (`:2222`), from the same fork. `tunnel_tls12`
  and `tunnel_insecure` came mostly as written (`PATCHES.md` §32, §33);
  `tunnel_sni` was rewritten so the certificate is always checked against
  `tunnel_remote_host` (§34).
- **The readable log**: plain sentences with stable codes
  (`docs/log-codes.md`), `log_debug` for the engineer lines. Certainly
  hands its MITM outcome to the proxy instead of logging it (§35), and
  far-end failures go through one explainer, `GWStream_Explain()`.
- **Settings that follow their choices**: the custom mail servers are read
  only under `provider = custom`, and both Preferences windows dim and
  comment out whatever does not apply under the current provider, TLS and
  proxy choices. The rule is `src/portable/gw_gate.c`.

---

## Resolved: `wayback_api` removed

Found in the post-0.3.7 audit: both Preferences windows showed "Find the
nearest available snapshot" (`wayback_api`), `gw_core.c` read it, and nothing
ever looked at the flag — a checkbox reporting a choice Gateway did not
honour. Gateway reaches the nearest snapshot by following the archive's own
redirect, which is upstream's `WAYBACK_API` *off* path; the *on* path would
have cost an extra TLS request per page for much the same answer. Removed on
`gw038`: the rows, the read, the struct field. A `wayback_api` line left in an
old prefs file is ignored, like any unknown key. If the Availability API is
ever wanted, `docs/settings-window.md` §4.3 has the design and its two traps.

## Resolved: `wayback_quick_images` removed

Both Preferences windows showed "Quick images", `gw_core.c` read it into
`GWWaybackSettings.quick_images`, and nothing ever looked at the flag: Gateway
always fetches with the archive's `id_` modifier, which returns the origin's
original bytes with no HTML to rewrite, so there was never anything for the
setting to turn on or off (`docs/module3-wayback.md` §3). Removed on `gw039`:
the rows, the read, the struct field, and the checkbox on the Wayback
settings page. The settings page still *accepts* `quickImages` in a submitted
query string -- an old bookmark or saved form keeps working -- it is simply
never read, like any other unknown key. A `wayback_quick_images` line left in
an old prefs file is likewise ignored.

## 1. Keep-alive inside the terminated tunnel — not worth building

Closed 2026-09-29 by the user's decision, without building it. Resumption
already removes the expensive part of each extra handshake — the RSA step —
and what keep-alive would add on top costs a splice slot held for as long as
the browser likes, out of `max_sessions` (12 by default, at most 16), and
breaks the one rule that keeps the cooperative loop simple. Reopen only with
a measured page where resumption is not enough. The original write-up
follows.


The larger version of resumption's win: one handshake per host rather than
per resource. It collides with the design rule that the client hop is always
`Connection: close` (CLAUDE.md, Module 1), which exists for the cooperative
loop's sake — a kept-alive TLS connection is a splice slot held for as long
as the browser likes. Resumption, shipped in 0.3.8, gets most of the benefit
without touching that rule; measure a page with it before deciding whether
this is still worth its cost.

## 2. IE 3.0's hello bytes — closed 2026-09-29

**Result.** Re-run on 2026-09-29 with 0.3.8 on Windows 95 OSR2 (86Box), the
16-bit Windows 3.1 build of IE 3.02, `connect_mitm 1`, a typed
`https://example.com/`: `#1 Gateway answers the browser's secure connection
to example.com:443 itself`, then `the browser closed the secure connection
without starting it (S01)`. The tunnel opens, the browser writes nothing and
closes — not a hang, and no hello bytes exist to read, so PCT on or off
cannot matter. This is the first logged confirmation of what 2026-09-20
inferred from silence. The same browser loads plain `http://` pages through
Gateway (FrogFind, in the same session), so it is served by `rewrite_https`,
the default, and that is its supported path.

The 32-bit question below stays unanswered, and is closed as untestable
rather than open: it needs a Windows 95 image that never had IE 4, since the
32-bit 3.02 setup will not install over IE 4 (see *Tried and not worth
repeating*). Reopen it only with such an image; the two runs below take ten
minutes.

What was planned, kept for that case:

32-bit IE 3.0x on Windows 95 has only ever produced "CONNECT, terminating
TLS, then silence" — three attempts alike, all on builds from before the
hello logger existed (fc4bb6f). No hello bytes from any IE 3 have been seen.
The working hypothesis is that it sends a PCT hello by default (`80 .. 80 01`
in the hello log), which is not SSL and is not served; that is what IE 3.0
was documented to do, not something the log has shown.

The test is therefore two steps on the 32-bit build, with 0.3.6 or later:
read the hello bytes as shipped, and then again with "PCT 1.0" unticked in
Options ▸ Advanced. If the second attempt shows an SSLv2-framed hello with
version `03 00`, PATCHES.md §22 already converts it and IE 3 needs a line in
the README, not code. If both attempts show nothing at all, IE 3's secure
path is failing before it writes — the same signature as the 16-bit build
below — and the abandon and timeout lines will say whether it closed or hung.

Already known: the 16-bit Windows 3.1 build of 3.0 fails identically with
PCT unticked and SSL 2/3 ticked (tried 2026-09-20). Its failure is before
protocol selection, so it says nothing about the 32-bit build.

**Reopened and answered, 2026-09-29.** A Windows 95 image without IE 4
turned up, so the 32-bit question above was no longer untestable. 32-bit IE
3.0 (4.70.1215), `connect_mitm 1`, `allow_sslv3 1`, a typed `https://`:

```
#2 the browser gave up after seeing our certificate (S03)
  SSLv2 hello, version 0300, suite 0003, rx 33, rx after our flight 0, in-rectype 22, incrypt 0, session new
  first bytes: 80 1f 01 03 00 00 06 00 00 00 10 02 00 80 00 00 03 ...
  offered suites: 020080 000003
```

Hello bytes at last: an SSLv2-framed hello, version `03 00` (already
converted by PATCHES.md §22, as hoped), offering only suite `0003` --
RSA_EXPORT_RC4_40_MD5, a 40-bit-only suite. Not a PCT default, not a dead
secure path: it asked for the one thing Gateway could not give it. RFC 6101
§5.6.3/§5.6.7 and RFC 2246 §7.4.3 both require a ServerKeyExchange carrying a
temporary key of at most 512 bits whenever the certificate key is longer,
which Gateway's 1024-bit leaf always is; Gateway sent none, so the browser
received our Certificate and ServerHelloDone with nothing it was allowed to
encrypt a pre-master secret to, and hung up. Built in PATCHES.md §38 (see
*Built for 0.3.9* at the top and the *RSA_EXPORT ServerKeyExchange* entry
below) -- host-tested for its wire framing, not yet re-run against this
hardware. Reopen this item only if that re-run still fails.

---

## Tried and not worth repeating

**IE 3.02 on an image that has IE 4.** The 3.02 setup refuses to run over
IE 4, and no `ieremove.exe` could be found to downgrade. Two ways round it
were tried on 2026-09-20 and both produced a CONNECT, `terminating TLS`, and
then silence — the client never sent a hello, so neither is a data point:

- *The 16-bit Windows 3.1 build of 3.0.* It carries its own secure-channel
  code rather than `schannel.dll`, and that code fails on Windows 95 after
  the tunnel opens. The browser reports the load as failed without writing
  a byte, with PCT on or off. The same build loads `http://www.howsmyssl.com/`
  through Gateway: that is the plain-proxy path, where Gateway follows the
  site's redirect to https itself and the browser does no TLS at all. So the
  16-bit IE 3 is usable with `rewrite_https` (the default); only its own SSL
  is dead on Windows 95.
- *An extracted 3.02 folder.* Its `iexplore.exe` creates the browser through
  COM, the registry points at IE 4's `shdocvw.dll`, and the result is IE 4's
  frame over a mix of DLLs. The https path dies with error 120,
  `ERROR_CALL_NOT_IMPLEMENTED` ("This function is only valid in Win32
  mode") before any handshake.

Testing 3.02 properly means a Win95 image that never had IE 4. The
verification the test was meant to provide — a real browser driving the §28
ClientKeyExchange and Finished bridges to completion — was provided instead
by IE 5.1.7 for Mac, which completes its second flight on suite 0004.

**IE 5.1.7's double connect.** It opens two CONNECT tunnels per host: the
first with an SSLv2-framed hello, abandoned at `rx-after-flight 0` without
replying to our certificate; the second with a native SSL 3.0 hello, which
completes. It is the browser's own probing, costs one abandoned handshake per
host, and is harmless. Resumption (item 1) would make the second tunnel
cheap; nothing makes the first go away.

**An RSA_EXPORT ServerKeyExchange — no longer in this section.** The
precondition this entry asked for (a client's hello offering nothing but
0003/0006, abandoning at `rx-after-flight 0`) was met on 2026-09-29 by
32-bit IE 3.0, and the message was built complete, with a real temporary
key and the matching ClientKeyExchange decrypt -- see *An RSA_EXPORT
ServerKeyExchange* under *Built for 0.3.9* and item 2 above, and PATCHES.md
§38. roytam1's earlier attempt (removed in 5e13663) is kept as reference in
that section: it was never sent, and separately from Netscape 3.04
rejecting the idea outright, its signature had no length prefix, so no
client that read that far would have accepted it either.

**The auto-configuration script on Classilla.** Not working as of
2026-09-20: Classilla fetches `/proxy.pac` (the log shows it served, 556
bytes), then makes every request DIRECT and fails its own handshake with "no
common encryption algorithms". JavaScript on, `network.proxy.type` = 2, the
same result via `127.0.0.1` and a hosts-file name. Manual settings with the
same address work and are what the README documents, so the user stays on
manual. Cause not established. Classilla's resolver
(`nsProxyAutoConfig.js`, 9.3.3) answers DIRECT while the script is still
loading and re-creates the loader on every Reload, and answers DIRECT for
ever if the script throws — the JavaScript Console names the error. Both
checks were then run: after a wait, a plain `http://frogfind.com/` load
produced no line in Gateway's log and no error in the JavaScript Console --
the resolver answers DIRECT for plain http too, silently. That leaves only
the PAC component failing to create, which reports nothing outside a debug
build. Classilla's source has everything the path needs (the component, the
sandbox globals, the handler flags); whether the shipped build has
`Components:nsProxyAutoConfig.js` was not checked. Nothing on Gateway's
side is implicated; treat it as a Classilla defect. The
script itself is verified: 557 bytes generated for that authority, syntax
within Netscape 3's engine. The commit that added the PAC (4281342) asserted
Classilla support without testing it; the README makes no such claim.
