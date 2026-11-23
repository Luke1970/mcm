#!/bin/sh
# build-static.sh - one mcm binary for every Linux distribution ("make static")
#
# Runs inside an Alpine Linux container (podman or docker, see the Makefile):
# musl libc, OpenSSL and ncurses linked in statically, so the program needs
# nothing from the system it runs on but the kernel. musl's iconv has the
# charsets mail uses (ISO-8859-*, Windows-125x, KOI8, UTF-16, CJK).
#
# ncurses is built here (not Alpine's) so that it finds the terminal
# descriptions where every distribution keeps them (Debian: /lib/terminfo),
# with the common ones built in for systems without any.
#
#   /src   the mcm source (read only)
#   /out   where dist/mcm-VERSION-linux-ARCH.tar.gz is written
set -eu

NCURSES=ncurses-6.5
NCURSES_SHA256=136d91bc269a9a5785e5f9e980bc76ab57428f604ce3e5a5a90cebc767971cc6

apk add -q --no-cache build-base linux-headers openssl-dev openssl-libs-static \
	curl tar ncurses-terminfo ncurses >/dev/null

# ncurses, wide characters, static
cd /tmp
curl -sSfLO "https://ftp.gnu.org/gnu/ncurses/$NCURSES.tar.gz"
echo "$NCURSES_SHA256  $NCURSES.tar.gz" | sha256sum -c -
tar xzf "$NCURSES.tar.gz"
cd "$NCURSES"
./configure -q --prefix=/opt/nc --without-shared --without-cxx --without-cxx-binding \
	--without-ada --without-manpages --without-tests --without-progs --disable-db-install \
	--enable-widec --enable-ext-colors --enable-ext-mouse --enable-sp-funcs \
	--with-terminfo-dirs=/etc/terminfo:/lib/terminfo:/usr/share/terminfo:/usr/lib/terminfo:/usr/share/lib/terminfo \
	--with-default-terminfo-dir=/usr/share/terminfo \
	--with-fallbacks=xterm-256color,xterm,xterm-direct,screen-256color,screen,tmux-256color,tmux,linux,vt100,vt220,alacritty,foot
make -s -j"$(nproc)" >/dev/null
make -s install.libs install.includes >/dev/null

# mcm, in a copy (the host's object files stay as they are)
rm -rf /tmp/mcm && mkdir /tmp/mcm
cd /src && tar cf - --exclude='*.o' --exclude=./mcm --exclude=./win --exclude=./dos \
	--exclude=./dist --exclude=./tests/run --exclude=./.git . | tar xf - -C /tmp/mcm
cd /tmp/mcm
VERSION=$(sed -n 's/.*MCM_VERSION *"\(.*\)".*/\1/p' src/mcm.h)
ARCH=$(uname -m)
MK="CC=gcc CURSES_CFLAGS=-I/opt/nc/include/ncursesw\ -I/opt/nc/include CURSES_LIBS=/opt/nc/lib/libncursesw.a \
    SSL_CFLAGS=-DMCM_STATIC SSL_LIBS=-lssl\ -lcrypto DATADIR=/usr/local/share/mcm"
# a static PIE: ASLR as in the normal build
eval make -s -j"$(nproc)" "$MK" "HARDEN_LDFLAGS='-static-pie -Wl,-z,relro,-z,now'"
eval make -s test "$MK" "HARDEN_LDFLAGS='-static-pie -Wl,-z,relro,-z,now'"
strip mcm
file mcm 2>/dev/null || true
./mcm --version

D=mcm-$VERSION-linux-$ARCH
rm -rf "/tmp/$D" && mkdir -p "/tmp/$D/themes"
cp mcm README.md LICENSE "/tmp/$D/"
cp themes/*.theme "/tmp/$D/themes/"
cat > "/tmp/$D/INSTALL.txt" <<EOF
Mail Commander (mcm) $VERSION for Linux ($ARCH), one static program.

It runs on any Linux distribution: nothing needs to be installed. Run it
from here:

    ./mcm

or install it (the themes go to /usr/local/share/mcm/themes):

    sudo install -m 755 mcm /usr/local/bin/mcm
    sudo mkdir -p /usr/local/share/mcm/themes
    sudo cp themes/*.theme /usr/local/share/mcm/themes/

Programs mcm starts are used when installed: gpg (PGP), xdg-open, an
editor (\$VISUAL, \$EDITOR, else vi), lp (printing). See README.md.
EOF
mkdir -p /out
tar czf "/out/$D.tar.gz" -C /tmp "$D"
cd /out && sha256sum "$D.tar.gz" > "$D.tar.gz.sha256"
echo "built /out/$D.tar.gz"
