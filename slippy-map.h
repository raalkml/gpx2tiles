/*
 * map latitude/longitude and zoom level to tile number (x/y)
 * see also: https://wiki.openstreetmap.org/wiki/Slippy_map_tilenames
 */
#ifndef _SLIPPY_MAP_H_
#define _SLIPPY_MAP_H_

#include <math.h>

static inline int long2tilex(double lon, int z)
{
	double x = (lon + 180.0) / 360.0 * pow(2.0, z);
	return (int)floor(x);
}

static inline int lat2tiley(double lat, int z)
{
	double lrad = lat * M_PI / 180.0;
	double y = (1.0 - log(tan(lrad) + 1.0 / cos(lrad)) / M_PI) / 2.0 * pow(2.0, z);
	return (int)floor(y);
}

static inline double tilex2long(int x, int z)
{
	return x / pow(2.0, z) * 360.0 - 180;
}

static inline double tiley2lat(int y, int z)
{
	double n = M_PI - 2.0 * M_PI * y / pow(2.0, z);
	return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

#endif
