// Offline navigation and places: routes and reachable areas on a road layer,
// searching places by name, bookmarks, the map's view, and what is under a
// click in the window's picture (maps.hit).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cmd.hpp"
#include "wetwaresuite/maps/tiles.hpp"

namespace wetwaresuite::maps::cmd {

namespace {

using namespace detail;

std::string latLonText(const Coord& c) { return fmt(c.y, 6) + "," + fmt(c.x, 6); }

// The road layer: layer=, else the top layer with OSM highway tags, else the top layer with lines.
Layer* roadLayer(Map& m, const Params& p, std::string& error) {
    if (!has(p, "layer")) {
        for (auto it = m.layers.rbegin(); it != m.layers.rend(); ++it) {
            if (it->isRaster()) continue;
            for (const auto& f : it->features) {
                if (f.geom.type == GeomType::Line && f.props.count("highway")) return &*it;
            }
        }
    }
    if (has(p, "layer")) return vectorLayer(m, p, error);
    // any lines, but not routes made earlier
    for (auto it = m.layers.rbegin(); it != m.layers.rend(); ++it) {
        if (it->isRaster() || it->features.empty()) continue;
        const Feature& f = it->features.front();
        if (f.props.count("duration_s") && f.props.count("profile")) continue;
        for (const auto& g : it->features) {
            if (g.geom.type == GeomType::Line) return &*it;
        }
    }
    error = "the map has no road layer: open an .osm export (maps.open path=area.osm) or import lines";
    return nullptr;
}

RouteOptions routeOptions(const Params& p) {
    RouteOptions o;
    o.profile = stringParam(p, "profile", "car");
    o.shortest = boolParam(p, "shortest", false);
    o.snapMax = std::max(1.0, numberParam(p, "snap", 5000));
    return o;
}

std::vector<ParamSpec> routeParams() {
    return {docParam(),
            layerParam("road layer (default: the top layer with OSM highway tags, else the top layer with lines)"),
            {"profile", ParamType::String, "how you travel: speeds and which roads you may use", false, {"car", "bike", "foot"}, std::string("car")},
            {"shortest", ParamType::Bool, "shortest distance instead of quickest time", false, {}, false},
            {"snap", ParamType::Number, "fail when a point is further than this (metres) from a usable road", false, {}, 5000.0}};
}

void registerRouting(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> params = routeParams();
    params.push_back({"from", ParamType::NumberList, "lat,lon of the start", true});
    params.push_back({"to", ParamType::NumberList, "lat,lon of the end", true});
    params.push_back({"via", ParamType::NumberList, "lat,lon pairs to pass through on the way, in order", false});
    params.push_back(outputParam("layer for the route (default 'route'; added to when it exists)"));
    params.push_back(previewParam());
    reg.add({"maps.route", "Route",
             "Plan a route offline on a road layer (an .osm export opened with maps.open, or any lines): the quickest (or "
             "shortest=true) way by car, bike or foot from= to= through via= points, with turn-by-turn directions. Car, bike "
             "and foot use different roads and speeds from OSM highway tags, and respect one-way streets and access=no; "
             "lines without tags are roads anyone may use. Adds the route as a line feature with distance_m and duration_s.",
             params, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* roads = roadLayer(doc.map, p, error);
                if (!roads) return Result::failure(error);
                std::vector<Coord> stops(1);
                if (!latLon(p, "from", stops[0], error)) return Result::failure(error);
                if (has(p, "via")) {
                    const NumberList v = listParam(p, "via");
                    if (v.size() % 2) return Result::failure("via needs lat,lon pairs");
                    for (std::size_t i = 0; i + 1 < v.size(); i += 2) stops.push_back({v[i + 1], v[i]});
                }
                Coord to;
                if (!latLon(p, "to", to, error)) return Result::failure(error);
                stops.push_back(to);
                const RouteOptions o = routeOptions(p);
                const RoadGraph graph(roads->features, o);
                const Route route = graph.route(stops);
                const std::string roadsName = roads->name;
                Feature f;
                f.geom = Geometry::line(route.line);
                f.props["profile"] = Prop::string(o.profile);
                f.props["distance_m"] = Prop::number(std::round(route.distance * 10) / 10);
                f.props["duration_s"] = Prop::number(std::round(route.duration));
                f.props["from"] = Prop::string(latLonText(stops.front()));
                f.props["to"] = Prop::string(latLonText(stops.back()));
                const std::string name = has(p, "output") ? stringParam(p, "output") : "route";
                Layer* out = doc.map.layer(name);
                if (out && out->isRaster()) return Result::failure("'" + name + "' is a raster layer");
                if (!out) {
                    Layer nl;
                    nl.style.stroke = {0xd0, 0x30, 0x30, 255};
                    nl.style.width = 4;
                    nl.style.opacity = 0.85;
                    out = &addLayer(doc.map, std::move(nl), name);
                }
                out->features.push_back(std::move(f));
                std::string m = o.profile + " route on " + roadsName + ": " + metres(route.distance) + ", " + duration(route.duration) + " (" +
                                (o.shortest ? "shortest" : "quickest") + "), " + plural(route.steps.size(), "step");
                for (std::size_t i = 0; i < route.steps.size(); ++i) {
                    const RouteStep& s = route.steps[i];
                    m += "\n" + std::to_string(i + 1) + ". " + s.instruction;
                    if (s.distance > 0) m += " (" + metres(s.distance) + ")";
                }
                for (std::size_t i = 0; i < route.snapped.size(); ++i) {
                    if (route.snapped[i] > 200) m += "\nnote: point " + std::to_string(i + 1) + " is " + metres(route.snapped[i]) + " from the nearest road";
                }
                return edited(m, p)
                    .with("layer", out->name)
                    .with("index", static_cast<std::int64_t>(out->features.size() - 1))
                    .with("distance_m", route.distance)
                    .with("duration_s", route.duration)
                    .with("steps", static_cast<std::int64_t>(route.steps.size()))
                    .with("nodes", static_cast<std::int64_t>(graph.nodeCount()));
            }));

    params = routeParams();
    params.push_back({"from", ParamType::NumberList, "lat,lon to start from", true});
    params.push_back({"minutes", ParamType::Number, "travel time (or, with shortest=true, distance in metres)", false, {}, 10.0});
    params.push_back({"buffer", ParamType::Number, "metres either side of the reachable roads", false, {}, 60.0});
    params.push_back(outputParam("new layer name (default 'reach')"));
    params.push_back(previewParam());
    reg.add({"maps.reach", "Reachable Area",
             "Everything reachable within minutes= by car, bike or foot along a road layer (an isochrone), as a polygon of the "
             "reachable roads widened by buffer= metres. With shortest=true, minutes= is a distance in metres instead.",
             params, true},
            mapCommand(session, [](MapDoc& doc, const Params& p) {
                std::string error;
                Layer* roads = roadLayer(doc.map, p, error);
                if (!roads) return Result::failure(error);
                Coord from;
                if (!latLon(p, "from", from, error)) return Result::failure(error);
                const RouteOptions o = routeOptions(p);
                const double amount = numberParam(p, "minutes", 10);
                if (!(amount > 0)) return Result::failure("minutes must be more than 0");
                const RoadGraph graph(roads->features, o);
                const auto lines = graph.reachable(from, o.shortest ? amount : amount * 60);
                if (lines.empty()) return Result::failure("nothing is reachable from there");
                Geometry net;
                net.type = GeomType::Line;
                net.lines = lines;
                Feature f;
                f.geom = buffer(net, std::max(1.0, numberParam(p, "buffer", 60)), 700);
                f.props["profile"] = Prop::string(o.profile);
                f.props[o.shortest ? "metres" : "minutes"] = Prop::number(amount);
                const double a = area(f.geom);
                f.props["area_km2"] = Prop::number(std::round(a / 1e4) / 100);
                Layer nl;
                nl.style.fill = {0x3a, 0x7b, 0xd5, 255};
                nl.style.stroke = {0x1d, 0x4f, 0x9a, 255};
                nl.style.fillOpacity = 0.3;
                nl.features.push_back(std::move(f));
                Layer& added = addLayer(doc.map, std::move(nl), has(p, "output") ? stringParam(p, "output") : "reach");
                return edited("reachable within " + (o.shortest ? metres(amount) : fmt(amount, 1) + " min") + " by " + o.profile + ": " + squareMetres(a) + " -> " + added.name, p)
                    .with("layer", added.name)
                    .with("area_m2", a)
                    .with("road_pieces", static_cast<std::int64_t>(lines.size()));
            }));
}

// ---- search -------------------------------------------------------------------------------

// Lower case, with the accents of Latin letters taken off (é -> e, ß -> ss), for matching.
std::string fold(const std::string& s) {
    static const char* const map2[64] = {"a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  // C3 80-8F
                                         "d", "n", "o", "o", "o", "o", "o", "x", "o", "u", "u", "u", "u", "y", "th", "ss",  // C3 90-9F
                                         "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",  // C3 A0-AF
                                         "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};  // C3 B0-BF
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const auto c = static_cast<unsigned char>(s[i]);
        if (c == 0xC3 && i + 1 < s.size()) {
            const auto d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0x80 && d <= 0xBF) {
                out += map2[d - 0x80];
                ++i;
                continue;
            }
        }
        if (c == 0xC5 && i + 1 < s.size()) {  // œ, š, ž, ł and friends
            const auto d = static_cast<unsigned char>(s[i + 1]);
            if (d == 0x92 || d == 0x93) out += "oe";
            else if (d == 0xA0 || d == 0xA1) out += "s";
            else if (d == 0xBD || d == 0xBE) out += "z";
            else if (d == 0x81 || d == 0x82) out += "l";
            else out += "?";
            ++i;
            continue;
        }
        if (c < 0x80) out += static_cast<char>(std::tolower(c));
        else out += static_cast<char>(c);
    }
    return out;
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || u >= 0x80) {
            cur += c;
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::size_t editDistance(const std::string& a, const std::string& b, std::size_t cap) {
    if (a.size() > b.size() + cap || b.size() > a.size() + cap) return cap + 1;
    std::vector<std::size_t> prev(b.size() + 1), cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        std::size_t best = cur[0];
        for (std::size_t j = 1; j <= b.size(); ++j) {
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
            best = std::min(best, cur[j]);
        }
        if (best > cap) return cap + 1;
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

// How well a text matches the query (0 = not at all).
int matchScore(const std::string& textFolded, const std::string& q, const std::vector<std::string>& qWords) {
    if (textFolded.empty()) return 0;
    if (textFolded == q) return 100;
    if (textFolded.rfind(q, 0) == 0) return 80;
    const auto tw = words(textFolded);
    for (const auto& w : tw) {
        if (w.rfind(q, 0) == 0) return 60;
    }
    if (textFolded.find(q) != std::string::npos) return 45;
    if (qWords.size() > 1) {
        bool all = true;
        for (const auto& qw : qWords) all = all && textFolded.find(qw) != std::string::npos;
        if (all) return 35;
    }
    // a typo or two: every query word must be there, short ones exactly, longer ones within one or two letters
    int exact = 0;
    for (const auto& qw : qWords) {
        bool ok = false;
        for (const auto& w : tw) {
            if (w.rfind(qw, 0) == 0) {
                ok = true;
                ++exact;
                break;
            }
        }
        if (!ok && qw.size() >= 4) {
            const std::size_t cap = qw.size() >= 8 ? 2 : 1;
            for (const auto& w : tw) {
                // a typo in the start of a longer word only counts for long queries ("paris" is not "pakistan")
                if (editDistance(qw, w, cap) <= cap || (qw.size() >= 7 && w.size() > qw.size() && editDistance(qw, w.substr(0, qw.size()), cap) <= cap)) {
                    ok = true;
                    break;
                }
            }
        }
        if (!ok) return 0;
    }
    return exact < static_cast<int>(qWords.size()) ? 20 + std::min(exact * 3, 14) : 0;
}

bool nameField(const std::string& k) {
    const std::string l = lower(k);
    return l == "name" || l.rfind("name:", 0) == 0 || l == "name_en" || l == "title" || l == "label" || l == "ref" || l == "official_name" || l == "alt_name";
}

struct Hit {
    int score = 0;
    std::string layer, text, field;
    std::size_t index = 0;
    Coord at;
    double distance = std::numeric_limits<double>::infinity();
};

void registerSearch(Session& session) {
    auto& reg = session.registry();
    reg.add({"maps.search", "Search Places",
             "Find places by name, offline: searches the text fields of every vector layer (or layer=), the place and "
             "point-of-interest names in installed map packs (read once, then kept in the cache folder), and the built-in "
             "countries and about 1,250 cities, ignoring case and accents, allowing a typo or two. Best matches first (exact, then "
             "starting with, then containing); near=lat,lon puts closer ones first among equals. Without doc= it searches the "
             "built-in countries and cities only.",
             {{"doc", ParamType::String, "map id (default: the built-in countries and cities only)", false}, {"query", ParamType::String, "what to look for, e.g. 'cafe flore'", true},
              layerParam("only this layer"), {"near", ParamType::NumberList, "lat,lon: prefer places close to here", false},
              {"countries", ParamType::Bool, "also search the built-in countries and cities", false, {}, true},
              {"packs", ParamType::Bool, "also search the place names in installed map packs", false, {}, true},
              {"limit", ParamType::Int, "how many to list", false, {}, std::int64_t{10}}},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p] {
                    try {
                        const std::string q = trim(fold(stringParam(p, "query")));
                        if (q.empty()) return Result::failure("query is empty");
                        const auto qWords = words(q);
                        std::optional<Coord> near;
                        if (has(p, "near")) {
                            Coord c;
                            std::string error;
                            if (!latLon(p, "near", c, error)) return Result::failure(error);
                            near = c;
                        }
                        std::vector<Hit> hits;
                        auto scan = [&](const std::string& layerName, const std::vector<Feature>& fs) {
                            for (std::size_t i = 0; i < fs.size(); ++i) {
                                Hit best;
                                for (const auto& [k, v] : fs[i].props) {
                                    if (v.kind != Prop::String) continue;
                                    int s = matchScore(fold(v.str), q, qWords);
                                    if (!s) continue;
                                    // names count most; a category such as amenity=cafe counts half
                                    s = nameField(k) ? s + 10 : s / 2;
                                    if (s > best.score) {
                                        best.score = s;
                                        best.text = v.str;
                                        best.field = k;
                                    }
                                }
                                if (!best.score || fs[i].geom.empty()) continue;
                                if (!nameField(best.field)) {  // found by its kind: still show its name
                                    const std::string n = featureName(fs[i]);
                                    if (!n.empty()) best.text = n;
                                }
                                best.layer = layerName;
                                best.index = i;
                                best.at = labelPoint(fs[i].geom);
                                if (near) best.distance = geodesicDistance(near->x, near->y, best.at.x, best.at.y);
                                hits.push_back(best);
                            }
                        };
                        const std::string id = stringParam(p, "doc");
                        if (!id.empty()) {
                            auto doc = session.workspace().getAs<MapDoc>(id);
                            if (!doc) return Result::failure("no open map called '" + id + "'");
                            if (has(p, "layer")) {
                                std::string error;
                                Layer* l = vectorLayer(doc->map, p, error);
                                if (!l) return Result::failure(error);
                                scan(l->name, l->features);
                            } else {
                                for (const auto& l : doc->map.layers) {
                                    if (!l.isRaster()) scan(l.name, l.features);
                                }
                            }
                        }
                        if (boolParam(p, "packs", true) && !has(p, "layer")) {
                            const auto packed = allPackPlaces();
                            scan("places (map pack)", *packed);
                        }
                        if (boolParam(p, "countries", true) && !has(p, "layer")) {
                            scan("places (built in)", places());
                            scan("countries (built in)", countries());
                        }
                        std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
                            if (a.score != b.score) return a.score > b.score;
                            return a.distance < b.distance;
                        });
                        const auto limit = static_cast<std::size_t>(std::clamp<std::int64_t>(intParam(p, "limit", 10), 1, 500));
                        if (hits.empty()) return Result::failure("nothing called '" + stringParam(p, "query") + "' in " + (id.empty() ? "the built-in countries" : id));
                        std::string m = plural(hits.size(), "match") + " for '" + stringParam(p, "query") + "'";
                        for (std::size_t i = 0; i < hits.size() && i < limit; ++i) {
                            const Hit& h = hits[i];
                            m += "\n" + std::to_string(i + 1) + ". " + h.text + " (" + h.layer + " #" + std::to_string(h.index) + ") at " + latLonText(h.at);
                            if (std::isfinite(h.distance)) m += ", " + metres(h.distance) + " away";
                        }
                        Result r = Result::success(m, hits.front().text);
                        return r.with("count", static_cast<std::int64_t>(hits.size()))
                            .with("name", hits.front().text)
                            .with("layer", hits.front().layer)
                            .with("index", static_cast<std::int64_t>(hits.front().index))
                            .with("lat", hits.front().at.y)
                            .with("lon", hits.front().at.x);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

}  // namespace

void registerNavCommands(Session& session) {
    registerRouting(session);
    registerSearch(session);
}

}  // namespace wetwaresuite::maps::cmd
