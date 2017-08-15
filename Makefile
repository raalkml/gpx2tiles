
sources := gpx2tiles.c gpx.c
odir := O
target = gpx2tiles
ofiles = $(patsubst %.c,$(odir)/%.o,$(sources))

LIBXML_CFLAGS := $(shell pkg-config --cflags libxml-2.0)
LIBXML_LIBS := $(shell pkg-config --libs libxml-2.0)
LIBGD_CFLAGS := $(shell pkg-config --cflags gdlib)
LIBGD_LIBS := $(shell pkg-config --libs gdlib)

CC := gcc
CFLAGS := -Wall -ggdb -O3
CPPFLAGS := $(LIBGD_CFLAGS) $(LIBXML_CFLAGS)
LDFLAGS := -Wall -ggdb -O3
LDLIBS := $(LIBGD_LIBS) $(LIBXML_LIBS) -lm -lpthread

build: $(target)

$(target): $(ofiles)
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

$(odir)/%.o: %.c
	@mkdir -p '$(odir)'
	$(COMPILE.c) $(OUTPUT_OPTION) $(CPPFLAGS) $(CFLAGS) $<

rebuild: clean
	$(MAKE) build

depclean:
	rm -f $(odir)/*.d
clean:
	rm -f $(odir)/*.o $(target)
distclean: clean depclean

install:
	install -m 0755 $(target)

tags:
	ctags $(srcs) *.h

$(odir)/%.d: %.c
	@mkdir -p '$(odir)'
	$(CC) -MM -o $@ $< $(CPPFLAGS)

.PHONY: tags build rebuild depclean clean distclean install

ifneq (clean,$(findstring clean,$(MAKECMDGOALS)))
-include $(patsubst %.c,$(odir)/%.d,$(sources))
endif
