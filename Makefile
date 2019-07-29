all: minit msvc hard-reboot killall5 serdo

CC ?= gcc
CFLAGS = -Wall -fomit-frame-pointer -Os

ifneq ($(DEBUG),)
CFLAGS += -g -DDEBUG
endif

MANDIR=/usr/man

minit: minit.o lib/split.o lib/openreadclose.o djb/str_len.o djb/fmt_ulong.o djb/fmt_long.o

msvc: msvc.o djb/str_len.o djb/str_start.o djb/fmt_ulong.o djb/fmt_long.o djb/fmt_str.o \
	djb/errmsg_info.o djb/errmsg_warn.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o

serdo: serdo.o djb/fmt_ulong.c djb/str_copy.o djb/str_chr.o djb/str_diff.o djb/byte_diff.o \
	djb/errmsg_warn.o djb/errmsg_warnsys.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o djb/str_len.o

hard-reboot: hard-reboot.o djb/str_len.o

killall5: killall5.o

%.o: %.c
	$(CC) $(CFLAGS) -c $<

djb/%.o: djb/%.c
	$(CC) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%: %.o
	$(CC) $(LDFLAGS) -o $@ $^

clean:
	rm -f *.o djb/*.o lib/*.o minit msvc hard-reboot killall5 serdo

install-files:
	install -d $(DESTDIR)/etc/minit $(DESTDIR)/sbin $(DESTDIR)/bin $(DESTDIR)$(MANDIR)/man8
	install minit hard-reboot $(DESTDIR)/sbin
	install msvc serdo $(DESTDIR)/bin
	test -f $(DESTDIR)/sbin/init || ln $(DESTDIR)/sbin/minit $(DESTDIR)/sbin/init
	install -m 644 man/hard-reboot.8 man/minit.8 man/msvc.8 man/serdo.8 $(DESTDIR)$(MANDIR)/man8

install-fifos:
	mkfifo -m 600 $(DESTDIR)/etc/minit/in $(DESTDIR)/etc/minit/out

install: install-files install-fifos
