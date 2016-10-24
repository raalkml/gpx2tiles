
sources := gpx2tiles.c

CFLAGS := -Wall
CPPFLAGS := $(shell pkg-config --cflags gdlib) $(shell pkg-config --cflags libxml-2.0)
LDFLAGS :=
LDLIBS := $(shell pkg-config --libs gdlib) $(shell pkg-config --libs libxml-2.0)

gpx2tiles: $(patsubst %.c,%.o,$(sources))
	$(LINK.o) $^ $(LOADLIBES) $(LDLIBS) -o $@

#%.o: %.c
#	$(COMPILE.c) $(OUTPUT_OPTION) $(CPPFLAGS) $(CFLAGS) $<
