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
