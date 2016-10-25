#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <gd.h>
#include "gpx.h"
#include "tstime.h"
#include "slippy-map.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define ASCII (char *)

struct xy
{
	int x, y;
};
static inline gdPoint xy2gdPoint(const struct xy xy)
{
	return (gdPoint){ .x = xy.x, .y = xy.y };
}

static inline void draw()
{
	//int x, y, dx, dy, i;
	FILE *fp;
	gdImage *img;
	int transparent = gdTrueColorAlpha(0, 0, 0, gdAlphaTransparent);
	
	img = gdImageCreateTrueColor(256, 256);
	gdImageColorTransparent(img, transparent);
	gdImageFilledRectangle(img, 0, 0, 256, 256, transparent);

	fp = fopen("tiles/empty.png", "wb");
	gdImagePngEx(img, fp, 4);
	fclose(fp);

	gdPoint poly[] = {
		{-100, -100},
		{100, 100},
		{1000, -100},
	};
	gdImageSetAntiAliased(img, gdTrueColorAlpha(0, 255, 0, 0));
	gdImageOpenPolygon(img, poly, countof(poly), gdAntiAliased);

	fp = fopen("tiles/filled.png", "wb");
	gdImagePngEx(img, fp, 4);
	fclose(fp);
	gdImageDestroy(img);
}

static inline struct xy get_tile_xy(const struct gpx_latlon *loc, int zoom)
{
	struct xy tile;

	tile.x = long2tilex(loc->lon, zoom);
	tile.y = lat2tiley(loc->lat, zoom);

	// gpx2png.pl uses this:
	// map latitude/longitude and zoom level to tile number (x/y)
	// see also: https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
	// tile.x = int( ( loc->lon + 180. ) / 360. * ( 1 << zoom ) );
	// double l  = log( tan( loc->lat * M_PI / 180 ) + sec( loc->lat * M_PI / 180 ) );
	// tile.y = int( ( 1 - l / M_PI ) / 2 * ( 1 << zoom ) );
	return tile;
}

#if NO

struct projection {
	double s, w, n, e;
};

static struct projection Project(const struct gpx_latlon *loc, int zoom)
{
    my ( $X, $Y, $Zoom ) = @_;
    my $Unit  = 1 / ( 2**$Zoom );
    my $relY1 = $Y * $Unit;
    my $relY2 = $relY1 + $Unit;

// note: $LimitY = ProjectF(degrees(atan(sinh(pi)))) = log(sinh(pi)+cosh(pi)) = pi
// note: degrees(atan(sinh(pi))) = 85.051128..
//my $LimitY = ProjectF(85.0511);

    # so stay simple and more accurate
    my $LimitY = pi;
    my $RangeY = 2 * $LimitY;
    $relY1 = $LimitY - $RangeY * $relY1;
    $relY2 = $LimitY - $RangeY * $relY2;
    my $Lat1 = ProjectMercToLat($relY1);
    my $Lat2 = ProjectMercToLat($relY2);
    $Unit = 360 / ( 2**$Zoom );
    my $Long1 = -180 + $X * $Unit;
    return ( ( $Lat2, $Long1, $Lat1, $Long1 + $Unit ) );    # S,W,N,E
}
// From gpx2png.pl:
// determine pixel position for a coordinate relative to the tileimage
// where it is drawn on
static struct xy getPixelPosForCoordinates(const struct gpx_latlon *loc, int zoom)
{
	struct xy tile = getTileNumber(loc, zoom);

	int xoffset = ( tile.x - $minxtile ) * 256;
	int yoffset = ( tile.y - $minytile ) * 256;

	my ( $south, $west, $north, $east ) = Project( $xtile, $ytile, $zoom );

	my $x = int( ( $lon - $west ) * 256 /  ( $east - $west ) + $xoffset );
	my $y = int( ( $lat - $north ) * 256 / ( $south - $north ) + $yoffset );

	if ( $x > $maxx ) { $maxx = $x; }
	if ( $x < $minx ) { $minx = $x; }
	if ( $y > $maxy ) { $maxy = $y; }
	if ( $y < $miny ) { $miny = $y; }

	return ( $x, $y );
}
#endif

struct gpx_file
{
	struct gpx_file *next;
	struct gpx_data *gpx;
};

static int zoom_min = 1, zoom_max = 29;

struct tile {
	struct tile *next;
	struct xy xy;
	int points;
};

#define ZOOM_TILE_HASH_SIZE (256)
struct zoom_level {
	struct tile *tiles[ZOOM_TILE_HASH_SIZE];
	int tile_cnt;
};

static struct zoom_level *zoom_levels; /* goes from 0 to zoom_max */

static inline unsigned hash_xy(const struct xy *xy)
{
	return ((xy->y << 3) | (xy->x & 0x7)) % ZOOM_TILE_HASH_SIZE;
}

static struct tile *find_tile(const struct xy *xy, int zoom)
{
	struct tile *tile = zoom_min <= zoom && zoom <= zoom_max ?
		zoom_levels[zoom].tiles[hash_xy(xy)] : NULL;

	for (; tile; tile = tile->next)
		if (tile->xy.x == xy->x && tile->xy.y == xy->y)
			break;
	return tile;
}

static struct tile *create_tile(const struct xy *xy, int zoom)
{
	struct tile *tile = NULL;

	if (zoom_min <= zoom && zoom <= zoom_max) {
		unsigned h = hash_xy(xy);
		tile = malloc(sizeof(*tile));
		tile->xy = *xy;
		tile->points = 0;
		tile->next = zoom_levels[zoom].tiles[h];
		zoom_levels[zoom].tiles[h] = tile;
		zoom_levels[zoom].tile_cnt++;
	}
	return tile;
}

static struct tile *get_tile(const struct xy *xy, int z)
{
	struct tile *tile = find_tile(xy, z);

	if (!tile)
		tile = create_tile(xy, z);
	return tile;
}

static void prepare_zoom_levels()
{
	free(zoom_levels);
	zoom_levels = calloc(sizeof(*zoom_levels), zoom_max + 1);
}

static void find_tiles(struct gpx_file *f, int z)
{
	int s = -1, w = -1, n = -1, e = -1;

	for (; f; f = f->next) {
		struct gpx_point *pt, *ppt;
		for (pt = ppt = f->gpx->points; pt; ppt = pt, pt = pt->next) {
			struct xy xy = get_tile_xy(&pt->loc, z);
			struct tile *tile = get_tile(&xy, z);

			if (!tile)
				continue;
			tile->points++;

			if (xy.x < w || w == -1)
				w = xy.x;
			if (xy.x > e || e == -1)
				e = xy.x;
			if (xy.y < n || n == -1)
				n = xy.y;
			if (xy.y > s || s == -1)
				s = xy.y;
		}
	}
	printf("Bounds: north %d east %d south %d west %d (%dx%d)\n",
	       n, e, s, w, e - w + 1, s - n + 1);
}

#include "dump.h"

int main(int argc, char *argv[])
{
	struct timespec start, end, duration;
	struct gpx_file *gf;
	struct gpx_file *files = NULL, **files_tail = &files;
	int points_cnt = 0, files_cnt = 0;
	int stdin_files = 0; /* read zero-terminated list of files from stdin */
	int opt;

	while ((opt = getopt(argc, argv, "z")) != -1)
		switch (opt)  {
		case 'z':
			stdin_files = 1;
			break;
		case '?':
			fprintf(stderr, "%s [-z] [--] [gpx files...]\n", argv[0]);
			exit(1);
		}
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (; optind < argc; ++optind) {
		gf = malloc(sizeof(*gf));
		gf->next = NULL;
		gf->gpx = gpx_read_file(argv[optind]);
		++files_cnt;
		*files_tail = gf;
		files_tail = &gf->next;
		points_cnt += gf->gpx->points_cnt;
	}
	if (stdin_files) {
		char buf[BUFSIZ];
		ssize_t rd = sizeof(buf), off = 0;
		size_t len, pos;

		while ((rd = read(STDIN_FILENO, buf + off, rd)) > 0) {
			for (pos = 0;;) {
				len = strnlen(buf + pos, sizeof(buf) - pos);
				if (pos + len < off + rd) {
					gf = malloc(sizeof(*gf));
					gf->next = NULL;
					gf->gpx = gpx_read_file(buf + pos);
					++files_cnt;
					*files_tail = gf;
					files_tail = &gf->next;
					points_cnt += gf->gpx->points_cnt;
					pos += len + 1;
				} else {
					if (len == rd) {
						fprintf(stderr, "%s: NUL terminator expected\n",
							argv[0]);
						exit(1);
					}
					len = rd + off - pos;
					if (len)
						memmove(buf, buf + pos, len);
					off = len;
					rd = sizeof(buf) - 1 - off;
					break;
				}
			}
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	duration = timespec_sub(end, start);
	fprintf(stderr, "%d files, %d points, %ld.%09ld\n", files_cnt, points_cnt,
	       duration.tv_sec, duration.tv_nsec);
	// dump_points(files);

	int z;

	prepare_zoom_levels();
	clock_gettime(CLOCK_MONOTONIC, &start);
	for (z = zoom_min; z <= zoom_max; ++z) {
		printf("z %d ", z); fflush(stdout);
		find_tiles(files, z);
		printf("(%d tiles)\n", zoom_levels[z].tile_cnt);
		// dump_zoom_level(z);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	duration = timespec_sub(end, start);
	fprintf(stderr, "processed in %ld.%09ld\n", duration.tv_sec, duration.tv_nsec);

	for (gf = files; gf;) {
		struct gpx_file *next = gf->next;

		gpx_free(gf->gpx);
		free(gf);
		gf = next;
	}
	return 0;
}

