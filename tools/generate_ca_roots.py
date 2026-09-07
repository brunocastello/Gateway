#!/usr/bin/env python3
"""
Regenerate third_party/certainly/src/ca_roots.c from a PEM bundle.

Certainly compiles its trust anchors in: a Mac OS 9 machine has no usable
system trust store and no way to add a root by hand, so whatever ships here is
the whole of what Gateway will trust. The upstream set was ten anchors, which
left common sites failing verification with BR_ERR_X509_NOT_TRUSTED (62) --
lite.cnn.com chains to GlobalSign ECC Root CA - R5, for one.

This produces the same C that BearSSL's `brssl ta` emits, so the output stays
drop-in compatible with ca_roots.h.

    python3 tools/generate_ca_roots.py \
        --bundle /etc/ssl/cert.pem \
        --out third_party/certainly/src/ca_roots.c

Roots a particular system bundle happens to omit go in tools/extra-roots/ as
PEM files; they are picked up automatically. ISRG Root X2 lives there because
macOS ships it in the keychain but not in /etc/ssl/cert.pem.

Requires: pip install cryptography
"""

import argparse
import datetime
import glob
import os
import re
import sys

from cryptography import x509
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

# BearSSL curve identifiers, from bearssl_ec.h.
BR_EC = {"secp256r1": 23, "secp384r1": 24, "secp521r1": 25}

# The roots to compile in, matched on the RFC 4514 subject.
#
# Chosen to cover what a vintage browser and mail client actually meet: the
# CAs behind the large CDNs, the free-certificate issuers, and the roots the
# Microsoft and Google endpoints chain to. Adding a root costs a few hundred
# bytes of data segment, so the list errs on the side of working sites.
WANTED = [
    # Let's Encrypt
    "CN=ISRG Root X1,O=Internet Security Research Group,C=US",
    "CN=ISRG Root X2,O=Internet Security Research Group,C=US",
    # Google Trust Services
    "CN=GTS Root R1,O=Google Trust Services LLC,C=US",
    "CN=GTS Root R2,O=Google Trust Services LLC,C=US",
    "CN=GTS Root R3,O=Google Trust Services LLC,C=US",
    "CN=GTS Root R4,O=Google Trust Services LLC,C=US",
    # DigiCert - Microsoft's endpoints chain here
    "CN=DigiCert Global Root CA,OU=www.digicert.com,O=DigiCert Inc,C=US",
    "CN=DigiCert Global Root G2,OU=www.digicert.com,O=DigiCert Inc,C=US",
    "CN=DigiCert Global Root G3,OU=www.digicert.com,O=DigiCert Inc,C=US",
    "CN=DigiCert High Assurance EV Root CA,OU=www.digicert.com,O=DigiCert Inc,C=US",
    "CN=DigiCert Trusted Root G4,OU=www.digicert.com,O=DigiCert Inc,C=US",
    # GlobalSign - CNN and much of Fastly chain here
    "CN=GlobalSign Root CA,OU=Root CA,O=GlobalSign nv-sa,C=BE",
    "CN=GlobalSign,O=GlobalSign,OU=GlobalSign Root CA - R3",
    "CN=GlobalSign,O=GlobalSign,OU=GlobalSign Root CA - R6",
    "CN=GlobalSign,O=GlobalSign,OU=GlobalSign ECC Root CA - R4",
    "CN=GlobalSign,O=GlobalSign,OU=GlobalSign ECC Root CA - R5",
    # Amazon / CloudFront
    "CN=Amazon Root CA 1,O=Amazon,C=US",
    "CN=Amazon Root CA 2,O=Amazon,C=US",
    "CN=Amazon Root CA 3,O=Amazon,C=US",
    "CN=Amazon Root CA 4,O=Amazon,C=US",
    "CN=Starfield Services Root Certificate Authority - G2,O=Starfield Technologies\\, Inc.,L=Scottsdale,ST=Arizona,C=US",
    # Sectigo / Comodo
    "CN=USERTrust RSA Certification Authority,O=The USERTRUST Network,L=Jersey City,ST=New Jersey,C=US",
    "CN=USERTrust ECC Certification Authority,O=The USERTRUST Network,L=Jersey City,ST=New Jersey,C=US",
    # Microsoft's own roots
    "CN=Microsoft RSA Root Certificate Authority 2017,O=Microsoft Corporation,C=US",
    "CN=Microsoft ECC Root Certificate Authority 2017,O=Microsoft Corporation,C=US",
    # Azure and a long tail of older deployments
    "CN=Baltimore CyberTrust Root,OU=CyberTrust,O=Baltimore,C=IE",
    "CN=Go Daddy Root Certificate Authority - G2,O=GoDaddy.com\\, Inc.,L=Scottsdale,ST=Arizona,C=US",
    "CN=IdenTrust Commercial Root CA 1,O=IdenTrust,C=US",
    "CN=Entrust Root Certification Authority - G2,OU=(c) 2009 Entrust\\, Inc. - for authorized use only,OU=See www.entrust.net/legal-terms,O=Entrust\\, Inc.,C=US",
]


def hexblock(data, indent="\t"):
    """Format bytes as C array initialiser rows, 12 per line."""
    out = []
    for i in range(0, len(data), 12):
        row = ", ".join("0x%02X" % b for b in data[i:i + 12])
        out.append(indent + row)
    return ",\n".join(out)


def load_bundle(path):
    pem = open(path, "rb").read()
    blocks = re.findall(
        rb"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", pem, re.S)
    certs = {}
    for b in blocks:
        try:
            c = x509.load_pem_x509_certificate(b)
            certs[c.subject.rfc4514_string()] = c
        except Exception:
            continue
    return certs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundle", default="/etc/ssl/cert.pem")
    ap.add_argument("--extra", nargs="*", default=None,
                    help="additional PEM files, for roots a given system "
                         "bundle happens to omit (default: tools/extra-roots/*)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--all", action="store_true",
                    help="compile in every root in the bundle, "
                         "not just the curated list")
    args = ap.parse_args()

    certs = load_bundle(args.bundle)

    extra = args.extra
    if extra is None:
        here = os.path.dirname(os.path.abspath(__file__))
        pattern = os.path.join(here, "extra-roots", "*.pem")
        extra = sorted(glob.glob(pattern))
    for path in extra:
        certs.update(load_bundle(path))

    if args.all:
        # Everything the bundle holds, in a stable order.
        #
        # The curated list below was an attempt to guess which authorities the
        # vintage web would need, and it kept being wrong: lite.cnn.com wanted
        # GlobalSign, code.jquery.com wants Sectigo, and each miss costs an
        # evening and a rebuild that only the author can perform. A trust
        # anchor is a distinguished name and a public key rather than a whole
        # certificate, so the entire set costs tens of kilobytes -- affordable
        # even in an 8 MB partition, and far cheaper than being wrong again.
        chosen = sorted(certs.items())
    else:
        chosen = []
        for name in WANTED:
            c = certs.get(name)
            if c is None:
                print("WARNING: not in bundle, skipping: %s" % name,
                      file=sys.stderr)
                continue
            chosen.append((name, c))

    if not chosen:
        sys.exit("No trust anchors matched; is --bundle correct?")

    body = []
    table = []

    for i, (name, cert) in enumerate(chosen):
        dn = cert.subject.public_bytes()
        body.append("static const unsigned char TA%d_DN[] = {\n%s\n};\n"
                    % (i, hexblock(dn)))

        pub = cert.public_key()
        if isinstance(pub, rsa.RSAPublicKey):
            nums = pub.public_numbers()
            n = nums.n.to_bytes((nums.n.bit_length() + 7) // 8, "big")
            e = nums.e.to_bytes((nums.e.bit_length() + 7) // 8, "big")
            body.append("static const unsigned char TA%d_RSA_N[] = {\n%s\n};\n"
                        % (i, hexblock(n)))
            body.append("static const unsigned char TA%d_RSA_E[] = {\n%s\n};\n"
                        % (i, hexblock(e)))
            table.append(
                "\t{\n"
                "\t\t{ (unsigned char *)TA%d_DN, sizeof TA%d_DN },\n"
                "\t\tBR_X509_TA_CA,\n"
                "\t\t{\n"
                "\t\t\tBR_KEYTYPE_RSA,\n"
                "\t\t\t{ .rsa = {\n"
                "\t\t\t\t(unsigned char *)TA%d_RSA_N, sizeof TA%d_RSA_N,\n"
                "\t\t\t\t(unsigned char *)TA%d_RSA_E, sizeof TA%d_RSA_E,\n"
                "\t\t\t} }\n"
                "\t\t}\n"
                "\t}" % (i, i, i, i, i, i))
        elif isinstance(pub, ec.EllipticCurvePublicKey):
            curve = pub.curve.name
            if curve not in BR_EC:
                print("WARNING: unsupported curve %s, skipping %s"
                      % (curve, name), file=sys.stderr)
                body.pop()
                continue
            q = pub.public_bytes(Encoding.X962, PublicFormat.UncompressedPoint)
            body.append("static const unsigned char TA%d_EC_Q[] = {\n%s\n};\n"
                        % (i, hexblock(q)))
            table.append(
                "\t{\n"
                "\t\t{ (unsigned char *)TA%d_DN, sizeof TA%d_DN },\n"
                "\t\tBR_X509_TA_CA,\n"
                "\t\t{\n"
                "\t\t\tBR_KEYTYPE_EC,\n"
                "\t\t\t{ .ec = {\n"
                "\t\t\t\tBR_EC_%s,\n"
                "\t\t\t\t(unsigned char *)TA%d_EC_Q, sizeof TA%d_EC_Q,\n"
                "\t\t\t} }\n"
                "\t\t}\n"
                "\t}" % (i, i, curve, i, i))
        else:
            print("WARNING: unsupported key type, skipping %s" % name,
                  file=sys.stderr)
            body.pop()
            continue

    header = (
        "/*\n"
        " * ca_roots.c - compiled-in root CA trust anchors\n"
        " * Generated on %s\n"
        " *\n"
        " * DO NOT EDIT. Regenerate with:\n"
        " *   python3 tools/generate_ca_roots.py --out %s\n"
        " *\n"
        " * Mac OS 9 has no usable system trust store, so this list is the\n"
        " * whole of what Gateway will trust. Anchors (%d):\n"
        "%s"
        " */\n\n"
        "#include \"ca_roots.h\"\n\n"
        % (datetime.date.today().isoformat(), args.out, len(table),
           "".join(" *   %s\n" % n for n, _ in chosen)))

    out = header + "\n".join(body) + "\nstatic const br_x509_trust_anchor TAs[%d] = {\n" % len(table)
    out += ",\n".join(table)
    out += "\n};\n\nconst size_t TAs_NUM = (sizeof TAs) / (sizeof TAs[0]);\n"

    # TAs is declared extern in ca_roots.h, so the definition must not be static.
    out = out.replace("static const br_x509_trust_anchor TAs[",
                      "const br_x509_trust_anchor TAs[")

    open(args.out, "w").write(out)
    print("wrote %s with %d trust anchors" % (args.out, len(table)))


if __name__ == "__main__":
    main()
