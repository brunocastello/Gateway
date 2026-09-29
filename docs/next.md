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
  nothing counts as "us" beyond 127.0.0.1, localhost and a remembered
  `self_host` -- the authority a script fetch was last served for -- so the
  first such request still loops once, which is also what teaches Gateway
  the host for next time.

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
Item 3 is done; see *Built for 0.3.9* at the top.

1. **Keep-alive inside the terminated tunnel** — one handshake per host
   rather than per resource. Measure a page with 0.3.8's resumption first;
   it may already be enough. (Item 1 below.)
2. **IE 3.0's hello bytes** — read what 32-bit IE 3 sends, with and without
   PCT ticked; it may need a README line rather than code. Needs a Windows 95
   run with `log_debug` on. (Item 2 below.)
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

## 1. Keep-alive inside the terminated tunnel

The larger version of resumption's win: one handshake per host rather than
per resource. It collides with the design rule that the client hop is always
`Connection: close` (CLAUDE.md, Module 1), which exists for the cooperative
loop's sake — a kept-alive TLS connection is a splice slot held for as long
as the browser likes. Resumption, shipped in 0.3.8, gets most of the benefit
without touching that rule; measure a page with it before deciding whether
this is still worth its cost.

## 2. IE 3.0 may need a checkbox, not code — but first, its hello bytes

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

**An RSA_EXPORT ServerKeyExchange.** SSL 3.0's export suites call for a
temporary 512-bit key when the certificate key is longer. roytam1 built one
(never sent, public half only) and found no client wants it: Netscape 3.04
rejects the message outright and every other client encrypts to the 1024-bit
leaf without objecting. It was removed in 5e13663. Bring it back only if a
client's hello offers nothing but 0003/0006 and it then abandons at
`rx-after-flight 0` — and bring it back complete, with a private key and the
matching ClientKeyExchange decrypt.

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

---

## Considered, and the premise turned out favourable

**Check for updates.** Set aside on 2026-09-20 on the grounds that the
browsers Gateway serves cannot reach GitHub — but through Gateway they can:
that is what the proxy is for. Typed as `http://github.com/.../releases/
download/<tag>/<asset>` into a browser pointed at `:8765`, the request is
fetched over TLS 1.3 by Gateway, both `https` redirect hops (to the release
and on to `objects.githubusercontent.com`) are followed internally under
`follow_redirects = auto`, and the binary body streams through untouched
with its `Content-Length`. Untested as an actual download, but nothing in the
design stands in its way. The feature itself would be small: at launch,
Gateway asks `api.github.com/repos/brunocastello/Gateway/releases/latest`
with its own stack, compares the tag to `GW_VERSION_STRING`, and logs one
line with the direct asset link for this platform. No page to render.
