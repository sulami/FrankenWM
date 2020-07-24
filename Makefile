WMNAME  = frankenwm

PREFIX ?= /usr/local
BINDIR ?= ${PREFIX}/bin
MANPREFIX ?= ${PREFIX}/share/man

INCS = -I. -I. `pkg-config --cflags xcb xcb-aux xcb-icccm xcb-keysyms xcb-ewmh`
LIBS = -lc -lX11 `pkg-config --libs xcb xcb-aux xcb-icccm xcb-keysyms xcb-ewmh`

CFLAGS   += -std=c99 -pedantic -Wall -Wextra ${INCS} ${CPPFLAGS}
LDFLAGS  += ${LIBS}

EXEC = ${WMNAME}

SRC = ${WMNAME}.c
OBJ = ${SRC:.c=.o}

ifeq (${DEBUG},0)
   CFLAGS  += -Os
else
   CFLAGS  += -g
   LDFLAGS += -g
endif

all: options ${WMNAME}

options:
	@echo ${WMNAME} build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.h

config.h:
	@echo creating $@ from config.def.h
	@cp config.def.h $@

${WMNAME}: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	@echo cleaning
	@rm -fv ${WMNAME} ${OBJ} ${WMNAME}-${VERSION}.tar.gz

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@install -Dm755 ${WMNAME} ${DESTDIR}${PREFIX}/bin/${WMNAME}
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man.1
	@install -Dm644 ${WMNAME}.1 ${DESTDIR}${MANPREFIX}/man1/${WMNAME}.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/${WMNAME}
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/${WMNAME}.1

.PHONY: all options clean install uninstall
