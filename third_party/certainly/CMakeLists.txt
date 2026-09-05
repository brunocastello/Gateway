cmake_minimum_required(VERSION 3.12)
project(certainly C)

# C99 standard — Retro68's GCC 12 supports this fully
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Compiler warnings
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# BearSSL
add_subdirectory(bearssl)

# Certainly — the TLS library
add_library(certainly STATIC
    src/certainly.c
    src/ot_transport.c
    src/entropy.c
    src/ca_roots.c
    src/tls13_keysched.c
    src/tls13_record.c
    src/tls13_handshake.c
)

target_include_directories(certainly
    PUBLIC  include          # certainly.h — consumers see this
    PRIVATE src              # internal headers
    PRIVATE bearssl/inc      # BearSSL headers
)

target_link_libraries(certainly
    PRIVATE bearssl
    PUBLIC  OpenTransportLib OpenTransportAppPPC OpenTptInternetLib OpenTptInetPPC
)

# Examples
add_subdirectory(examples/simple_get)
add_subdirectory(examples/postman)
