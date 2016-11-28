#include <stdlib.h>
#include <string.h>
#include <math.h>
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

void put_trk_point(struct gpx_data *gpxf, struct gpx_point *pt)
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

static int process_trk_points(struct gpx_data *gpxf, xmlNode *xpt /*, int trk, int nseg*/)
{
	int ptcnt = 0;
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
		//pt->trk = trk;
		//pt->seg = nseg;
		put_trk_point(gpxf, pt);
		++ptcnt;
		continue;
	fail:
		free_trk_point(pt);
	}
	return ptcnt;
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
	while (gpx->points) {
		struct gpx_point *pt = gpx->points;
		gpx->points = gpx->points->next;
		free_trk_point(pt);
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

