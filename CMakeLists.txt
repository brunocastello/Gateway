cmake_minimum_required(VERSION 3.12)
project(Gateway C CXX)


# Every target in this build is the Classic Mac OS one. src/gw_platform.h keys
# the Toolbox spellings of Ptr, UInt16 and NewPtr off this rather than guessing
# at which macro the toolchain predefines; the Windows build simply does not
# set it and gets the C library instead.
add_compile_definitions(GW_MAC_OS)

# The same build stamp the Windows makefile applies, so a log from either
# platform names the revision it came from.
# CI passes it in; the container running this has no repository of its own to
# ask, which is why every Mac log used to read "build unknown".
if(DEFINED ENV{GW_BUILD_ID})
    set(GW_BUILD_ID $ENV{GW_BUILD_ID})
else()
    execute_process(COMMAND git rev-parse --short HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE GW_BUILD_ID
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
endif()
if(NOT GW_BUILD_ID)
    set(GW_BUILD_ID "unknown")
endif()
add_compile_definitions(GW_BUILD_ID="${GW_BUILD_ID}")

#
# Gateway - a TLS 1.3 gateway that runs in the background on Mac OS 9 (PPC).
#
# The build has one unusual rule (CLAUDE.md rule 3). Retro68 ships the
# open-source Multiversal Interfaces, which have no Open Transport at all.
# Apple's Universal Interfaces are staged separately by the CI script into
# $GW_UNIVERSAL_DIR and put on the include path of the Certainly and Open
# Transport translation units *only*. src/main.cpp and everything else on the
# UI side stays on Multiversal, so the two never mix inside one file.
#
# GW_UNIVERSAL_DIR is the "universal" directory that
# Retro68's interfaces-and-libraries.sh produces inside the toolchain prefix:
#
#   $GW_UNIVERSAL_DIR/CIncludes   Apple's prepared C headers
#   $GW_UNIVERSAL_DIR/libppc      import libraries and the OT glue archives
#

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GW_UNIVERSAL_DIR "" CACHE PATH
    "Retro68 'universal' staging directory holding Apple's Universal Interfaces")

if(NOT GW_UNIVERSAL_DIR)
    message(FATAL_ERROR
        "GW_UNIVERSAL_DIR is not set. Stage Apple's Universal Interfaces first:\n"
        "  interfaces-and-libraries.sh <toolchain> third_party/InterfacesAndLibraries false true false\n"
        "  interfaces-and-libraries.sh <toolchain> --multiversal false true false\n"
        "then configure with -DGW_UNIVERSAL_DIR=<toolchain>/universal")
endif()

set(GW_UNIV_INC ${GW_UNIVERSAL_DIR}/CIncludes)
set(GW_UNIV_LIB ${GW_UNIVERSAL_DIR}/libppc)

# Open Transport: two CFM import libraries plus the static client glue.
set(GW_OT_LIBS
    ${GW_UNIV_LIB}/libOpenTransportLib.a
    ${GW_UNIV_LIB}/libOpenTptInternetLib.a
    ${GW_UNIV_LIB}/libOpenTransportAppPPC.a
    ${GW_UNIV_LIB}/libOpenTptInetPPC.a
)

set(CERTAINLY_DIR ${CMAKE_SOURCE_DIR}/third_party/certainly)

# --------------------------------------------------------------------
# BearSSL - plain C99 against the standard library only, so it builds
# happily against Multiversal.
# --------------------------------------------------------------------

file(GLOB_RECURSE BEARSSL_SOURCES
    ${CERTAINLY_DIR}/bearssl/src/aead/*.c
    ${CERTAINLY_DIR}/bearssl/src/codec/*.c
    ${CERTAINLY_DIR}/bearssl/src/ec/*.c
    ${CERTAINLY_DIR}/bearssl/src/hash/*.c
    ${CERTAINLY_DIR}/bearssl/src/int/*.c
    ${CERTAINLY_DIR}/bearssl/src/kdf/*.c
    ${CERTAINLY_DIR}/bearssl/src/mac/*.c
    ${CERTAINLY_DIR}/bearssl/src/rand/*.c
    ${CERTAINLY_DIR}/bearssl/src/rsa/*.c
    ${CERTAINLY_DIR}/bearssl/src/ssl/*.c
    ${CERTAINLY_DIR}/bearssl/src/symcipher/*.c
    ${CERTAINLY_DIR}/bearssl/src/x509/*.c
)

add_library(bearssl STATIC ${BEARSSL_SOURCES})
target_include_directories(bearssl
    PUBLIC  ${CERTAINLY_DIR}/bearssl/inc
    PRIVATE ${CERTAINLY_DIR}/bearssl/src
)

# --------------------------------------------------------------------
# Certainly - vendored, with the ChaCha20-Poly1305 tag check restored
# (see third_party/certainly/PATCHES.md). Needs Open Transport, so it
# gets the Universal Interfaces.
# --------------------------------------------------------------------

add_library(certainly STATIC
    ${CERTAINLY_DIR}/src/certainly.c
    ${CERTAINLY_DIR}/src/transport_ot.c
    ${CERTAINLY_DIR}/src/entropy.c
    ${CERTAINLY_DIR}/src/ca_roots.c
    ${CERTAINLY_DIR}/src/tls13_keysched.c
    ${CERTAINLY_DIR}/src/tls13_record.c
    ${CERTAINLY_DIR}/src/tls13_handshake.c
)

# GW_UNIV_INC is deliberately PRIVATE: Apple's headers must not leak along the
# link graph into src/main.cpp, which is a Multiversal translation unit.
# Selects the Open Transport implementation of certainly_transport.h and the
# EndpointRef spelling of CTSocket. A Windows build defines nothing and gets
# transport_win32.c instead.
target_compile_definitions(certainly PUBLIC CERTAINLY_OPEN_TRANSPORT)

target_include_directories(certainly BEFORE
    PRIVATE ${GW_UNIV_INC}
    PUBLIC  ${CERTAINLY_DIR}/include
    PRIVATE ${CERTAINLY_DIR}/src
    PRIVATE ${CERTAINLY_DIR}/bearssl/inc
)
target_link_libraries(certainly PRIVATE bearssl)
target_compile_options(certainly PRIVATE -Wall -Wno-unused-parameter)

# --------------------------------------------------------------------
# Portable core - protocol grammar with no system headers at all, so
# tests/host builds exactly these files with a Linux compiler.
# --------------------------------------------------------------------

add_library(gw_portable STATIC
    src/portable/gw_util.c
    src/portable/gw_url.c
    src/portable/gw_http.c
    src/portable/gw_chunked.c
    src/portable/gw_b64.c
    src/portable/gw_mailcmd.c
    src/portable/gw_oauth.c
    src/portable/gw_prefs.c
    src/portable/gw_log.c
    src/portable/gw_wayback.c
)
target_include_directories(gw_portable PUBLIC src/portable)
target_compile_options(gw_portable PRIVATE -Wall -Wextra -Wno-unused-parameter)

# --------------------------------------------------------------------
# Network core - every translation unit here reaches Open Transport, so
# every one of them gets the Universal Interfaces.
# --------------------------------------------------------------------

add_library(gw_net STATIC
    src/net/gw_net.c
    src/net/gw_stream.c
    src/gw_core.c
    src/gw_config.c
    src/gw_plat_mac.c
    src/proxy/gw_httpproxy.c
    src/proxy/gw_mail.c
    src/proxy/gw_token.c
)
target_include_directories(gw_net BEFORE PRIVATE ${GW_UNIV_INC})
target_include_directories(gw_net PUBLIC src)
target_link_libraries(gw_net PUBLIC certainly gw_portable)
target_compile_options(gw_net PRIVATE -Wall -Wno-unused-parameter)

# --------------------------------------------------------------------
# The application - Multiversal only. Note the absence of GW_UNIV_INC:
# main.cpp reaches the core through the plain-C src/gw_core.h.
# --------------------------------------------------------------------

add_application(Gateway
    src/main.cpp
    src/ui/gateway.r
    src/ui/gateway_icon.r
    TYPE "APPL"
    CREATOR "GT9A"
)

target_include_directories(Gateway PRIVATE src)
target_link_libraries(Gateway gw_net gw_portable certainly bearssl ${GW_OT_LIBS})

set_target_properties(Gateway PROPERTIES COMPILE_OPTIONS -ffunction-sections)
set_target_properties(Gateway PROPERTIES LINK_FLAGS "-Wl,-gc-sections")
