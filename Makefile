# Mail Commander (mcm) - console mail client
#
# GNU make is needed: "make" on Linux, "gmake" on FreeBSD, OpenBSD and NetBSD.
# See README.md, "Build", for the packages per system.
#
#   make                  build mcm
#   make install          install in $(PREFIX) (default /usr/local)
#   make uninstall        remove it again
#   make clean            remove what make built
#   make test             build and run the tests in tests/ (SANITIZE=1: with
#                         AddressSanitizer and UBSan; FUZZ=n: more fuzz rounds)
#   make SANITIZE=1       mcm itself with AddressSanitizer and UBSan (for
#                         testing; "make clean" first, so all is rebuilt)
#   make HARDEN=0         without the hardening flags (see below)
#   make win              Windows 10/11: win/mcm/mcm.exe (one file) and win/mcm.zip
#                         (cross-compiled with MinGW-w64, see README.md)
#   make static           Linux: one static program for every distribution,
#                         dist/mcm-VERSION-linux-ARCH.tar.gz (built in an
#                         Alpine container with podman or docker)
#   make dos-bench        MS-DOS: the TLS benchmark (Open Watcom, BearSSL)
#
# Libraries are found with pkg-config (ncursesw, openssl); without it the
# usual library names are used. Override on the command line if needed, e.g.
#   make CURSES_LIBS=-lncurses

CC      ?= cc
PREFIX  ?= /usr/local
DATADIR ?= $(PREFIX)/share/mcm
CFLAGS  ?= -O2 -g

OS := $(shell uname -s)

PKG_CONFIG ?= pkg-config
pc_cflags = $(shell $(PKG_CONFIG) --cflags $(1) 2>/dev/null)
pc_libs   = $(shell $(PKG_CONFIG) --libs $(1) 2>/dev/null || echo $(2))

CURSES_CFLAGS := $(call pc_cflags,ncursesw)
CURSES_LIBS   := $(call pc_libs,ncursesw,-lncursesw)
SSL_CFLAGS    := $(call pc_cflags,openssl)
SSL_LIBS      := $(call pc_libs,openssl,-lssl -lcrypto)
ICONV_LIBS    :=
OS_CFLAGS     :=
OS_LDFLAGS    :=

ifeq ($(OS),OpenBSD)
# ncurses (libcurses) and LibreSSL in the base system, iconv from packages.
# Tested on OpenBSD 8.0.
CURSES_CFLAGS := -D_XOPEN_SOURCE_EXTENDED
CURSES_LIBS   := -lcurses
ICONV_LIBS    := -liconv
OS_CFLAGS     := -I/usr/local/include
OS_LDFLAGS    := -L/usr/local/lib
endif
ifeq ($(OS),NetBSD)
# ncursesw and pkg-config from pkgsrc (/usr/pkg); OpenSSL in the base system.
OS_CFLAGS     := -I/usr/pkg/include
OS_LDFLAGS    := -L/usr/pkg/lib -Wl,-R/usr/pkg/lib
endif
ifeq ($(OS),FreeBSD)
# ncurses from packages (/usr/local), OpenSSL in the base system.
OS_CFLAGS     := -I/usr/local/include
OS_LDFLAGS    := -L/usr/local/lib
endif

# Hardening: mcm reads mail from anyone, so a bug in a parser should not
# become a way in. Stack protector and clash protection, control-flow
# protection (x86), checked string and memory functions (FORTIFY), a
# position-independent executable (ASLR) and read-only relocations (full
# RELRO). Only the flags the compiler accepts are used; HARDEN=0: none.
cc_flag = $(shell echo 'int main(void){return 0;}' | $(CC) $(1) -x c - -o /dev/null >/dev/null 2>&1 && echo "$(1)")
RELRO := -Wl,-z,relro,-z,now
ifneq ($(HARDEN),0)
HARDEN_CFLAGS  := $(call cc_flag,-fstack-protector-strong) $(call cc_flag,-fstack-clash-protection) \
                  $(call cc_flag,-fcf-protection) $(call cc_flag,-fPIE) -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3
HARDEN_LDFLAGS := $(call cc_flag,-pie) $(call cc_flag,$(RELRO))
endif
ifeq ($(SANITIZE),1)
SAN_FLAGS      := -fsanitize=address,undefined -fno-omit-frame-pointer
HARDEN_CFLAGS  := $(filter-out -D_FORTIFY_SOURCE=3,$(HARDEN_CFLAGS))
endif

MCM_CFLAGS := -std=gnu11 -Wall -Wextra -Wformat -Wformat-security -D_GNU_SOURCE \
               -DDATADIR=\"$(DATADIR)\" $(HARDEN_CFLAGS) $(SAN_FLAGS) \
               $(CURSES_CFLAGS) $(SSL_CFLAGS) $(OS_CFLAGS)
MCM_LDFLAGS := $(HARDEN_LDFLAGS) $(SAN_FLAGS)
MCM_LIBS   := $(CURSES_LIBS) $(SSL_LIBS) $(ICONV_LIBS)

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

all: mcm

mcm: $(OBJ)
	$(CC) $(MCM_LDFLAGS) $(LDFLAGS) $(OS_LDFLAGS) -o $@ $(OBJ) $(MCM_LIBS) $(LDLIBS)

src/%.o: src/%.c $(wildcard src/*.h)
	$(CC) $(CPPFLAGS) $(MCM_CFLAGS) $(CFLAGS) -c -o $@ $<

# The help texts are plain text files in help/; they are compiled in.
HELP := help/cmdline.txt help/usage.txt help/keys.txt help/editor.txt
src/helptext.h: $(HELP) tools/txt2c.awk
	{ echo "/* generated from the files in help/ by tools/txt2c.awk - edit those */"; \
	  awk -v name=HELP_CMDLINE -f tools/txt2c.awk help/cmdline.txt; \
	  awk -v name=HELP_USAGE -f tools/txt2c.awk help/usage.txt; \
	  awk -v name=HELP_KEYS -f tools/txt2c.awk help/keys.txt; \
	  awk -v name=HELP_EDITOR -f tools/txt2c.awk help/editor.txt; } > $@.tmp && mv $@.tmp $@

src/main.o src/keys.o src/editor.o: src/helptext.h

# Tests: tests/run, built from the tested sources themselves (so SANITIZE=1
# applies to them too). Only the parts without screen or network are tested:
# util, mime, textconv, config, vault.
TEST_SRC := $(wildcard tests/*.c) src/util.c src/mime.c src/textconv.c src/config.c src/vault.c
ifeq ($(SANITIZE),1)
TEST_FLAGS := -O1 -g -fno-sanitize-recover=undefined
else
TEST_FLAGS := -O1 -g
endif

test: tests/run
	./tests/run

tests/run: $(TEST_SRC) tests/check.h $(wildcard src/*.h) Makefile
	$(CC) $(CPPFLAGS) $(MCM_CFLAGS) $(TEST_FLAGS) $(MCM_LDFLAGS) $(LDFLAGS) $(OS_LDFLAGS) -o $@ $(TEST_SRC) $(MCM_LIBS) $(LDLIBS)

install: mcm
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(DATADIR)/themes
	install -m 755 mcm $(DESTDIR)$(PREFIX)/bin/mcm
	install -m 644 themes/*.theme $(DESTDIR)$(DATADIR)/themes/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/mcm
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	rm -f mcm $(OBJ) src/helptext.h tests/run
	rm -rf dist
	rm -rf win/obj win/mcm win/mcm.zip
	rm -rf dos/obj16 dos/obj32 dos/*.EXE dos/*.o dos/*.obj dos/*.err

# ---------------------------------------------------------------------------
# Linux, static: musl, OpenSSL and ncurses linked in (tools/build-static.sh).
CONTAINER ?= $(shell command -v podman >/dev/null 2>&1 && echo podman || echo docker)
ALPINE    ?= docker.io/library/alpine:3.22
# podman without systemd (or in a container): cgroupfs, no cgroups
CONTAINER_OPTS ?= $(if $(filter podman,$(CONTAINER)),--cgroup-manager=cgroupfs,)
CONTAINER_RUN  ?= $(if $(filter podman,$(CONTAINER)),--cgroups=disabled,)

static:
	mkdir -p dist
	$(CONTAINER) $(CONTAINER_OPTS) run $(CONTAINER_RUN) --rm --network=host -v "$(CURDIR)":/src:ro,Z \
	  -v "$(CURDIR)/dist":/out:Z $(ALPINE) sh /src/tools/build-static.sh

# ---------------------------------------------------------------------------
# Windows 10 / 11, cross-compiled with MinGW-w64. The result is one file,
# win/mcm/mcm.exe, with PDCurses (the Windows console), OpenSSL, zlib and
# win-iconv built in, plus win/mcm/themes; also packed as win/mcm.zip.
# Fedora:
#   dnf install mingw64-gcc mingw64-openssl-static mingw64-zlib-static \
#               mingw64-win-iconv-static
# PDCurses 3.8 is compiled from its source (PDCURSES = the unpacked
# PDCurses-3.8 directory, https://github.com/wmcbrine/PDCurses/releases),
# with the same settings as mcm: wide characters, UTF-8, RGB color numbers.
WIN_CC     ?= x86_64-w64-mingw32-gcc
WIN_AR     ?= x86_64-w64-mingw32-ar
WIN_RC     ?= x86_64-w64-mingw32-windres
WIN_ROOT   ?= /usr/x86_64-w64-mingw32/sys-root/mingw
WIN_LIBDIR ?= $(WIN_ROOT)/lib
PDCURSES   ?= PDCurses-3.8
WIN_OBJ    := $(patsubst src/%.c,win/obj/%.o,$(SRC)) win/obj/mcm.res
WIN_FLAGS  := -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -D__USE_MINGW_ANSI_STDIO=1 \
              -D_FILE_OFFSET_BITS=64 -include src/compat.h -I$(PDCURSES) -I$(WIN_ROOT)/include
PDC_FLAGS  := -O2 -DPDC_WIDE -DPDC_FORCE_UTF8 -DPDC_RGB -I$(PDCURSES)
PDC_SRC     = $(wildcard $(PDCURSES)/pdcurses/*.c) $(wildcard $(PDCURSES)/wincon/*.c)

win: win/mcm/mcm.exe

win-check:
	@test -f $(PDCURSES)/curses.h || { echo "PDCurses source not found: set PDCURSES=dir (now $(PDCURSES))"; exit 1; }
	@test -f $(WIN_LIBDIR)/libssl.a || { echo "static OpenSSL not found in $(WIN_LIBDIR) (mingw64-openssl-static)"; exit 1; }

win/obj/libpdcurses.a: | win-check
	@mkdir -p win/obj/pdc
	for f in $(PDC_SRC); do \
	  $(WIN_CC) $(PDC_FLAGS) -c -o win/obj/pdc/$$(basename $${f%.c}).o $$f || exit 1; done
	rm -f $@ && $(WIN_AR) rcs $@ win/obj/pdc/*.o

win/obj/%.o: src/%.c $(wildcard src/*.h) | win/obj/libpdcurses.a
	$(WIN_CC) $(WIN_FLAGS) $(CFLAGS) -c -o $@ $<

win/obj/main.o win/obj/keys.o win/obj/editor.o: src/helptext.h

win/obj/mcm.res: win/mcm.rc win/mcm.manifest
	@mkdir -p win/obj
	cd win && $(WIN_RC) -O coff -o obj/mcm.res mcm.rc

win/mcm/mcm.exe: $(WIN_OBJ) win/obj/libpdcurses.a $(wildcard themes/*.theme)
	rm -rf win/mcm/themes && mkdir -p win/mcm/themes
	$(WIN_CC) -static -s -Wl,--dynamicbase,--nxcompat,--high-entropy-va -o $@ $(WIN_OBJ) win/obj/libpdcurses.a -L$(WIN_LIBDIR) -lssl -lcrypto \
	  -lz -liconv -lws2_32 -lcrypt32 -lbcrypt -ladvapi32 -luser32 -lgdi32 -lpathcch -lshell32
	cp themes/*.theme win/mcm/themes/
	cd win && rm -f mcm.zip && { command -v zip >/dev/null && zip -qr mcm.zip mcm || true; }

# ---------------------------------------------------------------------------
# MS-DOS. mcm itself does not run on DOS yet (see README.md, "MS-DOS"); what
# builds is the TLS benchmark that measures BearSSL on an 8088 or 386:
#   dos/BENCH.EXE    16-bit, any PC (8088 and up), large memory model
#   dos/BENCH32.EXE  32-bit, 386 and up, with the DOS/32A extender built in
# It is cross-compiled with Open Watcom v2 (WATCOM = its directory) against
# BearSSL 0.6 (BEARSSL = its source directory, patched with
# dos/bearssl-0.6-16bit.patch).
WATCOM  ?= /opt/watcom
BEARSSL ?= bearssl-0.6
WBIN    := $(WATCOM)/binl64
WENV    := WATCOM=$(WATCOM) INCLUDE=$(WATCOM)/h PATH=$(WBIN):$(WATCOM)/binw:$$PATH
BR_DEFS := -dBR_USE_URANDOM=0 -dBR_USE_GETENTROPY=0 -dBR_USE_WIN32_RAND=0 -dBR_RDRAND=0 \
           -dBR_USE_UNIX_TIME=0 -dBR_USE_WIN32_TIME=0 -dBR_AES_X86NI=0 -dBR_SSE2=0 \
           -dBR_POWER8=0 -dBR_64=0 -dBR_INT128=0 -dBR_UMUL128=0 -dBR_LOMUL=0
BR_SRC   = $(wildcard $(BEARSSL)/src/*/*.c)

dos-bench: dos/BENCH.EXE dos/BENCH32.EXE

dos-check:
	@test -x $(WBIN)/wcc || { echo "Open Watcom not found: set WATCOM=dir (now $(WATCOM))"; exit 1; }
	@test -f $(BEARSSL)/inc/bearssl.h || { echo "BearSSL not found: set BEARSSL=dir (now $(BEARSSL))"; exit 1; }
	@grep -q 'seed\[0\]\.data' $(BEARSSL)/src/ssl/ssl_engine.c || \
	  { echo "BearSSL is not patched yet: cd $(BEARSSL) && patch -p1 < $(CURDIR)/dos/bearssl-0.6-16bit.patch"; exit 1; }

dos/obj16/bearssl.lib: | dos-check
	mkdir -p dos/obj16
	cd dos/obj16 && for f in $(abspath $(BR_SRC)); do \
	  $(WENV) wcc -q -bt=dos -ml -0 -ox -za99 $(BR_DEFS) -i=$(abspath $(BEARSSL))/inc \
	    -i=$(abspath $(BEARSSL))/src -fo=$$(basename $${f%.c}).obj $$f || exit 1; done
	cd dos/obj16 && $(WENV) wlib -q -n bearssl.lib $$(ls *.obj | sed 's/^/+/')

dos/obj32/bearssl.lib: | dos-check
	mkdir -p dos/obj32
	cd dos/obj32 && for f in $(abspath $(BR_SRC)); do \
	  $(WENV) wcc386 -q -bt=dos -mf -3r -ox -za99 $(BR_DEFS) -i=$(abspath $(BEARSSL))/inc \
	    -i=$(abspath $(BEARSSL))/src -fo=$$(basename $${f%.c}).obj $$f || exit 1; done
	cd dos/obj32 && $(WENV) wlib -q -n bearssl.lib $$(ls *.obj | sed 's/^/+/')

dos/BENCH.EXE: dos/bench.c dos/vectors.h dos/obj16/bearssl.lib
	cd dos && $(WENV) wcl -q -bt=dos -ml -0 -ox -za99 -k8192 $(BR_DEFS) \
	  -i=$(abspath $(BEARSSL))/inc -fe=BENCH.EXE bench.c obj16/bearssl.lib

dos/BENCH32.EXE: dos/bench.c dos/vectors.h dos/obj32/bearssl.lib
	cd dos && $(WENV) wcl386 -q -bt=dos -l=dos32a -3r -fpc -ox -za99 $(BR_DEFS) \
	  -i=$(abspath $(BEARSSL))/inc -fe=BENCH32.EXE bench.c obj32/bearssl.lib

.PHONY: all test static install uninstall clean win win-check dos-bench dos-check
