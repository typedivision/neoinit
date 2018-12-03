all: minit msvc pidfilehack hard-reboot write_proc killall5 shutdown \
minit-update serdo

#CFLAGS=-pipe -march=i386 -fomit-frame-pointer -Os -I../dietlibc/include
CC=gcc
CFLAGS=-Wall -W -pipe -fomit-frame-pointer -Os
CROSS=
#CROSS=arm-linux-
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

minit: minit.o split.o openreadclose.o opendevconsole.o djb/str_len.o djb/fmt_ulong.o
msvc: msvc.o djb/str_len.o djb/str_start.o djb/fmt_ulong.o djb/fmt_str.o \
	djb/errmsg_info.o djb/errmsg_warn.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o
minit-update: minit-update.o split.o openreadclose.o \
	djb/str_len.o djb/byte_copy.o djb/buffer_1.o djb/buffer_2.o djb/buffer_flush.o \
	djb/buffer_put.o djb/buffer_putflush.o djb/buffer_puts.o djb/buffer_putsflush.o djb/buffer_stubborn.c
serdo: serdo.o djb/fmt_ulong.c djb/str_copy.o djb/str_chr.o djb/str_diff.o djb/byte_diff.o \
	djb/errmsg_warn.o djb/errmsg_warnsys.o djb/errmsg_iam.o djb/errmsg_write.o djb/errmsg_puts.o djb/str_len.o

shutdown: shutdown.o split.o openreadclose.o opendevconsole.o djb/str_len.o
	$(DIET) $(CROSS)$(CC) $(LDFLAGS) -o shutdown $^

%.o: %.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -c $<

djb/%.o: djb/%.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -c $< -o $@

%: %.o
	$(DIET) $(CROSS)$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f *.o djb/*.o minit msvc pidfilehack hard-reboot write_proc killall5 \
	shutdown minit-update serdo

test: test.c
	gcc -nostdlib -o $@ $^ -I../dietlibc/include ../dietlibc/start.o ../dietlibc/dietlibc.a

pidfilehack: pidfilehack.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -o $@ $^

hard-reboot: hard-reboot.c djb/str_len.o
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -o $@ $^

write_proc: write_proc.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -o $@ $^

killall5: killall5.c
	$(DIET) $(CROSS)$(CC) $(CFLAGS) -o $@ $^

install-files:
	install -d $(DESTDIR)/etc/minit $(DESTDIR)/sbin $(DESTDIR)/bin $(DESTDIR)$(MANDIR)/man8
	install minit pidfilehack $(DESTDIR)/sbin
	install write_proc hard-reboot minit-update $(DESTDIR)/sbin
	install msvc serdo $(DESTDIR)/bin
	install -m 4750 shutdown $(DESTDIR)/sbin
	test -f $(DESTDIR)/sbin/init || ln $(DESTDIR)/sbin/minit $(DESTDIR)/sbin/init
	install -m 644 hard-reboot.8 minit-list.8 minit-shutdown.8 minit-update.8 minit.8 msvc.8 pidfilehack.8 serdo.8 $(DESTDIR)$(MANDIR)/man8

install-fifos:
	-mkfifo -m 600 $(DESTDIR)/etc/minit/in $(DESTDIR)/etc/minit/out

install: install-files install-fifos

VERSION=minit-$(shell head -n 1 CHANGES|sed 's/://')
CURNAME=$(notdir $(shell pwd))

tar: clean rename
	cd ..; tar cvvf $(VERSION).tar.bz2 --use=bzip2 --exclude CVS $(VERSION)

rename:
	if test $(CURNAME) != $(VERSION); then cd .. && mv $(CURNAME) $(VERSION); fi

