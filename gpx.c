#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <libxml/parser.h>
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

#define slist_init(p, field) do { \
	(p)->field = NULL; \
	(p)->field ## _tail = &(p)->field; \
} while(0)
#define slist_append(p, field, i) do { \
	typeof(*(p)->field) **__tail = (p)->field ## _tail; \
	(i)->next = NULL; \
	(p)->field ## _tail = &(i)->next; \
	*__tail = i; \
} while(0)

/*
const char GPX_SRC_GPS[] = "gps";
const char GPX_SRC_SYNTHETIC[] = "synt";
const char GPX_SRC_UNKNOWN[] = "unk";
*/

struct gpx_point *new_trk_point(void)
{
	struct gpx_point *pt = malloc(sizeof(*pt));

	pt->next = NULL;
	pt->flags = 0;
	//pt->src = GPX_SRC_SYNTHETIC;
	pt->loc.lat = pt->loc.lon = 0.0;
	return pt;
}

void free_trk_point(struct gpx_point *pt)
{
	free(pt);
}

void put_trk_point(struct gpx_segment *seg, struct gpx_point *pt)
{
	slist_append(seg, points, pt);
}

void parse_trkpt(xmlNode *xpt, struct gpx_point *pt)
{
	for (xpt = xmlFirstElementChild(xpt); xpt;
	     xpt = xmlNextElementSibling(xpt)) {
		xmlChar *s;
		float *f;
		if (xmlStrcasecmp(xpt->name, BAD_CAST "time") == 0) {
			pt->flags |= GPX_PT_TIME;
			s = xmlNodeGetContent(xpt);
			strlcpy(pt->time, ASCII s, sizeof(pt->time));
			goto frees;
#if 0
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "src") == 0) {
			s = xmlNodeGetContent(xpt);
			if (xmlStrcasecmp(s, BAD_CAST GPX_SRC_GPS) == 0)
				pt->src = GPX_SRC_GPS;
			else
				pt->src = GPX_SRC_UNKNOWN;
			goto frees;
#endif
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "speed") == 0) {
			pt->flags |= GPX_PT_SPEED;
			s = xmlNodeGetContent(xpt);
			pt->speed = strtod(ASCII s, NULL);
			goto frees;
		} else if (xmlStrcasecmp(xpt->name, BAD_CAST "sat") == 0) {
			pt->flags |= GPX_PT_SAT;
			s = xmlNodeGetContent(xpt);
			pt->speed = strtol(ASCII s, NULL, 10);
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
	struct tm tm;
	time_t utc = time(NULL);
	tm = *gmtime(&utc);
	char *p = strptime(t, "%Y-%m-%dT%H:%M:%S", &tm);
	if (p) {
		return mktime(&tm);
	}
	return utc;
}

static void synthesize_speed(struct gpx_point *pt, const struct gpx_point *ppt)
{
	extern int verbose;

	pt->flags = GPX_PT_SPEED;
	if ((ppt->flags & GPX_PT_SPEED) && pt->next && (pt->next->flags & GPX_PT_SPEED)) {
		pt->speed = (ppt->speed + pt->next->speed) / 2.0;
		if (verbose)
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
		if (verbose)
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
	struct gpx_segment *seg = NULL;
	int ptcnt = 0;
	int synspeed = 0;
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
		parse_trkpt(xpt, pt);
		if ((pt->flags & (GPX_PT_TIME|GPX_PT_SPEED)) == GPX_PT_TIME)
			synspeed = 1;
		//pt->trk = trk;
		//pt->seg = nseg;
		if (!seg)
			seg = new_trk_segment();
		put_trk_point(seg, pt);
		++ptcnt;
		continue;
	fail:
		free_trk_point(pt);
	}
	if (seg) {
		struct gpx_point *pt = seg->points;
		struct gpx_point *ppt = NULL;

		for (; synspeed && pt; ppt = pt, pt = pt->next)
			if ((pt->flags & (GPX_PT_TIME|GPX_PT_SPEED)) == GPX_PT_TIME && ppt)
				synthesize_speed(pt, ppt);
		put_trk_segment(gpxf, seg);
	}
	return ptcnt;
}

struct gpx_segment *new_trk_segment(void)
{
	struct gpx_segment *seg = malloc(sizeof(*seg));

	seg->next = NULL;
	slist_init(seg, points);
	return seg;
}

void free_trk_segment(struct gpx_segment *seg)
{
	while (seg->points) {
		struct gpx_point *pt = seg->points;
		seg->points = seg->points->next;
		free_trk_point(pt);
	}
	free(seg);
}

void put_trk_segment(struct gpx_data *gpx, struct gpx_segment *seg)
{
	slist_append(gpx, segments, seg);
}

struct gpx_data *gpx_read_file(const char *path)
{
	xmlNode *xe;
	xmlDoc *xml;
	//int ntrk = 0;
	struct gpx_data *gpxf = malloc(sizeof(*gpxf));

	gpxf->path = strdup(path);
	gpxf->points_cnt = 0;
	slist_init(gpxf, segments);
	memset(gpxf->time, 0, sizeof(gpxf->time));

	xml = xmlReadFile(gpxf->path,
			  NULL /* encoding */,
			  XML_PARSE_RECOVER |
			  XML_PARSE_NOERROR |
			  XML_PARSE_NOWARNING |
			  XML_PARSE_NONET |
			  XML_PARSE_NOXINCNODE);
	if (!xml)
		return gpxf;

	for (xe = xmlFirstElementChild(xmlDocGetRootElement(xml)); xe;
	     xe = xmlNextElementSibling(xe)) {
		xmlNode *seg;
		//int nseg = 0;

		if (xmlStrcasecmp(xe->name, BAD_CAST "time") == 0) {
			xmlChar *s = xmlNodeGetContent(xe);
			strlcpy(gpxf->time, ASCII s, sizeof(gpxf->time));
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
			gpxf->points_cnt += process_trk_points(gpxf,
				xmlFirstElementChild(seg) /* , ntrk, nseg */);
			//++nseg;
		}
		//++ntrk;
	}
	xmlFreeDoc(xml);
	return gpxf;
}

void gpx_free(struct gpx_data *gpx)
{
	while (gpx->segments) {
		struct gpx_segment *seg = gpx->segments;
		gpx->segments = gpx->segments->next;
		free_trk_segment(seg);
	}
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

