#if 0
if (pt->flags & GPX_PT_ELE)
	; // printf("  ele %f\n", pt->ele);
if (pt->flags & GPX_PT_SPEED)
	; // printf("  spd %f\n", pt->speed);
	if (pt->flags & (GPX_PT_HDOP|GPX_PT_VDOP|GPX_PT_PDOP)) {
		; // fputc(' ', stdout);
		if (pt->flags & GPX_PT_HDOP)
			; // printf(" hdop %f", pt->hdop);
		if (pt->flags & GPX_PT_VDOP)
			; // printf(" vdop %f", pt->vdop);
		if (pt->flags & GPX_PT_PDOP)
			; // printf(" pdop %f", pt->pdop);
		; // fputc('\n', stdout);
	}
#endif

static inline void dump_points(struct gpx_file *f)
{
	for (; f; f = f->next) {
		struct gpx_point *pt;
		printf("From %s (%d)\n", f->gpx->path, f->gpx->points_cnt);
		for (pt = f->gpx->points; pt; pt = pt->next) {
			printf(" %f,%f %s\n", pt->loc.lat, pt->loc.lon, pt->time);
			int z, len = 0;
			for (z = 1; z <= 18; ++z) {
				struct xy tile = get_tile_xy(&pt->loc, z);
				len += printf(" %d/%d/%d", z, tile.x, tile.y);
				if (len >= 60) {
					fputc('\n', stdout);
					len = 0;
				}
			}
			if (pt->flags & GPX_PT_ELE)
				printf("  ele %f\n", pt->ele);
			if (pt->flags & GPX_PT_SPEED)
				printf("  spd %f\n", pt->speed);
			if (pt->flags & (GPX_PT_HDOP|GPX_PT_VDOP|GPX_PT_PDOP)) {
				fputc(' ', stdout);
				if (pt->flags & GPX_PT_HDOP)
					printf(" hdop %f", pt->hdop);
				if (pt->flags & GPX_PT_VDOP)
					printf(" vdop %f", pt->vdop);
				if (pt->flags & GPX_PT_PDOP)
					printf(" pdop %f", pt->pdop);
				fputc('\n', stdout);
			}
		}
	}
}

static inline void dump_zoom_level(int z)
{
	int len = 0, h;
	for (h = 0; h < ZOOM_TILE_HASH_SIZE; ++h) {
		struct tile *tile;
		for (tile = zoom_levels[z].tiles[h]; tile; tile = tile->next) {
			len += printf(" %d/%d (%d)",
				      tile->xy.x, tile->xy.y,
				      tile->point_cnt);
			if (len >= 60) {
				fputc('\n', stdout);
				len = 0;
			}
		}
	}
	if (len)
		fputc('\n', stdout);
}
