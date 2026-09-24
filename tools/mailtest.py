#!/usr/bin/env python3
"""
Log into the provider's POP and IMAP servers with XOAUTH2 exactly as
Gateway's mail splice does, with Gateway out of the path.

When Outlook Express reports a login failure through Gateway, this settles
whose fault it is: if the provider refuses this script too, the problem is on
the account or the provider's side and nothing in Gateway will fix it.

    python3 mailtest.py you@hotmail.com            (Outlook, the default)
    python3 mailtest.py you@gmail.com gmail

Run it in a terminal of your own: it asks for the refresh_token line from
Gateway Prefs without echoing it. The token is used for one refresh and never
printed. Each session ends with QUIT / LOGOUT and nothing is read or changed.

Reading the result:
  POP  "+OK" after <token>  and  IMAP "a1 OK"   -> the account is fine.
  POP "-ERR ... bad password" with IMAP "NO User is authenticated but not
  connected" -> Microsoft accepts the token but will not open the mailbox;
  a server-side fault only Microsoft can clear.
"""

import base64
import getpass
import json
import socket
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

# The same clients and hosts as get-email-token.py and Gateway's defaults.
PROVIDERS = {
    "outlook": {
        "token": "https://login.microsoftonline.com/common/oauth2/v2.0/token",
        "client_id": "9e5f94bc-e8a4-4e73-b8be-63364c29d753",
        "client_secret": "",
        "pop": "outlook.office365.com",
        "imap": "outlook.office365.com",
    },
    "gmail": {
        "token": "https://oauth2.googleapis.com/token",
        "client_id": "406964657835-aq8lmia8j95dhl1a2bvharmfk3t1hgqj"
                     ".apps.googleusercontent.com",
        "client_secret": "kSmqreRr0qwBWJgbf5Y-PjSU",
        "pop": "pop.gmail.com",
        "imap": "imap.gmail.com",
    },
}


def refresh(p, rt):
    # Gateway sends no scope when oauth_scope is empty; do the same.
    form = {"client_id": p["client_id"], "grant_type": "refresh_token",
            "refresh_token": rt}
    if p["client_secret"]:
        form["client_secret"] = p["client_secret"]
    body = urllib.parse.urlencode(form).encode()
    try:
        with urllib.request.urlopen(p["token"], body, timeout=30) as r:
            j = json.load(r)
    except urllib.error.HTTPError as e:
        print("token refresh FAILED:", e.code,
              e.read().decode(errors="replace"))
        sys.exit(1)
    print("token refresh ok; scope granted:", j.get("scope"))
    return j["access_token"]


def xoauth2(user, tok):
    return base64.b64encode(
        ("user=%s\x01auth=Bearer %s\x01\x01" % (user, tok)).encode()).decode()


def connect(host, port):
    s = ssl.create_default_context().wrap_socket(
        socket.create_connection((host, port), timeout=30),
        server_hostname=host)
    return s, s.makefile("rb")


def recv(f, tag):
    line = f.readline().decode(errors="replace").rstrip("\r\n")
    print("  %s <- %s" % (tag, line[:200]))
    if line.startswith("+ ") and len(line) > 2:
        # A SASL failure challenge: base64 JSON saying why.
        try:
            print("       decoded:",
                  base64.b64decode(line[2:]).decode(errors="replace"))
        except ValueError:
            pass
    return line


def pop(host, user, tok):
    print("\nPOP  %s:995" % host)
    s, f = connect(host, 995)
    recv(f, "POP")
    # Two steps, as Gateway does: Exchange will not take the initial
    # response on the AUTH line.
    s.sendall(b"AUTH XOAUTH2\r\n")
    print("  POP -> AUTH XOAUTH2")
    if recv(f, "POP").startswith("+"):
        s.sendall(xoauth2(user, tok).encode() + b"\r\n")
        print("  POP -> <token>")
        r = recv(f, "POP")
        if r.startswith("+ "):              # failure challenge; answer empty
            s.sendall(b"\r\n")
            r = recv(f, "POP")
        if r.startswith("+OK"):
            s.sendall(b"STAT\r\n")
            recv(f, "POP")
    s.sendall(b"QUIT\r\n")
    recv(f, "POP")
    s.close()


def imap(host, user, tok):
    print("\nIMAP %s:993" % host)
    s, f = connect(host, 993)
    recv(f, "IMAP")
    s.sendall(b"a1 AUTHENTICATE XOAUTH2 " + xoauth2(user, tok).encode()
              + b"\r\n")
    print("  IMAP -> a1 AUTHENTICATE XOAUTH2 <token>")
    if recv(f, "IMAP").startswith("+"):
        s.sendall(b"\r\n")
        recv(f, "IMAP")
    s.sendall(b"a2 LOGOUT\r\n")
    recv(f, "IMAP")
    s.close()


def main():
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3
                                       and sys.argv[2] not in PROVIDERS):
        sys.exit(__doc__)
    user = sys.argv[1]
    p = PROVIDERS[sys.argv[2] if len(sys.argv) == 3 else "outlook"]
    tok = refresh(p, getpass.getpass("refresh_token: ").strip())
    pop(p["pop"], user, tok)
    imap(p["imap"], user, tok)


if __name__ == "__main__":
    main()
