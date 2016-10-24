#include <stdio.h>
#include <gd.h>
#include <libxml/parser.h>

#define countof(a) (sizeof(a) / sizeof((a)[0]))

static void draw()
{
	int x, y, dy;
	FILE *fp;
	gdImage *img;
	int transparent = gdTrueColorAlpha(0, 0, 0, gdAlphaTransparent);
	
	img = gdImageCreateTrueColor(256, 256);
	gdImageColorTransparent(img, transparent);
	gdImageFilledRectangle(img, 0, 0, 256, 256, transparent);

	fp = fopen("tiles/empty.png", "wb");
	gdImagePngEx(img, fp, 4);
	fclose(fp);

	gdPoint poly[256];
	y = 256 / 2;
	dy = -1;
	for (x = 0; x < countof(poly); ++x) {
		poly[x].x = x;
		poly[x].y = y;
		y += dy;
		if (x % 4 == 0)
			dy = -dy;
	}
	gdImageSetAntiAliased(img, gdTrueColorAlpha(0, 255, 0, 0));
	gdImageOpenPolygon(img, poly, countof(poly), gdAntiAliased);

	fp = fopen("tiles/filled.png", "wb");
	gdImagePngEx(img, fp, 4);
	fclose(fp);
	gdImageDestroy(img);
}

int main(int argc, char *argv[])
{
	xmlDoc *gpx = xmlReadFile("example.gpx", NULL /* encoding */,
				  XML_PARSE_RECOVER |
				  XML_PARSE_NOERROR |
				  XML_PARSE_NOWARNING |
				  XML_PARSE_NONET |
				  XML_PARSE_NOXINCNODE);
	printf("%p\n", gpx);
	xmlNode *root = xmlDocGetRootElement(gpx);
	printf("root %s\n", root->name);
	xmlNode *e;
	int ptcnt = 0;
	for (e = xmlFirstElementChild(root); e; e = xmlNextElementSibling(e)) {
		printf("\t%s\n", e->name);
		if (xmlStrcasecmp(e->name, "trk") == 0) {
			xmlNode *seg;
			for (seg = xmlFirstElementChild(e); seg; seg = xmlNextElementSibling(seg)) {
				printf("\t\t%s\n", seg->name);
				if (xmlStrcasecmp(seg->name, "trkseg") == 0) {
					xmlNode *pt;
					for (pt = xmlFirstElementChild(seg);
					     pt;
					     pt = xmlNextElementSibling(pt)) {
						printf("\t\t\t%s %s %s\n", pt->name,
						       xmlGetProp(pt, "lat"),
						       xmlGetProp(pt, "lon"));
						++ptcnt;
					}
				}
			}
		}
	}
	// xmlElemDump(stdout, gpx, root);
	xmlFreeDoc(gpx);
	printf("Done (%d).\n", ptcnt);
	return 0;
}

