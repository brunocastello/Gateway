# Log codes

Every line in Gateway's log that describes something going wrong, or a
decision a person might ask about, is a plain sentence followed by a short
code:

```
#2 the browser gave up after seeing our certificate (S04)
```

The sentence says what happened. The code says exactly which branch of the
code wrote the line, so a screenshot or a pasted log is enough to find it,
without the engineering detail. That detail -- hello bytes, suite numbers,
byte counts, library error numbers -- appears indented under the line when
the `log_debug` preference is 1 (see [prefs.md](prefs.md)).

Codes are one letter for the module and two digits. A code is never reused:
when a line is removed its code is retired, and a new line takes the next
free number.

| Letter | Where |
|---|---|
| `G` | Gateway itself: launch, settings, listeners, the certificate authority |
| `H` | Module 1, the HTTP proxy: requests, upstream connections, redirects, `CONNECT` |
| `S` | TLS towards the browser (`connect_mitm`): the handshake Gateway answers |
| `T` | Reaching the far end, for every module: the name lookup, the connection, and the TLS handshake Gateway starts |
| `M` | Module 2, the mail splice and the token refresh |
| `W` | Module 3, the Wayback proxy |
| `N` | Module 4, the tunnel |

## G — Gateway itself

| Code | Sentence | What it means |
|---|---|---|
| `G01` | Gateway has no certificate authority to hand out | The browser asked for `gateway-ca.crt` and the authority could not be made or loaded. The lines from its creation say why. |

## H — the HTTP proxy

| Code | Sentence | What it means |
|---|---|---|
| `H01` | the auto-configuration script did not fit its buffer | Gateway's fault. Report it with the prefs file. |
| `H02` | the request's headers are too large to forward | The browser's request, rewritten for the origin, is over 16 KB. |
| `H03` | Gateway could not start a connection to … | No connection could be created at all, before the network was involved. Usually memory. |
| `H04` | the browser's request is larger than 16 KB | Its head, not its body. |
| `H05` | the browser's request could not be understood | It is not HTTP Gateway can parse. |
| `H06` | Gateway could not start a tunnel to … | As `H03`, for a `CONNECT`. |
| `H07` | … closed the connection before taking the request | The origin went away while Gateway was sending the request. A reused idle connection is retried once on a fresh one before this is written. |
| `H08` | … closed the connection during the upload | The origin went away while the browser's request body was going through. |
| `H09` | … stopped answering | The connection failed while waiting for the response, with no error to name. |
| `H10` | … sent response headers larger than 16 KB | |
| `H11` | … closed the connection without answering | The origin accepted the request and closed without a response. |
| `H12` | … sent a response Gateway could not understand | The response head is not HTTP. |
| `H13` | the page redirected … times, so Gateway stopped following | A redirect loop, or a very long chain. |
| `H14` | the response headers from … are too large once rewritten | |
| `H15` | … could not be reached | Connecting failed with no error to name. When there is one, the line carries a `T` code instead. |
| `H16` | … closed the tunnel before it opened | A `CONNECT` whose far end went away before the tunnel was up. |
| `H17` | … sent part of a file without saying which part | A `206` with no `Content-Range`: the origin's fault. |
| `H18` | the page is larger than max_body_mb …, so it was cut short | Raise `max_body_mb`, or set it to 0 for no limit. |
| `H19` | … sent a broken body; the page was cut short | The origin's chunked encoding is malformed. |
| `H20` | a CONNECT was dropped: Gateway had no room to answer it | Gateway's fault. Report it. |
| `H21` | Gateway answers the browser's secure connection to … itself | `connect_mitm` is on, so the browser now meets Gateway's certificate. If it goes no further, the next line has an `S` code. |
| `H22` | no certificate could be made for …, so the tunnel stays encrypted | `connect_mitm` could not mint a leaf, so the `CONNECT` is a plain tunnel and the browser does its own TLS. |
| `H23` | nothing moved for … seconds, so the connection was closed | The idle timeout. |
| `H24` | the browser stopped reading, so the connection was dropped | The browser left data unread for the whole grace period. |
| `H25` | out of memory: a browser's connection was refused | Raise Gateway's partition in Get Info. |

## S — TLS towards the browser

Written when `connect_mitm` answers a browser's handshake and the handshake
does not complete. The line before it names the host (`terminating TLS
for …`). With `log_debug` on, the lines under it give BearSSL's error
number, the hello's framing, version and suite, byte counts, and the first
bytes the browser sent.

| Code | Sentence | What it means |
|---|---|---|
| `S01` | the browser closed the secure connection without starting it | The tunnel opened and the browser sent nothing. Internet Explorer 3 on Windows 95 does this: its SSL fails before it writes a byte. |
| `S02` | the browser left before Gateway answered its hello | It sent a hello and went before the reply. Usually a hello Gateway could not parse, so check the first bytes under `log_debug`: `80 01` in bytes 4–5 is Microsoft PCT, which is not SSL. |
| `S03` | the browser gave up after seeing our certificate | The browser does not trust the certificate: install Gateway's authority from `http://<gateway-address>:8765/gateway-ca.crt`. IE 5 for Mac also does this once per host with an SSLv2-framed probe and then retries successfully. That case is harmless. |
| `S04` | the browser answered our certificate, then gave up | It sent its key exchange and left before finishing. |
| `S05` | the browser finished its side of the handshake, then closed | It completed its second flight and went. Rare. |
| `S10` | the browser asked for SSL 2.0, which Gateway does not speak | SSL 3.0 and TLS 1.0 are both off in the browser. In Internet Explorer: Internet Options > Advanced. |
| `S11` | the browser speaks SSL 3.0 at best, and allow_sslv3 is off | Set `allow_sslv3 1`, or turn TLS 1.0 on in the browser. |
| `S12` | the browser speaks … at best, older than Gateway will | A version below SSL 3.0 that is not SSL 2.0, or an unknown one. |
| `S13` | the browser refused the version Gateway answered with | The browser sent `protocol_version` in response to our hello. |
| `S14` | the browser and Gateway have no cipher in common | Usually an export (40-bit) browser. |
| `S15` | the browser's records did not match the version it asked for | The record-layer version disagreed with the hello's. |
| `S16` | Gateway had no randomness for the handshake | Gateway's fault: the entropy pool was empty. Report it. |
| `S17` | the browser rejected our certificate | The browser sent `bad_certificate`, `certificate_unknown` or `unknown_ca`. Install Gateway's authority. |
| `S18` | Gateway refused the browser's handshake | Gateway sent a fatal alert not covered above. The number is under `log_debug`. |
| `S19` | the browser refused the handshake | The browser sent a fatal alert not covered above. |
| `S20` | the handshake with the browser failed | Any other BearSSL error. The number is under `log_debug`. |
| `S21` | the browser's handshake stalled and was dropped | No progress before the idle timeout. |

A completed handshake is not an event to diagnose, so it has no code:
`#N secure connection with the browser, SSL 3.0`.

## T — reaching the far end

Shared by every module that connects out: the HTTP proxy, the mail splice,
the token refresh and the tunnel. With `log_debug` on, the line under it
gives the bracketed detail: phase, Open Transport or Winsock error, BearSSL
error, TLS version and the address the name resolved to.

| Code | Sentence | What it means |
|---|---|---|
| `T01` | Gateway ran out of memory connecting to … | Raise Gateway's partition in Get Info. |
| `T02` | the name … could not be looked up | DNS failed: check the TCP/IP control panel's name server. |
| `T03` | … did not accept a connection | Refused or timed out. The host is down, or the port is wrong. |
| `T04` | the network failed while talking to … | An Open Transport or Winsock error with no more specific meaning. The number is under `log_debug`. |
| `T10` | the certificate for … is not from an authority Gateway trusts | |
| `T11` | the certificate for … has expired | Or the Mac's clock is wrong. |
| `T12` | the certificate … sent is for another name | |
| `T13` | … sent its certificate chain out of order | Usually the server sending a redundant or misordered chain, not a bad certificate. |
| `T14` | the certificate for … has a bad signature | |
| `T15` | … sent no certificate | |
| `T16` | Gateway could not check the dates on the certificate for … | The clock is unset. |
| `T17` | the certificate chain for … has an intermediate that is not an authority | |
| `T18` | the certificate for … has a key too weak to trust | |
| `T20` | … speaks only TLS 1.2, which this connection cannot fall back to | A connection Gateway did not dial itself (STARTTLS, or the tunnel through a proxy) cannot be redialled for TLS 1.2. Enable TLS 1.3 on the far end. |
| `T21` | … refused the secure connection | The far end sent a fatal alert. The alert is under `log_debug`. |
| `T22` | the secure connection to … failed | Any other handshake failure. The number is under `log_debug`. |
| `T23` | the secure connection to … broke while reading | |
| `T24` | the secure connection to … broke while sending | |

## W — the Wayback proxy

| Code | Sentence | What it means |
|---|---|---|
| `W01` | the Wayback settings page did not fit its buffer | Gateway's fault. Report it. |
| `W02` | the archived address for this page is too long | |
| `W03` | the archive has no snapshot within … days of …; the nearest is … | Widen `wayback_tolerance`, or move `wayback_date` towards the date given. The browser gets a 404 page saying so. |
| `W04` | … is on wayback_live, so it comes from the live web | |
| `W05` | … is not on wayback_live, so it comes from the archive | |
| `W06` | GeoCities is served by its successor, … | |
| `W07` | … refused the connection; trying again | The archive refuses connections when busy; Gateway waits and retries a few times before giving up. |


