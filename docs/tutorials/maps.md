# Maps: offline maps and lightweight GIS

`maps` makes maps and answers questions about places, headless and offline. A **map** holds layers, drawn bottom to top:

- **vector layers**: features (points, lines, polygons, and multi-part versions of each) with properties such as `name=Paris` or `pop_est=2100000`. Coordinates are always stored as WGS84 longitude/latitude, so any layer can be measured, joined or drawn against any other.
- **raster layers**: GeoTIFFs (elevation, densities, or RGB pictures), and PNG/JPG images placed by a world file.

It reads and writes GeoJSON, KML, GPX, CSV with lat/lon columns, Shapefile and WKT, and reads FIT activities and OpenStreetMap XML, all with its own code. Its measurements are geodesic on the WGS84 ellipsoid. It reprojects between WGS84, Web Mercator and UTM. It buffers, simplifies, clips, filters, joins, dissolves and overlays, and builds heatmaps. It plans car, bike and foot routes on OpenStreetMap roads, searches places, splits and cleans GPS tracks, and edits features vertex by vertex. It renders PNG, JPG, SVG and PDF maps with a scale bar, north arrow, legend and graticule.

**Nothing needs the internet.** The default background is an installed map pack (street-level offline maps from an [offline edition](offline-editions.md), Task 8) when you have one, and otherwise a built-in public-domain Natural Earth 1:110m country map. OpenStreetMap tiles are only downloaded when you ask for `basemap=osm`.

Each task below is shown three ways:

- **CLI**: single `wetwaresuite` commands. A map only lives inside one process, so one-shot commands are for looking at files (`maps.open path=hike.gpx`) and for measurements that need no map (`maps.measure`, `maps.transform`). For short edits, pipe a few lines into `wetwaresuite batch`.
- **Batch**: a script for multi-step work, using `-> name` captures and `$name` reuse (see [headless and batch](headless-and-batch.md)).
- **Claude**: a prompt for Claude over MCP, and the tools it calls (see [mcp.md](mcp.md)). `wetwaresuite mcp --tools maps` serves the 62 maps tools.

Every example was run against the real binary in a folder that held three small files: `cafes.csv` (15 Paris cafés with `name,lat,lon,rating,seats`), `trips.csv` (made-up days spent per country, keyed by ISO code) and `hike.gpx` (a 12 km loop near Chamonix with elevation and times). Tasks 10 to 14 add three more made-up files, described in Task 10. Outputs are copied from those runs. Repeated fields are cut where marked `...`.

## How maps works

**Coordinates.** Commands that take a point use `lat=` and `lon=`, or `lat,lon` pairs (`from=48.8566,2.3522`), as you would read them off a phone. Boxes are `bbox=min_lon,min_lat,max_lon,max_lat`, as in GeoJSON. WKT and `maps.transform` use x,y order (lon lat), as their standards do.

**Units.** Distances are metres (`distance=300`, `radius=600`, `tolerance=20`). Areas are square metres in results and km² in messages.

**Layers** are named after the file they came from (`cafes`, `hike tracks`, `hike waypoints`). Commands take `layer=` with a name or a 0-based index (0 is the bottom layer). Without `layer=`, a command works on the top vector layer. `maps.track_stats` and `maps.profile` pick the top layer with lines, and `maps.heatmap` the top layer with points. Names with spaces need quotes: `layer="5 min walk"`.

**Expressions.** `where=` (on most commands), `maps.filter` and `maps.calc` share one small language:

| piece | examples |
| --- | --- |
| fields and text | `name == 'France'`, `continent != 'Asia'`, `"field with spaces" > 3` |
| comparisons, logic | `== != < <= > >=`, `and`, `or`, `not`, parentheses |
| lists and text tests | `iso_a3 in ('ESP', 'PRT')`, `name contains 'land'`, `name startswith 'Le'`, `name like '%ville'` |
| missing values | `pop_est is null`, `days is not null` |
| arithmetic | `pop_est / 1e6`, `(a + b) * 2`, `-x`, `%` |
| geometry | `$area` (m²), `$length` (m), `$x` / `$y` (centroid lon/lat), `$id` (index), `$type`, `$vertices` |
| functions | `round(x, n)`, `abs`, `floor`, `ceil`, `sqrt`, `log10`, `min`, `max`, `lower`, `upper`, `len`, `coalesce(a, b)`, `str`, `num` |

Text values need quotes. Inside a `key="..."` argument, use single quotes: `where="continent == 'Europe'"`. If an expression names a field that no feature has, the result warns you. That usually means a missing quote.

**Undo.** Every edit (import, add, delete, style, buffer, filter, clip, join, ...) is one undo step that restores the whole map:

```text
maps.open path=cafes.csv -> m
maps.feature_delete doc=$m where="rating < 4.3"
undo
maps.info doc=$m
```

```text
deleted 4 features from cafes
...
undone
1 layer, 15 features
0: cafes (15 points)
```

`maps.info`, `maps.features`, `maps.measure`, `maps.bbox`, `maps.contains`, `maps.nearest`, `maps.transform`, `maps.track_stats`, `maps.track_splits`, `maps.raster_sample`, `maps.vertices`, `maps.search`, `maps.bookmarks`, `maps.hit` and `maps.preview` only read. `maps.view` changes what is shown but is not an undo step. `maps.render`, `maps.export`, `maps.save`, `maps.raster_save` and `maps.profile` write files and change nothing in the map.

## Task 1: a world map of where you've been

`trips.csv` holds days spent per country, keyed by ISO 3166 alpha-3 code:

```text
code,days,last_visit
FRA,21,2025
ESP,14,2024
...
XKX,3,2020
```

**Batch** (`trips.txt`):

```text
# trips.txt: a world map of where I've been, coloured by days spent
maps.new title="Where I've been" -> m
maps.add_countries doc=$m
maps.join doc=$m layer=countries csv=trips.csv key=iso_a3 csv_key=code
maps.style doc=$m layer=countries mode=graduated field=days ramp=blues classes=4 method=natural
maps.render doc=$m path=trips.png width=1200
maps.render doc=$m path=trips.pdf crs=wgs84 graticule=true title="Trips 2019-2026"
maps.save doc=$m path=trips.wwmap
```

```text
$ wetwaresuite batch trips.txt
created map1: 0 layers, 0 features, "Where I've been"
...
added 177 countries as countries
...
joined 2 column(s) onto 12 of 177 features of countries
1 CSV row matched no feature: XKX
...
styled countries: field, ramp, method, classes, mode
classes:
  #deebf7 5 - 7
  #91c2e1 7 - 10
  #4a84b2 10 - 14
  #08306b 14 - 21
...
rendered 1200x804 png to trips.png
...
rendered 1024x494 pdf to trips.pdf
...
saved trips.wwmap
```

`maps.add_countries` adds the built-in country layer, with the fields `name`, `iso_a3`, `continent` and `pop_est`. `maps.join` copies every other CSV column onto the features whose key matches. It reports the CSV rows that matched nothing (Kosovo has no ISO code in Natural Earth), so you can fix typos. Keys match exactly first, then ignoring case and spaces. Numbers match numerically, but codes with leading zeros such as `007` stay text.

`mode=graduated` colours a numeric field in `classes=` steps along a ramp. `method=` picks the breaks: `quantile` (equal counts, the default), `equal` (equal widths) or `natural` (Jenks natural breaks, also spelled `jenks`). Countries with no value keep the plain land colour. `mode=categorized field=continent` gives each value its own colour instead.

Natural breaks look for the gaps in your numbers. They put the dividing lines where the values jump, so each class holds countries that are alike and the classes differ as much as possible. Use them when the data has real groups, for example a few big countries and many small ones. Quantile classes are better when you want each colour to cover about the same number of countries. The breaks are the exact Jenks optimum (Fisher's method, the one QGIS and ArcGIS mean by "natural breaks"), and they are quick even for big layers. Up to 50,000 different values are classed exactly. Above that, nearby values are grouped first, and the widest gaps are always kept as possible breaks. If many features share the lowest value (lots of zeros, say), the first class can be just that one value.

The PNG uses Web Mercator, the projection of web maps. The PDF uses `crs=wgs84` (plate carrée), which shows the whole globe including Antarctica. A world map in Web Mercator stops at 60°S unless you pass a `bbox=`.

**CLI.** Look at the saved project, render it again, or export only the visited countries:

```sh
$ wetwaresuite maps.open path=trips.wwmap
opened trips.wwmap as map1: 1 layer, 177 features, "Where I've been", trips.wwmap
0: countries (177 polygons, graduated by days)
   fields: continent, iso_a3, name, pop_est, days, last_visit
...
$ printf 'maps.open path=trips.wwmap -> m\nmaps.render doc=$m path=trips-small.jpg width=600 legend=false\n' | wetwaresuite batch
$ printf 'maps.new -> m\nmaps.add_countries doc=$m\nmaps.join doc=$m csv=trips.csv key=iso_a3 csv_key=code\nmaps.export doc=$m path=out/visited.geojson where="days > 0" precision=3\n' | wetwaresuite batch
...
wrote 12 features of countries to out/visited.geojson
```

**Claude.** *"Make a world map from trips.csv, coloured by days per country, and save it as a PDF."* Claude calls `maps_new`, `maps_add_countries`, `maps_join` (`csv=trips.csv key=iso_a3 csv_key=code`), `maps_style` (`mode=graduated field=days`) and `maps_preview` to look at the result. It then calls `maps_render` with `path=trips.pdf`. If the join reports unmatched rows, Claude can tell you which codes to fix.

## Task 2: distances, areas and coordinates

These need no map, so they are one-shot commands.

**CLI.** The distance and bearings from Paris to New York, the length of a route through Brussels to Amsterdam, the area of a box around Paris, and the Eiffel Tower in UTM and Web Mercator:

```sh
$ wetwaresuite maps.measure from=48.8566,2.3522 to=40.7128,-74.0060
5852.9 km, initial bearing 291.83°, final bearing 233.72°
...
$ wetwaresuite maps.measure points=48.8566,2.3522,50.8503,4.3517,52.3676,4.9041
length 437.4 km over 2 segments
...
$ wetwaresuite maps.measure points=48.90,2.25,48.90,2.42,48.81,2.42,48.81,2.25 polygon=true
area 124.9 km², perimeter 44.968 km
...
$ wetwaresuite maps.transform points=2.2945,48.8584
WGS84 to UTM 31N (EPSG:32631):
2.2945,48.8584 -> 448252.001,5411954.91
...
$ wetwaresuite maps.transform points=448252,5411935 from=utm31n to=wgs84
UTM 31N to WGS84 (EPSG:4326):
448252,5411935 -> 2.2945025,48.85822091
...
$ wetwaresuite maps.transform points=2.2945,48.8584,-0.1246,51.5007 to=webmercator
WGS84 to Web Mercator (EPSG:3857):
2.2945,48.8584 -> 255422.572,6250868.901
-0.1246,51.5007 -> -13870.409,6710344.26
...
$ wetwaresuite maps.transform points=2.2945,48.8584 to=EPSG:32630
WGS84 to UTM 30N (EPSG:32630):
2.2945,48.8584 -> 888275.411,5425243.062
```

Distances are Vincenty geodesics on the WGS84 ellipsoid (accurate to well under a millimetre). Areas are computed on the authalic sphere, the sphere with the ellipsoid's surface area, which is good to about 0.1 %. `to=utm` (the default) picks the zone of the first point. The Norway and Svalbard zone exceptions are applied. The first output line gives the converted coordinates at full precision. The result fields printed below it (such as `points`) are rounded to 6 digits on the command line, but MCP returns them at full precision.

Coordinate systems are named as `wgs84` (or `EPSG:4326`), `webmercator` (`EPSG:3857`), `utm31n` / `utm31s` (`EPSG:32631` / `EPSG:32731`, or `EPSG:25831` for ETRS89 UTM, which is treated as WGS84), `equalearth` (`EPSG:8857`, an equal-area world map), `laea` (`EPSG:3035`, the Lambert azimuthal equal-area grid the EU uses for statistics) or `laea:lat,lon` (the same projection centred anywhere), and `auto` on renders. The two equal-area ones keep areas true everywhere on the map, so a choropleth of Greenland and Africa compares fairly: `maps.render crs=equalearth`.

**Batch.** With a map open, `maps.measure layer=` totals a layer's lengths and areas (per feature when there are few):

```text
maps.open path=hike.gpx -> m
maps.measure doc=$m layer="hike tracks"
```

**Claude.** *"How far is it from Lisbon to Tokyo, and which direction do I set off in?"* Claude calls `maps_measure` with `from=38.72,-9.14 to=35.68,139.69`. *"Convert these UTM 32N coordinates to lat/lon: 334180 5085090."* `maps_transform` with `points=334180,5085090 from=utm32n to=wgs84`.

## Task 3: cafés in Paris: walking areas, a heatmap, the best ones

**Batch** (`cafes.txt`):

```text
# cafes.txt: Paris cafés from a CSV: walking areas, a heatmap, the best ones, exports
maps.new title="Coffee in Paris" -> m
maps.import doc=$m path=cafes.csv
maps.filter doc=$m layer=cafes where="rating >= 4.5 and seats < 50" output=favourites
maps.nearest doc=$m layer=cafes lat=48.8606 lon=2.3376 count=3
maps.buffer doc=$m layer=cafes distance=300 dissolve=true output="5 min walk"
maps.measure doc=$m layer="5 min walk"
maps.heatmap doc=$m layer=cafes radius=600 weight=seats
maps.layer_move doc=$m layer=cafes to=top
maps.layer_remove doc=$m layer=favourites
maps.style doc=$m layer="5 min walk" fill=#2e8b57 fill_opacity=0.25 stroke=#2e8b57
maps.style doc=$m layer=cafes mode=graduated field=rating ramp=reds classes=3 point_size=9 label=name label_size=9
maps.render doc=$m path=paris.png width=900 basemap=none
maps.export doc=$m layer=cafes path=out/cafes.geojson
maps.export doc=$m layer=cafes path=out/cafes.kml
maps.export doc=$m layer="5 min walk" path=out/walk.shp crs=utm31n
maps.raster_save doc=$m layer="cafes heatmap" path=out/heat.tif
maps.raster_save doc=$m layer="cafes heatmap" path=out/heat.png
```

```text
$ wetwaresuite batch cafes.txt
...
imported 15 features into cafes (15 points)
fields: name, rating, seats
...
8 of 15 features match -> favourites
...
nearest in cafes to 48.8606,2.3376:
#3 347.54 m (at 48.8637,2.337) {name=Café Kitsuné; rating=4.5; seats=30}
#6 711.76 m (at 48.867,2.3375) {name=Télescope; rating=4.7; seats=20}
#1 798.92 m (at 48.854,2.3333) {name=Les Deux Magots; rating=4.2; seats=150}
...
buffered cafes by 300 m into 5 min walk (1 feature, 2.9743 km²)
...
5 min walk: 1 feature, area 2.9743 km², perimeter 17.723 km
...
heatmap cafes heatmap: 400x344 cells, peak 296.078
...
rendered 900x774 png to paris.png
...
wrote 1 feature of 5 min walk to out/walk.shp, out/walk.shx, out/walk.dbf, out/walk.prj, out/walk.cpg
...
wrote out/heat.tif
...
wrote out/heat.png, out/heat.pgw, out/heat.prj
```

The CSV columns `lat` and `lon` were found automatically. Column names such as `latitude`, `y`, `lng`, `long`, `longitude` and `x` work too, and so do values like `48°51'24"N`. The delimiter (comma, semicolon, tab or pipe) is detected. `lat=` and `lon=` name other columns, `wkt_column=` reads a geometry column, and `crs=` converts projected x/y columns (see Task 6). Numbers become numbers, except codes with leading zeros such as postcodes, which stay text.

`maps.buffer` turns a single point into a geodesic circle. Lines, polygons and groups of points are buffered exactly: the union of a box along every edge and a circle (`segments=` sides) on every vertex, worked out on the edges themselves on a local metric grid, so a 500 m buffer is 500 m to the millimetre. Very big layers (over about 600,000 edges once the circles are counted) fall back to a grid of `resolution=` cells (see [limitations](#limitations)). `dissolve=true` merges everything into one multi-polygon with a `count` field. A negative distance shrinks polygons.

`maps.heatmap` sums a quartic kernel of `radius=` metres around each point, weighted by `weight=`, into a value raster. Zero is transparent in the `heat` ramp. The raster can be saved as a float GeoTIFF or as a coloured PNG with a world file.

The shapefile export reprojects to UTM 31N and writes a matching `.prj`. Outer rings are written clockwise, as the format requires. `.cpg` says the attributes are UTF-8.

**CLI.** Open the CSV and list the best cafés:

```sh
$ wetwaresuite maps.open path=cafes.csv
opened cafes.csv as map1: 1 layer, 15 features
0: cafes (15 points)
   fields: name, rating, seats
...
$ printf 'maps.open path=cafes.csv -> m\nmaps.features doc=$m where="rating >= 4.6" fields=name,rating\n' | wetwaresuite batch
```

**Claude.** *"Here's cafes.csv. Show me the cafés with a heatmap weighted by seats, and shade everything within a 5 minute walk."* Claude calls `maps_open`, `maps_heatmap` (`weight=seats`), `maps_buffer` (`distance=400 dissolve=true`) and `maps_preview`. *"Which café is closest to the Louvre?"* `maps_nearest lat=48.8606 lon=2.3376`.

## Task 4: a GPX hike: stats, elevation profile and a map

**CLI.** A GPX file opens as one layer per kind of data: tracks, routes and waypoints.

```sh
$ wetwaresuite maps.open path=hike.gpx
opened hike.gpx as map1: 2 layers, 3 features
0: hike tracks (1 line)
   fields: name
1: hike waypoints (2 points)
   fields: name, ele
map1
...
$ printf 'maps.open path=hike.gpx -> m\nmaps.track_stats doc=$m hysteresis=15\n' | wetwaresuite batch
...
hike tracks: 12.378 km over 241 points
elevation: +610.7 m / -601.1 m, 1029.9 to 1660.6 m
time: 2026-07-12T08:30:00Z to 2026-07-12T12:17:00Z, 3:47:00 total, 3:30:58 moving
speed: average 3.27 km/h, moving 3.51 km/h, max 4.29 km/h, pace 17:06 min/km
```

**Batch** (`hike.txt`):

```text
# hike.txt: stats, an elevation profile and a map of a GPX track
maps.open path=hike.gpx -> m
maps.track_stats doc=$m layer="hike tracks"
maps.profile doc=$m layer="hike tracks" path=profile.png title="Col loop"
maps.simplify doc=$m layer="hike tracks" tolerance=10 output=simple
maps.export doc=$m layer=simple path=out/hike-simple.gpx
maps.layer_remove doc=$m layer=simple
maps.style doc=$m layer="hike tracks" stroke=#d03030 width=3
maps.style doc=$m layer="hike waypoints" label=name marker=triangle point_size=11 fill=#202020
maps.render doc=$m path=hike.png width=800 crs=auto graticule=true
maps.render doc=$m path=hike.svg width=800 center=45.935,6.88 radius_km=4
```

```text
...
hike tracks: 12.378 km over 241 points
elevation: +782.2 m / -787.3 m, 1029.9 to 1660.6 m
...
profile of hike tracks: 12.378 km, 241 points -> profile.png
...
simplified simple: 241 -> 49 vertices
...
rendered 800x824 png to hike.png
...
crs: EPSG:32632
...
rendered 800x803 svg to hike.svg
```

The gain with the default `hysteresis=3` (782 m) is higher than with `hysteresis=15` (611 m), because this track's elevation wobbles by a few metres, like real GPS. A climb only counts once it has risen more than the hysteresis. Use 3 for a barometric watch and 10 to 20 for GPS-only elevations. Moving time counts the stretches faster than 0.5 m/s, so the 15 minute lunch stop is left out. Max speed is smoothed over 5 points so a single GPS jump doesn't count.

`maps.simplify` (Douglas-Peucker) kept 49 of 241 points, and none of the dropped points is more than 10 m from the simplified line. `crs=auto` draws the map in the UTM zone of its centre (32N here), so distances and shapes are true and the north arrow tilts with grid north. `center=` and `radius_km=` frame an area around a point.

**Claude.** *"How long and how steep was my hike in hike.gpx? Draw the elevation profile."* Claude calls `maps_open`, `maps_track_stats` and `maps_profile` without `path=`. The chart comes back as an image in the tool result, so nothing is written to disk.

## Task 5: rasters: GeoTIFFs and pictures with world files

**Batch** (`rasters.txt`), using the files written in Task 3:

```text
# rasters.txt: a GeoTIFF of values and a picture placed by its world file
maps.open path=out/heat.tif -> m
maps.raster_sample doc=$m points=48.8541,2.3326,48.8640,2.3620,48.8800,2.3000
maps.style doc=$m layer=heat raster_ramp=viridis
maps.import doc=$m path=cafes.csv
maps.render doc=$m path=heat-viridis.png width=700 basemap=none
maps.new title="World file" -> w
maps.add_countries doc=$w where="iso_a3 == 'FRA'"
maps.raster_add doc=$w path=out/heat.png opacity=0.8
maps.info doc=$w
maps.render doc=$w path=placed.png width=700 center=48.857,2.345 radius_km=3
```

```text
opened out/heat.tif as map1: 1 layer, 0 features, 1 raster
0: heat (400x344 value grid (0 to 296.078), Web Mercator)
...
48.8541,2.3326: 282.176
48.864,2.362: 101.628
48.88,2.3: no data
...
added raster heat: 400x344 image, Web Mercator, extent 2.309408,48.837008,2.381192,48.877626
...
```

A single-band GeoTIFF (elevation, density, temperature) keeps its values. You can sample them, colour them with `raster_ramp=` (viridis, terrain, heat, gray, blues, reds, greens, spectral, or two colours `#rrggbb,#rrggbb`) and shade them with `hillshade=true`. RGB(A) GeoTIFFs and PNG/JPG images are drawn as pictures. `GDAL_NODATA` values are transparent.

A PNG/JPG is placed by its world file (`.pgw`, `.jgw`, `.pngw`, `.jpgw`, `.wld`). The world file's coordinate system comes from a `.prj` next to it, or from `crs=`. For an image with no world file, such as a scanned map, give its corners: `maps.raster_add doc=$m path=scan.jpg bbox=6.85,45.90,6.93,45.95`. Rasters are drawn in the map's projection, whatever their own.

**CLI.**

```sh
$ wetwaresuite maps.open path=out/heat.tif
opened out/heat.tif as map1: 1 layer, 0 features, 1 raster
0: heat (400x344 value grid (0 to 296.078), Web Mercator)
```

**Claude.** *"What's the elevation at these three summits in dem.tif?"* Claude calls `maps_open path=dem.tif`, then `maps_raster_sample` with the points. *"Overlay this scan of an old map on the country outlines; its corners are ..."* `maps_raster_add` with `bbox=`, `opacity=0.6`, then `maps_preview`.

## Task 6: converting between formats

**CLI.** `maps.open` reads any supported file. Conversions are two lines piped into batch:

```sh
$ wetwaresuite maps.open path=out/walk.shp
opened out/walk.shp as map1: 1 layer, 1 feature
0: walk (1 polygon)
   fields: count
...
bbox: 2.313517,48.839704,2.377084,48.874898
$ printf 'maps.open path=out/walk.shp -> m\nmaps.export doc=$m path=out/walk.geojson\n' | wetwaresuite batch
...
wrote 1 feature of walk to out/walk.geojson
$ printf 'maps.open path=hike.gpx -> m\nmaps.export doc=$m layer="hike tracks" path=out/hike.kml\nmaps.export doc=$m layer="hike waypoints" path=out/waypoints.csv\n' | wetwaresuite batch
...
wrote 1 feature of hike tracks to out/hike.kml
...
wrote 2 features of hike waypoints to out/waypoints.csv
```

The shapefile's UTM `.prj` was read, and the polygon came back in lon/lat.

A CSV of projected coordinates (`stations.csv`, semicolon-separated, UTM 32N easting/northing):

```text
station;easting;northing;elev
Chamonix;334180;5085090;1035
...
```

```sh
$ printf 'maps.new -> m\nmaps.import doc=$m path=stations.csv crs=utm32n lat=northing lon=easting\nmaps.features doc=$m\nmaps.export doc=$m path=out/stations.geojson precision=5\n' | wetwaresuite batch
...
imported 3 features into stations (3 points)
...
#0 point 45.899381,6.862373 (lat,lon) {elev=1035; station=Chamonix}
#1 point 45.955753,6.947311 (lat,lon) {elev=1252; station=Argentière}
#2 point 45.855118,6.778539 (lat,lon) {elev=1008; station=Les Houches}
...
wrote 3 features of stations to out/stations.geojson
```

**Batch.** The tracks of every GPX in a folder to GeoJSON, with a shell loop (see [automate](automate.md) for pipelines). `layer=0` is the bottom layer, which for a GPX file is the tracks:

```sh
for f in *.gpx; do
  printf 'maps.open path=%s -> m\nmaps.export doc=$m layer=0 path=%s.geojson\n' "$f" "${f%.gpx}" | wetwaresuite batch
done
```

**Claude.** *"Convert parcels.shp to GeoJSON in lat/lon, and tell me what fields it has."* `maps_open` (the summary lists the fields), then `maps_export`.

| format | read | write |
| --- | --- | --- |
| GeoJSON `.geojson` `.json` | Feature, FeatureCollection, bare geometries, the old `crs` member (EPSG:3857 / UTM), `coordTimes` | RFC 7946, `precision=`, per-vertex times as `coordTimes`, `crs=` reprojects (and adds a `crs` member) |
| KML `.kml` | Placemarks in Folders/Documents, Point/LineString/Polygon/MultiGeometry, `gx:Track`, ExtendedData, `name`/`description` | Placemarks with ExtendedData, the layer's colours as styles |
| GPX `.gpx` | tracks (with `ele` and `time`), routes, waypoints, as separate layers | points as waypoints, lines as tracks, with elevation and times |
| CSV `.csv` `.tsv` `.txt` | lat/lon columns (auto-detected, DMS and N/S/E/W), a WKT column, projected x/y with `crs=`, any delimiter | lat/lon columns for point layers, a `wkt` column otherwise |
| Shapefile `.shp` | Point, MultiPoint, PolyLine, Polygon and their Z/M variants, `.dbf` (with `.cpg` or Latin-1), `.prj` (WGS84, Web Mercator, UTM) | `.shp/.shx/.dbf/.prj/.cpg`; mixed layers write `name_points`, `name_lines`, `name_polygons` sets; `crs=` |
| WKT `.wkt` | one geometry per line, Z ignored | one geometry per line |
| `.wwmap` | the whole map with styles and rasters (`maps.open`; older `.achmap` files too) | `maps.save` |
| GeoTIFF `.tif` | strips or tiles; none/LZW/Deflate/PackBits; predictors; 8/16/32-bit int and float; EPSG 4326, 3857, UTM | `maps.raster_save`: float32 values or RGBA, Deflate, GeoKeys |
| PNG/JPG + world file | `.pgw` `.jgw` `.pngw` `.jpgw` `.wld` and `.prj`, or `bbox=` | `maps.raster_save` writes the image, `.pgw`/`.jgw` and `.prj` |

## Task 7: questions about places

**Batch** (`places.txt`):

```text
# places.txt: asking questions of a country layer
maps.new title="Europe" -> m
maps.add_countries doc=$m where="continent == 'Europe'"
maps.contains doc=$m lat=46.95 lon=7.45
maps.nearest doc=$m lat=59.0 lon=-1.0 count=2
maps.calc doc=$m field=area_km2 expr="round($area / 1e6)"
maps.calc doc=$m field=density expr="round(pop_est / area_km2, 1)"
maps.features doc=$m where="density > 200" fields=name,density,area_km2
maps.bbox doc=$m where="name in ('Spain', 'Portugal')"
maps.import doc=$m path=cafes.csv
maps.spatial_join doc=$m target=cafes join=countries fields=name,iso_a3 prefix=country_ count_field=cafes
maps.features doc=$m layer=cafes limit=2 fields=name,country_name
maps.features doc=$m layer=countries where="cafes > 0" fields=name,cafes
maps.layer_remove doc=$m layer=cafes
maps.clip doc=$m layer=countries bbox=-11,35,32,71
maps.simplify doc=$m layer=countries tolerance=20000 output=simple
maps.style doc=$m layer=simple visible=false
maps.style doc=$m layer=countries mode=graduated field=density ramp=viridis classes=6 method=quantile label=iso_a3 label_size=8
maps.render doc=$m path=europe.png width=900 crs=auto graticule=true
maps.render doc=$m path=europe.svg width=900 crs=auto
maps.export doc=$m layer=simple path=out/europe-simple.geojson precision=3
```

```text
added 39 countries as countries
...
46.95,7.45 is inside 1 feature of countries
#19 {continent=Europe; iso_a3=CHE; name=Switzerland; pop_est=8574832}
...
nearest in countries to 59,-1:
#28 122.8 km (at 58.635,-3.005) {continent=Europe; iso_a3=GBR; name=United Kingdom; pop_est=66834405}
#1 366.4 km (at 59.663,5.308) {continent=Europe; iso_a3=NOR; name=Norway; pop_est=5347896}
...
countries: 5 features match
#14 polygon, 357427.5 km² {area_km2=357427; density=232.6; name=Germany}
#20 polygon, 2416.5 km² {area_km2=2417; density=256.5; name=Luxembourg}
#21 polygon, 30135 km² {area_km2=30135; density=381.1; name=Belgium}
#22 polygon, 40031.2 km² {area_km2=40031; density=433; name=Netherlands}
#28 2 polygons, 249994.9 km² {area_km2=249995; density=267.3; name=United Kingdom}
...
map: -9.527,35.947,3.039,43.748 (min_lon,min_lat,max_lon,max_lat), about 1075.2 km x 866.2 km, centre in UTM 30N
...
15 of 15 features of cafes fall inside countries
...
#0 point 48.8541,2.3326 (lat,lon) {country_name=France; name=Café de Flore}
...
#2 3 polygons, 644858.8 km² {cafes=15; name=France}
...
clipped countries to -11,35,32,71: 38 of 39 features kept
...
simplified simple: 1296 -> 681 vertices
...
rendered 900x970 png to europe.png
warning: a transverse Mercator/UTM view this wide distorts a lot; webmercator or wgs84 suit large areas
```

The areas are of the 1:110m outlines, so they are only roughly right. France's polygons include French Guiana, and the clip cuts it off. `maps.spatial_join mode=within` gives each café the fields of the country it lies in and counts cafés per country. `mode=nearest` copies the fields of the nearest feature instead, plus a distance in metres (`max_distance=` limits it). `maps.clip` cuts lines and polygons at the box edges. The UTM warning is advice, not an error: a 43° wide view in UTM 32N still looks fine, but scale varies across it.

**CLI.** Which country is a point in:

```sh
$ printf 'maps.new -> m\nmaps.add_countries doc=$m\nmaps.contains doc=$m lat=-33.87 lon=151.21\n' | wetwaresuite batch
```

**Claude.** *"Which European countries have more than 200 people per km²? Map population density across Europe."* Claude calls `maps_add_countries` (`where="continent == 'Europe'"`), `maps_calc` twice, and `maps_features` with `where="density > 200"`. It then styles with `maps_style mode=graduated field=density` and checks the result with `maps_preview crs=auto`.

## Task 8: street maps with no internet (map packs)

The built-in countries are fine for world and country maps. For towns, roads and streets, install a **map pack**. A map pack is one big file of map tiles (a `.pmtiles` file) that sits on your disk. The maps app reads it straight from there, so it works on a computer that is never online. Map packs come on the [offline editions](offline-editions.md) (the USB-stick versions of wetwaresuite):

| pack | shows | size |
| --- | --- | --- |
| `maps-world-low` | countries, big cities, coastlines, lakes (zoom 0-6) | about 250 MB |
| `maps-world-mid` | towns and main roads (zoom 0-10) | about 6 GB |
| `maps-world-high` | streets in most places (zoom 0-12) | about 30 GB |
| `maps-planet` | the full map, down to buildings and footpaths (zoom 0-15) | about 125 GB |

The map data is from OpenStreetMap, cut by Protomaps. It needs credit: the maps app prints the credit in the corner of every map it draws.

### Step 1: install a map pack

1. Plug in the USB stick with the offline edition (or unpack the edition's archive).
2. Open a terminal (on Windows: the Start menu, type `cmd`, press Enter).
3. Run the hub that's on the stick, and ask for the maps app. Its map pack comes along:

   ```sh
   /media/usb/wetwaresuite-32gb/bin/hub install maps --from /media/usb/wetwaresuite-32gb
   ```

   On Windows, with the stick as drive E:

   ```bat
   E:\wetwaresuite-32gb\bin\hub.exe install maps --from E:\wetwaresuite-32gb
   ```

   Nothing is downloaded. The pack is copied into your wetwaresuite folder, under `packs/` (`~/.local/share/wetwaresuite/packs` on Linux, `~/Library/Application Support/wetwaresuite/packs` on a Mac, `%APPDATA%\wetwaresuite\packs` on Windows).
4. Check that the maps app can see it:

   ```sh
   wetwaresuite maps.tiles_info
   ```

   It answers with something like this (your folder and numbers will differ):

   ```text
   /home/me/.local/share/wetwaresuite/packs/maps-world-mid/world.pmtiles: mvt tiles (vector), zoom 0-10, area -180,-85.05113,180,85.05113 (lon,lat), ... tiles, gzip compressed
   layers: boundaries,buildings,earth,landcover,landuse,places,pois,roads,water
   credit: Protomaps © OpenStreetMap
   ```

   If it says *no map pack is installed*, the install in step 3 didn't include one. Run it again, and check that the edition has a `packs/maps-world-...` folder.

### Step 2: open a map

With a pack installed you don't have to change anything: the default background, `basemap=auto`, uses the most detailed pack you have and only falls back to the built-in countries when there is none.

**In the window** ([the wetwaresuite window](gui.md)): open a map file (for example `cafes.csv`), then search for **Preview Map** or **Render Map**. Leave **Basemap** as `auto`, set **Center** to a place (`48.857,2.342` for Paris) and **Radius km** to `2`, and press **Run**. To use a particular map file, type its path in **Basemap**, for example `E:\maps\france.pmtiles`.

**Batch** (`pack.txt`):

```text
# pack.txt: the cafés over an offline street map
maps.open path=cafes.csv -> m
maps.style doc=$m label=name fill=#c0392b
maps.render doc=$m path=paris-pack.png width=800
maps.render doc=$m path=paris-pack.pdf width=800 basemap=pack
maps.render doc=$m path=paris-file.png width=800 basemap=/media/usb/wetwaresuite-32gb/packs/maps-world-mid/world.pmtiles
```

The first render prints something like:

```text
rendered 800x654 png to paris-pack.png
map pack: /home/me/.local/share/wetwaresuite/packs/maps-world-mid/world.pmtiles, zoom 10, 12 tiles (Protomaps © OpenStreetMap)
warning: zoomed in past the map pack's detail (zoom 10); a bigger pack shows more
...
```

**CLI.**

```sh
$ printf 'maps.open path=cafes.csv -> m\nmaps.render doc=$m path=paris.png center=48.857,2.342 radius_km=2\n' | wetwaresuite batch
```

**Claude.** *"Show the cafés on a street map."* Claude calls `maps_preview` (the installed pack is used by itself) or `maps_tiles_info` first to see what area and zoom levels your packs cover. Nothing goes online.

How it works:

- `basemap=` takes `auto` (the default: the most detailed installed pack, else the countries), `pack` (the most detailed installed pack, with a plain warning if there is none), the path of any `.pmtiles` file, `countries`, `none` or `osm` (Task 9).
- Running the apps straight from the stick without installing? The packs on the stick aren't in your wetwaresuite folder, so give the file: `basemap=E:\wetwaresuite-32gb\packs\maps-world-low\world.pmtiles`.
- The tiles are **vector tiles** (Mapbox Vector Tiles, the Protomaps basemap layers): the app draws them itself with a built-in light style. Land, water, parks, woods and other land use are filled, buildings are shown when zoomed in, roads get colours and widths by type and zoom, borders are dashed, and towns and countries are labelled. Because they are drawn, not pictures, `.svg` and `.pdf` renders stay sharp.
- `.pmtiles` files of **pictures** (PNG or JPEG tiles, for example aerial photos or a scanned map you packed yourself) work too. They are placed like the `tile_url=` folder tiles in Task 9. WebP and AVIF tiles can't be shown.
- The zoom level follows the map scale and drops until the view needs at most `max_tiles=` tiles (64 by default). Zoomed in past the pack's deepest level, the app keeps drawing that level bigger, and tells you a bigger pack would show more. Places outside a pack's area stay blank, with a warning.
- **MBTiles** files are SQLite databases, which the maps app doesn't read. Convert one once, on any computer, with the free [`pmtiles`](https://github.com/protomaps/go-pmtiles/releases) tool: `pmtiles convert in.mbtiles out.pmtiles`. Then use `out.pmtiles`. (Or unpack it to a `{z}/{x}/{y}.png` folder for `tile_url=`, see Task 9.)
- Only gzip-compressed or uncompressed `.pmtiles` files can be read. The map packs are gzip. A file made with brotli or zstd compression gives a plain message saying so.

## Task 9: OpenStreetMap tiles

For the latest online map, `basemap=osm` draws OpenStreetMap tiles under your layers. This is the only background that can use the internet, and only when you ask for it:

```text
# osm.txt: the cafés over OpenStreetMap tiles
maps.open path=cafes.csv -> m
maps.style doc=$m label=name fill=#c0392b
maps.render doc=$m path=paris-osm.png width=800 basemap=osm tile_cache=tiles
maps.render doc=$m path=paris-offline.png width=800 basemap=osm tile_cache=tiles offline=true
```

How it works:

- Tiles are downloaded with `curl`, which must be on your PATH. They are fetched one at a time, with a `User-Agent` naming wetwaresuite, as the [OSM tile usage policy](https://operations.osmfoundation.org/policies/tiles/) asks.
- Tiles are cached under the user cache folder (`~/.cache/wetwaresuite/tiles`, `~/Library/Caches/wetwaresuite/tiles` or `%LOCALAPPDATA%\wetwaresuite\cache\tiles`), or under `tile_cache=`. They are reused for 30 days. `offline=true` never downloads and uses whatever is cached.
- The zoom level follows the map scale and drops until the view needs at most `max_tiles=` tiles (64 by default). This keeps one render from fetching hundreds of tiles.
- After the first failed download, the render stops trying, draws the missing area blank and warns you. The render still succeeds.
- "© OpenStreetMap contributors" is printed on the map, as the licence requires.
- `tile_url=` points at another server that uses `{z}/{x}/{y}` (for example your own tile server). Respect its usage terms.
- `tile_url=` can also be a **folder of tiles on your disk**, for example `tile_url=D:/maps/tiles/{z}/{x}/{y}.png` (tiles you exported from another program, copied from a friend, or kept from an earlier `tile_cache=`). Those are read straight from the folder and never downloaded, with or without `offline=true`. `tile_url=` can be a `.pmtiles` file too (same as `basemap=` with that file, Task 8). MBTiles files are SQLite databases, which the app doesn't read: convert one with `pmtiles convert in.mbtiles out.pmtiles` (Task 8), or unpack it to a `{z}/{x}/{y}.png` folder with a tool such as `mb-util`, making sure it writes XYZ rows (counted from the top), not TMS rows (counted from the bottom).

The run for this tutorial had no access to tile.openstreetmap.org, so it shows the offline behaviour:

```text
rendered 800x654 png to paris-osm.png
OSM tiles: zoom 14, 0 used, 0 downloaded (map data © OpenStreetMap contributors, ODbL)
warning: tile download failed (curl: (22) The requested URL returned error: 403); offline areas stay blank
warning: 12 OSM tiles weren't available
...
rendered 800x654 png to paris-offline.png
OSM tiles: zoom 14, 0 used, 0 downloaded (map data © OpenStreetMap contributors, ODbL)
warning: 12 OSM tiles weren't available (offline=true uses cached tiles only)
```

OSM tiles are Web Mercator pictures, so `basemap=osm` always renders in Web Mercator.

**CLI.**

```sh
$ printf 'maps.open path=cafes.csv -> m\nmaps.render doc=$m path=paris-osm.png basemap=osm\n' | wetwaresuite batch
```

**Claude.** *"Show the cafés on a street map."* Claude calls `maps_preview` with `basemap=osm`. Only ask for this when you are online and happy to fetch tiles: by default everything Claude does with maps stays offline.

## Task 10: routes and search on an OpenStreetMap export, offline

`maps` plans routes along any layer of lines that look like roads, with no server and no internet. The easiest source is an OpenStreetMap export: in a browser, the **Export** button on openstreetmap.org saves the area you are looking at as a `.osm` file (or use any `.osm` file another tool cut for you). `maps.open` reads it as three layers: roads (every `highway=*` way), areas (closed ways and multipolygons such as parks and buildings) and places (tagged nodes such as shops and stations).

The examples in Tasks 10 to 14 use three more made-up files: `oldtown.osm` (a 400 m grid of streets at 47° N 8° E with a one-way Church Street, a 40 km/h High Street, a footway, a path across a park, two buildings and four places), `morning.gpx` (a 1.6 km run with a 12 minute stop) and `ride.fit` (a 2 minute bike ride saved by a sports watch).

**Batch** (`route.txt`):

```text
# route.txt: routes through a town from an OpenStreetMap export, offline
maps.open path=oldtown.osm -> m
maps.search doc=$m query="cafe marche"
maps.search doc=$m query="bakery"
maps.route doc=$m from=47.0000,8.0000 to=47.0022,8.0020
maps.route doc=$m from=47.0000,8.0000 to=47.0009,8.0026 profile=foot
maps.route doc=$m from=47.0009,8.0040 to=47.0009,8.0005
maps.route doc=$m from=47.0000,8.0000 to=47.0036,8.0053 via=47.0022,8.0020 profile=bike
maps.reach doc=$m from=47.0018,8.0026 minutes=2 profile=foot buffer=25
maps.style doc=$m layer="oldtown places" label=name
maps.render doc=$m path=town.png width=700
maps.export doc=$m layer=route path=routes.gpx
```

```text
opened oldtown.osm as map1: 3 layers, 18 features
0: oldtown areas (3 polygons)
   fields: leisure, name, osm_id, building
1: oldtown roads (11 lines)
   fields: highway, name, osm_id, oneway, maxspeed
2: oldtown places (4 points)
   fields: amenity, name, osm_id, shop, railway
...
1 match for 'cafe marche'
1. Café du Marché (oldtown places #0) at 47.002248,8.001978
...
2 matches for 'bakery'
1. Old Mill Bakery (oldtown places #1) at 47.002158,8.004352
2. Baker Lane (oldtown roads #5) at 47.001799,8
...
car route on oldtown roads: 351.66 m, 0:00:39 (quickest), 3 steps
1. Head north on Baker Lane (200 m)
2. Turn right onto High Street (151.66 m)
3. Arrive at your destination
...
foot route on oldtown roads: 226.43 m, 0:02:43 (quickest), 2 steps
1. Head north-east on Park Path (226.43 m)
2. Arrive at your destination
...
car route on oldtown roads: 734.55 m, 0:01:20 (quickest), 6 steps
1. Head east on Church Street (96.66 m)
2. Turn left onto Hill Road (100 m)
3. Turn left onto High Street (399.98 m)
4. Turn left onto Baker Lane (100 m)
5. Turn left onto Church Street (37.92 m)
6. Arrive at your destination
...
bike route on oldtown roads: 799.98 m, 0:03:03 (quickest), 9 steps
1. Head north on Baker Lane (100 m)
2. Turn right onto Church Street (100 m)
3. Turn left onto School Road (100 m)
4. Turn right onto High Street (51.66 m)
5. Reach stop 1
6. Continue onto High Street (48.33 m)
7. Turn left onto Station Road (200.01 m)
8. Turn right onto River Lane (199.99 m)
9. Arrive at your destination
...
reachable within 2 min by foot: 5.585 ha -> reach
...
rendered 700x700 png to town.png
...
wrote 4 features of route to routes.gpx
```

What happened:

- **Search** ignores case and accents (`cafe marche` finds `Café du Marché`), matches the start of words, and forgives a typo or two in longer words. Names rank first, then other fields (`shop=bakery` found the bakery, `Baker Lane` came second). Each result gives `lat`/`lon`, so the next command can start or end a route there.
- **Routes** start and end at the nearest point on a road (up to `snap=` 5 km away), not only at junctions. `profile=car` keeps to roads cars may use, follows `oneway=yes` and drives at `maxspeed=` or a typical speed for the road type. `profile=foot` and `profile=bike` may use footways and paths and ride or walk at their own speeds. Bikes follow one-way streets unless `oneway:bicycle=no` or a contraflow `cycleway=opposite` says otherwise; walkers ignore them. `shortest=true` minimises distance instead of time. The third route starts on one-way Church Street heading west, so the car goes round the block.
- `via=` adds stops (`lat,lon` pairs, in order). Each route is one feature of the `route` layer (or `output=`) with `profile`, `distance_m`, `duration_s`, `from` and `to` as properties, so `maps.export` writes them as GPX for a phone or a bike computer. The turn-by-turn steps are in the message.
- **`maps.reach`** draws the area you can reach within `minutes=` (an isochrone): the roads reachable in time, `buffer=` metres either side.

The roads layer is found by itself (the top layer with `highway` tags, else the top layer with lines), or name it with `layer=`. Any line layer works, for example a shapefile of footpaths: lines that share an end point or vertex (within 5 cm) are joined.

**CLI.** Open and route in one short batch:

```sh
$ printf 'maps.open path=oldtown.osm -> m\nmaps.route doc=$m from=47.0000,8.0000 to=47.0022,8.0020 profile=foot\n' | wetwaresuite batch
...
foot route on oldtown roads: 351.66 m, 0:04:17 (quickest), 5 steps
1. Head north on Baker Lane (100 m)
2. Turn right onto Church Street (100 m)
3. Turn left onto School Road (100 m)
4. Turn right onto High Street (51.66 m)
5. Arrive at your destination
...
```

On a grid, many ways are equally short, and the router takes any of them: it has no penalty for turning, so a walk can zigzag where a person would take one corner.

**Claude.** *"In oldtown.osm, how do I walk from the station to the bakery, and what's within a 5 minute walk of the café?"* Claude calls `maps_open`, `maps_search` twice for the two places, `maps_route profile=foot` between their `lat`/`lon`, then `maps_reach minutes=5` and `maps_preview`.

## Task 11: GPS tracks: splits, cleaning, cutting and FIT files

**Batch** (`run.txt`):

```text
# run.txt: splits, cleaning and cutting a GPS track
maps.open path=morning.gpx -> m
maps.track_stats doc=$m
maps.track_splits doc=$m every=500 path=splits.csv
maps.track_smooth doc=$m window=5 max_kmh=30
maps.track_stats doc=$m
maps.track_split doc=$m gap_minutes=5
maps.features doc=$m fields=name,part
maps.track_join doc=$m
maps.track_trim doc=$m from=100 to=-100
maps.profile doc=$m path=run-profile.png title="Morning run"
maps.save doc=$m path=run.wwmap
```

```text
...
morning: 1.602 km over 105 points
elevation: +48.3 m / -45 m, 429.4 to 478.6 m
time: 2026-09-14T07:00:00Z to 2026-09-14T07:20:40Z, 0:20:40 total, 0:08:35 moving
speed: average 4.65 km/h, moving 11.09 km/h, max 11.11 km/h, pace 5:24 min/km
...
morning: 4 splits of 500 m
1: 500 m  0:02:42  5:24 min/km  11.1 km/h  +11/-0 m
2: 1 km  0:14:42  29:25 min/km  2 km/h  +37/-0 m
3: 1.5 km  0:02:42  5:25 min/km  11.1 km/h  +0/-34 m
4: 1.602 km  0:00:33  5:25 min/km  11.1 km/h  +0/-11 m
fastest: split 1 (5:24 min/km), slowest: split 2 (29:25 min/km)
wrote splits.csv
...
smoothed 1 track of morning; raw climb 63 m -> 51 m
...
morning: 1.56 km over 105 points
elevation: +47.5 m / -47 m, 429.8 to 478.2 m
...
split 1 track of morning into 2 parts
...
morning: 2 features
#0 line, 927.37 m {name=Morning run; part=1}
#1 line, 617.35 m {name=Morning run; part=2}
...
joined 2 track parts in time order into #0 of morning: 1.56 km
...
trimmed #0 of morning to 100 m - 1.46 km: 1.36 km (was 1.56 km)
...
profile of morning: 1.36 km, 93 points -> run-profile.png
...
saved run.wwmap
```

`splits.csv` has one row per split:

```text
split,from_m,to_m,distance_m,time_s,pace_min_km,speed_kmh,gain_m,loss_m
1,0,500,500,162.1,5:24,11.1,11.5,0
2,500,1000,500,882.3,29:25,2.04,36.8,0
3,1000,1500,500,162.3,5:25,11.09,0,33.6
4,1500,1602.1,102.1,33.2,5:25,11.07,0,11.4
```

What each command does:

- **`maps.track_splits`** cuts a track into `every=` metres (default 1000) and gives time, pace, speed, climb and descent for each, like the laps of a running app. Split 2 holds the 12 minute stop.
- **`maps.track_smooth`** first drops spikes (points that would need more than `max_kmh=` to reach), then averages positions and elevations over `window=` points. The raw climb (every rise summed) fell from 63 m to 51 m. Smoothing also rounds the corners, which is why the track got 42 m shorter: use a small window on twisty tracks.
- **`maps.track_split`** cuts a track where the time between two points is more than `gap_minutes=`, or `at=lat,lon`, or at `distance=` metres. Each part gets a `part` number. **`maps.track_join`** puts parts (or any lines, `indexes=`) back together in time order. **`maps.track_trim`** keeps the stretch between `from=` and `to=` metres; negative values count from the end, so `from=100 to=-100` drops the first and last 100 m (handy for hiding where you live before sharing a track). **`maps.track_reverse`** turns a line round.
- **`maps.track_elevation`** replaces a track's elevations with values read from an elevation GeoTIFF added with `maps.raster_add`, for files with no or poor GPS heights.

**FIT files** from Garmin, Wahoo, Coros and other sports watches open like GPX (`ride.txt`):

```text
# ride.txt: a FIT file from a sports watch
maps.open path=ride.fit -> r
maps.features doc=$r
maps.track_stats doc=$r
maps.export doc=$r path=ride.gpx
```

```text
opened ride.fit as map1: 1 layer, 1 feature
0: ride (1 line)
   fields: avg_hr, max_hr, sport
...
#0 line, 401.89 m {avg_hr=132; max_hr=147; sport=cycling}
...
ride: 401.89 m over 121 points
elevation: +0 m / -0 m, 453 to 456.8 m
time: 2025-09-05T09:46:40Z to 2025-09-05T09:48:40Z, 0:02:00 total, 0:02:00 moving
speed: average 12.06 km/h, moving 12.06 km/h, max 12.08 km/h, pace 4:58 min/km
...
wrote 1 feature of ride to ride.gpx
```

The climb is 0 because the elevation never rose more than the default 3 m `hysteresis=` (Task 4). Heart rate is kept as the average and maximum of the activity, not per point.

**Claude.** *"Here's my run from this morning (morning.gpx). What were my kilometre splits? Cut off the first and last 200 m and save it as GPX so I can share it."* Claude calls `maps_open`, `maps_track_splits`, `maps_track_trim from=200 to=-200` and `maps_export path=morning-shared.gpx`.

## Task 12: drawing and fixing features

Features can be drawn and corrected point by point, and every change is one undo step.

**Batch** (`edit.txt`):

```text
# edit.txt: draw and fix features by hand
maps.new -> m
maps.feature_add doc=$m layer=paths points=47.0000,8.0000,47.0009,8.0013,47.0018,8.0013 props="name=Park loop; surface=gravel"
maps.feature_add doc=$m layer=paths points=47.0018,8.0013,47.0018,8.0040 props="name=Hill link"
maps.vertices doc=$m layer=paths index=0
maps.vertex_add doc=$m layer=paths index=0 at=47.0005,8.0010
maps.vertex_move doc=$m layer=paths index=0 vertex=3 to=47.0018,8.0014
maps.vertex_delete doc=$m layer=paths index=0 vertex=1
maps.merge doc=$m layer=paths indexes=0,1 snap=10 props="name=Park and hill"
maps.split doc=$m layer=paths index=0 distance=250
maps.feature_update doc=$m layer=paths index=1 props="surface=asphalt; lit=true"
maps.feature_move doc=$m layer=paths index=1 dy=20
maps.features doc=$m layer=paths
undo
maps.vertices doc=$m layer=paths index=1
```

```text
...
added #0 to paths: line, 240.72 m
...
added #1 to paths: line, 205.34 m
...
#0 of paths: 3 vertices
0: 47,8
1: 47.0009,8.0013
2: 47.0018,8.0013
...
added vertex 1 to #0 of paths at 47.0005,8.001
...
moved vertex 3 of #0 by 7.61 m
...
deleted vertex 1 of #0 (3 vertices left)
...
merged 2 features of paths into #0 (1 part, line, 438.75 m)
...
split #0 of paths at 47.0018,8.001518: 250 m + 188.75 m (now #0 and #1)
...
updated 1 feature of paths: lit=true; surface=asphalt
...
moved #1 of paths, now centred at 47.00198,8.002759 (lat,lon)
...
paths: 2 features
#0 line, 250 m {name=Park and hill}
#1 line, 188.75 m {lit=true; name=Park and hill; surface=asphalt}
...
undone
#1 of paths: 2 vertices
0: 47.0018,8.001518
1: 47.0018,8.004
```

- `maps.vertices` numbers a feature's points (through every part and ring). `maps.vertex_move`, `maps.vertex_add` (after a vertex, or without `after=` into the nearest edge) and `maps.vertex_delete` use those numbers.
- `maps.merge` joins lines end to end where they meet within `snap=` metres (turning one round when needed); polygons become one multipolygon. `maps.split` cuts a line at `at=lat,lon` or `distance=` metres; both halves keep the properties.
- `maps.feature_update` edits the attribute table: one feature, `indexes=` or `where=`, and `unset=` removes fields. `maps.feature_move` moves a whole feature by `dx=`/`dy=` metres or `to=lat,lon`, keeping its size on the ground.
- `undo` took back the move: the second half is on the line again.

**Claude.** *"In paths.geojson, the Hill link should end at the church at 47.0018, 8.0042, and mark it as lit."* Claude calls `maps_open`, `maps_search query="Hill link"` (or `maps_features`), `maps_vertices`, `maps_vertex_move` and `maps_feature_update props="lit=true"`, then `maps_export`.

## Task 13: overlay, dissolve and select by location

**Batch** (`overlay.txt`):

```text
# overlay.txt: dissolve, overlay and select by location on the built-in countries
maps.new -> m
maps.add_countries doc=$m
maps.dissolve doc=$m layer=countries field=continent sum=pop_est where="continent == 'Europe' or continent == 'Africa'" output=continents
maps.features doc=$m layer=continents fields=continent,count,pop_est
maps.feature_add doc=$m layer=city lat=47.37 lon=8.54 props="name=Zurich"
maps.buffer doc=$m layer=city distance=150000 output="300 km circle"
maps.overlay doc=$m layer=countries with="300 km circle" op=intersection
maps.features doc=$m layer="countries intersection" fields=name
maps.select_location doc=$m layer=countries by=city mode=near distance=200000
maps.features doc=$m layer="countries selected" fields=name
maps.render doc=$m path=zurich.png center=47.37,8.54 radius_km=250 width=600
```

```text
...
dissolved countries by continent into continents (2 features)
...
continents: 2 features
#0 2 polygons, 29944859.9 km² {continent=Africa; count=51; pop_est=1306370215.3}
#1 22 polygons, 23057993.3 km² {continent=Europe; count=39; pop_est=745412452}
...
buffered city by 150 km into 300 km circle (1 feature, 70569.1 km²)
...
intersection of countries with 300 km circle -> countries intersection (5 features, 70460.3 km²)
...
countries intersection: 5 features
#0 polygon, 6250.6 km² {name=France}
#1 polygon, 4093.6 km² {name=Austria}
#2 polygon, 22208.9 km² {name=Germany}
#3 polygon, 36406.9 km² {name=Switzerland}
#4 2 polygons, 1500.4 km² {name=Italy}
...
5 of 177 features near city -> countries selected
...
countries selected: 5 features
#0 3 polygons, 644858.8 km² {name=France}
...
rendered 600x603 png to zurich.png
```

- **`maps.dissolve`** merges features into one per value of `field=`, removing shared borders, with a `count` and the sum of each `sum=` field.
- **`maps.overlay`** cuts a layer with the polygons of another (`with=`): `intersection` (the parts inside, carrying both layers' fields), `difference` (the parts outside), `union` and `sym_difference`. The five pieces add up to 70,460 km² of the circle's 70,569 km²: the rest is lake water the 1:110m countries leave out, and the circle's corners: its 64 sides cut a little off a true circle.
- **`maps.select_location`** keeps the features that `intersects`, are `within`, `contains`, are `disjoint` from, or are `near` (within `distance=` metres of) a feature of `by=`.

**Claude.** *"How much of each country lies within 150 km of Zurich?"* Claude calls `maps_add_countries`, `maps_feature_add`, `maps_buffer`, `maps_overlay` and reads the `area_m2` of each piece from `maps_features`.

## Task 14: bookmarks and the saved view

A bookmark is a named area, saved in the `.wwmap`. The **view** is what the map window, `maps.preview` and `maps.render` show when you give no `bbox=`; it is saved too, so a map opens where you left it.

**Batch** (`places.txt`):

```text
# places.txt: bookmarks and the saved view
maps.open path=oldtown.osm -> m
maps.bookmark doc=$m name=Market center=47.0022,8.0020 radius_km=0.15
maps.bookmark doc=$m name=Town layer="oldtown roads"
maps.bookmarks doc=$m
maps.view doc=$m bookmark=Market
maps.view doc=$m zoom=2
maps.view doc=$m pan=0.5,0
maps.render doc=$m path=market.png width=500
maps.render doc=$m path=town-all.png bookmark=Town width=500
maps.bookmark_delete doc=$m name=Town
maps.view doc=$m reset=true
maps.hit doc=$m x=400 y=400
maps.save doc=$m path=oldtown.wwmap
```

```text
...
added bookmark Market: 8.000028,47.000851,8.003972,47.003549
...
2 bookmarks
Market: 8.000028,47.000851,8.003972,47.003549 (about 300 m across)
Town: 7.999684,46.999684,8.005591,47.003914 (about 449.29 m across)
...
showing 8.000028,47.000851,8.003972,47.003549 (about 300 m across)
...
showing 8.001014,47.001525,8.002986,47.002875 (about 150 m across)
...
showing 8.002,47.001525,8.003972,47.002875 (about 150 m across)
...
rendered 500x502 png to market.png
...
rendered 500x525 png to town-all.png
...
deleted bookmark Town
...
showing the whole map
...
oldtown roads #7 Station Road: line, 399.91 m, vertex 2 {highway=residential; name=Station Road; osm_id=8} at 47.001799,8.002637 (lat,lon)
...
saved oldtown.wwmap
```

`zoom=2` zooms in around the middle (or `at=lat,lon`), `pan=0.5,0` moves half a view east. `maps.render bookmark=` draws a bookmark without changing the view. `maps.hit` answers "what is at this pixel?" for the 800 px picture the window shows; the window's click tools use it.

## Task 15: the map window

`wetwaresuite gui` (or opening a `.wwmap`, `.gpx`, `.fit`, `.osm`, ... file) shows the map with a toolbar of click tools. Each tool runs the same commands as above, so everything you click can be undone and Claude sees the same map:

| tool | click | command it runs |
| --- | --- | --- |
| Pan | drag to move, scroll to zoom the picture | |
| Identify | a feature | `maps.hit` |
| Measure | points; **Area** button after 3 | `maps.measure` |
| Point, Line, Area | each corner; right-click or Enter finishes; the layer and `name=...` boxes set the layer and properties | `maps.feature_add` |
| Move | a point or a line's corner, then where it goes | `maps.feature_move`, `maps.vertex_move` |
| Delete | a feature (Alt+click a corner deletes just the corner) | `maps.feature_delete`, `maps.vertex_delete` |
| Route | the start, then the end; car, bike or foot | `maps.route` |

Below the tools: **Zoom in**, **Zoom out**, **Show all** and arrow buttons (`maps.view`), a **Bookmarks** list that jumps to a bookmark or adds the current view, and a search box that finds a place (`maps.search`) and centres the map on it. The preview is drawn at the window's shape, so a wide window shows more map left and right instead of a square picture with bars. Esc cancels a line, measurement or move in progress.

## Styling

`maps.style` changes only the parameters you pass:

| parameter | what it does |
| --- | --- |
| `stroke`, `width` | line and outline colour and width in px (`width=0` removes outlines) |
| `fill`, `fill_opacity` | polygon and point fill |
| `opacity` | the whole layer |
| `point_size`, `marker` | marker diameter in px; `circle`, `square`, `triangle`, `diamond` |
| `dash` | `solid`, `dash`, `dot` |
| `label`, `label_size`, `label_color`, `halo` | text from a field; labels that would overlap are skipped |
| `mode` | `single`, `categorized` (a colour per value of `field=`), `graduated` (classes of a numeric `field=`) |
| `field`, `classes`, `method`, `ramp`, `palette` | categorized and graduated settings; `method` is `quantile`, `equal` or `natural` (= `jenks`); `ramp` is a name or `#rrggbb,#rrggbb` |
| `size_by_value` | graduated points: bigger markers for bigger values |
| `raster_ramp`, `hillshade` | value rasters |
| `visible` | show or hide the layer |

Colours are `#rrggbb`, `#rgb`, `#rrggbbaa` or CSS names such as `red`. Layer order is the drawing order: `maps.layer_move layer=cafes to=top` (or `bottom`, `up`, `down`, or an index).

## Rendering

`maps.render` writes PNG/JPG (antialiased) and SVG/PDF (vector, with real text). `maps.preview` takes the same view options and returns a PNG without writing anything. That is what Claude uses to look at a map.

| parameter | default | what it does |
| --- | --- | --- |
| `path` | required | `.png`, `.jpg`, `.svg` or `.pdf` (`format=` overrides) |
| `width`, `height` | 1024, from the extent | size in px; PDF pages are sized at `dpi=` (96) |
| `crs` | `webmercator` | `webmercator`, `wgs84` (plate carrée), `utm33n`..., `EPSG:...`, or `auto` (UTM of the centre) |
| `bbox` | the saved view (Task 14), else the visible layers plus a margin | `min_lon,min_lat,max_lon,max_lat` |
| `bookmark` | | draw a bookmark's area |
| `center`, `radius_km` | | frame an area around `lat,lon` |
| `basemap` | `auto` | `auto` (an installed map pack, else countries), `countries`, `none`, `pack`, a `.pmtiles` file (all offline), `osm` (online) |
| `background`, `land` | `#dde8f0`, `#f4f1ea` | sea and land colours of the countries basemap (map packs use their own style) |
| `scale_bar`, `north_arrow`, `legend`, `labels` | `true` | map furniture; the scale bar is true at the centre |
| `graticule`, `graticule_step` | `false`, automatic | latitude/longitude lines with labels |
| `title` | the map's title | text across the top |
| `tile_url`, `tile_cache`, `offline`, `max_tiles` | | OSM tiles (Task 9); `max_tiles` also limits map packs |

## Command reference

`wetwaresuite help maps.<command>` prints every parameter. In brief:

| command | what it does |
| --- | --- |
| `maps.new`, `maps.open`, `maps.save`, `maps.close` | create, open any supported file or `.wwmap`, save the project, close |
| `maps.info`, `maps.features` | describe the map; list features with `where=`, `fields=`, `offset=`, `limit=` |
| `maps.import`, `maps.export` | add a file as layers; write one layer (`crs=`, `precision=`, `where=`) |
| `maps.add_countries` | the built-in Natural Earth countries (`where=` to pick some) |
| `maps.feature_add`, `maps.feature_delete` | add a point (`lat=` `lon=`), a WKT geometry or `points=` with `kind=line/polygon`, with `props="name=Home; visits=3"`; delete by `index=` or `where=` |
| `maps.feature_update`, `maps.feature_move` | edit attributes (`props=`, `unset=`, by `index=`, `indexes=` or `where=`); move by metres or `to=lat,lon` |
| `maps.vertices`, `maps.vertex_move`, `maps.vertex_add`, `maps.vertex_delete` | list and edit a feature's points (Task 12) |
| `maps.split`, `maps.merge` | cut a line in two; join lines end to end, polygons into a multipolygon |
| `maps.layer_move`, `maps.layer_rename`, `maps.layer_remove` | layer order and names |
| `maps.style` | see [styling](#styling) |
| `maps.measure`, `maps.transform`, `maps.bbox` | geodesic distance/length/area; coordinate conversion; extents |
| `maps.contains`, `maps.nearest` | point in polygon; nearest features by geodesic distance |
| `maps.filter`, `maps.calc` | keep matching features (`output=` or `in_place=true`); set or delete a field from an expression |
| `maps.join`, `maps.spatial_join` | attach CSV columns by key; copy fields by containment or nearness |
| `maps.buffer`, `maps.simplify`, `maps.clip` | grow or shrink by metres; Douglas-Peucker in metres; cut to a box |
| `maps.dissolve`, `maps.overlay`, `maps.select_location` | merge by a field; intersection, difference, union, sym_difference with another layer; select by intersects, within, contains, disjoint or near (Task 13) |
| `maps.heatmap` | point density raster; lines (tracks) count along their length every `spacing=` metres |
| `maps.track_stats`, `maps.profile` | GPS track statistics; elevation chart |
| `maps.track_splits`, `maps.track_smooth`, `maps.track_trim` | per-km (or `every=`) splits, optionally to CSV; drop spikes and smooth; cut the ends (Task 11) |
| `maps.track_split`, `maps.track_join`, `maps.track_reverse`, `maps.track_elevation` | cut at gaps or a point; join in time order; turn round; heights from a DEM raster |
| `maps.route`, `maps.reach`, `maps.search` | offline routing by car, bike or foot with `via=` stops; the area reachable in `minutes=`; find places by name (Task 10) |
| `maps.bookmark`, `maps.bookmarks`, `maps.bookmark_delete`, `maps.view` | named areas; what the window and renders show (Task 14) |
| `maps.hit` | what is at a pixel of the window's picture (used by the click tools) |
| `maps.raster_add`, `maps.raster_sample`, `maps.raster_save` | add a GeoTIFF or placed image; read values; write GeoTIFF or PNG + world file |
| `maps.render`, `maps.preview` | draw to a file; draw to an attachment |
| `maps.tiles_info` | describe a `.pmtiles` file or the installed map packs: area, zooms, format, layers, credit |

## MCP

`wetwaresuite mcp --tools maps` serves the 62 maps tools (`maps_new`, `maps_import`, ..., `maps_render`). Every edit takes `preview=true`, which attaches a PNG of the whole map to the result. `maps_preview` renders any view without writing a file, and `maps_profile` without `path=` returns the elevation chart as an image. Results carry the numbers as structured fields, so Claude can do arithmetic on distances or areas without parsing text.

Example prompts:

- *"Plot these addresses' coordinates from customers.csv and tell me which ones are more than 50 km from our warehouse at 52.52, 13.40."* `maps_open`, then `maps_feature_add layer=warehouse`, then `maps_spatial_join mode=nearest target=customers join=warehouse`, then `maps_features where="distance_m > 50000"`.
- *"Colour African countries by the GDP column in gdp.csv and export a PDF poster."* `maps_add_countries where="continent == 'Africa'"`, `maps_join`, `maps_style mode=graduated`, `maps_preview`, `maps_render path=africa.pdf width=2400`.
- *"Turn my Strava GPX into a KML for Google Earth and tell me the total climb."* `maps_open`, `maps_track_stats`, `maps_export path=ride.kml`.

## Limitations

- **Datum**: WGS84 only. ETRS89 (`EPSG:258xx`) is treated as WGS84, which is within a metre or so in Europe. NAD27, OSGB36, other datums and local grids such as British National Grid or Lambert are not supported. Files that use them import with a warning or need converting first.
- **Projections**: WGS84 lon/lat, Web Mercator, UTM, Equal Earth and Lambert azimuthal equal-area (plus a local transverse Mercator used internally). There are no conic projections. Transverse Mercator views are cut to about 75° of longitude either side of the central meridian, and Web Mercator to ±85°.
- **Buffers** and the polygon results of `maps.overlay` and `maps.dissolve` are exact on the edges up to about 600,000 edges (a buffer counts each vertex's circle). Buffers are worked out on a transverse Mercator grid centred on the layer, which stretches distances by about 0.1 % at 500 km from its centre, so a buffer of a whole continent is that far off at its edges. Overlays are planar in lon/lat, as the map draws them. Points are snapped to about 0.02 mm so shared corners match. Bigger inputs fall back to a grid of `resolution=` cells across the extent (600 for buffers, 800 for overlays), good to about one cell. Points and lines are always cut exactly.
- **Clip** cuts to a lon/lat rectangle. For polygons, use `maps.overlay`.
- **Routing** builds its road graph from the layer each time and keeps it in memory, which suits a town or a county, not a country. Roads join where they share a point (within 5 cm), so lines that cross without a shared vertex (bridges, or data drawn without snapping) are not connected. There are no turn restrictions, turn costs, traffic, ferries or elevation in the costs; speeds are typical values per road type unless `maxspeed=` is set. Only multipolygon relations are read from `.osm` files; route relations and turn restrictions are skipped.
- **Search** looks at the open map's features, the built-in countries and cities, and the place and point-of-interest names in installed map packs (read once per pack, every zoom level as deep as 20,000 tiles allows, then kept as a small file in the cache folder: a city pack gives every shop and café, a planet pack only towns and big places). There are no house numbers or street addresses, so it finds "Gare du Nord" but not "12 rue de Rivoli".
- **FIT**: positions, elevation, time, the sport and the average and maximum heart rate are read. Per-point heart rate, power, cadence, laps, developer fields and course files are not, and the checksum is not verified.
- **Areas** are on the authalic sphere (about 0.1 % from the ellipsoidal value), with polygon edges taken as great circles.
- **The built-in countries** are Natural Earth 1:110m with coordinates rounded to 0.001°. That is right for world and continent maps, but coarse at city scale (coastlines are kilometres off). Use a map pack (Task 8), your own data or `basemap=osm` for close-ups. Disputed borders follow Natural Earth's de-facto choices.
- **GeoTIFF**: no BigTIFF, no planar-configuration (separate band) files, no JPEG-compressed TIFFs, no overviews. Only the first image is read. GeoKeys are limited to EPSG 4326, 3857 and UTM codes; for anything else, pass `crs=` or the raster is placed by `bbox=`. Rasters are resampled with nearest neighbour.
- **KML**: no NetworkLinks, GroundOverlays, KMZ archives or 3D models; styles are not read. **GPX**: extensions such as heart rate are ignored. **Shapefiles**: no MultiPatch; M values are dropped; `.dbf` fields are written as text, numbers or logicals, with names cut to 10 characters.
- **Feature properties** are kept in alphabetical order of field name, so CSV and DBF exports may reorder columns.
- **OpenStreetMap tiles** need `curl` and the network, and are drawn at the nearest zoom level, so some scaling blur is possible. Tile servers' usage policies forbid bulk downloading. The tile code has only been tested against a pre-filled cache: the tutorial run had no access to the tile server.
- **Map packs** are drawn with one built-in style: no road names, shields, icons or points of interest, only place names. Road and boundary lines are drawn per tile, so a dashed border can restart its dash at a tile edge. Place names come from the tiles' `name` field (the local name). The planet pack's deepest detail is zoom 15; beyond that, the map is the same data drawn bigger. The map pack reader and style were tested with packs the tests build themselves: a tiny hand-made one and a zoom 0-3 pack cut from the Natural Earth countries (borders, land, sea and country names, laid out like a Protomaps pack). They were also tried against a real Protomaps basemap pack (the Florence sample in the PMTiles repository, zoom 0-15): it draws with its streets, rivers, railways and place names, and search finds its 6,400 place and shop names ("uffizi", "ponte vecchio") in a quarter of a second. The run for this tutorial had no full planet pack, so the outputs in Task 8 are examples.
- **Labels** in PNG and PDF output use Helvetica for Western European text and the built-in font (Roboto, embedded in PDFs) for Greek and Cyrillic. Other scripts (CJK, Arabic, ...) come out as `?`. SVG output keeps the text and shows it with the viewer's fonts. Labels that would overlap are dropped, not moved.
- **The map window** was built and its click tools only call the commands above, but it has not been tried on a screen in this round of work.
- **Size**: everything is in memory, which is fine for tens of thousands of features. Big national datasets are slow to buffer and render.
