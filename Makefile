
sources := gpx2tiles.c gpx.c
odir := O
target = gpx2tiles
ofiles = $(patsubst %.c,$(odir)/%.o,$(sources))

LIBXML_CFLAGS := $(shell pkg-config --cflags libxml-2.0)
LIBXML_LIBS := $(shell pkg-config --libs libxml-2.0)
LIBGD_CFLAGS := $(shell pkg-config --cflags gdlib)
LIBGD_LIBS := $(shell pkg-config --libs gdlib)

PREFIX ?= /usr
CC := gcc
# CFLAGS :=
# CPPFLAGS :=
# LDFLAGS :=
# LDLIBS :=
PKG_CFLAGS = $(LIBGD_CFLAGS) $(LIBXML_CFLAGS)
PKG_LIBS = $(LIBGD_LIBS) $(LIBXML_LIBS)

_cflags := -Wall -ggdb -O3
_cppflags := -D_GNU_SOURCE
_ldflags := -Wall -ggdb -O3
_ldlibs := -lm -lpthread

build: $(target)

$(target): link_libs := $(LOADLIBES) $(PKG_LIBS) $(_ldlibs) $(LDLIBS)
$(target): link_flags := $(_ldflags) $(LDFLAGS) $(TARGET_ARCH)
$(target): $(ofiles)
	$(CC) $(link_flags) $^ $(link_libs) $(OUTPUT_OPTION)

$(odir)/%.o: %.c
	@mkdir -p '$(odir)'
	$(CC) -c $(_cflags) $(_cppflags) $(PKG_CFLAGS) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) $< $(OUTPUT_OPTION)

rebuild: clean
	$(MAKE) build

depclean:
	rm -f $(odir)/*.d
clean:
	rm -f $(odir)/*.o $(target)
distclean: clean depclean

install:
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(target) $(DESTDIR)$(PREFIX)/bin

tags:
	ctags $(srcs) *.h

$(odir)/%.d: %.c
	@mkdir -p '$(odir)'
	$(CC) -MM -o $@ $< $(_cppflags) $(CPPFLAGS)

.PHONY: tags build rebuild depclean clean distclean install

ifneq (clean,$(findstring clean,$(MAKECMDGOALS)))
-include $(patsubst %.c,$(odir)/%.d,$(sources))
endif
