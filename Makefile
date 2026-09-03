# SPDX-License-Identifier: MIT
CC     ?= cc
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu11
OBJCFLAGS := -fobjc-arc
LDLIBS := -framework Foundation
prefix ?= /usr/local

LIB   := libgptp.a
PROGS := gptp-refclock gptp-pps-offset
TOOLS := tsdump

all: $(LIB) $(PROGS) $(TOOLS)

$(LIB): gptp.o
	$(AR) rcs $@ $^

gptp.o: gptp.m gptp.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(OBJCFLAGS) -c -o $@ $<

gptp-refclock: gptp-refclock.o chrony-client.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

gptp-pps-offset: gptp-pps-offset.o $(LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

tsdump: tsdump.m
	$(CC) $(CFLAGS) $(OBJCFLAGS) -o $@ $< $(LDLIBS)

gptp-refclock.o: gptp-refclock.c chrony-client.h gptp.h
gptp-pps-offset.o: gptp-pps-offset.c gptp.h
chrony-client.o: chrony-client.c chrony-client.h

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: all
	install -d $(DESTDIR)$(prefix)/bin $(DESTDIR)$(prefix)/lib $(DESTDIR)$(prefix)/include
	install -m 755 $(PROGS) $(DESTDIR)$(prefix)/bin
	install -m 644 $(LIB) $(DESTDIR)$(prefix)/lib
	install -m 644 gptp.h $(DESTDIR)$(prefix)/include

clean:
	rm -f $(PROGS) $(TOOLS) $(LIB) *.o

.PHONY: all install clean
