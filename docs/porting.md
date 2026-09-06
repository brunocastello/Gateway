# Porting Gateway

Gateway is a Mac OS 9 application, but very little of it is *about* Mac OS 9.
This document is an inventory of which parts are which, and what it would
actually take to run the same proxy on classic Windows or on Mac OS X.

Everything here is measured against the tree as it stands, not estimated from
memory. Line counts come from `wc -l`; the dependency claims come from grepping
for the symbols in question. Where something is a guess, it says so.

---

## 1. The shape of the code

| Area | Lines | Ports how |
|---|---:|---|
| `src/portable/` | 1,730 | **unchanged** — no platform headers at all |
| `third_party/certainly/bearssl/` | 294 files | **unchanged** — self-contained C89 |
| `third_party/certainly/src/tls13_*.c`, `certainly.c`, `ca_roots.c` | ~5,280 | **near unchanged** — two Toolbox calls to replace |
| `src/proxy/` (Modules 1 and 2, OAuth) | 2,107 | **recompiles** behind a small shim, see §2 |
| `src/gw_core.c` | 190 | recompiles; it is only glue |
| `src/gw_config.c` | 229 | ~60 lines of file I/O to replace |
| `third_party/certainly/src/entropy.c` | 254 | rewrite — every source is Toolbox-specific |
| `third_party/certainly/src/ot_transport.c` | 496 | rewrite — Open Transport |
| `src/net/gw_net.c` + `.h` | 1,015 | rewrite — Open Transport |
| `src/main.cpp` | 680 | rewrite, or drop entirely |

So of roughly 12,000 lines of C and C++ that Gateway is built from, about
**2,400 are platform-specific** and the rest is not. The TLS implementation —
the part that would be genuinely painful to redo — is in the "unchanged"
column.

That split is not luck. CLAUDE.md rule 8 required the protocol grammar to
compile on host `gcc` from the beginning, and `.github/workflows/host-tests.yml`
fails the build if anything under `src/portable/` so much as includes a
platform header. The 165 host tests that guard it run on every push.

### What the proxy layer actually depends on

`src/proxy/` is where the two modules live, and it is the part that would be
tedious to rewrite. It does not need to be. Grepping it for Open Transport
symbols — `OTOpen*`, `OTSnd`, `OTRcv`, `OTBind`, `OTConnect`, `EndpointRef` —
returns **nothing**. The modules speak only to `GWConn` and `GWStream`.

Its entire dependency on the Mac is:

| Symbol | Uses | Replacement |
|---|---:|---|
| `Ptr` | 16 | `typedef char *Ptr;` |
| `UInt16` | 6 | `typedef unsigned short UInt16;` |
| `Size` | 3 | `typedef long Size;` |
| `OSStatus` | 1 | `typedef long OSStatus;` |
| `NewPtr`, `NewPtrClear`, `DisposePtr` | 30 | `malloc`, `calloc`, `free` |

That is a thirty-line compatibility header. The 2,107 lines above it compile
as they are.

---

## 2. The one interface worth extracting first

Before any port, split `src/net/gw_net.h` into an interface and an Open
Transport implementation. The interface already exists in all but name — this
is a renaming exercise, not a redesign:

```c
/* gw_transport.h - what a platform must provide */

int           GWNet_Init(void);
void          GWNet_Shutdown(void);
unsigned long GWNet_Ticks(void);          /* any 60 Hz-ish monotonic tick */

GWConn      *GWConn_Connect(const char *host, unsigned short port);
GWConnState  GWConn_Pump(GWConn *c);
long         GWConn_Send(GWConn *c, const void *buf, size_t len);
long         GWConn_Recv(GWConn *c, void *buf, size_t len);
void         GWConn_Close(GWConn *c);
void         GWConn_Destroy(GWConn *c);

GWListener  *GWListener_Open(unsigned short port, int backlog);
GWConn      *GWListener_Poll(GWListener *l);   /* NULL when nothing is ready */
void         GWListener_Close(GWListener *l);
```

Three conventions matter more than the signatures, because the state machines
above depend on them:

- **Nothing blocks.** Every call returns immediately. `GWConn_Pump()` is called
  once per pass of the event loop and does whatever small amount of work is
  available.
- **`Send` returning 0 means "flow controlled, try next slice"** — not an
  error, and not a closed connection.
- **`Recv` distinguishes three cases**: `>0` bytes, `0` for nothing available
  right now, `-1` for a broken connection, and `-2` for an orderly close by the
  peer. Collapsing `0` and `-2` breaks every splice.

On BSD sockets this is `O_NONBLOCK` plus `select()`, and `-2` is `recv()`
returning 0. On Winsock it is `ioctlsocket(FIONBIO)` plus `select()`, with
`WSAEWOULDBLOCK` meaning "try again". Both are a closer fit than Open
Transport, which needs an interrupt-time notifier and a flag-passing dance to
achieve the same thing.

Certainly needs the same treatment for `ot_transport.c`. Its interface is eight
functions (`ot_transport_create`, `_adopt`, `_pump`, `_send`, `_recv`,
`_close`, `_destroy`, plus the state enum), and the cleanest approach is a
small vtable so one build of the TLS library can serve any host.

---

## 3. Classic Windows — NT 4.0, 95, 98, Me, 2000, XP

**Verdict: viable, and the smallest of the three ports if the UI is dropped.**

### Networking

Winsock 2 covers NT 4.0, 98, Me, 2000 and XP directly. Windows 95 needs the
OSR2 update or the Winsock 2 redistributable; failing that, Winsock 1.1 does
everything Gateway needs, since none of `WSAPoll`, overlapped I/O or
`getaddrinfo` is required — `select()`, `gethostbyname()` and non-blocking
sockets are enough.

The listener path is markedly simpler than the Mac's. Open Transport needs the
`tilisten` module, a `T_LISTEN` notification, an `OTListen`/`OTAccept` pair and
a second endpoint bound with `qlen = 0`; on Winsock it is `listen()` plus a
non-blocking `accept()` polled from the loop.

### Crypto

Nothing to do. BearSSL is self-contained and has no operating system
dependency, so there is no CryptoAPI or SChannel involvement, and none of the
per-Windows-version behaviour that using the system TLS stack would drag in.
This is the single biggest reason the port is tractable: TLS 1.3 on Windows NT
4.0 is otherwise a research project.

### Entropy

`entropy.c` must be rewritten — all five of its sources (`Microseconds`,
`TickCount`, `GetMouse`, `LMGetTicks`, `ReadLocation`) are Toolbox calls.
`CryptGenRandom` exists from Windows 95 OSR2 onward and is the right answer for
2000 and XP. For NT 4.0 and plain 95, mix `QueryPerformanceCounter`,
`GetTickCount`, `GetCursorPos`, process and thread IDs, and `GlobalMemoryStatus`
— the same shape as the existing Mac implementation, which is worth reading
first for how it stirs the pool.

### The application

Three options, in increasing order of effort:

1. **A console program.** `main()`, a `select()` loop, log to `stdout`. This
   would be perhaps 150 lines and is the honest choice for a proxy.
2. **A Windows service** via `StartServiceCtrlDispatcher`. Right for something
   that starts with the machine, and only a little more than the console
   version. Not available on 95/98/Me, which have no service manager — there,
   a hidden window plus a `RunServices` registry entry is the equivalent.
3. **A Win32 window** replicating the log view. `CreateWindow`, a `WM_PAINT`
   handler drawing the same ring buffer, and a tray icon via
   `Shell_NotifyIcon`. Perhaps 400 lines.

### Toolchain

OpenWatcom or MinGW both produce binaries that run from NT 4.0 upward. MSVC 6
is the period-correct option and is what a 1998 target would have used.
CMake supports all three, so `CMakeLists.txt` needs a platform branch rather
than a rewrite.

### Watch out for

- **`SO_REUSEADDR` behaves differently on Windows** — it permits two sockets to
  bind the same port rather than merely allowing a quick rebind. Do not set it
  on the listeners.
- **`closesocket`, not `close`**, and `WSAGetLastError()`, not `errno`.
- **`WSAStartup`/`WSACleanup`** bracket everything.
- **Sockets are not file descriptors** on Windows, so nothing may assume they
  can be handed to `read`/`write`.

---

## 4. Mac OS X — 10.0 through 10.5, PowerPC

**Verdict: the easiest of the three, and there is precedent in the same
author's iWordle, which targets 10.0–10.5 with the same toolchain.**

### Networking

Use BSD sockets, not Open Transport. OT is present through 10.4 and would let
`gw_net.c` survive nearly unchanged, but it is deprecated, absent from 10.5,
and its notifier model is the most awkward part of the current code. Sockets
are a better fit for a pump loop and the rewrite is smaller than the
compatibility work would be.

`select()` is fine at Gateway's concurrency — four HTTP splices and four mail
sessions never approach `FD_SETSIZE`. `kqueue` exists from 10.3 but buys
nothing here.

### Crypto

BearSSL compiles unchanged. Do **not** switch to Secure Transport: it would
mean rewriting the whole TLS layer for an API that is itself now deprecated,
and it would forfeit the TLS 1.3 support that is the entire point of the
project — Secure Transport on 10.5 tops out at TLS 1.0.

### Entropy

Trivial: `/dev/urandom`. Delete `entropy.c` and read 32 bytes.

### Trust anchors

This is the one place where the OS X port should *not* copy Mac OS 9. The 29
compiled-in roots in `ca_roots.c` exist because Mac OS 9 has no usable system
trust store; on OS X there is one, and a compiled-in list will rot as roots
expire. Read the system keychain instead, or at minimum regenerate from it at
build time with the existing `tools/generate_ca_roots.py`, which already takes
a PEM bundle.

### The application

Carbon with the same Retro68 `retrocarbon` toolchain iWordle uses, or gcc 4.0
under Xcode 2/3 for a native build. `src/main.cpp` mostly survives — Carbon
keeps `WaitNextEvent`, menus and windows — though the accessor functions
(`GetPortBounds`, `SetPortWindowPort`) replace direct struct access, which is
exactly the difference visible between this project and iWordle's `main.c`.

The alternative, and probably the better one: drop the UI and ship a `launchd`
daemon (10.4+) or a `StartupItem` (10.0–10.3), logging through `syslog`. A
proxy has no business owning a window.

### Watch out for

- **Endianness stays big** through 10.5 on PowerPC, so nothing in the record
  layer changes. An Intel build would exercise byte-order paths that have never
  run; BearSSL handles this correctly, but Gateway's own header parsing should
  be re-read for it.
- **`SIGPIPE`** does not exist on Mac OS 9 and will kill the process on the
  first write to a closed socket. Set `SO_NOSIGPIPE`, or ignore the signal.
- **Case-sensitive volumes** are possible on OS X; the vendored Universal
  Interfaces contain `Memory.h` and `Strings.h`, which collide with newlib's
  on a case-insensitive one. Not a problem for a native OS X build, which needs
  neither.

---

## 5. What not to port

- **The `SIZE` resource machinery in `src/main.cpp`.** It exists because the
  Process Manager decides at launch whether an application appears in the
  Application menu. Nothing outside Mac OS 9 works this way.
- **The bundle-bit code.** Same reason.
- **The Universal Interfaces isolation in `CMakeLists.txt`.** The whole reason
  for `GW_UNIVERSAL_DIR` being a PRIVATE include directory is that Retro68
  ships Multiversal, which has no Open Transport. No other platform has this
  problem.
- **`gw_prefs.c`'s CR line-ending handling** should stay, but the reason
  changes: on Mac OS 9 it exists because files are typed on the Mac. Elsewhere
  it is only there to read a prefs file brought across from one.

---

## 6. Module 3 comes along for free

Module 3 (`docs/module3-wayback.md`) is designed to live entirely in
`src/portable/` plus a listener and two hooks. A Windows or Mac OS X build
therefore inherits the Wayback proxy without any additional platform work —
which matters, since the intent is for the classic Windows build to carry it
too. Build it once, on whichever platform is convenient, and the host test
suite proves it everywhere.

## 7. Order of work

1. Extract `gw_transport.h` from `gw_net.h` and make the Mac build use it. No
   behaviour change, and it can be verified on hardware before anything moves.
2. Give Certainly a transport vtable so `ot_transport.c` becomes one
   implementation rather than the only one.
3. Write the platform shim header — the thirty lines of §1 — and a
   `gw_platform.c` for file I/O and entropy.
4. Implement `gw_transport` on the target with non-blocking sockets.
5. Bring up Module 1 first. It is self-contained, and a browser pointed at it
   proves the transport, the TLS layer and both state machines at once.
6. Module 2 after, since it depends on the token refresher, which depends on
   Module 1's TLS path already working.

The host test suite runs on every platform from day one, so the protocol layer
never needs debugging on the target.
