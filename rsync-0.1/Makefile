# Generated automatically from Makefile.in by configure.
# Makefile for rsync. This is processed by configure to produce the final
# Makefile

INSTALL_BIN=/usr/bin
INSTALL_BIN1=/usr/local/bin
INSTALL_MAN=/usr/local/man

CCOPTFLAGS =      -O 

LIBS=
CC=gcc $(CCOPTFLAGS)

INSTALLCMD=/usr/bin/install -c


SRC=.
SHELL=/bin/sh


.SUFFIXES:
.SUFFIXES: .c .o

OBJS=rsync.o util.o md4.o main.o checksum.o match.o flist.o

all: rsync

install: all
	${INSTALLCMD} -s -o root -m 755 rsync ${INSTALL_BIN}
	${INSTALLCMD} -s -o root -m 755 rsync ${INSTALL_BIN1}

rsync: $(OBJS)
	$(CC) $(CFLAGS) -o rsync $(OBJS) $(LIBS)

proto:
	cat *.c | awk -f mkproto.awk > proto.h

clean:
	rm -f *~ *.o rsync config.cache config.log config.status

dist: 
	tar --exclude-from .ignore -czf dist.tar.gz .
	-mkdir rsync-$(VERSION)
	(cd rsync-$(VERSION) ; tar xzf ../dist.tar.gz)
	tar -czf rsync-$(VERSION).tar.gz rsync-$(VERSION)
	rm -f dist.tar.gz
	echo rsync-$(VERSION) >> .cvsignore
