#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <sstream>
#include <string>

#include "wetwaresuite/core/cli.hpp"
#include "wetwaresuite/maps/maps.hpp"
#include "wetwaresuite/maps/tiles.hpp"
#include "wetwaresuite/mcp/server.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "check.hpp"

using namespace wetwaresuite;
using namespace wetwaresuite::maps;
namespace fs = std::filesystem;

namespace {

fs::path tempDir() {
    std::random_device rd;
    const auto stamp = static_cast<unsigned long long>(std::chrono::high_resolution_clock::now().time_since_epoch().count()) ^ rd();
    fs::path d = fs::temp_directory_path() / ("wetwaresuite_maps_" + std::to_string(stamp));
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

std::string q(const fs::path& p) { return "\"" + p.generic_string() + "\""; }

void writeText(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary);
    out << s;
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

Result run(Session& s, const std::string& line) {
    cli::Batch batch(s);
    Result r = batch.runLine(line);
    if (!r.ok) std::printf("  (%s -> %s)\n", line.c_str(), r.message.c_str());
    return r;
}

double num(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return -999999;
    if (auto d = std::get_if<double>(v)) return *d;
    if (auto i = std::get_if<std::int64_t>(v)) return static_cast<double>(*i);
    return -999998;
}

std::string str(const Result& r, const std::string& key) {
    const Value* v = r.field(key);
    if (!v) return {};
    return toText(*v);
}

Map& mapOf(Session& s, const std::string& id) { return s.workspace().getAs<MapDoc>(id)->map; }

void testGeodesy() {
    // Vincenty's classic test line: Flinders Peak -> Buninyong
    const Inverse inv = geodesicInverse(144.424867889, -37.951033416, 143.926495528, -37.652821139);
    CHECK(near(inv.distance, 54972.271, 0.01));
    CHECK(near(inv.azimuth1, 306.868158, 1e-5));
    const Coord back = geodesicDirect(144.424867889, -37.951033416, inv.azimuth1, inv.distance);
    CHECK(near(back.x, 143.926495528, 1e-8));
    CHECK(near(back.y, -37.652821139, 1e-8));
    CHECK(near(geodesicDistance(0, 0, 1, 0), 111319.49, 0.05));
    // nearly antipodal falls back without hanging
    CHECK(geodesicDistance(0, 0, 179.7, 0.5) > 19.9e6);
    // a 1x1 degree cell at the equator is about 12,300 km^2
    const double a = ringArea({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    CHECK(near(a / 1e6, 12308.8, 25));
    // orientation doesn't matter, holes subtract
    CHECK(near(ringArea({{0, 1}, {1, 1}, {1, 0}, {0, 0}}), a, 1));
    Polygon donut{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}, {{0.25, 0.25}, {0.25, 0.75}, {0.75, 0.75}, {0.75, 0.25}}};
    CHECK(near(polygonArea(donut), a * 0.75, a * 0.01));
}

void testProjections() {
    const Crs wm = Crs::webMercator();
    CHECK(near(wm.forward({180, 0}).x, 20037508.34, 0.01));
    CHECK(near(wm.forward({0, 85.0511287798}).y, 20037508.34, 1));
    CHECK(near(wm.inverse(wm.forward({12.5, 41.9})).y, 41.9, 1e-9));
    // 3 degrees east of the central meridian on the equator (zone 31): E = 833978.556
    const Crs z31 = Crs::utm(31, false);
    const Coord e = z31.forward({6, 0});
    CHECK(near(e.x, 833978.556, 0.01));
    CHECK(near(e.y, 0, 0.001));
    const Crs z33 = Crs::utmFor(13.4, 52.52);
    CHECK(z33.zone == 33 && !z33.south);
    const Coord b = z33.forward({13.4, 52.52});
    CHECK(b.x > 391000 && b.x < 392000 && b.y > 5819000 && b.y < 5821000);
    const Coord ll = z33.inverse(b);
    CHECK(near(ll.x, 13.4, 1e-9) && near(ll.y, 52.52, 1e-9));
    CHECK(Crs::utmFor(151.2, -33.9).south && Crs::utmFor(151.2, -33.9).zone == 56);
    CHECK(Crs::utmFor(5, 60).zone == 32);  // Norway exception
    CHECK(Crs::parse("EPSG:32633")->zone == 33);
    CHECK(Crs::parse("utm 33s")->south);
    CHECK(Crs::parse("epsg:3857")->kind == Crs::WebMercator);
    CHECK(Crs::parse("WGS84")->geographic());
    CHECK(!Crs::parse("epsg:2154"));
    CHECK(!Crs::parse("banana"));
    // .prj WKT round trip
    CHECK(Crs::parse(Crs::utm(17, true).wkt())->epsg() == 32717);
    CHECK(Crs::parse(wm.wkt())->kind == Crs::WebMercator);
    CHECK(Crs::parse(Crs::wgs84().wkt())->geographic());
    // Equal Earth (EPSG:8857): the published extents, a round trip, and equal areas
    const Crs ee = Crs::equalEarth();
    CHECK(Crs::parse("epsg:8857")->kind == Crs::EqualEarth && Crs::parse("equalearth")->epsg() == 8857);
    CHECK(near(ee.forward({180, 0}).x, 17243959.06, 2) && near(ee.forward({0, 90}).y, 8392927.6, 2));
    const Coord eeBack = ee.inverse(ee.forward({-73.5, 41.25}));
    CHECK(near(eeBack.x, -73.5, 1e-7) && near(eeBack.y, 41.25, 1e-7));
    auto cellArea = [](const Crs& c, double lon, double lat) {
        const Coord a = c.forward({lon, lat}), b = c.forward({lon + 0.01, lat}), d = c.forward({lon, lat + 0.01});
        return std::abs((b.x - a.x) * (d.y - a.y) - (b.y - a.y) * (d.x - a.x));
    };
    // on the ellipsoid a 0.01 deg cell at 60N is cos(60)-ish times one at the equator: compare with the authalic q
    auto q = [](double latDeg) {
        const double e2 = kF * (2 - kF), e = std::sqrt(e2), s = std::sin(latDeg * kDeg);
        return (1 - e2) * (s / (1 - e2 * s * s) - std::log((1 - e * s) / (1 + e * s)) / (2 * e));
    };
    const double wantRatio = (q(60.01) - q(60)) / (q(0.01) - q(0));
    CHECK(near(cellArea(ee, 0, 60) / cellArea(ee, 0, 0), wantRatio, 1e-4));
    CHECK(near(cellArea(ee, 150, 60) / cellArea(ee, 0, 0), wantRatio, 1e-4));
    // LAEA Europe (EPSG:3035): its centre is the false origin, round trips, equal areas
    const Crs eu = *Crs::parse("epsg:3035");
    CHECK(eu.kind == Crs::Laea && eu.epsg() == 3035);
    CHECK(near(eu.forward({10, 52}).x, 4321000, 0.01) && near(eu.forward({10, 52}).y, 3210000, 0.01));
    const Coord euBack = eu.inverse(eu.forward({2.35, 48.85}));
    CHECK(near(euBack.x, 2.35, 1e-7) && near(euBack.y, 48.85, 1e-7));
    CHECK(near(cellArea(eu, 30, 60) / cellArea(eu, 10, 0), wantRatio, 1e-4));
    const auto custom = Crs::parse("laea:-33.9,151.2");
    CHECK(custom && custom->kind == Crs::Laea && near(custom->lat0, -33.9, 1e-9) && near(custom->forward({151.2, -33.9}).x, 0, 1e-6));
    CHECK(Crs::parse(ee.wkt())->kind == Crs::EqualEarth && Crs::parse(eu.wkt())->epsg() == 3035);
}

void testExpressions() {
    Feature f;
    f.geom = Geometry::point(2.35, 48.85);
    f.props["name"] = Prop::string("Paris");
    f.props["pop"] = Prop::number(2.1e6);
    f.props["capital"] = Prop::boolean(true);
    f.props["code"] = Prop::infer("075");
    CHECK(f.props["code"].kind == Prop::String);
    CHECK(Prop::infer("12.5").kind == Prop::Number && Prop::infer("1e3").num == 1000);
    CHECK(Expr::compile("pop > 1e6 and name == 'Paris'").test(f));
    CHECK(!Expr::compile("pop > 1e6 and not capital").test(f));
    CHECK(Expr::compile("name in ('Lyon', 'Paris')").test(f));
    CHECK(Expr::compile("name not in ('Lyon')").test(f));
    CHECK(Expr::compile("name like 'par%'").test(f));
    CHECK(Expr::compile("name contains 'ari' or missing > 3").test(f));
    CHECK(Expr::compile("missing is null").test(f));
    CHECK(Expr::compile("round(pop / 1e6, 1) == 2.1").test(f));
    CHECK(Expr::compile("$x > 2 and $y < 49 and $type == 'point'").test(f));
    CHECK(Expr::compile("upper(name) + '!'").eval(f).str == "PARIS!");
    CHECK(Expr::compile("code == '075'").test(f));
    const Expr e = Expr::compile("[my field] >= 3 or other = 1");
    CHECK(e.fields().size() == 2);
    bool threw = false;
    try {
        Expr::compile("pop > ");
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

void testGeoJsonAndWkt(const fs::path& dir) {
    Geometry g = parseWkt("POLYGON ((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))");
    CHECK(g.type == GeomType::Polygon && g.polygons[0].size() == 2 && g.polygons[0][0].size() == 4);
    CHECK(parseWkt("MULTIPOINT ((1 2), (3 4))").points.size() == 2);
    CHECK(parseWkt("MULTIPOINT (1 2, 3 4)").points.size() == 2);
    CHECK(near(parseWkt("POINT Z (1 2 3)").points[0].z, 3, 0));
    CHECK(parseWkt("SRID=4326;LINESTRING (0 0, 1 1)").lines.size() == 1);
    CHECK(parseWkt(toWkt(g)).polygons[0][1].size() == 4);
    Feature f{g, {{"name", Prop::string("box")}, {"n", Prop::number(3)}}};
    const std::string json = toGeoJson({f});
    auto back = parseGeoJson(json);
    CHECK(back.size() == 1 && back[0].props["name"].str == "box" && back[0].props["n"].num == 3);
    CHECK(back[0].geom.polygons[0].size() == 2);
    // projected output says its crs and reads back to lon/lat
    ExportOptions o;
    o.crs = Crs::webMercator();
    auto merc = parseGeoJson(toGeoJson({Feature{Geometry::point(10, 50), {}}}, o));
    CHECK(near(merc[0].geom.points[0].x, 10, 1e-6) && near(merc[0].geom.points[0].y, 50, 1e-6));
    // GeometryCollection splits, CRLF is fine
    auto gc = parseGeoJson("{\r\n\"type\":\"Feature\",\"properties\":{\"a\":1},\r\n\"geometry\":{\"type\":\"GeometryCollection\",\"geometries\":[{\"type\":\"Point\",\"coordinates\":[1,2]},{\"type\":\"LineString\",\"coordinates\":[[0,0],[1,1]]}]}}\r\n");
    CHECK(gc.size() == 2 && gc[1].geom.type == GeomType::Line);
    // track times survive GeoJSON
    Line track{{0, 0, 10, 1000}, {0.01, 0, 12, 1100}};
    auto tj = parseGeoJson(toGeoJson({Feature{Geometry::line(track), {}}}));
    CHECK(near(tj[0].geom.lines[0][1].t, 1100, 1e-6) && near(tj[0].geom.lines[0][1].z, 12, 1e-9));
    (void)dir;
}

void testKmlGpxCsv(const fs::path& dir) {
    const std::string kml =
        "<?xml version=\"1.0\"?>\r\n<kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document><Folder><name>Trip</name>\r\n"
        "<Placemark><name>Cafe &amp; Bar</name><ExtendedData><Data name=\"stars\"><value>4</value></Data></ExtendedData>"
        "<Point><coordinates>2.35,48.85,35</coordinates></Point></Placemark>\r\n"
        "<Placemark><name>Park</name><Polygon><outerBoundaryIs><LinearRing><coordinates>0,0 1,0 1,1 0,1 0,0</coordinates></LinearRing></outerBoundaryIs>"
        "<innerBoundaryIs><LinearRing><coordinates>0.2,0.2 0.4,0.2 0.4,0.4 0.2,0.2</coordinates></LinearRing></innerBoundaryIs></Polygon></Placemark>"
        "<Placemark><MultiGeometry><LineString><coordinates>0,0 1,1</coordinates></LineString><LineString><coordinates>2,2 3,3</coordinates></LineString></MultiGeometry></Placemark>"
        "</Folder></Document></kml>";
    auto k = parseKml(kml);
    CHECK(k.size() == 3);
    CHECK(k[0].props["name"].str == "Cafe & Bar" && k[0].props["stars"].num == 4 && k[0].props["folder"].str == "Trip");
    CHECK(near(k[0].geom.points[0].z, 35, 0));
    CHECK(k[1].geom.polygons[0].size() == 2);
    CHECK(k[2].geom.lines.size() == 2);
    Layer kl;
    kl.name = "k";
    kl.features = k;
    auto k2 = parseKml(toKml(kl));
    CHECK(k2.size() == 3 && k2[1].geom.polygons[0].size() == 2);

    // a GPX track along the equator: 0.001 deg ~ 111.3 m every 10 s, climbing 1 m then 5 m steps
    std::string gpx = "<?xml version=\"1.0\"?>\n<gpx version=\"1.1\"><wpt lat=\"0.0005\" lon=\"0.002\"><ele>12</ele><name>Spring</name></wpt><trk><name>Run</name><trkseg>\n";
    const double eles[] = {10, 11, 10, 15, 20, 25, 20, 15, 16, 10, 10};
    for (int i = 0; i <= 10; ++i) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "<trkpt lat=\"0\" lon=\"%.4f\"><ele>%.1f</ele><time>2024-05-01T08:00:%02dZ</time></trkpt>\r\n", i * 0.001, eles[i], i * 5);
        gpx += buf;
    }
    gpx += "</trkseg></trk></gpx>\n";
    GpxData d = parseGpx(gpx);
    CHECK(d.tracks.size() == 1 && d.waypoints.size() == 1 && d.waypoints[0].props["name"].str == "Spring");
    std::vector<const Line*> segs{&d.tracks[0].geom.lines[0]};
    TrackStats s = trackStats(segs, 3);
    CHECK(near(s.distance, 1113.19, 0.5));
    CHECK(near(s.gain, 15, 1e-9));   // 10 -> 15 -> 20 -> 25 (the 1 m wiggle is ignored)
    CHECK(near(s.loss, 15, 1e-9));   // 25 -> 20 -> 15 -> 10
    CHECK(near(s.duration, 50, 1e-9) && near(s.avgSpeed, 1113.19 / 50, 0.05));
    CHECK(near(s.maxSpeed, 111.319 / 5, 0.05));
    CHECK(near(s.movingTime, 50, 1e-9));
    Layer tl;
    tl.name = "t";
    tl.features = d.tracks;
    tl.features.push_back(d.waypoints[0]);
    GpxData d2 = parseGpx(toGpx(tl));
    CHECK(d2.tracks.size() == 1 && d2.waypoints.size() == 1 && d2.tracks[0].geom.lines[0].size() == 11);
    CHECK(near(d2.tracks[0].geom.lines[0][3].t, d.tracks[0].geom.lines[0][3].t, 1e-6));

    // CSV: header detection, DMS, semicolons, CRLF
    ImportOptions io;
    auto rows = parseCsv("name;Latitude;Longitude;zip\r\nA;48.85;2.35;01234\r\nB;51\xC2\xB0" "30'0\"N;0\xC2\xB0" "7'30\"W;99\r\nC;;;\r\n", io);
    CHECK(rows.size() == 2);
    CHECK(rows[0].props["zip"].str == "01234");
    CHECK(near(rows[1].geom.points[0].y, 51.5, 1e-9) && near(rows[1].geom.points[0].x, -0.125, 1e-9));
    io.crs = Crs::utm(33, false);
    auto utmRows = parseCsv("id,x,y\n1,391440.032,5820079.667\n", io);
    CHECK(utmRows.size() == 1 && near(utmRows[0].geom.points[0].x, 13.4, 1e-6));
    ImportOptions wk;
    auto wktRows = parseCsv("id,wkt\n1,\"LINESTRING (0 0, 1 1)\"\n", wk);
    CHECK(wktRows.size() == 1 && wktRows[0].geom.type == GeomType::Line);
    Layer cl;
    cl.features = rows;
    const std::string csv = toCsv(cl);
    CHECK(csv.find("lat,lon") != std::string::npos && csv.find("01234") != std::string::npos);
    (void)dir;
}

void testShapefile(const fs::path& dir) {
    Layer l;
    l.name = "parts";
    Feature a{Geometry::polygon({{{0, 0}, {2, 0}, {2, 2}, {0, 2}}, {{0.5, 0.5}, {0.5, 1}, {1, 1}, {1, 0.5}}}), {}};
    a.props["name"] = Prop::string("Zoë's field");
    a.props["area"] = Prop::number(12.25);
    a.props["ok"] = Prop::boolean(true);
    Feature b{Geometry::polygon({{{5, 5}, {6, 5}, {6, 6}}}), {}};
    b.props["name"] = Prop::string("tri");
    b.props["area"] = Prop::number(3);
    Feature c{Geometry::point(1, 1), {{"name", Prop::string("pt")}}};
    Feature d{Geometry::line({{0, 0, 5}, {1, 1, 7}}), {{"name", Prop::string("road")}}};
    l.features = {a, b, c, d};
    ExportOptions o;
    o.crs = Crs::utm(31, false);
    const auto files = writeShapefile(l, dir / "parts.shp", o);
    CHECK(files.size() == 15);  // three geometry types x shp/shx/dbf/prj/cpg
    CHECK(fs::exists(dir / "parts_polygons.shp") && fs::exists(dir / "parts_lines.dbf"));
    std::vector<std::string> warnings;
    Layer back = readShapefile(dir / "parts_polygons.shp", std::nullopt, &warnings);
    CHECK(back.features.size() == 2);
    CHECK(back.features[0].props["name"].str == "Zoë's field");
    CHECK(near(back.features[0].props["area"].num, 12.25, 1e-9) && back.features[0].props["ok"].truthy());
    CHECK(back.features[0].geom.polygons.size() == 1 && back.features[0].geom.polygons[0].size() == 2);
    CHECK(near(back.features[0].geom.polygons[0][0][1].x, 2, 1e-6));  // reprojected from UTM 31N via .prj
    CHECK(signedArea(back.features[0].geom.polygons[0][0]) > 0);       // outer rings come back counter-clockwise
    Layer lines = readShapefile(dir / "parts_lines.shp", std::nullopt);
    CHECK(lines.features.size() == 1 && near(lines.features[0].geom.lines[0][1].z, 7, 1e-9));
    Layer pts = readShapefile(dir / "parts_points.shp", std::nullopt);
    CHECK(pts.features.size() == 1 && near(pts.features[0].geom.points[0].y, 1, 1e-7));
    // .shx index is consistent: 100-byte header, one 8-byte record per shape
    CHECK(fs::file_size(dir / "parts_polygons.shx") == 100 + 2 * 8);
}

void testRasters(const fs::path& dir) {
    // value grid -> GeoTIFF -> back
    Grid g;
    g.width = 4;
    g.height = 3;
    g.values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, -9999};
    g.nodata = -9999;
    g.crs = Crs::utm(33, false);
    g.geo = {30, 0, 400000, 0, -30, 5800000};
    g.updateRange();
    CHECK(g.maxValue == 11);
    auto t = readGeoTiff(encodeGeoTiff(g));
    CHECK(t->width == 4 && t->height == 3 && t->values.size() == 12 && t->values[5] == 6);
    CHECK(t->crs.zone == 33 && near(t->geo.c, 400000, 1e-6) && near(t->geo.e, -30, 1e-9));
    CHECK(t->nodata == -9999);
    const Coord centre = g.crs.inverse({400000 + 45, 5800000 - 45});  // pixel (1,1)
    CHECK(t->sample(centre.x, centre.y) == 6);
    // RGBA image GeoTIFF
    Grid img;
    img.width = 2;
    img.height = 2;
    img.image = raster::Image(2, 2, {200, 10, 10, 255});
    img.geo = {0.5, 0, 10, 0, -0.5, 50};
    auto ti = readGeoTiff(encodeGeoTiff(img));
    CHECK(!ti->image.empty() && ti->image.at(1, 1).r == 200 && ti->crs.geographic());
    // PNG + world file
    raster::save(raster::Image(10, 10, {0, 0, 255, 255}), dir / "blue.png");
    writeText(dir / "blue.pgw", "0.1\r\n0\r\n0\r\n-0.1\r\n5.05\r\n45.95\r\n");
    auto w = readRaster(dir / "blue.png", std::nullopt, std::nullopt);
    const BBox b = w->bounds();
    CHECK(near(b.minX, 5, 1e-9) && near(b.maxY, 46, 1e-9) && near(b.maxX, 6, 1e-9));
    CHECK(near(w->sample(5.5, 45.5), 0.0722 * 255, 0.01));
    bool threw = false;
    try {
        raster::save(raster::Image(4, 4), dir / "nowhere.png");
        readRaster(dir / "nowhere.png", std::nullopt, std::nullopt);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
    // write a picture with world file and read it back in place
    const auto files = writeRaster(g, dir / "grid.png", "viridis");
    CHECK(files.size() == 3 && fs::exists(dir / "grid.pgw") && fs::exists(dir / "grid.prj"));
    auto gp = readRaster(dir / "grid.png", std::nullopt, std::nullopt);
    CHECK(gp->crs.zone == 33 && near(gp->geo.c, 400000, 1e-6));
}

#include "tiff_fixtures.inc"

void testTiffCodecs() {
    auto rgb = readGeoTiff(kLzwRgb);
    CHECK(rgb->width == 16 && rgb->height == 8 && !rgb->image.empty());
    CHECK(rgb->image.at(5, 3).r == 80 && rgb->image.at(5, 3).g == 96 && rgb->image.at(5, 3).b == 100);
    CHECK(rgb->image.at(15, 7).r == 240 && rgb->image.at(15, 7).g == 224);
    bool placed = true;
    auto gray = readGeoTiff(kDeflateGray16, nullptr, &placed);
    CHECK(!placed && gray->hasValues() && gray->width == 16);
    CHECK(gray->values[3 * 16 + 7] == 703.0f && gray->maxValue == 1507);
}

void testAnalysis() {
    // a point buffer is a geodesic circle
    Geometry circle = buffer(Geometry::point(10, 50), 1000);
    CHECK(near(area(circle), 3.14159265 * 1e6, 0.01 * 3.14159265e6));
    // a 10 km line buffered by 500 m: 2*d*L + pi*d^2
    const Coord end = geodesicDirect(10, 50, 90, 10000);
    Geometry road = Geometry::line({{10, 50}, end});
    Geometry rb = buffer(road, 500);
    CHECK(near(area(rb), 2 * 500 * 10000 + 3.14159265 * 250000, 0.02 * 1.08e7));
    CHECK(pointInGeometry(rb, 10.05, 50.002) && !pointInGeometry(rb, 10.05, 50.02));
    // a polygon grows and shrinks
    Geometry sq = Geometry::polygon({{{10, 50}, {10.1, 50}, {10.1, 50.1}, {10, 50.1}}});
    const double a0 = area(sq);
    CHECK(area(buffer(sq, 100)) > a0);
    const double shrunk = area(buffer(sq, -500));
    CHECK(shrunk < a0 && shrunk > a0 * 0.6);
    // two overlapping circles dissolve into one polygon
    Geometry p1 = Geometry::point(0, 0), p2 = Geometry::point(0.01, 0);
    Geometry u = bufferUnion({&p1, &p2}, 1000);
    CHECK(u.polygons.size() == 1);
    // a ring buffer keeps its hole
    Geometry ring = Geometry::line({{0, 0}, {0.1, 0}, {0.1, 0.1}, {0, 0.1}, {0, 0}});
    Geometry rbuf = buffer(ring, 200);
    CHECK(rbuf.polygons.size() == 1 && rbuf.polygons[0].size() == 2);
    // exact: a square shrunk by 500 m is a square 1 km smaller each way (to the projection's error)
    const double side = geodesicDistance(10, 50.05, 10.1, 50.05), tall = geodesicDistance(10.05, 50, 10.05, 50.1);
    CHECK(near(shrunk, (side - 1000) * (tall - 1000), 0.003 * shrunk));
    CHECK(near(area(rb), 2 * 500 * 10000 + 500 * 500 * 48 * std::sin(2 * kPi / 48) / 2, 0.002 * 1.08e7));

    // exact overlays on lon/lat: two unit squares overlapping by half
    auto planar = [](const Geometry& g) {
        double a = 0;
        for (const auto& poly : g.polygons) {
            for (std::size_t i = 0; i < poly.size(); ++i) a += signedArea(poly[i]);
        }
        return a;
    };
    Geometry A = Geometry::polygon({{{0, 0}, {1, 0}, {1, 1}, {0, 1}}});
    Geometry B = Geometry::polygon({{{0.5, 0.25}, {1.5, 0.25}, {1.5, 1.25}, {0.5, 1.25}}});
    CHECK(near(planar(exactOverlay({&A}, {&B}, OverlayOp::Intersection)), 0.375, 1e-12));
    CHECK(near(planar(exactOverlay({&A}, {&B}, OverlayOp::Union)), 1.625, 1e-12));
    CHECK(near(planar(exactOverlay({&A}, {&B}, OverlayOp::Difference)), 0.625, 1e-12));
    const Geometry sym = exactOverlay({&A}, {&B}, OverlayOp::SymDifference);
    CHECK(near(planar(sym), 1.25, 1e-12) && sym.polygons.size() == 2);
    // a hole punched right through, and a shared edge merging away
    Geometry inner = Geometry::polygon({{{0.25, 0.25}, {0.75, 0.25}, {0.75, 0.75}, {0.25, 0.75}}});
    const Geometry holed = exactOverlay({&A}, {&inner}, OverlayOp::Difference);
    CHECK(holed.polygons.size() == 1 && holed.polygons[0].size() == 2 && near(planar(holed), 0.75, 1e-12));
    Geometry right = Geometry::polygon({{{1, 0}, {2, 0}, {2, 1}, {1, 1}}});
    const Geometry joined = exactOverlay({&A, &right}, {}, OverlayOp::Union);
    CHECK(joined.polygons.size() == 1 && joined.polygons[0].size() == 1 && near(planar(joined), 2, 1e-12));
    CHECK(exactOverlay({&A}, {&right}, OverlayOp::Intersection).empty());
    // corners touching stay two shapes; a polygon minus itself is nothing
    Geometry corner = Geometry::polygon({{{1, 1}, {2, 1}, {2, 2}, {1, 2}}});
    CHECK(exactOverlay({&A, &corner}, {}, OverlayOp::Union).polygons.size() == 2);
    CHECK(exactOverlay({&A}, {&A}, OverlayOp::Difference).empty());
    // a star (self-crossing edges everywhere) against a square
    Ring star;
    for (int i = 0; i < 5; ++i) star.push_back({0.5 + 0.6 * std::cos(kPi / 2 + i * 4 * kPi / 5), 0.5 + 0.6 * std::sin(kPi / 2 + i * 4 * kPi / 5)});
    Geometry starG = Geometry::polygon({star});
    const double starArea = planar(exactOverlay({&starG}, {}, OverlayOp::Union));
    CHECK(starArea > 0.3 && starArea < 0.6);  // nonzero winding fills the middle pentagon too
    CHECK(planar(exactOverlay({&starG}, {&A}, OverlayOp::Intersection)) < starArea);

    // simplify a noisy line
    Line noisy;
    for (int i = 0; i <= 200; ++i) noisy.push_back({i * 0.0001, (i % 2) * 0.000001});
    Geometry s = simplify(Geometry::line(noisy), 5);
    CHECK(s.lines[0].size() == 2);
    // clip
    Geometry c = clip(Geometry::line({{-1, 0.5}, {2, 0.5}}), BBox{0, 0, 1, 1});
    CHECK(c.lines.size() == 1 && near(c.lines[0][0].x, 0, 1e-12) && near(c.lines[0][1].x, 1, 1e-12));
    Geometry cp = clip(Geometry::polygon({{{-1, -1}, {2, -1}, {2, 2}, {-1, 2}}}), BBox{0, 0, 1, 1});
    CHECK(near(std::abs(signedArea(cp.polygons[0][0])), 1, 1e-12));
    CHECK(clip(Geometry::point(5, 5), BBox{0, 0, 1, 1}).empty());
    // nearest
    const Nearest n = nearestPoint(Geometry::line({{0, 0}, {0, 1}}), 0.01, 0.5);
    CHECK(near(n.distance, 1113.2, 1) && near(n.point.y, 0.5, 1e-6));
    CHECK(nearestPoint(sq, 10.05, 50.05).distance == 0);
    // heatmap peak sits on the cluster
    std::vector<Feature> pts;
    for (int i = 0; i < 20; ++i) pts.push_back({Geometry::point(10 + i * 0.0001, 50), {}});
    pts.push_back({Geometry::point(10.5, 50.3), {}});
    HeatmapOptions ho;
    ho.radius = 2000;
    auto hm = heatmap(pts, ho);
    CHECK(hm->sample(10.001, 50) > hm->sample(10.5, 50.3) * 5);
    CHECK(hm->maxValue > 10);
}

void testRendering(const fs::path& dir) {
    Map m;
    m.title = "Test";
    Layer pts;
    pts.name = "points";
    pts.style.fill = {255, 0, 0, 255};
    pts.style.pointSize = 20;
    pts.style.labelField = "name";
    pts.features.push_back({Geometry::point(2.35, 48.85), {{"name", Prop::string("Paris")}, {"kind", Prop::string("a")}}});
    pts.features.push_back({Geometry::point(13.4, 52.52), {{"name", Prop::string("Berlin")}, {"kind", Prop::string("b")}}});
    m.layers.push_back(pts);
    RenderOptions o;
    o.width = 400;
    o.basemap = "none";
    o.legend = false;
    RenderInfo info;
    raster::Image img = renderImage(m, o, &info);
    CHECK(img.width() == 400 && img.height() > 50);
    int red = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const Color c = img.at(x, y);
            if (c.r > 220 && c.g < 40 && c.b < 40) ++red;
        }
    }
    CHECK(red > 200);  // two 20 px markers
    CHECK(info.metresPerPixel > 100);
    o.basemap = "countries";
    o.legend = true;
    o.graticule = true;
    const std::string svg = renderMap(m, o, "svg");
    CHECK(svg.find("<svg") == 39 || svg.find("<svg") != std::string::npos);
    CHECK(svg.find(">Paris</text>") != std::string::npos);
    const std::string pdf = renderMap(m, o, "pdf");
    CHECK(pdf.rfind("%PDF-", 0) == 0 && pdf.find("%%EOF") != std::string::npos);
    // categorized classes
    m.layers[0].style.mode = "categorized";
    m.layers[0].style.field = "kind";
    CHECK(styleClasses(m.layers[0]).size() == 2);
    // graduated quantiles
    Layer g;
    for (int i = 1; i <= 100; ++i) g.features.push_back({Geometry::point(i * 0.1, 0), {{"v", Prop::number(i)}}});
    g.style.mode = "graduated";
    g.style.field = "v";
    g.style.classes = 4;
    auto cls = styleClasses(g);
    CHECK(cls.size() == 4 && near(cls[0].hi, 25, 1e-9) && near(cls[3].hi, 100, 1e-9));
    g.style.method = "equal";
    CHECK(near(styleClasses(g)[1].hi, 50.5, 1e-9));
    // tile maths
    double tx = 0, ty = 0;
    lonLatToTile(0, 0, 1, tx, ty);
    CHECK(near(tx, 1, 1e-12) && near(ty, 1, 1e-12));
    double lon = 0, lat = 0;
    tileToLonLat(0, 0, 0, lon, lat);
    CHECK(near(lon, -180, 1e-9) && near(lat, 85.0511287798, 1e-6));
    // offline OSM basemap from a pre-filled cache (never touches the network)
    o.basemap = "osm";
    o.offline = true;
    o.tileCache = (dir / "tiles").string();
    o.legend = false;
    RenderInfo first;
    renderImage(m, o, &first);
    CHECK(first.zoom >= 0 && first.tilesUsed == 0);
    const fs::path server = dir / "tiles" / "https___tile_openstreetmap_org_";
    const raster::Image green(256, 256, {0, 200, 0, 255});
    const int z = first.zoom;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    lonLatToTile(first.bbox.minX, first.bbox.maxY, z, x0, y0);
    lonLatToTile(first.bbox.maxX, first.bbox.minY, z, x1, y1);
    for (int x = static_cast<int>(x0); x <= static_cast<int>(x1); ++x) {
        for (int y = static_cast<int>(y0); y <= static_cast<int>(y1); ++y) {
            fs::create_directories(server / std::to_string(z) / std::to_string(x));
            raster::save(green, server / std::to_string(z) / std::to_string(x) / (std::to_string(y) + ".png"));
        }
    }
    RenderInfo second;
    raster::Image withTiles = renderImage(m, o, &second);
    CHECK(second.tilesUsed > 0 && second.tilesFetched == 0);
    CHECK(withTiles.at(withTiles.width() / 3, 5).g > 150);
    // a local tile folder as tile_url: read straight from disk, even with offline=false, never fetched
    o.offline = false;
    o.tileCache.clear();
    o.tileUrl = (server / "{z}" / "{x}" / "{y}.png").string();
    RenderInfo local;
    raster::Image fromFolder = renderImage(m, o, &local);
    CHECK(local.tilesUsed == second.tilesUsed && local.tilesFetched == 0);
    CHECK(fromFolder.at(fromFolder.width() / 3, 5).g > 150);
}

void testCommands(const fs::path& dir) {
    Session s;
    registerCommands(s);
    writeText(dir / "cafes.csv", "name,lat,lon,rating,country\nA,48.857,2.352,4.5,FRA\nB,48.86,2.34,3,FRA\nC,52.52,13.40,5,DEU\n");
    writeText(dir / "stats.csv", "code,gdp\nFRA,2.9\nDEU,4.2\nXXX,1\n");
    Result r = run(s, "maps.new title=Cafes");
    CHECK(r.ok);
    const std::string id = toText(r.value);
    CHECK(run(s, "maps.import doc=" + id + " path=" + q(dir / "cafes.csv")).ok);
    CHECK(mapOf(s, id).layers.size() == 1 && mapOf(s, id).layers[0].features.size() == 3);
    r = run(s, "maps.filter doc=" + id + " where=\"rating >= 4\"");
    CHECK(r.ok && num(r, "count") == 2);
    CHECK(run(s, "undo").ok);
    CHECK(mapOf(s, id).layers.size() == 1);
    r = run(s, "maps.add_countries doc=" + id + " where=\"continent == 'Europe'\"");
    CHECK(r.ok && num(r, "features") > 30);
    r = run(s, "maps.join doc=" + id + " layer=countries csv=" + q(dir / "stats.csv") + " key=iso_a3 csv_key=code");
    CHECK(r.ok && num(r, "matched") == 2 && num(r, "unmatched_rows") == 1);
    r = run(s, "maps.contains doc=" + id + " layer=countries lat=48.86 lon=2.35");
    CHECK(r.ok && num(r, "count") == 1 && r.message.find("France") != std::string::npos);
    r = run(s, "maps.spatial_join doc=" + id + " target=cafes join=countries fields=name prefix=in_ count_field=cafes");
    CHECK(r.ok && num(r, "matched") == 3);
    CHECK(mapOf(s, id).layer("cafes")->features[2].props["in_name"].str == "Germany");
    r = run(s, "maps.nearest doc=" + id + " layer=cafes lat=48.858 lon=2.351 count=2");
    CHECK(r.ok && num(r, "index") == 0 && num(r, "distance_m") < 200);
    r = run(s, "maps.calc doc=" + id + " layer=countries field=km2 expr=\"round($area / 1e6)\"");
    CHECK(r.ok && num(r, "changed") > 30);
    r = run(s, "maps.style doc=" + id + " layer=countries mode=graduated field=km2 ramp=blues");
    CHECK(r.ok && num(r, "classes") >= 3);
    CHECK(!run(s, "maps.style doc=" + id + " layer=countries mode=categorized field=nope").ok);
    r = run(s, "maps.buffer doc=" + id + " layer=cafes distance=500 output=walk");
    CHECK(r.ok && num(r, "features") == 3);
    r = run(s, "maps.heatmap doc=" + id + " layer=cafes radius=3000");
    CHECK(r.ok && num(r, "max") > 0);
    r = run(s, "maps.raster_save doc=" + id + " layer=\"cafes heatmap\" path=" + q(dir / "heat.tif"));
    CHECK(r.ok && fs::exists(dir / "heat.tif"));
    r = run(s, "maps.export doc=" + id + " layer=cafes path=" + q(dir / "out" / "cafes.shp") + " crs=webmercator");
    CHECK(r.ok && fs::exists(dir / "out" / "cafes.shp") && readAll(dir / "out" / "cafes.prj").find("Auxiliary_Sphere") != std::string::npos);
    r = run(s, "maps.export doc=" + id + " layer=walk path=" + q(dir / "walk.geojson"));
    CHECK(r.ok && readAll(dir / "walk.geojson").find("\"Polygon\"") != std::string::npos);
    r = run(s, "maps.export doc=" + id + " layer=cafes path=" + q(dir / "cafes.kml"));
    CHECK(r.ok);
    r = run(s, "maps.export doc=" + id + " layer=cafes path=" + q(dir / "cafes.gpx"));
    CHECK(r.ok);
    r = run(s, "maps.import doc=" + id + " path=" + q(dir / "out" / "cafes.shp") + " name=back");
    CHECK(r.ok && num(r, "features") == 3);
    CHECK(near(mapOf(s, id).layer("back")->features[0].geom.points[0].y, 48.857, 1e-6));
    r = run(s, "maps.render doc=" + id + " path=" + q(dir / "map.png") + " width=500");
    CHECK(r.ok && raster::load(dir / "map.png").width() == 500);
    CHECK(run(s, "maps.render doc=" + id + " path=" + q(dir / "map.pdf") + " center=48.86,2.35 radius_km=5 crs=auto").ok);
    CHECK(run(s, "maps.render doc=" + id + " path=" + q(dir / "map.svg") + " crs=wgs84 graticule=true").ok);
    r = run(s, "maps.preview doc=" + id);
    CHECK(r.ok && r.attachments.size() == 1 && r.attachments[0].mime == "image/png");
    r = run(s, "maps.save doc=" + id + " path=" + q(dir / "cafes.wwmap"));
    CHECK(r.ok);
    {
        // a project from before the rename: .achmap with the "achclwss-map" id
        std::string old = readAll(dir / "cafes.wwmap");
        const std::string fresh = "\"wetwaresuite-map\"";
        CHECK(old.find(fresh) != std::string::npos);
        if (old.find(fresh) != std::string::npos) old.replace(old.find(fresh), fresh.size(), "\"achclwss-map\"");
        writeText(dir / "old.achmap", old);
        CHECK(run(s, "maps.open path=" + q(dir / "old.achmap")).ok);
    }
    r = run(s, "maps.open path=" + q(dir / "cafes.wwmap"));
    CHECK(r.ok);
    const std::string id2 = toText(r.value);
    CHECK(mapOf(s, id2).layers.size() == mapOf(s, id).layers.size());
    CHECK(mapOf(s, id2).layer("cafes heatmap")->grid != nullptr);
    CHECK(mapOf(s, id2).layer("countries")->style.mode == "graduated");
    r = run(s, "maps.measure points=48.85,2.35,48.85,2.36,48.86,2.36 polygon=true");
    CHECK(r.ok && num(r, "area_m2") > 1e5);
    r = run(s, "maps.transform points=13.4,52.52 to=utm");
    CHECK(r.ok && str(r, "crs") == "EPSG:32633");
    r = run(s, "maps.bbox doc=" + id + " layer=cafes");
    CHECK(r.ok && near(num(r, "max_lat"), 52.52, 1e-9));
    CHECK(run(s, "maps.feature_add doc=" + id + " layer=route points=48.85,2.35,52.52,13.40 props=\"name=Train\"").ok);
    r = run(s, "maps.measure doc=" + id + " layer=route");
    CHECK(r.ok && near(num(r, "length_m"), 878e3, 5e3));
    CHECK(run(s, "maps.feature_delete doc=" + id + " layer=cafes where=\"name == 'B'\"").ok);
    CHECK(mapOf(s, id).layer("cafes")->features.size() == 2);
    CHECK(!run(s, "maps.filter doc=" + id + " where=\"rating >\"").ok);
    // GPX through the commands
    std::string gpx = "<gpx><trk><trkseg>";
    for (int i = 0; i < 5; ++i) gpx += "<trkpt lat=\"46\" lon=\"" + std::to_string(7 + i * 0.01) + "\"><ele>" + std::to_string(500 + i * 20) + "</ele><time>2024-01-01T10:0" + std::to_string(i) + ":00Z</time></trkpt>";
    gpx += "</trkseg></trk></gpx>";
    writeText(dir / "hike.gpx", gpx);
    r = run(s, "maps.open path=" + q(dir / "hike.gpx"));
    CHECK(r.ok);
    const std::string hike = toText(r.value);
    r = run(s, "maps.track_stats doc=" + hike);
    CHECK(r.ok && near(num(r, "gain_m"), 80, 1e-9) && near(num(r, "duration_s"), 240, 1e-9) && num(r, "avg_kmh") > 40);
    r = run(s, "maps.profile doc=" + hike + " path=" + q(dir / "profile.svg"));
    CHECK(r.ok && readAll(dir / "profile.svg").find("<svg") != std::string::npos && r.attachments.size() == 1);
    // raster commands
    raster::save(raster::Image(8, 8, {255, 255, 255, 255}), dir / "scan.png");
    r = run(s, "maps.raster_add doc=" + id + " path=" + q(dir / "scan.png") + " bbox=2,48,3,49");
    CHECK(r.ok);
    r = run(s, "maps.raster_sample doc=" + id + " layer=scan points=48.5,2.5,10,10");
    CHECK(r.ok && r.message.find("no data") != std::string::npos);
    CHECK(run(s, "maps.close doc=" + hike).ok);
    CHECK(!s.workspace().get(hike));
}

// ---- offline map packs: PMTiles + vector tiles, all built here (no network) ----------------

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void pbVarint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out += static_cast<char>((v & 0x7F) | 0x80);
        v >>= 7;
    }
    out += static_cast<char>(v);
}
void pbBytes(std::string& out, int field, const std::string& bytes) {
    pbVarint(out, static_cast<std::uint64_t>(field) << 3 | 2);
    pbVarint(out, bytes.size());
    out += bytes;
}
void pbInt(std::string& out, int field, std::uint64_t v) {
    pbVarint(out, static_cast<std::uint64_t>(field) << 3);
    pbVarint(out, v);
}
std::uint32_t zz(std::int64_t v) { return static_cast<std::uint32_t>((v << 1) ^ (v >> 63)); }

struct TestFeature {
    int type;  // 1 point, 2 line, 3 polygon
    std::vector<std::vector<std::pair<int, int>>> parts;
    std::vector<std::pair<std::string, std::string>> tags;
};

// A hand-encoded MVT layer: geometry commands, zigzag deltas, string values.
std::string mvtLayer(const std::string& name, const std::vector<TestFeature>& features) {
    std::string layer;
    pbInt(layer, 15, 2);
    pbBytes(layer, 1, name);
    std::vector<std::string> keys, values;
    auto index = [](std::vector<std::string>& list, const std::string& s) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i] == s) return static_cast<std::uint32_t>(i);
        }
        list.push_back(s);
        return static_cast<std::uint32_t>(list.size() - 1);
    };
    for (const auto& f : features) {
        std::string feat, tags, geom;
        for (const auto& [k, v] : f.tags) {
            pbVarint(tags, index(keys, k));
            pbVarint(tags, index(values, v));
        }
        std::int64_t cx = 0, cy = 0;
        for (const auto& part : f.parts) {
            pbVarint(geom, 1 | (1 << 3));  // MoveTo 1
            pbVarint(geom, zz(part[0].first - cx));
            pbVarint(geom, zz(part[0].second - cy));
            cx = part[0].first;
            cy = part[0].second;
            if (part.size() > 1) {
                pbVarint(geom, 2 | ((part.size() - 1) << 3));  // LineTo n-1
                for (std::size_t i = 1; i < part.size(); ++i) {
                    pbVarint(geom, zz(part[i].first - cx));
                    pbVarint(geom, zz(part[i].second - cy));
                    cx = part[i].first;
                    cy = part[i].second;
                }
            }
            if (f.type == 3) pbVarint(geom, 7 | (1 << 3));  // ClosePath
        }
        pbBytes(feat, 2, tags);
        pbInt(feat, 3, static_cast<std::uint64_t>(f.type));
        pbBytes(feat, 4, geom);
        pbBytes(layer, 2, feat);
    }
    for (const auto& k : keys) pbBytes(layer, 3, k);
    for (const auto& v : values) {
        std::string val;
        pbBytes(val, 1, v);
        pbBytes(layer, 4, val);
    }
    pbInt(layer, 5, 4096);
    return layer;
}

std::string mvtTile(const std::vector<std::string>& layers) {
    std::string t;
    for (const auto& l : layers) pbBytes(t, 3, l);
    return t;
}

// z0: land on the western half with a square lake around lon -90, lat 0; a road and a town.
std::string worldTile() {
    TestFeature earth{3, {{{-64, -64}, {2048, -64}, {2048, 4160}, {-64, 4160}}, {{512, 1536}, {512, 2560}, {1536, 2560}, {1536, 1536}}}, {{"kind", "earth"}}};
    TestFeature road{2, {{{100, 1000}, {1900, 1000}}}, {{"kind", "highway"}}};
    TestFeature town{1, {{{300, 3000}}}, {{"kind", "locality"}, {"name", "Testville"}}};
    return mvtTile({mvtLayer("earth", {earth}), mvtLayer("roads", {road}), mvtLayer("places", {town})});
}

// z1: the western tiles are all land, the eastern all sea.
std::string halfTile(bool land) {
    if (!land) return mvtTile({mvtLayer("water", {TestFeature{3, {{{0, 0}, {4096, 0}, {4096, 4096}, {0, 4096}}}, {{"kind", "ocean"}}}})});
    return mvtTile({mvtLayer("earth", {TestFeature{3, {{{-64, -64}, {4160, -64}, {4160, 4160}, {-64, 4160}}}, {}}})});
}

bool isLand(Color c) { return near(c.r, 0xf2, 6) && near(c.g, 0xef, 6) && near(c.b, 0xe9, 6); }
bool isWater(Color c) { return near(c.r, 0xa6, 6) && near(c.g, 0xcb, 6) && near(c.b, 0xe3, 6); }

Color pixelAt(const raster::Image& img, const RenderInfo& info, double lon, double lat) {
    // a Web Mercator view: x is linear in longitude, y in the Mercator ordinate
    auto merc = [](double la) { return std::log(std::tan(kPi / 4 + la * kDeg / 2)); };
    const double x = (lon - info.bbox.minX) / info.bbox.width() * img.width();
    const double y = (merc(info.bbox.maxY) - merc(lat)) / (merc(info.bbox.maxY) - merc(info.bbox.minY)) * img.height();
    return img.at(std::clamp(static_cast<int>(x), 0, img.width() - 1), std::clamp(static_cast<int>(y), 0, img.height() - 1));
}

void testPmtiles(const fs::path& dir) {
    // Hilbert tile ids, as in the PMTiles spec
    CHECK(zxyToTileId(0, 0, 0) == 0);
    CHECK(zxyToTileId(1, 0, 0) == 1 && zxyToTileId(1, 0, 1) == 2 && zxyToTileId(1, 1, 1) == 3 && zxyToTileId(1, 1, 0) == 4);
    CHECK(zxyToTileId(2, 0, 0) == 5 && zxyToTileId(3, 0, 0) == 21);
    CHECK(zxyToTileId(12, 3423, 1763) == 19078479);
    std::mt19937 rng(7);
    bool roundTrip = true;
    for (int i = 0; i < 2000; ++i) {
        const int z = static_cast<int>(rng() % 26);
        const std::uint32_t n = 1u << z, x = rng() % n, y = rng() % n;
        int z2 = -1;
        std::uint32_t x2 = 0, y2 = 0;
        roundTrip = roundTrip && tileIdToZxy(zxyToTileId(z, x, y), z2, x2, y2) && z2 == z && x2 == x && y2 == y;
    }
    CHECK(roundTrip);
    int tz = -1;
    std::uint32_t txx = 0, tyy = 0;
    CHECK(tileIdToZxy(19078479, tz, txx, tyy) && tz == 12 && txx == 3423 && tyy == 1763);
    // gzip and directories
    const std::string text(5000, 'm');
    CHECK(gunzip(gzipCompress(text)) == text);
    const std::vector<PmtilesEntry> entries{{1, 0, 10, 1}, {2, 10, 20, 3}, {9, 0, 10, 1}, {40, 500, 7, 0}};
    const auto back = parsePmtilesDirectory(encodePmtilesDirectory(entries));
    CHECK(back.size() == 4 && back[1].offset == 10 && back[1].runLength == 3 && back[2].offset == 0 && back[3].tileId == 40 && back[3].runLength == 0 &&
          back[3].offset == 500);
    // MVT decoding
    const auto layers = decodeMvt(worldTile());
    CHECK(layers.size() == 3 && layers[0].name == "earth" && layers[0].extent == 4096);
    CHECK(layers[0].features.size() == 1 && layers[0].features[0].geom.type == GeomType::Polygon);
    CHECK(layers[0].features[0].geom.polygons.size() == 1 && layers[0].features[0].geom.polygons[0].size() == 2);  // outer ring + lake
    CHECK(layers[1].features[0].geom.type == GeomType::Line && near(layers[1].features[0].geom.lines[0][1].x, 1900, 0));
    CHECK(layers[2].features[0].props.at("name").str == "Testville" && near(layers[2].features[0].geom.points[0].y, 3000, 0));
    // a tiny vector pack: z0 + z1, leaf directories forced, tiles gzipped
    std::vector<PmtilesTile> tiles{{0, 0, 0, worldTile()}, {1, 0, 0, halfTile(true)}, {1, 0, 1, halfTile(true)}, {1, 1, 0, halfTile(false)}, {1, 1, 1, halfTile(false)}};
    const std::string meta = R"({"name":"test world","attribution":"<a href=\"https://openstreetmap.org\">&copy; OpenStreetMap</a>","vector_layers":[{"id":"earth"},{"id":"water"},{"id":"roads"},{"id":"places"}]})";
    const fs::path file = dir / "world.pmtiles";
    writeText(file, writePmtiles(tiles, TileType::Mvt, meta, true, 2));
    PmtilesReader reader(file);
    CHECK(reader.header().minZoom == 0 && reader.header().maxZoom == 1 && reader.header().tileType == TileType::Mvt);
    CHECK(reader.header().tileCompression == TileCompression::Gzip && reader.header().leafLength > 0);
    CHECK(reader.tile(0, 0, 0) == worldTile() && reader.tile(1, 1, 1) == halfTile(false) && reader.tile(1, 0, 1) == halfTile(true));
    CHECK(!reader.tile(2, 0, 0) && !reader.tile(1, 2, 0));
    const TilesInfo ti = describePmtiles(reader);
    CHECK(ti.name == "test world" && ti.layers.size() == 4 && ti.attribution == "\xC2\xA9 OpenStreetMap");
    // render the world from z0: land west, sea east, the lake is a hole
    Map m;
    RenderOptions o;
    o.width = 512;
    o.bbox = BBox{-180, -70, 180, 70};
    o.basemap = file.string();
    o.legend = o.scaleBar = o.northArrow = false;
    RenderInfo info;
    const raster::Image img = renderImage(m, o, &info);
    CHECK(info.tileSource == file.string() && info.zoom == 0 && info.tilesUsed == 1);
    CHECK(isLand(pixelAt(img, info, -150, 0)) && isLand(pixelAt(img, info, -30, -20)));
    CHECK(isWater(pixelAt(img, info, 90, 0)) && isWater(pixelAt(img, info, -90, 0)));  // the sea, and the lake
    const std::string svg = renderMap(m, o, "svg", &info);
    CHECK(svg.find(">Testville</text>") != std::string::npos);
    CHECK(svg.find("OpenStreetMap</text>") != std::string::npos);
    // overzoom: a close-up far past zoom 1 still draws from the z1 tiles
    o.bbox = BBox{-151, -1, -149, 1};
    RenderInfo close;
    const raster::Image land = renderImage(m, o, &close);
    CHECK(close.zoom == 1 && isLand(land.at(land.width() / 2, land.height() / 2)));
    o.bbox = BBox{89, -1, 91, 1};
    const raster::Image sea = renderImage(m, o, &close);
    CHECK(isWater(sea.at(sea.width() / 2, sea.height() / 2)));
    // a raster pack renders like a tile folder
    const raster::Image green(256, 256, {0, 200, 0, 255});
    writeText(dir / "green.pmtiles", writePmtiles({{0, 0, 0, raster::encodePng(green)}}, TileType::Png, "{}", false));
    o.bbox = BBox{-60, -40, 60, 40};
    o.basemap = (dir / "green.pmtiles").string();
    RenderInfo rinfo;
    const raster::Image rimg = renderImage(m, o, &rinfo);
    CHECK(rinfo.tilesUsed == 1 && rimg.at(rimg.width() / 2, rimg.height() / 2).g > 150 && rimg.at(rimg.width() / 2, rimg.height() / 2).r < 60);
    // MBTiles: a plain message, the countries instead
    o.basemap = (dir / "x.mbtiles").string();
    RenderInfo mb;
    renderImage(m, o, &mb);
    CHECK(!mb.warnings.empty() && mb.warnings[0].find("pmtiles convert") != std::string::npos);
    // installed packs: none yet, then one in <home>/packs/maps-world-low/
    Session s;
    registerCommands(s);
    Result r = run(s, "maps.tiles_info");
    CHECK(!r.ok && r.message.find("no map pack is installed") != std::string::npos);
    o.basemap = "pack";
    RenderInfo none;
    renderImage(m, o, &none);
    CHECK(none.tileSource.empty() && !none.warnings.empty() && none.warnings[0].find("no map pack is installed") != std::string::npos);
    const fs::path packDir = packsDir() / "maps-world-low";
    fs::create_directories(packDir);
    fs::copy_file(file, packDir / "world.pmtiles", fs::copy_options::overwrite_existing);
    CHECK(bestMapPack() && bestMapPack()->filename() == "world.pmtiles");
    RenderOptions defaults;  // basemap=auto picks the pack up
    defaults.width = 256;
    RenderInfo autoInfo;
    renderImage(m, defaults, &autoInfo);
    CHECK(!autoInfo.tileSource.empty());
    // offline place search reads the pack's place names (and caches them)
    r = run(s, "maps.search query=Testvile countries=false");
    CHECK(r.ok && str(r, "name") == "Testville" && str(r, "layer") == "places (map pack)" && near(num(r, "lon"), -153.6, 0.2));
    CHECK(run(s, "maps.search query=Testville").ok);  // second time from the cache
    CHECK(!run(s, "maps.search query=Testville packs=false countries=false").ok);
    r = run(s, "maps.tiles_info");
    CHECK(r.ok && str(r, "format") == "mvt" && num(r, "max_zoom") == 1 && str(r, "layers").find("earth") != std::string::npos);
    r = run(s, "maps.tiles_info path=" + q(dir / "green.pmtiles"));
    CHECK(r.ok && str(r, "format") == "png" && num(r, "min_zoom") == 0);
    r = run(s, "maps.new title=Pack");
    CHECK(r.ok);
    r = run(s, "maps.render doc=" + toText(r.value) + " path=" + q(dir / "pack.png") + " bbox=-180,-70,180,70 width=300");
    CHECK(r.ok && str(r, "tile_source").find("world.pmtiles") != std::string::npos && fs::exists(dir / "pack.png"));
    fs::remove_all(packsDir());
}

// ---- a multi-zoom pack cut from real data -----------------------------------------------------
// No real Protomaps/planetiler pack is vendored (none could be fetched offline with a clear licence),
// so this builds one the way they are laid out: the public-domain Natural Earth countries cut into
// MVT tiles for zooms 0-3, clipped with a buffer, outer rings clockwise and holes anticlockwise,
// water under every tile, gzip, deduplicated ocean tiles and leaf directories. Testing against a
// pack written by another tool is still open.

using TilePt = std::pair<int, int>;

// One Sutherland-Hodgman pass: keeps the side of the line axis = bound where (v >= bound) == keepAbove.
std::vector<std::pair<double, double>> clipEdge(const std::vector<std::pair<double, double>>& pts, int axis, double bound, bool keepAbove) {
    std::vector<std::pair<double, double>> out;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::pair<double, double> cur = pts[i];
        const std::pair<double, double> prev = pts[(i + n - 1) % n];
        const double cv = axis == 0 ? cur.first : cur.second;
        const double pv = axis == 0 ? prev.first : prev.second;
        const bool curIn = keepAbove ? cv >= bound : cv <= bound;
        const bool prevIn = keepAbove ? pv >= bound : pv <= bound;
        if (curIn != prevIn) {
            const double t = (bound - pv) / (cv - pv);
            out.emplace_back(prev.first + (cur.first - prev.first) * t, prev.second + (cur.second - prev.second) * t);
        }
        if (curIn) out.push_back(cur);
    }
    return out;
}

// Sutherland-Hodgman against the tile square plus a buffer, in tile units.
std::vector<TilePt> clipRing(const std::vector<std::pair<double, double>>& ring, double lo, double hi) {
    std::vector<std::pair<double, double>> pts = ring;
    for (int edge = 0; edge < 4 && !pts.empty(); ++edge) pts = clipEdge(pts, edge < 2 ? 0 : 1, edge % 2 == 0 ? lo : hi, edge % 2 == 0);
    std::vector<TilePt> q;
    for (const auto& p : pts) {
        const TilePt t{static_cast<int>(std::lround(p.first)), static_cast<int>(std::lround(p.second))};
        if (q.empty() || q.back() != t) q.push_back(t);
    }
    while (q.size() > 1 && q.front() == q.back()) q.pop_back();
    return q;
}

double ringArea(const std::vector<TilePt>& r) {  // positive = clockwise on screen (y down)
    double a = 0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const auto& p = r[i];
        const auto& n = r[(i + 1) % r.size()];
        a += static_cast<double>(p.first) * n.second - static_cast<double>(n.first) * p.second;
    }
    return a / 2;
}

std::string countriesTile(int z, int x, int y, std::size_t& earthFeatures) {
    const double lo = -64, hi = 4096 + 64;
    std::vector<TestFeature> earth, places, borders;
    for (const auto& f : countries()) {
        TestFeature tf{3, {}, {{"kind", "earth"}}};
        for (const auto& poly : f.geom.polygons) {
            std::vector<std::vector<TilePt>> rings;
            for (std::size_t ri = 0; ri < poly.size(); ++ri) {
                std::vector<std::pair<double, double>> pts;
                for (const auto& c : poly[ri]) {
                    double tx = 0, ty = 0;
                    lonLatToTile(c.x, std::clamp(c.y, -85.0, 85.0), z, tx, ty);
                    pts.push_back({(tx - x) * 4096, (ty - y) * 4096});
                }
                if (ri == 0) {
                    // the border as a line: runs of points in the tile, plus one point either side (cut by the renderer)
                    auto in = [&](std::size_t i) {
                        const auto& q = pts[i % pts.size()];
                        return q.first >= lo && q.first <= hi && q.second >= lo && q.second <= hi;
                    };
                    std::vector<TilePt> run;
                    for (std::size_t i = 0; i <= pts.size(); ++i) {
                        const bool keep = in(i) || in(i + 1) || (i > 0 && in(i - 1));
                        if (keep) run.push_back({static_cast<int>(std::lround(pts[i % pts.size()].first)), static_cast<int>(std::lround(pts[i % pts.size()].second))});
                        if ((!keep || i == pts.size()) && run.size() >= 2) borders.push_back(TestFeature{2, {run}, {{"kind", "country"}}});
                        if (!keep) run.clear();
                    }
                }
                auto r = clipRing(pts, lo, hi);
                if (r.size() < 3 || std::abs(ringArea(r)) < 1) {
                    if (ri == 0) break;  // the outer ring is outside this tile: skip the polygon
                    continue;
                }
                const bool outer = ri == 0;
                if ((ringArea(r) > 0) != outer) std::reverse(r.begin(), r.end());
                rings.push_back(std::move(r));
            }
            for (auto& r : rings) tf.parts.push_back(std::move(r));
        }
        if (tf.parts.empty()) continue;
        earth.push_back(std::move(tf));
        // a label point: the middle of the first outer ring's box, when it falls inside this tile
        const auto& ring = f.geom.polygons[0][0];
        double minX = 1e9, minY = 1e9, maxX = -1e9, maxY = -1e9;
        for (const auto& c : ring) {
            minX = std::min(minX, c.x), maxX = std::max(maxX, c.x), minY = std::min(minY, c.y), maxY = std::max(maxY, c.y);
        }
        double tx = 0, ty = 0;
        lonLatToTile((minX + maxX) / 2, (minY + maxY) / 2, z, tx, ty);
        const int px = static_cast<int>((tx - x) * 4096), py = static_cast<int>((ty - y) * 4096);
        if (px >= 0 && px < 4096 && py >= 0 && py < 4096 && z >= 2) {
            places.push_back(TestFeature{1, {{{px, py}}}, {{"kind", "country"}, {"name", f.props.at("name").text()}}});
        }
    }
    earthFeatures += earth.size();
    // like Protomaps: the background is sea, land is the earth layer; open-ocean tiles hold one water square
    if (earth.empty()) return mvtTile({mvtLayer("water", {TestFeature{3, {{{-64, -64}, {4160, -64}, {4160, 4160}, {-64, 4160}}}, {{"kind", "ocean"}}}})});
    std::vector<std::string> layers{mvtLayer("earth", earth), mvtLayer("boundaries", borders)};  // borders may be empty (an island tile)
    if (!places.empty()) layers.push_back(mvtLayer("places", places));
    return mvtTile(layers);
}

void testRealDataPack(const fs::path& dir) {
    std::vector<PmtilesTile> tiles;
    std::size_t earthFeatures = 0;
    // a fresh counter per tile: accumulating straight into earthFeatures across this loop came out
    // wrong on MSVC CI (each tile counted alone was right, the loop total was not)
    for (int z = 0; z <= 3; ++z) {
        const int n = 1 << z;
        for (int x = 0; x < n; ++x) {
            for (int y = 0; y < n; ++y) {
                std::size_t count = 0;
                std::string bytes = countriesTile(z, x, y, count);
                earthFeatures += count;
                tiles.push_back({z, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), std::move(bytes)});
            }
        }
    }
    CHECK(tiles.size() == 85 && earthFeatures > 177 * 2);
    const std::string meta = R"j({"name":"natural earth z0-3","attribution":"Natural Earth (public domain)","vector_layers":[{"id":"water"},{"id":"earth"},{"id":"boundaries"},{"id":"places"}]})j";
    const fs::path file = dir / "ne.pmtiles";
    writeText(file, writePmtiles(tiles, TileType::Mvt, meta, true, 16));
    CHECK(fs::file_size(file) < 1500000);
    PmtilesReader reader(file);
    const auto& h = reader.header();
    CHECK(h.minZoom == 0 && h.maxZoom == 3 && h.addressedTiles == 85 && h.leafLength > 0);
    CHECK(h.tileContents < h.tileEntries || h.tileEntries < h.addressedTiles);  // open-ocean tiles are stored once
    bool allBack = true;
    for (const auto& t : tiles) allBack = allBack && reader.tile(t.z, t.x, t.y) == t.bytes;
    CHECK(allBack);
    CHECK(!reader.tile(4, 0, 0));
    // every tile decodes, with outer rings first
    bool decodes = true;
    for (const auto& t : tiles) {
        const auto layers = decodeMvt(t.bytes);
        decodes = decodes && !layers.empty() && (layers[0].name == "water" || (layers[0].name == "earth" && layers[1].name == "boundaries" && !layers[0].features.empty()));
    }
    CHECK(decodes);
    const TilesInfo ti = describePmtiles(reader);
    CHECK(ti.layers.size() == 4 && ti.attribution == "Natural Earth (public domain)");

    // the whole world from z0
    Map m;
    RenderOptions o;
    o.width = 720;
    o.bbox = BBox{-180, -70, 180, 75};
    o.basemap = file.string();
    o.legend = o.scaleBar = o.northArrow = false;
    RenderInfo info;
    const raster::Image world = renderImage(m, o, &info);
    CHECK(info.tileSource == file.string() && info.zoom <= 2 && info.tilesUsed >= 1);
    struct Spot {
        double lon, lat;
        bool land;
    };
    const std::vector<Spot> spots = {{3, 27, true},      // Sahara (Algeria)
                                     {-60, -10, true},   // Amazon
                                     {100, 60, true},    // Siberia
                                     {135, -25, true},   // Australia
                                     {-35, 20, false},   // mid-Atlantic
                                     {-150, 0, false},   // Pacific
                                     {80, -30, false}};  // Indian Ocean
    bool worldOk = true;
    for (const auto& sp : spots) worldOk = worldOk && (sp.land ? isLand(pixelAt(world, info, sp.lon, sp.lat)) : isWater(pixelAt(world, info, sp.lon, sp.lat)));
    CHECK(worldOk);
    // western Europe at the deepest zoom: several z3 tiles stitched together
    o.width = 1400;
    o.bbox = BBox{-12, 35, 20, 60};
    RenderInfo eu;
    const raster::Image europe = renderImage(m, o, &eu);
    CHECK(eu.zoom == 3 && eu.tilesUsed >= 2);
    CHECK(isLand(pixelAt(europe, eu, 2.35, 47.0)) && isLand(pixelAt(europe, eu, -3.7, 40.4)) && isLand(pixelAt(europe, eu, 10, 51)));  // France, Spain, Germany
    CHECK(isWater(pixelAt(europe, eu, -9, 46)) && isWater(pixelAt(europe, eu, 4, 55)) && isWater(pixelAt(europe, eu, 5, 38)));  // Biscay, North Sea, Mediterranean
    const std::string svg = renderMap(m, o, "svg", &eu);
    CHECK(svg.find(">France</text>") != std::string::npos || svg.find(">Spain</text>") != std::string::npos);
    // a city-scale close-up past the deepest zoom still draws from z3
    o.width = 400;
    o.bbox = BBox{-3.8, 40.3, -3.6, 40.5};
    RenderInfo city;
    const raster::Image madrid = renderImage(m, o, &city);
    CHECK(city.zoom == 3 && isLand(madrid.at(madrid.width() / 2, madrid.height() / 2)));
    // through the commands, as an installed pack
    Session s;
    registerCommands(s);
    Result r = run(s, "maps.tiles_info path=" + q(file));
    CHECK(r.ok && num(r, "max_zoom") == 3 && str(r, "layers").find("earth") != std::string::npos);
    r = run(s, "maps.new title=Europe");
    CHECK(r.ok);
    r = run(s, "maps.render doc=" + toText(r.value) + " path=" + q(dir / "europe.png") + " basemap=" + q(file) + " bbox=-12,35,20,60 width=600");
    CHECK(r.ok && str(r, "tile_source").find("ne.pmtiles") != std::string::npos && fs::exists(dir / "europe.png"));
}

void testMcp(const fs::path& dir) {
    Session session;
    registerCommands(session);
    mcp::Server server(session, "test", "0");
    auto call = [&](const std::string& tool, const std::string& args) {
        return server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":")" + tool + R"(","arguments":)" + args + "}}").value_or("");
    };
    const std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"maps_new", "maps_open", "maps_import", "maps_export", "maps_render", "maps_preview", "maps_buffer", "maps_filter",
                             "maps_join", "maps_heatmap", "maps_track_stats", "maps_measure", "maps_transform", "maps_style", "maps_nearest",
                             "maps_contains", "maps_simplify", "maps_clip", "maps_raster_add", "maps_add_countries", "maps_spatial_join"}) {
        CHECK(list.find(std::string("\"") + tool + "\"") != std::string::npos);
    }
    std::string out = call("maps_new", R"({"title":"MCP"})");
    CHECK(out.find("map1") != std::string::npos);
    out = call("maps_feature_add", R"({"doc":"map1","layer":"pins","lat":40.4168,"lon":-3.7038,"props":"name=Madrid"})");
    CHECK(out.find("\"isError\":false") != std::string::npos || out.find("isError") == std::string::npos);
    out = call("maps_style", R"({"doc":"map1","layer":"pins","label":"name","preview":true})");
    CHECK(out.find("\"type\":\"image\"") != std::string::npos);
    out = call("maps_preview", R"({"doc":"map1","bbox":[-10,35,5,44]})");
    CHECK(out.find("image/png") != std::string::npos);
    const std::string path = (dir / "mcp.geojson").generic_string();
    out = call("maps_export", R"({"doc":"map1","layer":"pins","path":")" + path + R"("})");
    CHECK(fs::exists(dir / "mcp.geojson"));
    out = call("maps_measure", R"({"from":[40.4168,-3.7038],"to":[41.3874,2.1686]})");
    CHECK(out.find("distance_m") != std::string::npos);
}

}  // namespace

// ---- Jenks natural breaks -------------------------------------------------------------------

double classCost(const std::vector<double>& v, std::size_t b, std::size_t e) {  // squared deviation of v[b..e)
    double mean = 0;
    for (std::size_t i = b; i < e; ++i) mean += v[i];
    mean /= static_cast<double>(e - b);
    double c = 0;
    for (std::size_t i = b; i < e; ++i) c += (v[i] - mean) * (v[i] - mean);
    return c;
}

// Total squared deviation when sorted values are split at the class tops in `breaks`.
double breaksCost(const std::vector<double>& sorted, const std::vector<double>& breaks) {
    double c = 0;
    std::size_t b = 0;
    for (std::size_t k = 1; k < breaks.size(); ++k) {
        std::size_t e = b;
        while (e < sorted.size() && (sorted[e] <= breaks[k] || k + 1 == breaks.size())) ++e;
        if (e > b) c += classCost(sorted, b, e);
        b = e;
    }
    return c;
}

// The true optimum by trying every way to cut n sorted values into k runs.
double bruteForce(const std::vector<double>& v, int k) {
    double best = 1e300;
    std::vector<std::size_t> cuts;
    std::function<void(std::size_t, int, double)> go = [&](std::size_t start, int left, double acc) {
        if (left == 1) {
            best = std::min(best, acc + classCost(v, start, v.size()));
            return;
        }
        for (std::size_t e = start + 1; e + static_cast<std::size_t>(left - 1) <= v.size(); ++e) go(e, left - 1, acc + classCost(v, start, e));
    };
    go(0, k, 0);
    return best;
}

void testJenks() {
    // three obvious groups
    auto b = jenksBreaks({22, 1, 11, 2, 20, 3, 12, 10, 21}, 3);
    CHECK(b == (std::vector<double>{1, 3, 12, 22}));
    // simple-statistics' ckmeans example: [-1 x4], [2 x3], [4, 5, 6]
    b = jenksBreaks({-1, 2, -1, 2, 4, 5, 6, -1, 2, -1}, 3);
    CHECK(b == (std::vector<double>{-1, -1, 2, 6}));
    // fewer distinct values than classes, one class, nothing usable
    CHECK(jenksBreaks({5, 5, 7}, 4) == (std::vector<double>{5, 5, 7}));
    CHECK(jenksBreaks({3, 9, 4}, 1) == (std::vector<double>{3, 9}));
    CHECK(jenksBreaks({std::nan(""), INFINITY}, 3).empty());
    CHECK(jenksBreaks({}, 3).empty());
    // the optimum, checked against every possible split
    std::mt19937 rng(11);
    bool optimal = true;
    for (int trial = 0; trial < 60; ++trial) {
        std::vector<double> v;
        const int n = 6 + static_cast<int>(rng() % 9);
        for (int i = 0; i < n; ++i) v.push_back(static_cast<double>(rng() % 1000) / 10.0 + (trial % 3 == 0 ? 1e6 : 0));  // + an offset: precision
        std::sort(v.begin(), v.end());
        for (int k = 2; k <= std::min(5, n); ++k) {
            const auto jb = jenksBreaks(v, k);
            optimal = optimal && jb.front() == v.front() && jb.back() == v.back() && std::abs(breaksCost(v, jb) - bruteForce(v, k)) <= 1e-6 * (1 + bruteForce(v, k));
        }
    }
    CHECK(optimal);
    // big inputs: 300k values in three bands, merged into runs, still fast and still finds the gaps
    std::vector<double> big;
    std::uniform_real_distribution<double> u(0, 1);
    for (int i = 0; i < 300000; ++i) big.push_back((i % 3) * 100 + u(rng) * 10);
    const auto t0 = std::chrono::steady_clock::now();
    b = jenksBreaks(big, 3);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(b.size() == 4 && b[1] < 10 && b[2] > 100 && b[2] < 110 && b[3] > 200);
    CHECK(ms < 5000);
    b = jenksBreaks(big, 3, 64);  // only 64 runs: the gaps are still run edges
    CHECK(b.size() == 4 && b[1] < 10 && b[2] > 100 && b[2] < 110 && b[3] > 200);
    // and through maps.style: a graduated layer with method=jenks
    Layer l;
    l.name = "pts";
    for (double x : {1.0, 2.0, 3.0, 10.0, 11.0, 12.0, 20.0, 21.0, 22.0}) {
        Feature f;
        f.geom.type = GeomType::Point;
        f.geom.points.push_back({x, 0});
        f.props["v"] = Prop::number(x);
        l.features.push_back(f);
    }
    l.style.mode = "graduated";
    l.style.field = "v";
    l.style.classes = 3;
    l.style.method = "jenks";
    auto cls = styleClasses(l);
    CHECK(cls.size() == 3 && cls[0].lo == 1 && cls[0].hi == 3 && cls[1].hi == 12 && cls[2].hi == 22);
    l.style.method = "natural";
    CHECK(styleClasses(l).size() == 3 && styleClasses(l)[1].hi == 12);
    // a class that is only the minimum survives the duplicate-break clean-up
    for (int i = 0; i < 6; ++i) {
        Feature f;
        f.geom.type = GeomType::Point;
        f.geom.points.push_back({0, 0});
        f.props["v"] = Prop::number(0);
        l.features.push_back(f);
    }
    l.style.method = "quantile";  // tops 0, 10, 22: the first class is just the zeros
    cls = styleClasses(l);
    CHECK(cls.size() == 3 && cls[0].lo == 0 && cls[0].hi == 0 && cls[1].hi == 10);
}

int main() {
    const fs::path dir = tempDir();
    // map packs are looked up under $WETWARESUITE_HOME: keep the user's own out of the tests
    setEnv("WETWARESUITE_HOME", (dir / "home").string());
    testGeodesy();
    testProjections();
    testExpressions();
    testGeoJsonAndWkt(dir);
    testKmlGpxCsv(dir);
    testShapefile(dir);
    testRasters(dir);
    testTiffCodecs();
    testAnalysis();
    testJenks();
    testRendering(dir);
    testCommands(dir);
    testPmtiles(dir);
    testRealDataPack(dir);
    testMcp(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    return check::summary("maps");
}
