# Build BearSSL as a static library for PowerPC Mac OS
#
# BearSSL's source is organized by category:
#   src/codec/   — Base64, PEM decoding
#   src/ec/      — Elliptic curve math (ECDH, ECDSA)
#   src/hash/    — SHA-256, SHA-1, MD5, etc.
#   src/int/     — Big integer arithmetic (RSA math)
#   src/kdf/     — Key derivation (HKDF, etc.)
#   src/mac/     — HMAC
#   src/rand/    — HMAC_DRBG (the PRNG BearSSL uses internally)
#   src/rsa/     — RSA encrypt/decrypt/sign/verify
#   src/ssl/     — The TLS state machine — the core of what we use
#   src/symcipher/ — AES, ChaCha20, DES
#   src/x509/    — Certificate chain validation

file(GLOB_RECURSE BEARSSL_SOURCES
    src/aead/*.c
    src/codec/*.c
    src/ec/*.c
    src/hash/*.c
    src/int/*.c
    src/kdf/*.c
    src/mac/*.c
    src/rand/*.c
    src/rsa/*.c
    src/ssl/*.c
    src/symcipher/*.c
    src/x509/*.c
)

add_library(bearssl STATIC ${BEARSSL_SOURCES})

target_include_directories(bearssl
    PUBLIC inc        # BearSSL public headers — consumers see these
    PRIVATE src       # BearSSL internal headers — only BearSSL sees these
)

# BearSSL detects endianness and word size automatically.
# PowerPC is big-endian, 32-bit — BearSSL handles this via
# its internal BR_POWER_ASM / BR_BE checks in src/inner.h.
#
# BR_LOMUL: use 32x32→64 multiplies instead of 64x64.
# PowerPC G3/G4 has a fast 32-bit multiplier but 64-bit
# multiply is emulated. This flag tells BearSSL to use the
# more efficient path.
target_compile_definitions(bearssl PRIVATE BR_LOMUL=1)
