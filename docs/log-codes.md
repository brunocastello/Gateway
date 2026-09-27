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
| `T` | TLS towards the origin: the handshake Gateway starts |
| `M` | Module 2, the mail splice and the token refresh |
| `W` | Module 3, the Wayback proxy |
| `N` | Module 4, the tunnel |

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
