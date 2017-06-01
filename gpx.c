#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <libxml/parser.h>
#include "slist.h"
#include "gpx.h"

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define ASCII (char *)

static inline size_t strlcpy(char* tgt, const char* src, size_t size)
{
	size_t r = strlen(src);
	if ( size )
	{
		size_t l = size > r ? r: size - 1;
		memcpy(tgt, src, l);
		tgt[l] = '\0';
	}
	return r;
}

const char GPX_SRC_GPS[] = "gps";
const char GPX_SRC_NETWORK[] = "network";
const char GPX_SRC_UNKNOWN[] = "";

static int static_gpx_src(const char *s)
{
	return s == GPX_SRC_UNKNOWN || s == GPX_SRC_GPS || s == GPX_SRC_NETWORK;
}

struct segtab_entry
{
	struct segtab_entry *next;
	char *src;
	struct gpx_segment *seg;
};

struct segtab {
	SLIST_STACK_DECLARE(struct segtab_entry, segs);
	struct segtab_entry reserve[10];
	int reserve_use;
};

static struct segtab_entry *put_segtab_src(struct segtab *tab, char *src)
{
	struct segtab_entry *e;
	if (tab->reserve_use == countof(tab->reserve))
		e = malloc(sizeof(*e));
	else
		e = tab->reserve + tab->reserve_use++;
	e->src = src;
	e->seg = NULL;
	slist_push(&tab->segs, e);
	return e;
}

static struct segtab_entry *get_segtab(struct segtab *tab, const char *src)
{
	struct segtab_entry *e;

	slist_for_each(e, &tab->segs)
		if (strcmp(e->src, src) == 0)
			goto done;
	e = put_segtab_src(tab, strdup(src));
done:
	return e;
}

static void init_segtab(struct segtab *tab)
{
	tab->segs.head = NULL;
	tab->reserve_use = 0;
	put_segtab_src(tab, (char *)GPX_SRC_UNKNOWN);
	put_segtab_src(tab, (char *)GPX_SRC_NETWORK);
	put_segtab_src(tab, (char *)GPX_SRC_GPS);
}

static void segtab_cleanup(struct segtab *tab)
{
	while (!slist_empty(&tab->segs)) {
		struct segtab_entry *e = slist_stack_pop(&tab->segs);

		if (tab->reserve <= e && e < tab->reserve + countof(tab->reserve))
			continue;
		if (!static_gpx_src(e->src))
			free(e->src);
		free_trk_segment(e->seg);
		free(e);
	}
}

struct gpx_point *new_trk_point(void)
{
	struct gpx_point *pt = malloc(sizeof(*pt));

	pt->next = NULL;
	pt->flags = 0;
	pt->loc.lat = pt->loc.lon = 0.0;
	return pt;
}

void free_trk_point(struct gpx_point *pt)
{
	free(pt);
}

void put_trk_point(struct gpx_segment *seg, struct gpx_point *pt)
{
	slist_append(&seg->points, pt);
}

void merge_trk_points(struct gpx_point *dest, const struct gpx_point *src)
{
	unsigned flags = src->flags & ~dest->flags;

	if (flags & GPX_PT_LATLON) {
		dest->loc = src->loc;
		dest->flags |= GPX_PT_LATLON;
	}
	if (flags & GPX_PT_TIME) {
		strcpy(dest->time, src->time);
		dest->flags |= GPX_PT_TIME;
	}
	if (flags & GPX_PT_ELE) {
		dest->ele = src->ele;
		dest->flags |= GPX_PT_ELE;
	}
	if (flags & GPX_PT_COURSE) {
		dest->course = src->course;
		dest->flags |= GPX_PT_COURSE;
	}
	if (flags & GPX_PT_SPEED) {
		dest->speed = src->speed;
		dest->flags |= GPX_PT_SPEED;
	}
	if (flags & GPX_PT_HDOP) {
		dest->hdop = src->hdop;
		dest->flags |= GPX_PT_HDOP;
	}
	if (flags & GPX_PT_VDOP) {
		dest->vdop = src->vdop;
		dest->flags |= GPX_PT_VDOP;
	}
	if (flags & GPX_PT_PDOP) {
		dest->pdop = src->pdop;
		dest->flags |= GPX_PT_PDOP;
	}
	if (flags & GPX_PT_SAT) {
		dest->sat = src->sat;
		dest->flags |= GPX_PT_SAT;
	}
}

/* returns the flags of a where the values are equal to b */
unsigned gpx_point_compare(const struct gpx_point *a, const struct gpx_point *b)
{
	unsigned flags = a->flags & b->flags;

	if ((flags & GPX_PT_LATLON) && !(a->loc.lat == b->loc.lat &&
					 a->loc.lon == b->loc.lon))
		flags &= ~GPX_PT_LATLON;
	if ((flags & GPX_PT_TIME) && strcmp(a->time, b->time))
		flags &= ~GPX_PT_TIME;
	if ((flags & GPX_PT_ELE) && a->ele != b->ele)
		flags &= ~GPX_PT_ELE;
	if ((flags & GPX_PT_COURSE) && a->course != b->course)
		flags &= ~GPX_PT_COURSE;
	if ((flags & GPX_PT_SPEED) && a->speed != b->speed)
		flags &= ~GPX_PT_SPEED;
	if ((flags & GPX_PT_HDOP) && a->hdop != b->hdop)
		flags &= ~GPX_PT_HDOP;
	if ((flags & GPX_PT_VDOP) && a->vdop != b->vdop)
		flags &= ~GPX_PT_VDOP;
	if ((flags & GPX_PT_PDOP) && a->pdop != b->pdop)
		flags &= ~GPX_PT_PDOP;
	if ((flags & GPX_PT_SAT) && a->sat != b->sat)
		flags &= ~GPX_PT_SAT;
	return flags;
}

static struct segtab_entry *parse_trkpt(xmlNode *xpt, struct gpx_point *pt, struct segtab *segs)
{
	struct segtab_entry *e = NULL;

	for (xpt = xmlFirstElementChild(xpt); xpt;
	     xpt = xmlNextElementSibling(xpt)) {
		xmlChar *s;
		float *f;
		if (xmlStrcasecmp(xpt->name, BAD_CAST "time") == 0) {
			pt->flags |= GPX_PT_TIME;
			s = xmlNodeGetContent(xpt);
			strlcpy(pt->time, ASCII s, sizeof(pt->time));
			goto frees;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "src") == 0) {
			s = xmlNodeGetContent(xpt);
			e = get_segtab(segs, (char *)s);
			goto frees;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "speed") == 0) {
			pt->flags |= GPX_PT_SPEED;
			s = xmlNodeGetContent(xpt);
			pt->speed = strtod(ASCII s, NULL);
			goto frees;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "sat") == 0) {
			pt->flags |= GPX_PT_SAT;
			s = xmlNodeGetContent(xpt);
			pt->sat = strtol(ASCII s, NULL, 10);
			goto frees;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "ele") == 0) {
			pt->flags |= GPX_PT_ELE;
			f = &pt->ele;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "geoidheight") == 0) {
			pt->flags |= GPX_PT_ELE;
			f = &pt->geoidheight;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "course") == 0) {
			pt->flags |= GPX_PT_COURSE;
			f = &pt->course;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "hdop") == 0) {
			pt->flags |= GPX_PT_HDOP;
			f = &pt->hdop;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "vdop") == 0) {
			pt->flags |= GPX_PT_VDOP;
			f = &pt->vdop;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "pdop") == 0) {
			pt->flags |= GPX_PT_PDOP;
			f = &pt->pdop;
		} else
			continue;
		s = xmlNodeGetContent(xpt);
		*f = strtof(ASCII s, NULL);
	frees:
		xmlFree(s);
	}
	return e;
}

/*
 * Distance between two geographical points using spherical law of cosines
 * approximation.
 * The implementation shamelessly stolen from Leaflet 1.0
 */
double earth_distance(const struct gpx_latlon *latlng1,
		      const struct gpx_latlon *latlng2)
{
	// Mean Earth Radius, as recommended for use by
	// the International Union of Geodesy and Geophysics,
	// see http://rosettacode.org/wiki/Haversine_formula
	const double R = 6371000;

	const double rad = M_PI / 180.0;
	const double lat1 = latlng1->lat * rad;
	const double lat2 = latlng2->lat * rad;
	double a = sin(lat1) * sin(lat2) + cos(lat1) * cos(lat2) * cos((latlng2->lon - latlng1->lon) * rad);

	return R * acos(a < 1.0 ? a : 1.0);
}

static time_t gpxtime2sec(const char *t)
{
	char *p;
	struct tm tm;
	time_t utc = time(NULL);

	tm = *gmtime(&utc);
	p = strptime(t, "%Y-%m-%dT%H:%M:%S", &tm);
	if (p)
		return mktime(&tm);
	return utc;
}

static void synthesize_speed(struct gpx_point *pt, const struct gpx_point *ppt)
{
	extern int verbose;

	pt->flags = GPX_PT_SPEED;
	if ((ppt->flags & GPX_PT_SPEED) && pt->next && (pt->next->flags & GPX_PT_SPEED)) {
		pt->speed = (ppt->speed + pt->next->speed) / 2.0;
		if (verbose > 1)
			fprintf(stderr, "speed:   averaged %5.2f kph from %s (%3.2f) to %s (%3.2f)\n",
				pt->speed * 3.6,
				ppt->time, ppt->speed * 3.6,
				pt->next->time, pt->next->speed * 3.6);
	} else {
		double d = earth_distance(&ppt->loc, &pt->loc);
		time_t t = gpxtime2sec(pt->time) - gpxtime2sec(ppt->time);
		if (t < 1)
			t = 1;
		pt->speed = d / (double)t;
		if (verbose > 1)
			fprintf(stderr, "speed: calculated %5.2f kph from %s to %s: %.2f m, %ld sec, "
				"PDOP %.1f\n",
				3.6 * pt->speed,
				ppt->time, pt->time,
				d, (long)t,
				pt->flags & GPX_PT_PDOP ? pt->pdop : 99.);
	}
}

static int process_trk_points(struct gpx_data *gpxf, xmlNode *xpt /*, int trk, int nseg*/)
{
	int ptcnt = 0;
	int synspeed = 0;
	struct gpx_point *ppt = NULL;
	struct segtab_entry *e, *unknown;
	struct segtab segs;

	init_segtab(&segs);
	unknown = get_segtab(&segs, GPX_SRC_UNKNOWN);

	for (; xpt; xpt = xmlNextElementSibling(xpt)) {
		char *err, *nptr;
		struct gpx_point *pt;

		if (xmlStrcasecmp(xpt->name, BAD_CAST "trkpt") != 0) {
			fprintf(stderr, "Unknown element %s\n", xpt->name);
			continue;
		}
		pt = new_trk_point();
		nptr = ASCII xmlGetProp(xpt, BAD_CAST "lat");
		pt->loc.lat = strtod(nptr, &err);
		xmlFree(nptr);
		if ((pt->loc.lat == 0.0 && nptr == err) || pt->loc.lat == HUGE_VAL)
			goto fail;
		nptr = ASCII xmlGetProp(xpt, BAD_CAST "lon");
		pt->loc.lon = strtod(nptr, &err);
		xmlFree(nptr);
		if ((pt->loc.lon == 0.0 && nptr == err) || pt->loc.lon == HUGE_VAL)
			goto fail;
		pt->flags |= GPX_PT_LATLON;
		e = parse_trkpt(xpt, pt, &segs);
		if (!e)
			e = unknown;
		if ((pt->flags & (GPX_PT_TIME|GPX_PT_SPEED)) == GPX_PT_TIME)
			synspeed = 1;
		if (!(pt->flags & GPX_PT_TIME))
			sprintf(pt->time, "%d", ptcnt);
		//pt->trk = trk;
		//pt->seg = nseg;
		if (!e->seg)
			e->seg = new_trk_segment(e->src);
		if (ppt) {
			unsigned same = pt->flags & ppt->flags;

			if ((same & GPX_PT_LATLON) &&
			    (same & GPX_PT_TIME) &&
			    pt->loc.lat == ppt->loc.lat &&
			    pt->loc.lon == ppt->loc.lon &&
			    strcmp(pt->time, ppt->time) == 0)
				merge_trk_points(ppt, pt);
			same = gpx_point_compare(ppt, pt);
			if (same == pt->flags)
				goto fail;
		}
		put_trk_point(e->seg, pt);
		ppt = pt;
		++ptcnt;
		continue;
	fail:
		free_trk_point(pt);
	}
	if (!slist_empty(&segs.segs))
		slist_for_each(e, &segs.segs) {
			if (e->seg) {
				if (synspeed) {
					struct gpx_point *pt;
					struct gpx_point *ppt = NULL;

					for (pt = e->seg->points.head; pt; ppt = pt, pt = pt->next)
						if ((pt->flags & (GPX_PT_TIME|GPX_PT_SPEED)) == GPX_PT_TIME && ppt)
							synthesize_speed(pt, ppt);
				}
				e->seg->free_src = !static_gpx_src(e->seg->src);
				e->src = NULL;
				put_trk_segment(gpxf, e->seg);
				e->seg = NULL;
			}
		}
	segtab_cleanup(&segs);
	return ptcnt;
}

struct gpx_segment *new_trk_segment(const char *src)
{
	struct gpx_segment *seg = malloc(sizeof(*seg));

	seg->src = src;
	seg->next = NULL;
	slist_init(&seg->points);
	return seg;
}

void free_trk_segment(struct gpx_segment *seg)
{
	if (!seg)
		return;
	while (seg->points.head)
		free_trk_point(slist_pop(&seg->points));
	if (seg->free_src)
		free((char *)seg->src);
	free(seg);
}

void put_trk_segment(struct gpx_data *gpx, struct gpx_segment *seg)
{
	slist_append(&gpx->segments, seg);
}

struct gpx_data *gpx_read_file(const char *path)
{
	xmlNode *xe;
	xmlDoc *xml;
	struct gpx_data *gpx = malloc(sizeof(*gpx));

	gpx->path = strdup(path);
	gpx->points_cnt = 0;
	slist_init(&gpx->segments);
	memset(gpx->time, 0, sizeof(gpx->time));

	xml = xmlReadFile(gpx->path,
			  NULL /* encoding */,
			  XML_PARSE_RECOVER |
			  XML_PARSE_NOERROR |
			  XML_PARSE_NOWARNING |
			  XML_PARSE_NONET |
			  XML_PARSE_NOXINCNODE);
	if (!xml)
		return gpx;

	for (xe = xmlFirstElementChild(xmlDocGetRootElement(xml)); xe;
	     xe = xmlNextElementSibling(xe)) {
		xmlNode *seg;

		if (xmlStrcasecmp(xe->name, BAD_CAST "time") == 0) {
			xmlChar *s = xmlNodeGetContent(xe);
			strlcpy(gpx->time, ASCII s, sizeof(gpx->time));
			xmlFree(s);
			continue;
		}
		if (xmlStrcasecmp(xe->name, BAD_CAST "wpt") == 0) {
			// fprintf(stderr, "discarded waypoint (not implemented)\n");
			continue;
		}
		if (xmlStrcasecmp(xe->name, BAD_CAST "trk") != 0)
			continue;
		for (seg = xmlFirstElementChild(xe); seg;
		     seg = xmlNextElementSibling(seg)) {
			if (xmlStrcasecmp(seg->name, BAD_CAST "trkseg") != 0)
				continue;
			gpx->points_cnt += process_trk_points(gpx,
				xmlFirstElementChild(seg));
		}
	}
	xmlFreeDoc(xml);
	return gpx;
}

void gpx_free(struct gpx_data *gpx)
{
	while (gpx->segments.head)
		free_trk_segment(slist_pop(&gpx->segments));
	free(gpx->path);
	free(gpx);
}

/* for valgrind */
void gpx_libxml_cleanup(void)
{
	xmlCleanupCharEncodingHandlers();
	xmlCleanupParser();
	xmlCleanupGlobals();
}

