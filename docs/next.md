# After 0.3.9 — what is worth doing next

## Shipped in 0.3.9

Verified on hardware on 2026-09-29 unless a bullet says otherwise.

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

  Followed by a user-triggered "Check for Updates..." (Windows: the tray
  menu, above "About Gateway..."; Mac OS 9: the File menu, below
  "Settings..."), which runs the same check by hand -- ignoring
  `check_updates` and an already-finished launch check, and joining rather
  than duplicating one still in flight (`GWUpdater_RequestManual()`,
  `GWUpdater_ManualResult()`) -- and shows the answer in a dialog: newer with
  Download/Later, current, or why it failed, in the log's own plain words.
  Windows shows it with `MessageBoxA`, its OK/Cancel buttons relabelled
  Download/Later through a thread-local `WH_CBT` hook, since the shell has no
  other way to change a message box's button text. Mac OS 9 shows it with
  `NoteAlert`/`StopAlert` over a hand-written `ALRT`/`DITL` pair
  (`src/ui/gateway.r`) -- both exist in the Multiversal Interfaces, confirmed
  against `autc04/multiversal`'s own `DialogMgr.yaml`. Download opens the
  platform asset URL in the default browser: `ShellExecuteA` on Windows,
  looked up in `shell32.dll` at run time rather than linked, for the same
  reason `main_win32.c` already looks up `Shell_NotifyIconA` that way (NT
  3.51 has no `shell32.dll` at all); Internet Config's `ICLaunchURL` on Mac
  OS 9, reached from `gw_plat_mac.c` because that file already carries
  Apple's Universal Interfaces for Open Transport (CLAUDE.md rule 3), and
  linked against `libInternetConfigLib.a`, a CFM import stub that
  `interfaces-and-libraries.sh`'s `MakeImport` pass already produces for
  every shared library under `third_party/InterfacesAndLibraries` -- nothing
  new to stage (it has to follow the `gw_net` group on the link line; placed
  in front of it, the first build failed on three undefined references).

  The Download button was verified after the release, from a 0.3.8 install
  checking against the published 0.3.9 (2026-09-29). The Mac dialogs were
  then centred on the main screen (`999cb93`) and the 0.3.9 Mac downloads
  rebuilt from it.

0.3.9 shipped on 2026-09-29, and 0.3.8 on 2026-09-28. The upstream side is TLS 1.3 and finished; the
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
Items 1 and 2 are closed; item 3 was built, then reverted.

1. **Keep-alive inside the terminated tunnel** — closed 2026-09-29 as not
   worth building. (Item 1 below.)
2. **IE 3.0's hello bytes** — closed 2026-09-29: the 16-bit build logs S01
   on 0.3.8; the 32-bit build sends an SSLv2-framed SSL 3.0 hello and hangs
   up after our first flight regardless of the ServerKeyExchange or CA
   trust. Both are served by `rewrite_https`. (Item 2 below.)
3. **roytam1's `connect_upgrade`** — built, then reverted 2026-09-29: no
   client on hand could deliver a well-formed inner request end to end. See
   *Deferred, not rejected* below.

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

**16-bit result.** Re-run on 2026-09-29 with 0.3.8 on Windows 95 OSR2 (86Box), the
16-bit Windows 3.1 build of IE 3.02, `connect_mitm 1`, a typed
`https://example.com/`: `#1 Gateway answers the browser's secure connection
to example.com:443 itself`, then `the browser closed the secure connection
without starting it (S01)`. The tunnel opens, the browser writes nothing and
closes — not a hang, and no hello bytes exist to read, so PCT on or off
cannot matter. This is the first logged confirmation of what 2026-09-20
inferred from silence. The same browser loads plain `http://` pages through
Gateway (FrogFind, in the same session), so it is served by `rewrite_https`,
the default, and that is its supported path.

**32-bit result.** Reopened 2026-09-29 with a Windows 95 image that never had
IE 4 (see *Tried and not worth repeating*). 32-bit IE 3.01 (4.70.1215),
`connect_mitm 1`, `allow_sslv3 1`, a typed `https://`: it sends an
SSLv2-framed SSL 3.0 hello offering only suite `0003`
(RSA_EXPORT_RC4_40_MD5) -- plus PCT markers when "PCT 1.0" is ticked -- and
hangs up after our first flight (Certificate + ServerHelloDone) with no
alert: `S03`, `SSLv2 hello, version 0300, suite 0003, rx 51, rx after our
flight 0`, `offered suites: 8f8001 800001 810001 820001 830004 842840
020080 000003`. This held with or without Gateway's CA installed in the
browser's trust store, and with or without a correctly framed RSA_EXPORT
ServerKeyExchange supplying the temporary key the suite calls for (built and
tested on this hardware 2026-09-29, then reverted -- see PATCHES.md §38's
history and *An RSA_EXPORT ServerKeyExchange* below). Neither the
certificate chain nor the missing key was the cause.

No SSLv2-framed hello has ever completed a handshake with any browser
tested: IE 5.1.7 Mac abandons its own SSLv2-framed attempt at the same point
(`rx after our flight 0`) and then succeeds on a second tunnel carrying a
native SSL 3.0 hello (see *IE 5.1.7's double connect* below). The
SSLv2-framed reply path is therefore the leading suspect for what stops
IE 3 -- unproven, and nothing here isolates it further. One untried idea:
IE 3.0 may not accept a sha1WithRSA-signed leaf, since certificates it
shipped trusting in 1996 were MD5-signed; Gateway's leaf is always SHA-1
(CLAUDE.md, TLS Library rule).

Both builds are served today by `rewrite_https` (verified: FrogFind loads
over the 16-bit build's plain-proxy path). Closed with that outcome; reopen
only with a specific new idea to test, such as an MD5-signed leaf.

---

## Deferred, not rejected

**`connect_upgrade`**, built and reverted on `gw039` (0fbad79, follow-up
c63522f). Answers a plaintext `CONNECT host:80` by terminating the inner
request as HTTP and re-originating it over TLS to port 443, single-shot --
the opposite case from `connect_mitm`. Off by default, wired into both
Preferences windows, documented in `docs/prefs.md` and
`docs/prefs-example.txt`; log code `H27`.

Reverted 2026-09-29: hardware evidence found no client on hand that could
drive it end to end. PuTTY 0.53b (Raw, HTTP proxy pointed at Gateway) sent
`CONNECT 140.82.121.4:80`, and Gateway logged `H27` correctly, but the inner
request never arrived whole -- with local line editing on, the final blank
line was never sent (35 bytes buffered: `HEAD / HTTP/1.0\r\nHost:
github.com\r\n`, then the `H23` idle timeout); with it off, Gateway answered
`H05` (unparseable). Win95's `telnet.exe` cannot speak HTTP to Gateway at
all -- even a plain `GET` gets nothing back. Nothing verified the upgrade
end to end, so it is deferred rather than confirmed working.

A future test needs a client that sends `CONNECT host:80` and then a
complete HTTP request in one go -- `curl -p -x http://<gateway>:8765
http://example.com/` from a machine that can reach Gateway does exactly
that. Revive it by reverting the revert (the commits above, plus the `H27`
part of dfe5aef, which is otherwise general and stays).

**`http_upstream`** (not started), from roytam1's `gw034` branch (`5a2dbff`,
2026-09-30). Has `:8765` and `:8888` connect through an HTTP `CONNECT` or
SOCKS5 proxy before the origin, the way `tunnel_proxy` already does for
Module 4. Off by default. Worth having because Gateway's outbound traffic
leaves through the OS 9 machine's own route: a VPN or `ssh -D` on another
machine does not touch it, so this is the only way to steer it without
changing the router. Cases: a SOCKS5 exit abroad (`ssh -D 1080`) for
region-locked or slow-from-here sites and the Archive; Tor
(`socks5`, port 9050 -- the hostname goes to the proxy, so `.onion` resolves);
mitmproxy or Charles on another machine to watch what Gateway sends
upstream; a venue or office network that only lets a proxy out.

Not a cherry-pick: the branch is based on `fa64f91`, 175 commits behind, and
does not compile against main (`gw_fwd_socks_conn_reply` takes three
arguments now; `session_fail` and logging use the readable-log codes). Port
by hand:

1. Lift the handshake out of `src/proxy/gw_tunnel.c` (`build_handshake`,
   `step_http_hello`, `step_socks_hello`, `keep_leftover`) into one shared
   dialler both modules use, instead of the patch's second copy.
2. Hook it into the three dial sites in `src/proxy/gw_httpproxy.c`:
   `session_start_upstream`, `session_retry_fresh`, and the `CONNECT` dial
   in `step_recv_request`. `connect_mitm` rides on the first for free.
   Take the patch's `kHPProxyLink` state, and count it in
   `connecting_count` and the peer-gone check, as it does.
3. Plain `http://` origins through an `http` upstream: send the
   absolute-form request to the proxy, not `CONNECT host:80` -- Squid's
   default denies CONNECT to non-SSL ports, which would make every
   `http://` page a 502. CONNECT only for TLS origins and CONNECT tunnels.
4. New log codes; a non-200 (including 407) answered as a 502 with the code
   in the log, as the patch does. Say "read once at launch" only if it is --
   the patch reads the prefs on every dial.
5. Decide on a bypass (at least private addresses and `localhost`), and
   document that the update check and mail still connect directly.
6. Prefs docs from the patch's `docs/prefs.md` and `docs/prefs-example.txt`
   rows, corrected for the above.

Verify with `ssh -D 1080` on the MacBook and `http_upstream = socks5`, then
mitmproxy as an `http` upstream, on both listeners and through
`connect_mitm`.

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
first (never sent, public half only, and its signature had no length
prefix) and it was removed in 5e13663. Built again complete on 2026-09-29
(54f01fe, PATCHES.md §38: a real temporary key, host-tested wire framing,
and the matching ClientKeyExchange decrypt against it) once 32-bit IE 3.0's
hello finally showed the precondition -- an offer of nothing but 0003, then
`rx-after-flight 0`. Tested on the hardware that motivated it and made no
difference: IE 3 still hangs up after our first flight with no alert,
message present or not. Reverted the same day. See item 2 above for what
the hardware showed instead. Bring it back only alongside a specific reason
to think the missing key -- rather than the SSLv2-framed reply path or the
certificate's signature algorithm -- is what stops a client.

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
