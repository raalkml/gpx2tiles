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
#include <getopt.h>
#include <pthread.h>
#include <gd.h>
#include "slist.h"
#include "gpx.h"
#include "tstime.h"
#include "slippy-map.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define intmod(a) ({int __a = (a); __a < 0 ? -__a : __a;})

static int zoom_min = 1, zoom_max = 18;

extern int verbose;
int verbose;

/* Do not draw lines at zoom levels below Z_NO_LINES */
#define Z_NO_LINES 7

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
	struct xy xy;
	struct gpx_latlon loc;
	int point_cnt;
	gdImage *img;
};

#define ZOOM_TILE_HASH_SIZE (256u)
struct zoom_level {
	slist_stack_declare(struct tile, tiles[ZOOM_TILE_HASH_SIZE]);
	double xunit, yunit;
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
		zoom_levels[zoom].tiles[hash_xy(xy)].head : NULL;

	for (; tile; tile = tile->next)
		if (tile->xy.x == xy->x && tile->xy.y == xy->y)
			break;
	return tile;
}

static slist_stack_define(struct tile, free_tiles);

static struct tile *create_tile(const struct xy *xy, int z)
{
	struct tile *tile = NULL;

	if (zoom_min <= z && z <= zoom_max) {
		unsigned h = hash_xy(xy);
		const int transparent = gdTrueColorAlpha(0, 0, 0, gdAlphaTransparent);
	
		if (free_tiles.head) {
			tile = slist_stack_pop(&free_tiles);
			gdImageFilledRectangle(tile->img, 0, 0, 256, 256, 0);
		} else {
			tile = malloc(sizeof(*tile));
			tile->img = gdImageCreateTrueColor(256, 256);;
			gdImageColorTransparent(tile->img, transparent);
			gdImageSetAntiAliased(tile->img, gdTrueColorAlpha(0, 255, 0, 0));
		}
		gdImageFilledRectangle(tile->img, 0, 0, 256, 256, transparent);
		tile->xy = *xy;
		tile->loc.lat = tiley2lat(xy->y, z);
		tile->loc.lon = tilex2long(xy->x, z);
		tile->point_cnt = 0;

		slist_push(&zoom_levels[z].tiles[h], tile);
		zoom_levels[z].tile_cnt++;
	}
	return tile;
}
static void destroy_tile(struct tile *tile)
{
	slist_push(&free_tiles, tile);
}

static struct tile *get_tile(const struct xy *xy, int z)
{
	struct tile *tile = find_tile(xy, z);

	if (!tile)
		tile = create_tile(xy, z);
	return tile;
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
			destroy_tile(slist_stack_pop(&zl->tiles[h]));
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
		struct tile *tile = get_tile(&xy, z);

		if (!tile)
			continue;

		tile->point_cnt++;
		struct xy pix = getPixelPosForCoordinates(&pt->loc, z);
		struct xy ppix = pix;
		struct xy pxy = xy;
		struct tile *ptile = tile;
		int speed = 0;

		if (ppt != pt) {
			pxy = get_tile_xy(&ppt->loc, z);
			ptile = get_tile(&pxy, z);
			ppix = getPixelPosForCoordinates(&ppt->loc, z);
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
			continue;

		gdImageSetAntiAliased(tile->img, color);
		if (z >= 17 && (pt->flags & GPX_PT_PDOP) && pt->pdop > 1.8) {
			int d = (int)floor(pt->pdop * 3);

			gdImageEllipse(tile->img, pix.x, pix.y,
				       d, d, (20 << 24) | color);
		}
		if (tile == ptile) {
			if (ppix.x != pix.x || ppix.y != pix.y)
				gdImageLine(tile->img, pix.x, pix.y, ppix.x, ppix.y, color);
			continue;
		}
#if 0
		if (intmod(tile->xy.x - ptile->xy.x) > 1 || intmod(tile->xy.y - ptile->xy.y) > 1)
			printf("\nz %d %s and %s far apart: dx=%d, dy=%d\n\n",
			       z, pt->time, ppt->time,
			       tile->xy.x - ptile->xy.x,
			       tile->xy.y - ptile->xy.y);
#endif
		int px = ppix.x - (tile->xy.x - ptile->xy.x) * 256;
		int py = ppix.y - (tile->xy.y - ptile->xy.y) * 256;

		gdImageLine(tile->img, px, py, pix.x, pix.y, color /*0xff00ef*/);

		int ppx = pix.x - (ptile->xy.x -tile->xy.x) * 256;
		int ppy = pix.y - (ptile->xy.y - tile->xy.y) * 256;

		gdImageLine(ptile->img, ppix.x, ppix.y, ppx, ppy, color /*0x00006f*/);
	}
}

static void make_tiles(struct gpx_file *files, int z)
{
	struct gpx_file *f;

	for (f = files; f; f = f->next) {
		struct gpx_segment *seg;

		for (seg = f->gpx->segments.head; seg; seg = seg->next)
			draw_track_points(seg->points.head, z);
	}
}

static inline void save_zoom_level(int z)
{
	int len = 0, h;

	for (h = 0; h < ZOOM_TILE_HASH_SIZE; ++h) {
		struct tile *tile;

		for (tile = zoom_levels[z].tiles[h].head;
		     tile;
		     tile = tile->next) {
			char path[128];

			snprintf(path, sizeof(path), "%d", z);
			mkdir(path, 0775);
			snprintf(path, sizeof(path), "%d/%d", z, tile->xy.x);
			mkdir(path, 0775);
			snprintf(path, sizeof(path), "%d/%d/%d.png", z,
				 tile->xy.x, tile->xy.y);

			FILE *fp = fopen(path, "wb");

			if (fp) {
				gdImagePngEx(tile->img, fp, 4);
				fclose(fp);
			}
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

#include "dump.h"

struct load
{
	struct load *next;
	const char *path;
	struct gpx_file *gf;
};

static pthread_mutex_t load_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t load_q_cond = PTHREAD_COND_INITIALIZER;
static slist_define(struct load, load_q);
static slist_stack_define(struct load, load_free);

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
	slist_define(struct gpx_file, files);
	int points_cnt, files_cnt = 0;
	int stdin_files = 0; /* read zero-terminated list of files from stdin */
	int parallel = 4;
	pthread_t *loaders;
	int opt;

	while ((opt = getopt(argc, argv, "0z:Z:C:j:v")) != -1)
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
		case 'z':
			zoom_min = strtol(optarg, NULL, 0);
			break;
		case 'Z':
			zoom_max = strtol(optarg, NULL, 0);
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
	if (!points_cnt)
		exit(0);

	if (verbose > 2)
		dump_points(files.head);

	if (cd_to != -1 && fchdir(cd_to) == -1) {
		perror("chdir");
		exit(2);
	}

	int z;

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
		gdImageDestroy(t->img);
		free(t);
	}
	return 0;
}

