#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <getopt.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <sys/wait.h>
#include <gd.h>
#include <gdfonts.h>
#include "slist.h"
#include "gpx.h"
#include "tstime.h"
#include "slippy-map.h"
#include "rgbhsv.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define nabs(a) ({int __a = (a); __a < 0 ? -__a : __a;})
#define min(a, b) ({ \
                  typeof(a) __a = (a); \
                  typeof(a) __b = (b); \
                  __a < __b ? __a : __b; \
                  })
#define max(a, b) ({ \
                  typeof(a) __a = (a); \
                  typeof(a) __b = (b); \
                  __a > __b ? __a : __b; \
                  })

#define ZOOM_MAX (19)

static int zoom_min = 1, zoom_max = 18;

extern int verbose;
int verbose;

static int reinitialize; /* don't update the tiles, redraw them from scratch */

#define SHADOW (0xc0c0c0)
static int drop_shadows; /* draw diagnostic shadows */
#define SPEED_CLR (0xc0c0c0)
static int draw_speed; /* draw speed in kph */
#define HIGHLIGHT (0xff00ef)
static int highlight_tile_cross; /* use different color to highlight crossing a tile */

#define HEATMAP_MODE ((int)(~0u >> 1))
/* Do not draw lines at zoom levels below z_no_lines */
static int z_no_lines = 7;
static int z_no_wpts = 16;
static int z_heatmap_bigdots = 15; /* draw big dots below this zoom in heatmap mode */
static int z_max_tiles = INT_MAX;
static int z_thickness[ZOOM_MAX + 1];

/* Do not draw lines if the speed between points is below that: */
static double no_lines_speed = 1.0; /* kph */

static int set_speed = INT_MIN;

#define TILE_W (256)
#define TILE_H (256)

static int fixclr = 0; /* used if set_speed == INT_MAX */

static const struct { int kph, clr; } spdclr[] = {
	{ 0, gdTrueColor(0x00, 0x00, 0x7f) },
	{10, gdTrueColor(0xcf, 0x00, 0x00) }, // darkred
	{20, gdTrueColor(0xa4, 0x61, 0x00) }, // brown
	{25, gdTrueColor(0xf4, 0xfb, 0x39) }, // yellow
	{40, gdTrueColor(0x00, 0x7f, 0x00) }, // green
	{50, gdTrueColor(0x00, 0xff, 0x00) }, // very green
	{55, gdTrueColor(0x4a, 0xf9, 0xff) }, // light cyan
	{60, gdTrueColor(0xf5, 0x80, 0xff) }, // light magenta
	/*
	 * Using only blue colors:
	 *
	 * gdTrueColor(0x00, 0x00, 0x33),
	 * gdTrueColor(0x06, 0x1a, 0x5b),
	 * gdTrueColor(0x10, 0x45, 0x9f),
	 * gdTrueColor(0x18, 0x69, 0xd8),
	 * gdTrueColor(0x1e, 0x83, 0xff),
	 *
	 */
};

static const int heatmapclr = gdTrueColor(0x06, 0x1a, 0x5b);

static int point_circle_color = gdTrueColor(0x06, 0x1a, 0x5b);
static int point_circle_diameter = 9;

struct xy
{
	int x, y;
};
static inline gdPoint xy2gdPoint(const struct xy xy)
{
	return (gdPoint){ .x = xy.x, .y = xy.y };
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

static inline double ProjectMercToLat(double merc_lat)
{
    return 180.0 / M_PI * atan(sinh(merc_lat));
}

struct projection
{
	double s, w, n, e;
};

static struct projection Project(struct xy xy, int z)
{
    double unit = 1.0 / pow(2.0, z);
    double relY1 = xy.y * unit;
    double relY2 = relY1 + unit;

// note: $LimitY = ProjectF(degrees(atan(sinh(pi)))) = log(sinh(pi)+cosh(pi)) = pi
// note: degrees(atan(sinh(pi))) = 85.051128..
//my $LimitY = ProjectF(85.0511);

    // so stay simple and more accurate
    double limitY = M_PI;
    double rangeY = 2 * limitY;
    relY1 = limitY - rangeY * relY1;
    relY2 = limitY - rangeY * relY2;
    double lat1 = ProjectMercToLat(relY1);
    double lat2 = ProjectMercToLat(relY2);
    unit = 360.0 / pow(2.0, z);
    double long1 = -180.0 + xy.x * unit;
    return (struct projection){ .s = lat2, .w = long1, .n = lat1, .e = long1 + unit };
}

// From gpx2png.pl:
// determine pixel position for a coordinate relative to the tileimage
// where it is drawn on
static struct xy getPixelPosForCoordinates(const struct gpx_latlon *loc,
					   int z)
{
	struct projection proj = Project(get_tile_xy(loc, z), z);

	return (struct xy){
		.x = (loc->lon - proj.w) * TILE_W /  (proj.e - proj.w),
		.y = (loc->lat - proj.n) * TILE_H / (proj.s - proj.n),
	};
}

struct gpx_file
{
	struct gpx_file *next;
	struct gpx_data *gpx;
};

struct tile {
	struct tile *next;
	int refcnt;
	struct xy xy;
	struct gpx_latlon loc;
	int point_cnt;
	unsigned has_speed:1;
	gdImage *img;
};

#define ZOOM_TILE_HASH_SIZE (256u)
struct zoom_level {
	SLIST_STACK_DECLARE(struct tile, tiles[ZOOM_TILE_HASH_SIZE]);
	double xunit, yunit;
	int tile_cnt, image_cnt;
};

static struct zoom_level *zoom_levels; /* goes from 0 to zoom_max */

static inline unsigned hash_xy(const struct xy *xy)
{
	return ((xy->y << 3) | (xy->x & 0x7)) % ZOOM_TILE_HASH_SIZE;
}

static struct tile *find_tile(const struct xy *xy, int zoom)
{
	struct tile *tile;

	if (zoom < zoom_min || zoom > zoom_max)
		return NULL;
	unsigned h = hash_xy(xy);
	struct tile **prev = &zoom_levels[zoom].tiles[h].head;
	slist_for_each(tile, &zoom_levels[zoom].tiles[h]) {
		if (tile->xy.x == xy->x && tile->xy.y == xy->y)
			break;
		prev = &tile->next;
	}
	if (tile) {
		*prev = tile->next;
		zoom_levels[zoom].tiles[h].head = tile;
	}
	return tile;
}

static pthread_mutex_t free_tiles_lock = PTHREAD_MUTEX_INITIALIZER;
static SLIST_STACK_DEFINE(struct tile, free_tiles);
#define GD_ANTIALIAS_COLOR (gdTrueColorAlpha(0, 255, 0, 0))

static char *get_tile_png_path(char *path, size_t maxlen, const struct xy *xy, int z)
{
	snprintf(path, maxlen, "%d/%d/%d.png", z, xy->x, xy->y);
	return path;
}

static struct tile *alloc_tile(const struct xy *xy, int z)
{
	struct tile *tile = NULL;

	if (zoom_min <= z && z <= zoom_max) {
		const int transparent = gdTrueColorAlpha(0, 0, 0, gdAlphaTransparent);
		unsigned h = hash_xy(xy);
	
		pthread_mutex_lock(&free_tiles_lock);
		if (!free_tiles.head) {
			tile = malloc(sizeof(*tile));
			tile->img = NULL;
		} else {
			tile = slist_stack_pop(&free_tiles);
			if (tile->img) {
				gdImageFilledRectangle(tile->img, 0, 0, TILE_W, TILE_H, 0);
				gdImageFilledRectangle(tile->img, 0, 0, TILE_W, TILE_H, transparent);
				zoom_levels[z].image_cnt++;
			}
		}
		pthread_mutex_unlock(&free_tiles_lock);
		tile->has_speed = 0;
		tile->xy = *xy;
		tile->loc.lat = tiley2lat(xy->y, z);
		tile->loc.lon = tilex2long(xy->x, z);
		tile->point_cnt = 0;
		tile->refcnt = 0;
		slist_push(&zoom_levels[z].tiles[h], tile);
		zoom_levels[z].tile_cnt++;
	}
	return tile;
}
static void free_tile(struct tile *tile)
{
	pthread_mutex_lock(&free_tiles_lock);
	slist_push(&free_tiles, tile);
	pthread_mutex_unlock(&free_tiles_lock);
}

static struct tile *get_tile_at(const struct xy *xy, int z)
{
	struct tile *tile = find_tile(xy, z);

	if (!tile)
		tile = alloc_tile(xy, z);
	return tile;
}

static struct tile *open_tile(struct tile *tile, int z)
{
	tile->refcnt++;
	if (tile->img)
		return tile;

	const int transparent = gdTrueColorAlpha(0, 0, 0, gdAlphaTransparent);
	FILE *fp;
	char path[128];

	get_tile_png_path(path, sizeof(path), &tile->xy, z);
	fp = fopen(path, "rb");
	if (fp) {
		tile->img = gdImageCreateFromPng(fp);
		if (tile->img) {
			gdImageColorTransparent(tile->img, transparent);
			zoom_levels[z].image_cnt++;
		}
		fclose(fp);
	}
	if (!tile->img) {
		tile->img = gdImageCreateTrueColor(TILE_W, TILE_H);
		gdImageColorTransparent(tile->img, transparent);
		gdImageFilledRectangle(tile->img, -1, -1, TILE_W, TILE_H, transparent);
		if (drop_shadows) {
			gdImageLine(tile->img, 0, TILE_H - 1, TILE_W - 1, TILE_H - 1, SHADOW);
			gdImageLine(tile->img, TILE_W - 1, 0, TILE_W - 1, TILE_H - 1, SHADOW);
		}
		zoom_levels[z].image_cnt++;
	}
	gdImageSetAntiAliased(tile->img, GD_ANTIALIAS_COLOR);
	gdImageSetThickness(tile->img, z <= ZOOM_MAX ? z_thickness[z] : 1);
	return tile;
}

static void flush_tile(struct tile *tile, int z, int verbosity)
{
	char path[PATH_MAX], *p;
	FILE *fp;

	get_tile_png_path(path, sizeof(path), &tile->xy, z);
	strcat(path, ".tmp");
	fp = fopen(path, "wb");
	if (!fp) {
		p = strchr(path, '/');
		*p = '\0';
		mkdir(path, 0775);
		*p = '/';
		p = strchr(p + 1, '/');
		*p = '\0';
		mkdir(path, 0775);
		*p = '/';
		fp = fopen(path, "wb");
	}
	if (!fp)
		perror(path);
	else {
		gdImagePngEx(tile->img, fp, 4);
		fclose(fp);
		gdImageDestroy(tile->img);
		tile->img = NULL;
		zoom_levels[z].image_cnt--;
		if (verbosity > 1)
			printf("z %d %d/%d (%d)\n", z,
			       tile->xy.x, tile->xy.y, tile->point_cnt);
	}
	p = strdup(path);
	path[strlen(path) - 4] = '\0';
	if (rename(p, path) < 0)
		perror(p);
	free(p);
}

static void close_tile(struct tile *tile, int z)
{
	if (tile->refcnt-- < 0) {
		fprintf(stderr, "Used closed tile %d,%d\n", tile->xy.x, tile->xy.y);
		abort();
	}

	unsigned int h;
	int need = zoom_levels[z].image_cnt - z_max_tiles;

	while (need > 0) {
		int flushed = 0;

		for (h = 0; h < ZOOM_TILE_HASH_SIZE; ++h) {
			struct tile *tile, *last = NULL;

			slist_for_each(tile, &zoom_levels[z].tiles[h])
				if (tile->img && tile->refcnt == 0)
					last = tile;
			if (last) {
				flush_tile(last, z, verbose);
				++flushed;
				if (--need <= 0)
					break;
			}
		}
		if (!flushed) {
			fprintf(stderr, "z %d: %d needed\n", z, need);
			break;
		}
	}
}

static void prepare_zoom_levels(void)
{
	int z;

	free(zoom_levels);
	zoom_levels = calloc(sizeof(*zoom_levels), zoom_max + 1);
	for (z = zoom_min; z <= zoom_max; ++z) {
		zoom_levels[z].xunit = 360.0 / pow(2.0, z);
		zoom_levels[z].yunit = 1.0 / pow(2.0, z);
	}
}
static void free_zoom_level(int z)
{
	struct zoom_level *zl = zoom_levels + z;
	unsigned int h;

	if (verbose > 1)
		printf("tile counts at zoom %d: %u\n", z, zoom_levels[z].tile_cnt);
	zl->tile_cnt = 0;
	for (h = 0; h < ZOOM_TILE_HASH_SIZE; ++h) {
		int hl = 0;
		while (zl->tiles[h].head) {
			free_tile(slist_stack_pop(&zl->tiles[h]));
			++hl;
		}
		if (verbose > 1)
			printf(" %d", hl);
	}
	if (verbose > 1)
		printf("\n");
}

#define XY(_x, _y) (struct xy){.x = _x, .y = _y}

/*
 * This seems to be a well known test for line segments intersection
 * http://stackoverflow.com/a/16725715
 */
static int turn(struct xy p1, struct xy p2, struct xy p3)
{
	int64_t a = p1.x;
	int64_t b = p1.y;
	int64_t c = p2.x;
	int64_t d = p2.y;
	int64_t e = p3.x;
	int64_t f = p3.y;
	int64_t A = (f - b) * (c - a);
	int64_t B = (d - b) * (e - a);
	return A > B ? 1 : A < B ? -1 : 0;
}

static int intersects(struct xy p1, struct xy p2, struct xy p3, struct xy p4)
{
	return turn(p1, p3, p4) != turn(p2, p3, p4) && turn(p1, p2, p3) != turn(p1, p2, p4);
}

static int crossing_tile(int x1, int y1, int x2, int y2)
{
	if (intersects(XY(x1, y1), XY(x2, y2), XY(0, 0), XY(TILE_W-1, 0)) ||
	    intersects(XY(x1, y1), XY(x2, y2), XY(0, 0), XY(0, TILE_H - 1)) ||
	    intersects(XY(x1, y1), XY(x2, y2), XY(TILE_W-1, 0), XY(TILE_W-1, TILE_H - 1)) ||
	    intersects(XY(x1, y1), XY(x2, y2), XY(0, TILE_H - 1), XY(TILE_W-1, TILE_H - 1)))
		return 1;
	return 0;
}

static int intensify(int c, double step)
{
	struct r_g_b rgb;
	struct h_s_v hsv;

	rgb.r = gdTrueColorGetRed(c);
	rgb.r /= 255.;
	rgb.g = gdTrueColorGetGreen(c);
	rgb.g /= 255.;
	rgb.b = gdTrueColorGetBlue(c);
	rgb.b /= 255.;
	hsv = rgb2hsv(rgb);
	hsv.v += step;
	if (hsv.v > 1.)
		hsv.v = 1.;
	rgb = hsv2rgb(hsv);
	return gdTrueColor((int)(rgb.r * 255.),
			   (int)(rgb.g * 255.),
			   (int)(rgb.b * 255.));
}

static int speed_kph_to_clridx(double kph)
{
	int speed;

	// TODO: for more speed levels switch to binary search
	for (speed = 0; speed < (int)countof(spdclr); ++speed)
		if ((int)kph <= spdclr[speed].kph)
			return speed;
	return countof(spdclr) - 1;
}

static void diag_draw_tile_speed(struct tile *tile, const struct gpx_point *pt,
				 const struct xy pix)
{
	char speed[8];
	int xx, yy;

	tile->has_speed = 1;
	snprintf(speed, sizeof(speed), "%.1f", pt->speed * 3.6);
	gdImageString (tile->img, gdFontSmall, 0, 0,
		       (unsigned char *)speed, SPEED_CLR);
	xx = gdFontSmall->w * strlen(speed);
	yy = gdFontSmall->h + 1;
	gdImageLine(tile->img, 0, yy, xx, yy, SPEED_CLR);
	gdImageLine(tile->img, xx, yy, pix.x, pix.y, SPEED_CLR);
}

static void diag_draw_point(int z, struct tile *tile,
			    const struct gpx_point *pt,
			    const struct xy pix,
			    int color)
{
	if (z >= 17 && (pt->flags & GPX_PT_PDOP) && pt->pdop > 1.8) {
		int d = (int)floor(pt->pdop * 3);

		gdImageEllipse(tile->img, pix.x, pix.y,
			       d, d, (20 << 24) | color);
	}
	else if (drop_shadows)
		gdImageEllipse(tile->img, pix.x, pix.y,
			       5, 5, (20 << 24) | SHADOW);
}

struct neigh_tile {
	struct xy xy, pix;
	struct xy lt, rb;
};

static void xy_out_of_range(struct xy *xy, int z)
{
	int max = (1 << z) - 1;
	if (xy->x < 0)
		xy->x = 0;
	if (xy->y < 0)
		xy->y = 0;
	if (xy->x > max)
		xy->x = max;
	if (xy->y > max)
		xy->y = max;
}

static struct neigh_tile neigh_tile_circle(int z, const struct tile *tile,
					   const struct xy pix,
					   int radius)
{
	struct neigh_tile n;
	int d;

	n.lt = n.rb = tile->xy;
	d = pix.x - radius;
	if (d < 0)
		n.lt.x += d / TILE_W - 1;
	d = pix.x + radius;
	if (d > TILE_W)
		n.rb.x += d / TILE_W;
	d = pix.y - radius;
	if (d < 0)
		n.lt.y += d / TILE_H - 1;
	d = pix.y + radius;
	if (d > TILE_W)
		n.rb.y += d / TILE_H;
	xy_out_of_range(&n.lt, z);
	xy_out_of_range(&n.rb, z);
	n.xy = n.lt;
	n.pix.x = pix.x + (radius + TILE_W) / TILE_W;
	n.pix.y = pix.y + (radius + TILE_H) / TILE_H;
	return n;
}

static int next_neigh_tile(struct neigh_tile *n)
{
	struct xy pix;
	if (n->xy.x < n->rb.x) {
		n->xy.x++;
		pix.x = -TILE_W;
		pix.y = 0;
	} else {
		if (n->xy.y == n->rb.y)
			return 0;
		n->xy.x = n->lt.x;
		n->xy.y++;
		pix.x = TILE_W * (n->rb.x - n->lt.x);
		pix.y = -TILE_H;
	}
	n->pix.x += pix.x;
	n->pix.y += pix.y;
	return 1;
}

static void draw_point_circle(int z, struct tile *tile,
			      const struct gpx_point *pt,
			      const struct xy pix,
			      int color)
{
	struct neigh_tile n = neigh_tile_circle(z, tile, pix, point_circle_diameter);

	do {
		tile = get_tile_at(&n.xy, z);
		if (!tile)
			continue;
		open_tile(tile, z);
		gdImageFilledEllipse(tile->img, n.pix.x, n.pix.y,
				     point_circle_diameter,
				     point_circle_diameter,
				     point_circle_color);
		close_tile(tile, z);
	} while (next_neigh_tile(&n));
}

#define DRAW_TRKPTR_NO_LINES (1u)
#define DRAW_TRKPTR_BADSRC (2u)
#define DRAW_TRKPTR_CIRCLE (4u)
static void draw_track_points(struct gpx_point *points, int z, unsigned flags)
{
	struct gpx_point *pt, *ppt;
	struct xy ppix = { 0 }, pxy;

	for (pt = ppt = points; pt; pt = pt->next) {
		int color;
		struct tile *ptile;
		struct xy pix = {0}; // ???
		struct xy xy = get_tile_xy(&pt->loc, z);
		struct tile *tile = get_tile_at(&xy, z);

		if (!tile)
			goto setppt;
		open_tile(tile, z);
		tile->point_cnt++;
		pix = getPixelPosForCoordinates(&pt->loc, z);
		if (ppt == pt) {
			ptile = open_tile(tile, z);
			ppix = pix;
			pxy = xy;
		} else {
			ptile = get_tile_at(&pxy, z);
			open_tile(ptile, z);
		}
		if (z_no_lines == HEATMAP_MODE) {
			color = gdImageGetTrueColorPixel(tile->img, pix.x, pix.y);
			color = color ? intensify(color, 0.05) : heatmapclr;
			if (z < z_heatmap_bigdots)
				gdImageSetPixel(tile->img, pix.x, pix.y, color);
			else
				gdImageFilledRectangle(tile->img,
						       pix.x - 1, pix.y - 1,
						       pix.x + 1, pix.y + 1,
						       color);
		} else {
			int speed = 0;

			if (!(flags & DRAW_TRKPTR_BADSRC) && (pt->flags & GPX_PT_SPEED))
				speed = speed_kph_to_clridx(pt->speed * 3.6);
			switch (set_speed) {
			case INT_MIN:
				color = spdclr[speed].clr;
				break;
			case INT_MAX:
				color = fixclr;
				break;
			default:
				speed = speed_kph_to_clridx((double)set_speed);
				color = spdclr[speed].clr;
				break;
			}
			gdImageSetPixel(tile->img, pix.x, pix.y, color);
		}

		gdImageSetAntiAliased(tile->img, color);
		if (flags & DRAW_TRKPTR_CIRCLE)
			draw_point_circle(z, tile, pt, pix, color);
		diag_draw_point(z, tile, pt, pix, color);
		if (draw_speed && !tile->has_speed)
			diag_draw_tile_speed(tile, pt, pix);
		if (flags & DRAW_TRKPTR_NO_LINES)
			goto close_tiles;
		/* Don't draw slow segments */
		if ((pt->flags & GPX_PT_SPEED) &&
		    pt->speed * 3.6 < no_lines_speed)
			goto close_tiles;
		if (tile == ptile) {
			if (ppix.x != pix.x || ppix.y != pix.y)
				gdImageLine(tile->img, pix.x, pix.y,
					    ppix.x, ppix.y, color);
			goto close_tiles;
		}
		const int dx = tile->xy.x - ptile->xy.x;
		const int dy = tile->xy.y - ptile->xy.y;
		int x, y;
		for (x = ptile->xy.x; ; x += dx > 0 ? 1: -1) {
			for (y = ptile->xy.y; ; y += dy > 0 ? 1 : -1) {
				int x1 = ppix.x - TILE_W * (x - ptile->xy.x);
				int y1 = ppix.y - TILE_H * (y - ptile->xy.y);
				int x2 = pix.x - TILE_W * (x - tile->xy.x);
				int y2 = pix.y - TILE_H * (y - tile->xy.y);

				if (crossing_tile(x1, y1, x2, y2)) {
					struct xy ixy = { .x = x, .y = y };
					struct tile *itile = get_tile_at(&ixy, z);
					open_tile(itile, z);
					/*
					printf("z %d %f,%f %d,%d line (%d,%d, %d,%d)\n", z,
					       pt->loc.lat, pt->loc.lon,
					       x, y, x1, y1, x2, y2);
					*/
					gdImageLine(itile->img, x1, y1, x2, y2,
						    highlight_tile_cross ? HIGHLIGHT : color);
					close_tile(itile, z);
				}
				if (y == tile->xy.y)
					break;
			}
			if (x == tile->xy.x)
				break;
		}
	close_tiles:
		close_tile(ptile, z);
		close_tile(tile, z);
	setppt:
		ppix = pix;
		pxy = xy;
		ppt = pt;
	}
}

static void make_tiles(struct gpx_file *files, int z)
{
	struct gpx_file *f;

	for (f = files; f; f = f->next) {
		struct gpx_segment *seg;

		slist_for_each(seg, &f->gpx->segments) {
			/*
			 * Th GPS data source GPX_SRC_NETWORK is not reliable
			 * and precise enough to use it for speed analysis.
			 *
			 * Don't draw lines at high zoom levels, because they
			 * all are on the same pixel.
			 */
			draw_track_points(seg->points.head, z,
					  (z < z_no_lines ? DRAW_TRKPTR_NO_LINES : 0) |
					  (seg->src == GPX_SRC_NETWORK ? DRAW_TRKPTR_BADSRC : 0));
		}
		if (z > z_no_wpts)
			draw_track_points(f->gpx->wpts.head, z,
					  DRAW_TRKPTR_NO_LINES | DRAW_TRKPTR_CIRCLE);
		if (verbose > 0)
			printf("z %2d %s (%d points, tiles %d)\n", z, f->gpx->path,
			       f->gpx->points_cnt,
			       zoom_levels[z].tile_cnt);
	}
}

static inline void save_zoom_level(int z)
{
	int len = 0, h;

	for (h = 0; h < ZOOM_TILE_HASH_SIZE; ++h) {
		struct tile *tile;

		slist_for_each(tile, &zoom_levels[z].tiles[h]) {
			if (tile->img)
				flush_tile(tile, z, 0);
			if (verbose > 1) {
				if (!len)
					len += printf("z %d", z);
				len += printf(" %d/%d (%d)",
					      tile->xy.x, tile->xy.y,
					      tile->point_cnt);
				if (len >= 60) {
					fputc('\n', stdout);
					len = 0;
				}
			}
		}
	}
	if (verbose > 1 && len)
		fputc('\n', stdout);
}

static void remove_tiles(int z)
{
	DIR *zdir;
	struct dirent *ze;
	char d[16];

	snprintf(d, sizeof(d), "%d", z);
	zdir = opendir(d);
	if (!zdir)
		return;
	while ((ze = readdir(zdir))) {
		if (ze->d_name[0] == '.')
			continue;
		int xdirfd = openat(dirfd(zdir), ze->d_name, O_DIRECTORY|O_RDONLY);
		if (xdirfd != -1) {
			DIR *xdir = fdopendir(xdirfd);
			if (!xdir)
				close(xdirfd);
			else {
				struct dirent *xe;
				while ((xe = readdir(xdir))) {
					if (xe->d_name[0] == '.')
						continue;
					unlinkat(dirfd(xdir), xe->d_name, 0);
				}
				closedir(xdir);
			}
		}
		unlinkat(dirfd(zdir), ze->d_name, AT_REMOVEDIR);
	}
	closedir(zdir);
}

#include "dump.h"

struct load
{
	struct load *next;
	char *path;
	size_t path_size;
	struct gpx_file *gf;
};

static pthread_mutex_t load_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t load_q_cond = PTHREAD_COND_INITIALIZER;
static SLIST_DEFINE(struct load, load_q);
static SLIST_STACK_DEFINE(struct load, load_free);

static void *loader(void *arg)
{
	struct load *lq = NULL;

	while (1) {
		pthread_mutex_lock(&load_q_lock);
		if (lq) {
			slist_push(&load_free, lq);
			lq = NULL;
		}
		if (slist_empty(&load_q))
			pthread_cond_wait(&load_q_cond, &load_q_lock);
		if (load_q.head) {
			if (!load_q.head->gf) {
				pthread_mutex_unlock(&load_q_lock);
				pthread_exit(NULL);
			}
			lq = slist_pop(&load_q);
		}
		pthread_mutex_unlock(&load_q_lock);
		if (!lq)
			continue;
		if (verbose > 0)
			fprintf(stderr, "%ld: %s open\n", (long)pthread_self(), lq->path);
		lq->gf->gpx = gpx_read_file(lq->path);
		if (verbose > 0)
			fprintf(stderr, "%ld: %s loaded\n", (long)pthread_self(), lq->path);
	}
}

struct tile_proc
{
	pthread_t thr;
	struct gpx_file *files;
	int start, end;
	const int *order;
};

static void *tile_processor(void *arg)
{
	const struct tile_proc *tp = arg;
	int i;

	for (i = tp->start; i < tp->end; ++i) {
		int tile_cnt, z = tp->order[i];

		make_tiles(tp->files, z);
		tile_cnt = zoom_levels[z].tile_cnt;
		save_zoom_level(z);
		free_zoom_level(z);
		printf("z %2d (%d tiles)\n", z, tile_cnt);
		fflush(stdout);
	}
	return NULL;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"%s [-z <min-zoom>] [-Z <max-zoom>] [-C <output-dir>] "
		"[-j <jobs>] [-T <max-tiles>] [-Ivh] [-L <line-zoom>] "
		"( [--] [gpx files...] | -0 < file-list )\n"
		"  -C <output-dir> directory to save the tiles to\n"
		"  -I delete zoom directories before saving the tiles\n"
		"  -T <max-tiles> max number of tiles to keep in memory\n"
		"  -j <jobs> number of tracks to load in parallel\n"
		"  -L <line-zoom> zoom level above which stop drawing lines (only dots) (default %d)\n"
		"  -P <line-zoom> zoom level above which stop drawing waypoints (default %d)\n"
		"  -H heatmap mode\n"
		"  -0 read the list of GPX files from stdin, NUL-terminated\n"
		"     The files given on command-line are processed first\n"
		"  -z <min-zoom>/-Z <max-zoom> only generate tiles from <min-zoom> to <max-zoom>\n"
		"     (by default the tiles are generated from 1 to 18)\n"
		"  -v increase verbosity\n"
		"  -d <mask> if bit0 set in <mask>, the generated tiles and track points are drawn\n"
		"     with diagnostics information around them (lines)\n"
		"     bit1 - highlights tile crossings\n"
		"     bit2 - draws the speed in tile\n"
		"  -t <zoom>:<line-thickness>[+] set thickness for lines at the level\n"
		"     extends till the last zoom level if ends with a \"+\", i.e. -t 12:3+\n"
		"  -c <hex-color> draw all lines with the specified color\n"
		"  -S <kph> assume the speed to be always <kph>\n"
		"  -p <diameter> diameter (in px) for <wpt> circles\n"
		"  -h gives this message\n",
		argv0,
		z_no_lines,
		z_no_wpts);
}

int main(int argc, char *argv[])
{
	int cd_to = -1;
	struct timespec start, end, duration;
	struct gpx_file *gf;
	SLIST_DEFINE(struct gpx_file, files);
	int points_cnt, files_cnt = 0;
	int stdin_files = 0; /* read zero-terminated list of files from stdin */
	size_t parallel = 4;
	pthread_t *loaders;
	int opt;

	while ((opt = getopt(argc, argv, "0z:Z:C:j:vT:Id:L:Hht:S:p:P:c:")) != -1)
		switch (opt)  {
			char *p;
			int z;
		case '0':
			stdin_files = 1;
			break;
		case 'C':
			cd_to = open(optarg, O_DIRECTORY | O_PATH | O_CLOEXEC);
			if (cd_to < 0) {
				perror(optarg);
				exit(2);
			}
			break;
		case 'I':
			reinitialize = 1;
			break;
		case 'S':
			set_speed = strtol(optarg, NULL, 0);
			break;
		case 't':
			z = strtol(optarg, &p, 0);
			if (z < 0 || z > ZOOM_MAX) {
				fprintf(stderr, "Invalid zoom level %s\n",
					optarg);
				exit(1);
			}
			while (*p && !isdigit(*p))
				++p;
			z_thickness[z] = strtol(p, &p, 0);
			if (!z_thickness[z])
				z_thickness[z] = 1;
			if ('+' == *p) {
				int t = z_thickness[z];
				for ( ++z; z <= ZOOM_MAX; ++z)
					z_thickness[z] = t;
			}
			break;
		case 'c':
			fixclr = strtol(optarg, &p, 16);
			fixclr = gdTrueColor((fixclr >> 16) & 0xff,
					     (fixclr >> 8) & 0xff,
					     fixclr & 0xff);
			set_speed = INT_MAX;
			break;
		case 'T':
			z_max_tiles = strtol(optarg, NULL, 0);
			if (z_max_tiles < 1)
				z_max_tiles = 1;
			break;
		case 'L':
			z_no_lines = strtol(optarg, NULL, 0);
			break;
		case 'P':
			z_no_wpts = strtol(optarg, NULL, 0);
			break;
		case 'H':
			z_no_lines = HEATMAP_MODE;
			break;
		case 'z':
			zoom_min = strtol(optarg, NULL, 0);
			break;
		case 'Z':
			zoom_max = strtol(optarg, NULL, 0);
			if (zoom_max > ZOOM_MAX) {
				fprintf(stderr, "Zoom level %d is more than maximum %d\n",
					zoom_max, ZOOM_MAX);
				exit(1);
			}
			break;
		case 'd':
			opt = strtol(optarg, NULL, 0);
			if (opt & 0x01)
				drop_shadows = 1;
			if (opt & 0x02)
				highlight_tile_cross = 1;
			if (opt & 0x04)
				draw_speed = 1;
			break;
		case 'p':
			point_circle_diameter = strtol(optarg, NULL, 0);
			break;
		case 'v':
			++verbose;
			break;
		case 'j':
			parallel = strtol(optarg, NULL, 0);
			break;
		case '?':
		case 'h':
			usage(argv[0]);
			exit(1);
		}
	if (argc == 1) {
		usage(argv[0]);
		exit(1);
	}
	/*
	 * Extend zoom_max if zoom_min is bigger that it, so that it is
	 * possible to easily generate zoom level 19 with jist one -z19
	 */
	if (zoom_max < zoom_min)
		zoom_max = zoom_min;
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct load *lq;
	for (; optind < argc; ++optind) {
		lq = malloc(sizeof(*lq));
		lq->next = NULL;
		lq->path_size = strlen(argv[optind]) + 1;
		lq->path = malloc(lq->path_size);
		memcpy(lq->path, argv[optind], lq->path_size);
		lq->gf = calloc(1, sizeof(*lq->gf));
		lq->gf->next = NULL;
		lq->gf->gpx = NULL;
		++files_cnt;
		slist_append(&files, lq->gf);
		slist_append(&load_q, lq);
	}
	if (parallel < 1)
		parallel = 1;
	if (parallel >= SIZE_MAX / 2 - 1)
		parallel = SIZE_MAX / 2 - 1;
	loaders = malloc(parallel * sizeof(*loaders));
	for (opt = 0; opt < parallel; ++opt) {
		int err = pthread_create(loaders + opt, NULL, loader, NULL);
		if (err) {
			fprintf(stderr, "pthread_create: %s (%d)\n", strerror(err), err);
			break;
		}
	}
	if (stdin_files) {
		char buf[BUFSIZ];
		ssize_t off = 0;

		while (!feof(stdin)) {
			int c = fgetc(stdin);
			if (off == sizeof(buf)) {
				fprintf(stderr, "%s: file name too long:\n%.*s\n",
					argv[0], (int)sizeof(buf), buf);
				exit(1);
			}
			buf[off++] = c;
			if (c == EOF || c == '\0') {
				if (off == 1) {
					if (c == '\0')
						fprintf(stderr, "%s: empty file name\n",
							argv[0]);
					continue;
				}
				if (verbose > 0)
					fprintf(stderr, "\t% 4d %.*s\n",
						files_cnt, (int)off - 1, buf);
				pthread_mutex_lock(&load_q_lock);
				if (load_free.head) {
					lq = slist_stack_pop(&load_free);
					if (lq->path_size < off) {
						lq->path = realloc(lq->path, off);
						lq->path_size = off;
					}
				} else {
					lq = malloc(sizeof(*lq));
					lq->path = malloc(off);
					lq->path_size = off;
				}
				memcpy(lq->path, buf, off - 1);
				lq->path[off - 1] = '\0';
				off = 0;
				lq->next = NULL;
				lq->gf = calloc(1, sizeof(*lq->gf));
				slist_append(&files, lq->gf);
				slist_append(&load_q, lq);
				++files_cnt;
				pthread_cond_broadcast(&load_q_cond);
				pthread_mutex_unlock(&load_q_lock);
				/*
				 * Wait for load queue to be processed to
				 * avoid taking too much memory by the file
				 * names.
				 */
				if (files_cnt && (files_cnt % 100) == 0) {
					while (1) {
						int done;
						pthread_mutex_lock(&load_q_lock);
						done = slist_empty(&load_q);
						pthread_mutex_unlock(&load_q_lock);
						if (done)
							break;
						usleep(100);
					}
				}
			}
		}
	}
	/* loader queue terminator */
	lq = calloc(1, sizeof(*lq));
	pthread_mutex_lock(&load_q_lock);
	slist_append(&load_q, lq);
	pthread_cond_broadcast(&load_q_cond);
	pthread_mutex_unlock(&load_q_lock);
	for (opt = 0; opt < parallel; ++opt)
		pthread_join(loaders[opt], NULL);
	clock_gettime(CLOCK_MONOTONIC, &end);
	while (load_free.head) {
		lq = slist_stack_pop(&load_free);
		free(lq->path);
		free(lq);
	}
	free(slist_pop(&load_q));
	free(loaders);
	points_cnt = 0;
	for (gf = files.head; gf; gf = gf->next)
		points_cnt += gf->gpx->points_cnt;
	duration = timespec_sub(end, start);
	fprintf(stderr, "%d files, %d points, -j%zu, %ld.%09ld sec\n",
		files_cnt, points_cnt, parallel,
	       duration.tv_sec, duration.tv_nsec);
	if (verbose > 3)
		dump_points(files.head);

	if (cd_to != -1 && fchdir(cd_to) == -1) {
		perror("chdir");
		exit(2);
	}

	int z;

	if (reinitialize) {
		fprintf(stderr, "Reinitializing zoom %d - %d\n",
			zoom_min, zoom_max);
		for (z = zoom_min; z <= zoom_max; ++z)
			remove_tiles(z);
	}
	if (!points_cnt)
		exit(0);

	prepare_zoom_levels();
	clock_gettime(CLOCK_MONOTONIC, &start);
	if (parallel == 1) {
		for (z = zoom_min; z <= zoom_max; ++z) {
			printf("z %d ", z); fflush(stdout);
			make_tiles(files.head, z);
			printf("(%d tiles, dx %f dy %f)%s",
			       zoom_levels[z].tile_cnt,
			       zoom_levels[z].xunit, zoom_levels[z].yunit,
			       verbose > 1 ? "\n" : "");
			fflush(stdout);
			if (verbose > 3)
				dump_zoom_level(z);
			save_zoom_level(z);
			free_zoom_level(z);
			if (!verbose)
				printf(" ... saved\n");
		}
	} else {
		int order[ZOOM_MAX + 1];
		int zooms = zoom_max - zoom_min + 1;
		struct tile_proc *tp, *tproc;
		int a = zoom_min, b = zoom_max;

		for (z = zoom_min; z <= zoom_max; ++z)
			order[z] = z & 1 ? b-- : a++;
		parallel = min(parallel, zooms);
		tproc = calloc(parallel, sizeof(*tproc));
		for (tp = tproc, z = zoom_min; z <= zoom_max; ++tp) {
			int err;

			tp->order = order;
			tp->start = z;
			tp->end = z + zooms / parallel;
			if (tp - tproc == parallel - 1)
				tp->end = zoom_max + 1;
			tp->files = files.head;
			//printf("%d z %d(%d)\n", (int)(tp - tproc),
			//       tp->start, tp->end - tp->start);
			err = pthread_create(&tp->thr, NULL, tile_processor, tp);
			if (err) {
				fprintf(stderr, "z %d: pthread_create: %s (%d)\n",
					z, strerror(err), err);
				break;
			}
			z = tp->end;
		}
		while (tp-- > tproc) {
			int err = pthread_join(tp->thr, NULL);
			if (err) {
				fprintf(stderr, "pthread_join: %s (%d)\n",
					strerror(err), err);
			}
		}
		free(tproc);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);
	duration = timespec_sub(end, start);
	fprintf(stderr, "z %d-%d processed in %ld.%09ld\n",
		zoom_min, zoom_max, duration.tv_sec, duration.tv_nsec);
	while (files.head) {
		gf = slist_pop(&files);
		gpx_free(gf->gpx);
		free(gf);
	}
	free(zoom_levels);
	while (free_tiles.head) {
		struct tile *t = slist_stack_pop(&free_tiles);

		if (t->img)
			gdImageDestroy(t->img);
		free(t);
	}
	gpx_libxml_cleanup();
	return 0;
}

