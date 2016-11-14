# gpx2tiles

An utility to create Openstreetmap slippy map tiles from a set of GPX files.

Inpired by the http://wiki.openstreetmap.org/wiki/Gpx2png

The gpx2png is a Perl script and as such is too slow and too resource hungry
to be used on the puny server hardware originally intended for the task.
Besides, its dependencies (ImageMagick in particular) make its deployment a
bit extensive a task.

Gpx2tiles requires two external libraries LidGD (https://www.libgd.org/) - to
draw the tiles, and LibXML2 (www.xmlsoft.org/) - for parsing the XML of the
GPX files.

The program parallelizes loading of the tracks (to speedup processing), can
regenerate the speed (from distance and timestamps, to handle tracklogs
without complete GPS data).

The tiles can be either generated completely from scratch or only updated with
the new tracks, which is obviously faster (and default mode of operation).

The output is immediately usable by Leaflet and OpenLayers: the directory
structure corresponds to the popular .../{z}/{x}/{y}.png layout.

The usage of memory can be restricted to be able to run it in constrained
environments.

Usage example:

    $ gpx2tile -C tiles/tracklogs /mnt/gps/logs/*.gpx

This will place the tiles in the "tiles/tracklogs" directory, creating the
necessary zoom levels and latitude directories, updating the existing tiles
with the new track information.

