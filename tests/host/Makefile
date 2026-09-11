# Host tests for Gateway's portable code.
#
# CLAUDE.md rule 8: the HTTP request parser, the OAuth form body and the IMAP
# LOGIN extraction must not include <OpenTransport.h> or <Windows.h>, so they
# build with a plain Linux or macOS cc and can be exercised without Retro68.

CC      ?= cc
PORTABLE = ../../src/portable
CFLAGS  += -std=c99 -Wall -Wextra -Wno-unused-parameter -I$(PORTABLE) -g

SOURCES = \
	$(PORTABLE)/gw_util.c \
	$(PORTABLE)/gw_url.c \
	$(PORTABLE)/gw_http.c \
	$(PORTABLE)/gw_chunked.c \
	$(PORTABLE)/gw_b64.c \
	$(PORTABLE)/gw_mailcmd.c \
	$(PORTABLE)/gw_oauth.c \
	$(PORTABLE)/gw_prefs.c \
	$(PORTABLE)/gw_log.c \
	$(PORTABLE)/gw_rewrite.c \
	$(PORTABLE)/gw_x509write.c \
	$(PORTABLE)/gw_wayback.c

all: test

run_tests: run_tests.c $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $^

test: run_tests
	./run_tests

clean:
	rm -f run_tests

.PHONY: all test clean
