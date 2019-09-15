
CC ?= gcc
CFLAGS = -Wall -fomit-frame-pointer -Os

ifneq ($(DEBUG),)
CFLAGS += -g -DDEBUG
endif

NIROOT ?= /etc/neoinit
ifneq ($(NIROOT),/etc/neoinit)
D_NIROOT = -DNIROOT=\"$(NIROOT)\"
endif

MANDIR=/usr/man

all: neoinit neorc hard-reboot killall5 serdo

neoinit: neoinit.o lib/split.o lib/openreadclose.o djb/str_len.o djb/fmt_ulong.o djb/fmt_long.o

neorc: neorc.o djb/str_len.o djb/str_start.o djb/fmt_ulong.o djb/fmt_long.o djb/fmt_str.o \
	djb/errmsg_info.o djb/errmsg_warn.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o

serdo: serdo.o djb/fmt_ulong.c djb/str_copy.o djb/str_chr.o djb/str_diff.o djb/byte_diff.o \
	djb/errmsg_warn.o djb/errmsg_warnsys.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o djb/str_len.o

hard-reboot: hard-reboot.o djb/str_len.o

killall5: killall5.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< $(D_NIROOT)

djb/%.o: djb/%.c
	$(CC) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%: %.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o djb/*.o lib/*.o neoinit neorc hard-reboot killall5 serdo
	rm -rf debug test/etc

install-files:
	install -d $(DESTDIR)/sbin $(DESTDIR)/bin $(DESTDIR)$(MANDIR)/man8
	install neoinit hard-reboot $(DESTDIR)/sbin
	install neorc serdo $(DESTDIR)/bin
	install -m 644 man/hard-reboot.8 man/neoinit.8 man/neorc.8 man/serdo.8 $(DESTDIR)$(MANDIR)/man8

install-fifos:
	install -d $(DESTDIR)$(NIROOT)
	rm -f $(DESTDIR)$(NIROOT)/in $(DESTDIR)$(NIROOT)/out
	mkfifo -m 600 $(DESTDIR)$(NIROOT)/in $(DESTDIR)$(NIROOT)/out

install: install-files install-fifos

test/test-again:
	git clone https://github.com/typedivision/test-again.git test/test-again

debug: export DEBUG = 1
debug: neoinit.c neorc.c
	$(MAKE) clean neoinit neorc
	mkdir _debug
	cp neoinit neorc _debug
	$(MAKE) clean
	mv _debug debug

check: export NIROOT = $(CURDIR)/test/etc/neoinit
check: PATH := test/test-again/bin:$(PATH)
check: debug test/test-again
	@ [ -d test/etc ] || $(MAKE) install-fifos
	test/neoinit.ta $(TEST)
