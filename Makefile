all: minit msvc hard-reboot killall5 serdo

#CFLAGS=-pipe -march=i386 -fomit-frame-pointer -Os -I../dietlibc/include
CC=gcc
CFLAGS=-Wall -W -pipe -fomit-frame-pointer -Os
LDFLAGS=-s
MANDIR=/usr/man

path = $(subst :, ,$(PATH))
diet_path = $(foreach dir,$(path),$(wildcard $(dir)/diet))
ifeq ($(strip $(diet_path)),)
ifneq ($(wildcard /opt/diet/bin/diet),)
DIET=/opt/diet/bin/diet
else
DIET=
endif
else
DIET:=$(strip $(diet_path))
endif

ifneq ($(DEBUG),)
CFLAGS+=-g
LDFLAGS+=-g
else
CFLAGS+=-O2 -fomit-frame-pointer
LDFLAGS+=-s
ifneq ($(DIET),)
DIET+=-Os
endif
endif

libowfat_path = $(strip $(foreach dir,../libowfat*,$(wildcard $(dir)/textcode.h)))
ifneq ($(libowfat_path),)
LDLIBS=-lowfat
CFLAGS+=$(foreach fnord,$(libowfat_path),-I$(dir $(fnord)))
LDFLAGS+=$(foreach fnord,$(libowfat_path),-L$(dir $(fnord)))
endif

minit: minit.o lib/split.o lib/openreadclose.o djb/str_len.o djb/fmt_ulong.o

msvc: msvc.o djb/str_len.o djb/str_start.o djb/fmt_ulong.o djb/fmt_str.o \
	djb/errmsg_info.o djb/errmsg_warn.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o

serdo: serdo.o djb/fmt_ulong.c djb/str_copy.o djb/str_chr.o djb/str_diff.o djb/byte_diff.o \
	djb/errmsg_warn.o djb/errmsg_warnsys.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o djb/str_len.o

%.o: %.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -c $<

djb/%.o: djb/%.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%: %.o
	$(DIET) $(CROSS)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

hard-reboot: hard-reboot.c djb/str_len.o
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -o $@ $^

killall5: killall5.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -o $@ $^

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
