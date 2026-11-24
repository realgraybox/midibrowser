# midibrowser - build with:
#   make                     normal (dynamic) build
#   make STATIC=1            static build (default on i386-* toolchains, e.g. uClibc)
#   make SMALL_FONTS=1       only the 6x13 font (scaled on large screens), about 17 KB smaller
#   make install PREFIX=/usr DESTDIR=/tmp/pkg
#
# On modern distributions a static libX11 also needs its dependencies:
#   make STATIC=1 LDLIBS="-lX11 -lxcb -lXau -lXdmcp"

CC      = gcc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

TARGET  = midibrowser
SRCS    = midibrowser.c
OBJS    = $(SRCS:.c=.o)

# Keep the previous behaviour: i386 toolchains link statically unless told otherwise.
TARGET_MACHINE := $(shell $(CC) -dumpmachine)
ifneq ($(findstring i386,$(TARGET_MACHINE)),)
    STATIC ?= 1
endif

CFLAGS  ?= -Os
CFLAGS  += -Wall -Wextra -Wshadow -Wformat=2 -std=gnu99 \
           -ffunction-sections -fdata-sections
CPPFLAGS += -I./xbmfont
LDFLAGS += -Wl,--gc-sections,--sort-common,-s
LDLIBS  ?= -lX11

ifeq ($(STATIC),1)
    LDFLAGS += -static
endif

ifeq ($(SMALL_FONTS),1)
    CPPFLAGS += -DMIDIBROWSER_SMALL_FONTS
endif

.PHONY: all clean install uninstall

all: $(TARGET)

$(OBJS): xbmfont/xbmfont.h

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)
