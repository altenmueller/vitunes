# Makefile for [Open|Free|Net]BSD and Mac OS X

# install locations
PREFIX?=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/man/man1

# non-base dependency build info
CDEPS=`taglib-config --cflags`
LDEPS=`taglib-config --libs` -ltag_c

# build info
CC?=/usr/bin/cc
CFLAGS+=-c -std=c89 -Wall -Wextra -Wno-unused-value $(CDEPS) $(CDEBUG)
LDFLAGS+=-lm -lncurses -lutil $(LDEPS)

VPATH=players

OBJS=commands.o compat.o e_commands.o \
	  keybindings.o medialib.o meta_info.o \
	  mplayer.o paint.o player.o player_utils.o \
	  playlist.o socket.o str2argv.o \
	  uinterface.o vitunes.o

.PATH: players

# main targets

.PHONY: debug clean install uninstall publish-repos man-debug linux

vitunes: $(OBJS)
	$(CC) -o $@ $(LDFLAGS) $(OBJS)

.c.o:
	$(CC) $(CFLAGS) $<

debug:
	make CDEBUG="-DDEBUG -g"

clean:
	rm -f *.o
	rm -f vitunes vitunes.core vitunes-debug.log

install: vitunes
	/usr/bin/install -c -m 0555 vitunes $(BINDIR)
	/usr/bin/install -c -m 0444 vitunes.1 $(MANDIR)

uninstall:
	rm -f $(BINDIR)/vitunes
	rm -f $(MANDIR)/vitunes.1

# misc.

linux:
	make -f Makefile.linux

man-debug:
	mandoc -Wall vitunes.1 > /dev/null

cscope.out: *.h *.c
	cscope -bke

publish-repos:
	hg push $(myhg)/vitunes
	hg push $(mybb)/vitunes
	hg push $(mygit)/vitunes

