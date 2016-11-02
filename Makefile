
sources := gpx2tiles.c gpx.c
odir := O
target = gpx2tiles
ofiles = $(patsubst %.c,$(odir)/%.o,$(sources))

CC := gcc
CFLAGS := -Wall -ggdb -O3
CPPFLAGS := $(shell pkg-config --cflags gdlib) $(shell pkg-config --cflags libxml-2.0)
LDFLAGS := -Wall -ggdb -O3
LDLIBS := $(shell pkg-config --libs gdlib) $(shell pkg-config --libs libxml-2.0) \
    -lm -lpthread

build: $(target)

$(target): $(ofiles)
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

$(odir)/%.o: %.c
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
	$(CC) -MM -o $@ $< $(CPPFLAGS)

.PHONY: tags build rebuild depclean clean distclean install

ifneq (clean,$(findstring clean,$(MAKECMDGOALS)))
-include $(patsubst %.c,$(odir)/%.d,$(sources))
endif
