#!/usr/bin/env python3
"""
Get the refresh token Gateway's mail splice needs, for Gmail or Outlook.

Run this on a modern computer -- macOS, Windows or Linux, any Python 3, no
packages to install. It opens the provider's sign-in page in your browser,
catches the redirect on 127.0.0.1, exchanges the code for tokens, and prints
the lines to put in Gateway Prefs on the old machine.

    python3 get-email-token.py             (asks which provider)
    python3 get-email-token.py outlook
    python3 get-email-token.py gmail --user you@gmail.com

On Windows, "py get-email-token.py" if "python3" is not on the path.

The OAuth clients are Thunderbird's public desktop clients, which both
providers have registered with a loopback redirect -- the same thing
Thunderbird itself does. Their identifiers are not secrets; the sign-in is
what protects the account. Gateway needs the client that issued the token, so
the printout includes it.

The refresh token is a bearer credential for the mailbox. It is printed to
this terminal and nowhere else; do not paste it into a chat, a bug report or
a commit.
"""

import argparse
import base64
import hashlib
import http.server
import json
import secrets
import socket
import socketserver
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
import webbrowser

PROVIDERS = {
    "outlook": {
        "name": "Outlook.com, Hotmail, Microsoft 365",
        "authorize": "https://login.microsoftonline.com/common/oauth2/v2.0/authorize",
        "token": "https://login.microsoftonline.com/common/oauth2/v2.0/token",
        "client_id": "9e5f94bc-e8a4-4e73-b8be-63364c29d753",
        "client_secret": "",
        "scope": ("offline_access "
                  "https://outlook.office.com/IMAP.AccessAsUser.All "
                  "https://outlook.office.com/POP.AccessAsUser.All "
                  "https://outlook.office.com/SMTP.Send"),
        "extra": {},
    },
    "gmail": {
        "name": "Gmail",
        "authorize": "https://accounts.google.com/o/oauth2/v2/auth",
        "token": "https://oauth2.googleapis.com/token",
        "client_id": "406964657835-aq8lmia8j95dhl1a2bvharmfk3t1hgqj"
                     ".apps.googleusercontent.com",
        # Google demands this at the token endpoint even for a desktop
        # client, and treats it as public (RFC 8252 section 8.5).
        "client_secret": "kSmqreRr0qwBWJgbf5Y-PjSU",
        "scope": "https://mail.google.com/",
        # Without these Google returns no refresh token on any sign-in
        # after the first.
        "extra": {"access_type": "offline", "prompt": "consent"},
    },
}

WAIT_SECONDS = 600


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


class Callback(http.server.BaseHTTPRequestHandler):
    """Catches the one redirect the provider sends back."""

    result = None
    event = threading.Event()

    def do_GET(self):
        query = urllib.parse.urlparse(self.path).query
        params = dict(urllib.parse.parse_qsl(query))
        if "code" in params or "error" in params:
            Callback.result = params
            body = ("<html><body style='font-family:sans-serif'>"
                    "<h2>Gateway</h2><p>%s You can close this window and go "
                    "back to the terminal.</p></body></html>"
                    % ("Signed in." if "code" in params
                       else "The sign-in did not complete."))
            Callback.event.set()
        else:
            body = "<html><body>Not the page you are looking for.</body></html>"
        data = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass                                # keep the terminal for our own output


class LoopbackServer(http.server.HTTPServer):
    """
    HTTPServer without the reverse lookup. Its server_bind calls
    socket.getfqdn() on the bound address, and on macOS 15 that lookup is
    what raises the "find devices on your local network" permission dialog
    -- for a server that will only ever be spoken to over loopback.
    """

    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = "127.0.0.1"
        self.server_port = self.server_address[1]


def ask_provider():
    print("Which provider?")
    print("  1. Outlook.com, Hotmail, Microsoft 365")
    print("  2. Gmail")
    while True:
        answer = input("1 or 2: ").strip().lower()
        if answer in ("1", "outlook", "o"):
            return "outlook"
        if answer in ("2", "gmail", "g", "google"):
            return "gmail"


def main():
    parser = argparse.ArgumentParser(
        description="Obtain a Gateway refresh token for Gmail or Outlook.")
    parser.add_argument("provider", nargs="?", choices=sorted(PROVIDERS),
                        help="gmail or outlook (asked for if omitted)")
    parser.add_argument("-u", "--user",
                        help="the account address, to pre-fill the sign-in "
                             "and the oauth_user line")
    parser.add_argument("--no-browser", action="store_true",
                        help="print the sign-in URL instead of opening it")
    args = parser.parse_args()

    provider = args.provider or ask_provider()
    p = PROVIDERS[provider]

    # Bind first, so the redirect URI can carry the port the OS handed out.
    # Loopback only: nothing on the network can reach this, and neither
    # operating system's firewall has anything to say about it.
    server = LoopbackServer(("127.0.0.1", 0), Callback)
    port = server.server_address[1]
    redirect_uri = "http://127.0.0.1:%d/" % port

    verifier = b64url(secrets.token_bytes(32))
    challenge = b64url(hashlib.sha256(verifier.encode("ascii")).digest())
    state = b64url(secrets.token_bytes(12))

    params = {
        "client_id": p["client_id"],
        "redirect_uri": redirect_uri,
        "response_type": "code",
        "scope": p["scope"],
        "state": state,
        "code_challenge": challenge,
        "code_challenge_method": "S256",
    }
    params.update(p["extra"])
    if args.user:
        params["login_hint"] = args.user
    url = p["authorize"] + "?" + urllib.parse.urlencode(params)

    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()

    print()
    print("Sign in to %s in the browser and allow access to your mail."
          % p["name"])
    if args.no_browser or not webbrowser.open(url):
        print("Open this address in a browser on this computer:")
        print()
        print(url)
    print()
    print("Waiting for the sign-in to finish (up to %d minutes)..."
          % (WAIT_SECONDS // 60))

    if not Callback.event.wait(WAIT_SECONDS):
        server.shutdown()
        sys.exit("Timed out. Run it again when you are ready to sign in.")
    server.shutdown()
    result = Callback.result

    if "error" in result:
        sys.exit("The provider said %s: %s"
                 % (result["error"], result.get("error_description", "")))
    if result.get("state") != state:
        sys.exit("The reply did not match this request (state differs). "
                 "Run it again.")

    body = {
        "client_id": p["client_id"],
        "code": result["code"],
        "redirect_uri": redirect_uri,
        "grant_type": "authorization_code",
        "code_verifier": verifier,
    }
    if p["client_secret"]:
        body["client_secret"] = p["client_secret"]

    request = urllib.request.Request(
        p["token"], data=urllib.parse.urlencode(body).encode("ascii"),
        headers={"Content-Type": "application/x-www-form-urlencoded",
                 "Accept": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            tokens = json.load(response)
    except urllib.error.HTTPError as e:
        try:
            detail = json.load(e)
            sys.exit("The token endpoint said %d %s: %s"
                     % (e.code, detail.get("error", ""),
                        detail.get("error_description", "")))
        except ValueError:
            sys.exit("The token endpoint said %d" % e.code)
    except (urllib.error.URLError, socket.timeout) as e:
        sys.exit("Could not reach the token endpoint: %s" % e)

    refresh = tokens.get("refresh_token")
    if not refresh:
        sys.exit("The provider returned no refresh token. For Gmail this "
                 "means consent was not asked for again; run it again and "
                 "make sure the permission screen appears.")

    print()
    print("Done. Put these lines in Gateway Prefs on the old machine --")
    print("System Folder:Preferences:Gateway Prefs on Mac OS 9, Gateway.ini")
    print("beside Gateway.exe on Windows -- or into the Mail pane of Gateway's")
    print("Settings window. Lines already there with these names are replaced.")
    print()
    print("provider            = %s" % provider)
    if args.user:
        print("oauth_user          = %s" % args.user)
    else:
        print("oauth_user          = (the address you signed in with)")
    print("oauth_client_id     = %s" % p["client_id"])
    if p["client_secret"]:
        print("oauth_client_secret = %s" % p["client_secret"])
    print("refresh_token       = %s" % refresh)
    print()
    print("The refresh token is the key to the mailbox. Do not paste it")
    print("anywhere but Gateway.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print()
        sys.exit(1)
