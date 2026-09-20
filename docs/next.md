# After 0.3.6 — what is worth doing next

0.3.6 shipped on 2026-09-20. The upstream side is TLS 1.3 and finished; the
browser side now serves everything from Netscape 3 to Classilla over SSL 3.0
or TLS 1.0; mail works through `get-email-token.py`. What remains is making
the machine Gateway runs on feel less punished for it. This document records
the candidates in order of value, with the evidence each rests on, so a later
session can pick one up without re-deriving it — and records what was tried
and is not worth trying again.

---

## 1. TLS session resumption

**The evidence.** IE 5.1.7 for Mac OS 9 loading howsmyssl.com with
`connect_mitm = 1` (build fc4bb6f, 2026-09-20):

```
#3 MITM client hello: 5 bytes, first: 16 03 00 00 35
#9 MITM client hello: 5 bytes, first: 16 03 00 00 55
```

Both are native SSL 3.0 hellos to the same host, seconds apart. The second is
32 bytes longer: it carries the session ID from the first. The browser is
asking to resume, and Gateway performs a full handshake anyway, because no
session cache is set on the server context — `third_party/certainly/src/server.c`
calls `br_ssl_server_init_full_rsa` and nothing else.

**Why it matters.** Every resource on a page is a fresh CONNECT (the client
hop is always `Connection: close`), and every fresh CONNECT is a 1024-bit RSA
private-key operation on a 1997 PowerPC. That operation is most of what
"slow" means with `connect_mitm` on. An abbreviated handshake skips it.

**The change.** BearSSL's `br_ssl_session_cache_lru` — a few kilobytes,
`br_ssl_session_cache_lru_init` once at startup and `br_ssl_server_set_cache`
per server context. Nothing in the proxy needs to know.

**What has to be verified.** The SSL 3.0 bridges of PATCHES.md §28 fire at
most once per connection under conditions written for the full handshake. An
abbreviated handshake has a different flight order — the server's
ChangeCipherSpec and Finished go first, there is no ClientKeyExchange, and the
36-byte Finished must be produced and checked in both directions. Test on the
clients that negotiate SSL 3.0 (IE 5.1.7 Mac, Netscape 4.75, 16-bit IE 5) as
well as a TLS 1.0 one, and confirm in the log that the second hello to a host
ends in `handshake done` with a smaller `rx` than the first. The abandon
readout is already there to say where it stops if it does.

## 2. Keep-alive inside the terminated tunnel

The larger version of the same win: one handshake per host rather than per
resource. It collides with the design rule that the client hop is always
`Connection: close` (CLAUDE.md, Module 1), which exists for the cooperative
loop's sake — a kept-alive TLS connection is a splice slot held for as long
as the browser likes. Resumption gets most of the benefit without touching
that rule, so do it first and measure before deciding whether this is still
worth its cost.

## 3. IE 3.0 may need a checkbox, not code — but first, its hello bytes

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

## 4. Documentation drift

`CLAUDE.md` still describes Module 3 as "designed, not built" and
`docs/module3-wayback.md` opens with "Nothing here is implemented", while the
log shows `listening on port 8888` and `wayback: serving 20011231 +730 days`.
The wayback proxy is built and running. Both files steer future sessions and
should say so.

---

## Tried and not worth repeating

**IE 3.02 on an image that has IE 4.** The 3.02 setup refuses to run over
IE 4, and no `ieremove.exe` could be found to downgrade. Two ways round it
were tried on 2026-09-20 and both produced a CONNECT, `terminating TLS`, and
then silence — the client never sent a hello, so neither is a data point:

- *The 16-bit Windows 3.1 build of 3.0.* It carries its own secure-channel
  code rather than `schannel.dll`, and that code fails on Windows 95 after
  the tunnel opens. The browser reports the load as failed without writing
  a byte, with PCT on or off.
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
