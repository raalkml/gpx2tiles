#ifndef _GPX_H_
#define _GPX_H_

struct gpx_latlon
{
	double lat, lon;
};

struct gpx_point;

struct gpx_data
{
	char *path;
	char time[24];

	/* just all points, in all tracks, all segments */
	struct gpx_point *points, **points_tail;
	int points_cnt, track_cnt;
};

#define GPX_PT_LATLON  (1 << 0)
#define GPX_PT_ELE     (1 << 1)
#define GPX_PT_COURSE  (1 << 2)
#define GPX_PT_SPEED   (1 << 3)
#define GPX_PT_HDOP    (1 << 4)
#define GPX_PT_VDOP    (1 << 5)
#define GPX_PT_PDOP    (1 << 6)
#define GPX_PT_SAT     (1 << 7)
#define GPX_PT_TIME    (1 << 8)

struct gpx_point
{
	struct gpx_point *next;

	unsigned flags;
	struct gpx_latlon loc;
	double speed;
	int sat;
	float ele, geoidheight;
	float course;
	float hdop, vdop, pdop;

	char time[24];
	const char *src; // "gps"
	// struct gpx_data *container;
	// struct gpx_segment *seg;
	// struct gpx_track *trk;
	int trk, seg;
};

extern const char GPX_SRC_GPS[];
extern const char GPX_SRC_SYNTHETIC[];
extern const char GPX_SRC_UNKNOWN[];

struct gpx_point *new_trk_point(void); /* GPX_SRC_SYNTHETIC */
void free_trk_point(struct gpx_point *);
void put_trk_point(struct gpx_data *, struct gpx_point *);

struct gpx_data *gpx_read_file(const char *path);
void gpx_free(struct gpx_data *);

void gpx_libxml_cleanup(void);

#endif /* _GPX_H_ */

