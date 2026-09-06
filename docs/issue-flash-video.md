# Open issue — Flash video stalls through Gateway, plays through the modern-Mac proxy

Recorded 2026-09-06. **Fixes for hypotheses 1, 3 and 4 have landed and the
logging has been rebuilt; hypothesis 2 is addressed by a change of default.**
Not yet confirmed on hardware — the section at the end says what to look for.

## Symptom

A 2009-style YouTube front end, self-hosted at `93.245.69.158:8080`, loads its
page and images through Gateway but the Flash player never gets a video — the
spinner turns forever. The same VM, pointed at the proxy on the modern Mac,
plays the video.

From Gateway's log while it was stuck:

```
TLS -  HTTP 404  93.245.69.158

#272 GET 93.245.69.158:8080
#271 <- 304 i.ytimg.com
#272 <- 200 93.245.69.158
#273 <- 304 93.245.69.158
#276 <- 302 93.245.69.158
#276 -> http://93.245.69.158 (redirect 1)
#276 <- 404 93.245.69.158
#278 <- 302 ... -> ... (redirect 1) ... <- 404
#279 <- 302 ... -> ... (redirect 1) ... <- 404
#280 <- 302 ... -> ... (redirect 1) ... <- 404
```

Four sessions each take a `302`, follow it once, and get a `404`. Ordinary page
assets are fine — the `200` and the several `304`s are the page and its images.

## What this rules out

**TLS is not involved.** The status line reads `TLS -` and the origin is
`:8080` over plain HTTP, so Certainly, the trust anchors, the cipher suites and
the whole TLS 1.3 layer are out of the picture. This is Gateway's HTTP handling
alone, which narrows the search considerably.

## Hypotheses, most likely first

### 1. Content-Length is stripped from every response

`kHopByHop[]` in `src/portable/gw_http.c` includes `Content-Length`, and
`gw_http_filter_response()` drops every header in that list. The client hop is
deliberately EOF-framed: HTTP/1.0 with `Connection: close` and no length.

That is correct for HTML and fine for images. For media it is likely fatal — a
Flash player wants the byte length to size its buffer, show a duration and
seek, and commonly refuses a stream that arrives without one. A conventional
proxy passes the length through, which is what the modern-Mac proxy will be
doing.

Worth checking first because it is deliberate, it is Gateway-specific, and it
explains "everything renders but the video will not start".

### 2. Redirects are followed internally, so the browser never sees the 302

`step_recv_head()` in `src/proxy/gw_httpproxy.c` follows up to `GW_MAX_REDIRECT`
(5) redirects itself and returns only the final response. The browser never
sees the intermediate `302`, which means:

- any `Set-Cookie` on the redirect is discarded, and
- the player never learns the final URL.

Token-authenticated media URLs frequently depend on both. The modern-Mac proxy
forwards the `302` and lets the browser follow it with its own cookie jar.

The `404` in the log is the *followed* request failing, so whatever the origin
wanted from the second request, Gateway is not supplying it.

### 3. The 2 MiB body cap

`GW_BODY_CAP` is 2 MiB and `step_body()` truncates past it. Even once the `404`
is solved, no video of any length will play. This is certain, not hypothetical
— it just has not been reached yet.

CLAUDE.md asks for the cap on "buffered GETs". Streaming media is a different
case and wants a different rule.

### 4. GW_MAX_PATH is 1024 bytes

`gw_url_split()` and `gw_url_resolve()` return failure when a URL will not fit,
and Gateway answers `502` with "unusable Location header". Signed media URLs
routinely exceed 1024 characters.

Not what the log shows here — a `404` came from the origin, so the request was
made — but adjacent, and it will bite as soon as hypothesis 2 is addressed.

### 5. Four concurrent splices

`GW_MAX_SESSIONS` is 4. A page of this shape opens many parallel connections
and Netscape will use several at once; the surplus is refused and logged as
"proxy busy, dropped a connection". Not obviously the cause of these `404`s,
but a likely contributor to stalling, and worth confirming from the log.

## The log cannot currently distinguish these

Before changing any behaviour, make the log able to answer the question. Today:

```c
gw_log("#%ld -> %s%s (redirect %d)", s->id, next.tls ? "https://" : "http://",
       next.host, s->redirects);
```

It prints scheme and host and **nothing else** — no port, no path, no query. So
`#276 -> http://93.245.69.158` could be any of a dozen different requests, and
whether the port survived the redirect is invisible. That is the first thing to
fix, and it would have made this note much shorter.

Worth adding, all cheap:

- the full redirect target: port and path, truncated to fit the window;
- the request path on `GET`, not just `host:port`;
- `Content-Type` and `Content-Length` from the origin response;
- a line when a request carries `Range:`, and what status came back;
- confirmation of whether "proxy busy" appears during a page load.

## What was changed

All of this is in the build following this note.

- **`Content-Length` is forwarded** whenever the body passes through untouched,
  and dropped only when de-chunking changes it. `gw_http_filter_response()`
  takes a `keep_length` argument; the caller passes `!res.chunked`.
- **Redirects default to `auto`**: Gateway follows a `3xx` only when the client
  could not have — plaintext to TLS — and passes everything else back, so the
  browser follows it with its own cookies and learns the final URL.
  `follow_redirects = auto|always|never` restores the old behaviour if needed.
- **The body ceiling is gone by default.** `max_body_mb = 0`. The 2 MiB cap
  protected nothing, since bodies stream through a 32 KB buffer and are never
  held.
- **`GW_MAX_PATH` is 4096**, so signed media URLs are no longer rejected as an
  unusable Location header. `GWResponse` moved off the stack accordingly.
- **The log now says what happened**: the request path, the response length or
  `chunked` or `no length`, the full redirect target including port and path,
  and a line when a redirect is passed back rather than followed.

One hypothesis was disproved on the way. A test now covers it: a relative
`Location` resolves against the origin including its **port**, so `:8080` was
never being lost on a redirect. The old log simply never printed it.

## Confirmed on hardware, and the sting in the tail

The video plays. But on the first run after the fix it stopped after fifteen
seconds, and the log said why:

```
#15 <- 302 93.245.69.158 54 bytes
#15 passing 302 to the client: ../assets/m7k9UMcHbr0.flv
#17 GET 93.245.69.158:8080/assets/m7k9UMcHbr0.flv
#17 <- 304 93.245.69.158 no length
```

A **304**. The video was never fetched at all — the browser was replaying a
copy from its own cache, and that copy had been written by the previous build.

The two old bugs had combined to poison it. The 2 MiB ceiling truncated the
file, and stripping `Content-Length` meant the browser had no way to know the
body was short, so it stored the fragment as if it were complete. Fifteen
seconds of 360p FLV is about 1.9 MB, which is the ceiling almost exactly.

**Clearing the browser cache is part of the fix.** Nothing in Gateway can undo
a bad cache entry: a conditional request is answered by the origin, and `304`
means the client already has what it asked for.

Worth noting that this cannot recur. With `Content-Length` now forwarded, a
truncated body no longer matches the announced length, and a browser will
discard it rather than cache it.

The same log also settled the concurrency question — `proxy busy, dropped a
connection` appears during the page load — so `max_sessions` now defaults to 8
rather than 4.

## If it still stalls

The log will now distinguish the remaining possibilities:

- `<- 200 host no length` on the media fetch means the origin itself sends no
  `Content-Length`, and the player is unlikely to start. That is an origin
  behaviour, not a Gateway one, and would need buffering to synthesise.
- `passing 302 to the client` followed by no further request means the browser
  declined to follow it — check whether the target was `https` and the browser
  cannot reach it.
- `proxy busy, dropped a connection` means `GW_MAX_SESSIONS` (4) is the
  bottleneck for a page opening many parallel fetches; raise it and re-measure.
- A `206` with a `Content-Range` and no `Content-Length` would indicate the
  player is using range requests and needs both forwarded.

## Fixes still to evaluate, if needed

- **Buffer to synthesise a length** if the origin sends none and the player
  refuses to start without one. Expensive, and a last resort.

## The quickest experiment

Point the browser at the site with redirect following disabled and
`Content-Length` passed through. If the video plays, hypotheses 1 and 2 are
confirmed together and can then be separated. Both changes are small and
reversible, and neither touches the TLS path.
