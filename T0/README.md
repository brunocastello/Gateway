# T0

BearSSL's handshake logic is not C. `ssl_hs_client.c` and `ssl_hs_server.c`
in `third_party/certainly/bearssl/src/ssl/` are generated from the `.t0`
sources beside them by T0Comp, a small Forth-like compiler that BearSSL ships
as C# and that needs .NET to run. `t0comp.py` is a Python 3 port of it,
standard library only, contributed by [roytam1](https://github.com/roytam1).
`kern.t0` is BearSSL's kernel, unchanged; the compiler reads it from its own
directory.

It reproduces the C# compiler's output byte for byte for Gateway's sources
(checked 2026-09-19 against the files the C# compiler had generated), so the
`.t0` files are the source of truth and the `.c` files are regenerated, never
hand-edited.

## Regenerate

From `third_party/certainly/bearssl/src/ssl/`:

    python3 ../../../../../T0/t0comp.py -o ssl_hs_server -r br_ssl_hs_server -m main ssl_hs_common.t0 ssl_hs_server.t0
    python3 ../../../../../T0/t0comp.py -o ssl_hs_client -r br_ssl_hs_client -m main ssl_hs_common.t0 ssl_hs_client.t0

The compiler writes a bare `/* Automatically generated code */` line; the
checked-in files carry a longer header saying the same and where Gateway's
changes live. Keep that header when regenerating.

Options match `T0Comp.cs`: `-o` output base, `-r` run-function base, `-m`
entry point(s), `-nf` to disable flow analysis.
