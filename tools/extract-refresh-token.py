#!/usr/bin/env python3
"""
Print the plaintext refresh token stored in an email-oauth2-proxy config.

email-oauth2-proxy does not store tokens in the clear. It derives a key from
the account password with PBKDF2-HMAC-SHA256 (using the `token_salt` and
`token_iterations` in the config) and encrypts each token with Fernet, which is
why `refresh_token` in that file starts with "gAAAAA". Copying that value
straight into Gateway Prefs cannot work: Gateway would post ciphertext to the
token endpoint and the provider would reject it.

Run this on the modern Mac, where the config lives, and paste what it prints
into `refresh_token` in Gateway Prefs.

    python3 tools/extract-refresh-token.py ~/path/to/emailproxy.config

Requires the same library the proxy itself uses:  pip install cryptography

The token is a bearer credential for the mailbox. It is printed to stdout and
nowhere else; do not paste it into a chat, a bug report, or a commit.
"""

import argparse
import base64
import configparser
import getpass
import sys

try:
    from cryptography.fernet import Fernet, InvalidToken
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives import hashes
    from cryptography.hazmat.primitives.kdf.pbkdf2 import PBKDF2HMAC
except ImportError:
    sys.exit("This needs the 'cryptography' package: pip install cryptography")


def accounts(config):
    """Config sections that look like an account (i.e. have a refresh token)."""
    return [s for s in config.sections() if config.has_option(s, "refresh_token")]


def main():
    parser = argparse.ArgumentParser(
        description="Decrypt the refresh token in an email-oauth2-proxy config."
    )
    parser.add_argument("config", help="path to emailproxy.config")
    parser.add_argument("-a", "--account",
                        help="account section to read (default: the only one)")
    args = parser.parse_args()

    config = configparser.ConfigParser()
    if not config.read(args.config):
        sys.exit("Could not read %s" % args.config)

    found = accounts(config)
    if not found:
        sys.exit("No account section in that file has a refresh_token.")

    if args.account:
        if args.account not in found:
            sys.exit("No such account. Found: %s" % ", ".join(found))
        account = args.account
    elif len(found) == 1:
        account = found[0]
    else:
        sys.exit("Several accounts present; pick one with --account: %s"
                 % ", ".join(found))

    stored = config.get(account, "refresh_token")
    if not stored.startswith("gAAAAA"):
        # Not Fernet, so the proxy was configured without encryption.
        print(stored)
        return

    salt = config.get(account, "token_salt", fallback=None)
    if not salt:
        sys.exit("The token is encrypted but the config has no token_salt.")
    iterations = config.getint(account, "token_iterations", fallback=1200000)

    password = getpass.getpass(
        "Password for %s (the one you give the mail client): " % account)

    # Exactly what emailproxy.py does when it builds its Fernet key.
    key = base64.urlsafe_b64encode(
        PBKDF2HMAC(algorithm=hashes.SHA256(), length=32,
                   salt=base64.b64decode(salt.encode("utf-8")),
                   iterations=iterations,
                   backend=default_backend()).derive(password.encode("utf-8")))

    try:
        token = Fernet(key).decrypt(stored.encode("utf-8")).decode("utf-8")
    except InvalidToken:
        sys.exit("Wrong password, or this config was written with a different "
                 "token_salt / token_iterations.")

    print()
    print("Paste this into Gateway Prefs as refresh_token:")
    print()
    print(token)
    print()
    print("Also copy these across from the same account section:")
    print("  oauth_user      = %s" % account)
    print("  oauth_client_id = %s"
          % config.get(account, "client_id", fallback="(missing)"))
    scope = config.get(account, "oauth2_scope", fallback=None)
    if scope:
        print("  oauth_scope     = %s" % scope)


if __name__ == "__main__":
    main()
