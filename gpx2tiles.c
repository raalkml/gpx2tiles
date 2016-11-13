#define _GNU_SOURCE
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
#include <gd.h>
#include "slist.h"
#include "gpx.h"
#include "tstime.h"
#include "slippy-map.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define nabs(a) ({int __a = (a); __a < 0 ? -__a : __a;})

static int zoom_min = 1, zoom_max = 18;

extern int verbose;
int verbose;

static int reinitialize; /* don't update the tiles, redraw them from scratch */

#define SHADOW (0xc0c0c0)
static int drop_shadows; /* draw diagnostic shadows */
#define HIGHLIGHT (0xff00ef)
static int highlight_tile_cross; /* use different color to highlight crossing a tile */

/* Do not draw lines at zoom levels below Z_NO_LINES */
#define Z_NO_LINES 7
static int z_max_tiles = INT_MAX;

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
		.x = (loc->lon - proj.w) * 256 /  (proj.e - proj.w),
		.y = (loc->lat - proj.n) * 256 / (proj.s - proj.n),
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
	slist_for_each(tile, &zoom_levels[zoom].tiles[hash_xy(xy)])
		if (tile->xy.x == xy->x && tile->xy.y == xy->y)
			break;
	return tile;
}

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
	
		if (!free_tiles.head) {
			tile = malloc(sizeof(*tile));
			tile->img = NULL;
		} else {
			tile = slist_stack_pop(&free_tiles);
			if (tile->img) {
				gdImageFilledRectangle(tile->img, 0, 0, 256, 256, 0);
				gdImageFilledRectangle(tile->img, 0, 0, 256, 256, transparent);
				zoom_levels[z].image_cnt++;
			}
		}
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
	slist_push(&free_tiles, tile);
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
		tile->img = gdImageCreateTrueColor(256, 256);
		gdImageColorTransparent(tile->img, transparent);
		gdImageFilledRectangle(tile->img, -1, -1, 256, 256, transparent);
		if (drop_shadows) {
			gdImageLine(tile->img, 0, 255, 255, 255, SHADOW);
			gdImageLine(tile->img, 255, 0, 255, 255, SHADOW);
		}
		zoom_levels[z].image_cnt++;
	}
	gdImageSetAntiAliased(tile->img, GD_ANTIALIAS_COLOR);
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
		if (verbosity)
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
	if (tile->refcnt-- < 0)
		fprintf(stderr, "Used closed tile %d,%d\n", tile->xy.x, tile->xy.y);

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

	zl->tile_cnt = 0;
	for (h = 0; h < ZOOM_TILE_HASH_SIZE; ++h)
		while (zl->tiles[h].head)
			free_tile(slist_stack_pop(&zl->tiles[h]));
}

static const int spdclr[] = {
	gdTrueColor(0x00, 0x00, 0x7f),
	gdTrueColor(0xcf, 0x00, 0x00), // darkred
	gdTrueColor(0xa4, 0x61, 0x00), // brown
	gdTrueColor(0xf4, 0xfb, 0x39), // yellow
	gdTrueColor(0x00, 0x7f, 0x00), // green
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
static void draw_track_points(struct gpx_point *points, int z)
{
	struct gpx_point *pt, *ppt;

	for (pt = ppt = points; pt; ppt = pt, pt = pt->next) {
		struct xy xy = get_tile_xy(&pt->loc, z);
		struct tile *tile = get_tile_at(&xy, z);

		if (!tile)
			continue;

		open_tile(tile, z);
		tile->point_cnt++;
		struct xy pix = getPixelPosForCoordinates(&pt->loc, z);
		struct xy ppix = pix;
		struct xy pxy = xy;
		struct tile *ptile;
		int speed = 0;

		if (ppt == pt)
			ptile = open_tile(tile, z);
		else {
			pxy = get_tile_xy(&ppt->loc, z);
			ptile = get_tile_at(&pxy, z);
			ppix = getPixelPosForCoordinates(&ppt->loc, z);
			open_tile(ptile, z);
		}

		if (pt->flags & GPX_PT_SPEED) {
			double kph = pt->speed * 3.6;
			if (kph <= 10.0)
				speed = 1;
			else if (kph <= 20.0)
				speed = 2;
			else if (kph <= 25.0)
				speed = 3;
			else if (kph > 25.0)
				speed = 4;
		}

		int color = spdclr[speed]; // gdAntiAliased produced really bad results on light maps

		gdImageSetPixel(tile->img, pix.x, pix.y, color);

		if (z < Z_NO_LINES)
			goto close_tiles;

		gdImageSetAntiAliased(tile->img, color);
		if (z >= 17 && (pt->flags & GPX_PT_PDOP) && pt->pdop > 1.8) {
			int d = (int)floor(pt->pdop * 3);

			gdImageEllipse(tile->img, pix.x, pix.y,
				       d, d, (20 << 24) | color);
		}
		else if (drop_shadows)
			gdImageEllipse(tile->img, pix.x, pix.y,
				       5, 5, (20 << 24) | SHADOW);
		if (tile == ptile) {
			if (ppix.x != pix.x || ppix.y != pix.y)
				gdImageLine(tile->img, pix.x, pix.y, ppix.x, ppix.y, color);
			goto close_tiles;
		}
		const int dx = tile->xy.x - ptile->xy.x;
		const int dy = tile->xy.y - ptile->xy.y;
		int x, y;
		for (x = ptile->xy.x; ; x += dx > 0 ? 1: -1) {
			for (y = ptile->xy.y; ; y += dy > 0 ? 1 : -1) {
				int x1 = ppix.x - 256 * (x - ptile->xy.x);
				int y1 = ppix.y - 256 * (y - ptile->xy.y);
				int x2 = pix.x - 256 * (x - tile->xy.x);
				int y2 = pix.y - 256 * (y - tile->xy.y);

				struct xy ixy = { .x = x, .y = y };
				struct tile *itile = get_tile_at(&ixy, z);
				open_tile(itile, z);
				/*
				printf("z %d %d,%d line (%d,%d, %d,%d)\n", z,
				       x, y, x1, y1, x2, y2);
				*/
				gdImageLine(itile->img, x1, y1, x2, y2,
					    highlight_tile_cross ? HIGHLIGHT : color);
				close_tile(itile, z);
				if (y == tile->xy.y)
					break;
			}
			if (x == tile->xy.x)
				break;
		}
	close_tiles:
		close_tile(ptile, z);
		close_tile(tile, z);
	}
}

static void make_tiles(struct gpx_file *files, int z)
{
	struct gpx_file *f;

	for (f = files; f; f = f->next) {
		struct gpx_segment *seg;

		slist_for_each(seg, &f->gpx->segments)
			draw_track_points(seg->points.head, z);
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
			if (!verbose)
				continue;
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
	if (verbose && len)
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
	const char *path;
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
		if (verbose)
			fprintf(stderr, "%ld: %s open\n", (long)pthread_self(), lq->path);
		lq->gf->gpx = gpx_read_file(lq->path);
		if (verbose)
			fprintf(stderr, "%ld: %s loaded\n", (long)pthread_self(), lq->path);
	}
}

int main(int argc, char *argv[])
{
	int cd_to = -1;
	struct timespec start, end, duration;
	struct gpx_file *gf;
	SLIST_DEFINE(struct gpx_file, files);
	int points_cnt, files_cnt = 0;
	int stdin_files = 0; /* read zero-terminated list of files from stdin */
	int parallel = 4;
	pthread_t *loaders;
	int opt;

	while ((opt = getopt(argc, argv, "0z:Z:C:j:vT:Id:")) != -1)
		switch (opt)  {
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
		case 'T':
			z_max_tiles = strtol(optarg, NULL, 0);
			if (z_max_tiles < 1)
				z_max_tiles = 1;
			break;
		case 'z':
			zoom_min = strtol(optarg, NULL, 0);
			break;
		case 'Z':
			zoom_max = strtol(optarg, NULL, 0);
			break;
		case 'd':
			opt = strtol(optarg, NULL, 0);
			if (opt & 0x01)
				drop_shadows = 1;
			if (opt & 0x02)
				highlight_tile_cross = 1;
			break;
		case 'v':
			++verbose;
			break;
		case 'j':
			parallel = strtol(optarg, NULL, 0);
			break;
		case '?':
			fprintf(stderr, "%s [-z] [--] [gpx files...]\n", argv[0]);
			exit(1);
		}
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct load *lq;
	for (; optind < argc; ++optind) {
		lq = malloc(sizeof(*lq));
		lq->next = NULL;
		lq->path = argv[optind];
		lq->gf = calloc(1, sizeof(*lq->gf));
		lq->gf->next = NULL;
		lq->gf->gpx = NULL;
		++files_cnt;
		slist_append(&files, lq->gf);
		slist_append(&load_q, lq);
	}
	if (parallel < 1)
		parallel = 1;
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
		ssize_t rd = sizeof(buf), off = 0;
		size_t len, pos;

		while ((rd = read(STDIN_FILENO, buf + off, rd)) > 0) {
			for (pos = 0;;) {
				len = strnlen(buf + pos, rd - pos);
				if (pos + len < off + rd) {
					pthread_mutex_lock(&load_q_lock);
					if (load_free.head)
						lq = slist_stack_pop(&load_free);
					else
						lq = malloc(sizeof(*lq));
					lq->path = buf + pos;
					lq->next = NULL;
					lq->gf = calloc(1, sizeof(*lq->gf));
					++files_cnt;
					slist_append(&files, lq->gf);
					slist_append(&load_q, lq);
					pthread_cond_broadcast(&load_q_cond);
					pthread_mutex_unlock(&load_q_lock);
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
	/* loader queue terminator */
	lq = calloc(1, sizeof(*lq));
	pthread_mutex_lock(&load_q_lock);
	slist_append(&load_q, lq);
	pthread_cond_broadcast(&load_q_cond);
	pthread_mutex_unlock(&load_q_lock);
	for (opt = 0; opt < parallel; ++opt)
		pthread_join(loaders[opt], NULL);
	clock_gettime(CLOCK_MONOTONIC, &end);
	while (load_free.head)
		free(slist_stack_pop(&load_free));
	free(slist_pop(&load_q));
	free(loaders);
	points_cnt = 0;
	for (gf = files.head; gf; gf = gf->next)
		points_cnt += gf->gpx->points_cnt;
	duration = timespec_sub(end, start);
	fprintf(stderr, "%d files, %d points, -j%d, %ld.%09ld sec\n",
		files_cnt, points_cnt, parallel,
	       duration.tv_sec, duration.tv_nsec);
	if (verbose > 2)
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
	for (z = zoom_min; z <= zoom_max; ++z) {
		printf("z %d ", z); fflush(stdout);
		make_tiles(files.head, z);
		printf("(%d tiles, dx %f dy %f)%s", zoom_levels[z].tile_cnt,
		       zoom_levels[z].xunit, zoom_levels[z].yunit,
		       verbose ? "\n" : "");
		fflush(stdout);
		if (verbose > 2)
			dump_zoom_level(z);
		save_zoom_level(z);
		free_zoom_level(z);
		if (!verbose)
			printf(" ... saved\n");
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
	return 0;
}

