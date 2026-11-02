#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "internal.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/ops.hpp"

namespace wetwaresuite::pdf::tool {

// ---- helpers -------------------------------------------------------------------------

fs::path pathFromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string readBytes(const std::string& path) {
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) throw std::runtime_error("can't open " + path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void writeBytes(const std::string& path, const std::string& bytes) {
    const fs::path p = pathFromUtf8(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    if (!out) throw std::runtime_error("can't write " + path);
    out << bytes;
    if (!out) throw std::runtime_error("couldn't finish writing " + path);
}

std::string utf8Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::optional<Color> parseColor(const std::string& textIn) {
    std::string t = utf8Lower(textIn);
    t.erase(std::remove(t.begin(), t.end(), ' '), t.end());
    if (t.empty()) return std::nullopt;
    static const std::map<std::string, Color> named = {
        {"black", {0, 0, 0, 255}},        {"white", {255, 255, 255, 255}},   {"red", {220, 30, 30, 255}},
        {"green", {20, 150, 60, 255}},     {"blue", {30, 80, 220, 255}},      {"navy", {20, 40, 110, 255}},
        {"yellow", {255, 225, 0, 255}},    {"orange", {255, 150, 0, 255}},    {"pink", {255, 120, 190, 255}},
        {"purple", {130, 60, 200, 255}},   {"gray", {128, 128, 128, 255}},    {"grey", {128, 128, 128, 255}},
        {"cyan", {0, 200, 230, 255}},      {"teal", {0, 128, 128, 255}},      {"brown", {120, 70, 30, 255}},
        {"lightgreen", {140, 230, 120, 255}}, {"lightblue", {130, 200, 255, 255}}, {"transparent", {0, 0, 0, 0}},
    };
    auto it = named.find(t);
    if (it != named.end()) return it->second;
    if (t[0] == '#') t = t.substr(1);
    auto hex = [&](std::size_t at) { return std::stoi(t.substr(at, 2), nullptr, 16); };
    try {
        if (t.size() == 3) {
            const int r = std::stoi(t.substr(0, 1), nullptr, 16), g = std::stoi(t.substr(1, 1), nullptr, 16), b = std::stoi(t.substr(2, 1), nullptr, 16);
            return Color{static_cast<std::uint8_t>(r * 17), static_cast<std::uint8_t>(g * 17), static_cast<std::uint8_t>(b * 17), 255};
        }
        if (t.size() == 6 || t.size() == 8) {
            if (t.find_first_not_of("0123456789abcdef") != std::string::npos) return std::nullopt;
            return Color{static_cast<std::uint8_t>(hex(0)), static_cast<std::uint8_t>(hex(2)), static_cast<std::uint8_t>(hex(4)),
                         static_cast<std::uint8_t>(t.size() == 8 ? hex(6) : 255)};
        }
        if (t.rfind("rgb(", 0) == 0 && t.back() == ')') {
            std::vector<int> v;
            std::stringstream ss(t.substr(4, t.size() - 5));
            std::string part;
            while (std::getline(ss, part, ',')) v.push_back(std::clamp(std::stoi(part), 0, 255));
            if (v.size() == 3) return Color{static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]), static_cast<std::uint8_t>(v[2]), 255};
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::string colorText(Color c) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
    return buf;
}

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

bool Mark::isAnnotation() const {
    return kind == "highlight" || kind == "underline" || kind == "strikeout" || kind == "note" || kind == "ink" || kind == "rect" ||
           kind == "ellipse" || kind == "line" || kind == "arrow";
}

// ---- geometry -------------------------------------------------------------------------

namespace {

Matrix turnMatrix(int rot, double W, double H) {
    switch (rot) {
        case 90: return {0, 1, -1, 0, H, 0};
        case 180: return {-1, 0, 0, -1, W, H};
        case 270: return {0, -1, 1, 0, 0, W};
        default: return {};
    }
}

}  // namespace

PageGeom geometry(const DocData& d, int page) {
    const PageEntry& p = d.pages.at(static_cast<std::size_t>(page));
    PageGeom g;
    if (p.file >= 0) {
        const PageBox pb = pageBox(*d.files[static_cast<std::size_t>(p.file)], p.index);
        g.baseWidth = pb.width;
        g.baseHeight = pb.height;
        g.userToBase = pb.toPage;
    } else {
        g.baseWidth = p.width;
        g.baseHeight = p.height;
        g.userToBase = {1, 0, 0, -1, 0, p.height};
    }
    g.turn = ((p.rotate % 360) + 360) % 360;
    const bool sideways = g.turn == 90 || g.turn == 270;
    g.width = sideways ? g.baseHeight : g.baseWidth;
    g.height = sideways ? g.baseWidth : g.baseHeight;
    return g;
}

void fromBase(const PageGeom& g, double bx, double by, double& x, double& y) { turnMatrix(g.turn, g.baseWidth, g.baseHeight).apply(bx, by, x, y); }

void toBase(const PageGeom& g, double x, double y, double& bx, double& by) {
    turnMatrix(g.turn, g.baseWidth, g.baseHeight).inverse().apply(x, y, bx, by);
}

// ---- text of marks ----------------------------------------------------------------------

double lineHeight(const Mark& m) { return m.size * 1.2; }

std::vector<std::string> markLines(const Mark& m) {
    std::vector<std::string> out;
    std::stringstream ss(m.text);
    std::string para;
    while (std::getline(ss, para)) {
        if (!para.empty() && para.back() == '\r') para.pop_back();
        if (m.w <= 0 || m.kind != "text") {
            out.push_back(para);
            continue;
        }
        // wrap at spaces to the box width
        std::string line;
        std::size_t pos = 0;
        while (pos <= para.size()) {
            std::size_t sp = para.find(' ', pos);
            std::string word = para.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(m.font, trial, m.size) > m.w) {
                out.push_back(line);
                line = word;
            } else {
                line = trial;
            }
            if (sp == std::string::npos) break;
            pos = sp + 1;
        }
        out.push_back(line);
    }
    if (out.empty()) out.emplace_back();
    return out;
}

// ---- fields ------------------------------------------------------------------------------

std::vector<FieldInfo> readFields(const PdfFile& f) {
    std::vector<FieldInfo> out;
    const Object* af = f.catalog().get("AcroForm");
    if (!af) return out;
    const Object& acro = f.resolve(*af);
    const Object* fieldsRef = acro.get("Fields");
    if (!fieldsRef) return out;
    // which page each annotation sits on
    std::map<int, int> annotPage;
    for (int i = 0; i < f.pageCount(); ++i) {
        if (const Object* a = f.pages()[static_cast<std::size_t>(i)].dict.get("Annots")) {
            for (const auto& it : f.resolve(*a).items) {
                if (it.kind == Object::Ref) annotPage[static_cast<int>(it.number)] = i;
            }
        }
    }
    std::set<int> seen;
    struct Inherit {
        std::string name, type, value;
        int flags = 0;
        int valueObject = 0;
        const Object* opt = nullptr;
    };
    std::function<void(const Object&, Inherit, int)> walk = [&](const Object& ref, Inherit inh, int depth) {
        if (depth > 20) return;
        const int num = ref.kind == Object::Ref ? static_cast<int>(ref.number) : 0;
        if (num && !seen.insert(num).second) return;
        const Object& node = f.resolve(ref);
        if (node.kind != Object::Dict) return;
        if (const Object* t = node.get("T")) {
            const std::string part = decodeTextString(f.resolve(*t).str);
            inh.name = inh.name.empty() ? part : inh.name + "." + part;
        }
        if (const Object* ft = node.get("FT")) inh.type = f.resolve(*ft).str;
        if (const Object* ff = node.get("Ff")) inh.flags = static_cast<int>(f.resolve(*ff).number);
        if (const Object* opt = node.get("Opt")) inh.opt = &f.resolve(*opt);
        if (const Object* v = node.get("V")) {
            const Object& vv = f.resolve(*v);
            inh.value = vv.kind == Object::Name ? vv.str : vv.kind == Object::String ? decodeTextString(vv.str) : "";
            if (vv.kind == Object::Array && !vv.items.empty()) inh.value = decodeTextString(f.resolve(vv.items[0]).str);
            inh.valueObject = num;
        } else if (node.get("T")) {
            inh.valueObject = num;
        }
        // children that are fields (they have /T) go deeper; the rest are widgets of this field
        std::vector<const Object*> kidFields, kidWidgets;
        if (const Object* kids = node.get("Kids")) {
            for (const auto& k : f.resolve(*kids).items) {
                const Object& kn = f.resolve(k);
                if (kn.get("T")) kidFields.push_back(&k);
                else kidWidgets.push_back(&k);
            }
        }
        for (const Object* k : kidFields) walk(*k, inh, depth + 1);
        if (!kidFields.empty() && kidWidgets.empty()) return;
        FieldInfo fi;
        fi.name = inh.name;
        fi.value = inh.value;
        fi.fieldObject = inh.valueObject ? inh.valueObject : num;
        fi.readOnly = inh.flags & 1;
        if (inh.type == "Tx") {
            fi.type = "text";
            fi.multiline = inh.flags & (1 << 12);
        } else if (inh.type == "Ch") {
            fi.type = "choice";
        } else if (inh.type == "Sig") {
            fi.type = "signature";
        } else if (inh.type == "Btn") {
            fi.type = (inh.flags & (1 << 16)) ? "button" : (inh.flags & (1 << 15)) ? "radio" : "checkbox";
        } else {
            fi.type = inh.type.empty() ? "unknown" : inh.type;
        }
        if (inh.opt) {
            for (const auto& o : inh.opt->items) {
                const Object& ov = f.resolve(o);
                if (ov.kind == Object::String) fi.options.push_back(decodeTextString(ov.str));
                else if (ov.kind == Object::Array && ov.items.size() >= 2) fi.options.push_back(decodeTextString(f.resolve(ov.items[1]).str));
            }
        }
        std::vector<std::pair<const Object*, int>> widgets;
        if (kidWidgets.empty()) widgets.emplace_back(&node, num);
        for (const Object* k : kidWidgets) widgets.emplace_back(&f.resolve(*k), k->kind == Object::Ref ? static_cast<int>(k->number) : 0);
        for (const auto& [w, wnum] : widgets) {
            FieldInfo::Widget wi;
            wi.object = wnum;
            auto ap = annotPage.find(wnum);
            wi.page = ap != annotPage.end() ? ap->second : -1;
            if (const Object* r = w->get("Rect")) {
                const Object& ra = f.resolve(*r);
                for (std::size_t k = 0; k < 4 && k < ra.items.size(); ++k) wi.rect[k] = f.resolve(ra.items[k]).number;
                if (wi.rect[0] > wi.rect[2]) std::swap(wi.rect[0], wi.rect[2]);
                if (wi.rect[1] > wi.rect[3]) std::swap(wi.rect[1], wi.rect[3]);
            }
            if (fi.type == "checkbox" || fi.type == "radio") {
                if (const Object* apd = w->get("AP")) {
                    if (const Object* n = f.resolve(*apd).get("N")) {
                        for (const auto& k : f.resolve(*n).keys) {
                            if (k != "Off" && std::find(fi.options.begin(), fi.options.end(), k) == fi.options.end()) fi.options.push_back(k);
                        }
                    }
                }
            }
            fi.widgets.push_back(wi);
        }
        if (fi.type == "checkbox" && fi.value.empty()) fi.value = "Off";
        out.push_back(std::move(fi));
    };
    for (const auto& it : f.resolve(*fieldsRef).items) walk(it, {}, 0);
    return out;
}

const std::vector<FieldInfo>& PdfDoc::fields() const {
    const PdfFile* f = data.files.empty() ? nullptr : data.files[0].get();
    if (f != fieldsFor_) {
        fields_ = f ? readFields(*f) : std::vector<FieldInfo>{};
        fieldsFor_ = f;
    }
    return fields_;
}

// ---- drawing --------------------------------------------------------------------------------

namespace {

Color faded(Color c, double opacity) {
    c.a = static_cast<std::uint8_t>(std::lround(c.a * std::clamp(opacity, 0.0, 1.0)));
    return c;
}

Polygon ellipsePoly(double cx, double cy, double rx, double ry) {
    Polygon p;
    for (int i = 0; i < 64; ++i) {
        const double t = 6.283185307179586 * i / 64;
        p.push_back({cx + rx * std::cos(t), cy + ry * std::sin(t)});
    }
    return p;
}

void drawImageInto(raster::Image& img, const raster::Image& pic, double x, double y, double w, double h, double opacity) {
    const int iw = std::max(1, static_cast<int>(std::lround(w))), ih = std::max(1, static_cast<int>(std::lround(h)));
    if (iw > 20000 || ih > 20000) return;
    raster::Image scaled = raster::resize(pic, iw, ih, (iw < pic.width() / 2) ? raster::Filter::Area : raster::Filter::Bilinear);
    raster::blendOnto(img, scaled, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)), raster::Blend::Normal, opacity);
}

void arrowHead(raster::Image& img, double x1, double y1, double x2, double y2, double width, Color c, double opacity) {
    const double len = std::hypot(x2 - x1, y2 - y1);
    if (len < 1e-6) return;
    const double ux = (x2 - x1) / len, uy = (y2 - y1) / len;
    const double s = std::max(6.0, width * 4);
    fillShapes(img, {{{x2, y2}, {x2 - ux * s - uy * s * 0.5, y2 - uy * s + ux * s * 0.5}, {x2 - ux * s + uy * s * 0.5, y2 - uy * s - ux * s * 0.5}}}, c,
               opacity);
}

}  // namespace

void drawMark(raster::Image& img, const Mark& m, double s) {
    if (m.kind == "ocr") return;  // invisible text: the scan shows the words already
    const double op = m.opacity;
    if (m.kind == "text") {
        const auto lines = markLines(m);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const double by = m.y + m.size * 0.8 + static_cast<double>(i) * lineHeight(m);
            drawText(img, m.x * s, by * s, lines[i], m.font, m.size * s, faded(m.color, op));
        }
    } else if (m.kind == "rect" || m.kind == "whiteout" || m.kind == "redact") {
        Polygon box{{m.x * s, m.y * s}, {(m.x + m.w) * s, m.y * s}, {(m.x + m.w) * s, (m.y + m.h) * s}, {m.x * s, (m.y + m.h) * s}};
        if (m.fill) fillShapes(img, {box}, *m.fill, op);
        if (m.kind == "rect" && m.width > 0) strokeLines(img, {box}, true, m.width * s, m.color, op);
    } else if (m.kind == "ellipse") {
        Polygon e = ellipsePoly((m.x + m.w / 2) * s, (m.y + m.h / 2) * s, m.w / 2 * s, m.h / 2 * s);
        if (m.fill) fillShapes(img, {e}, *m.fill, op);
        if (m.width > 0) strokeLines(img, {e}, true, m.width * s, m.color, op);
    } else if (m.kind == "line" || m.kind == "arrow") {
        if (m.points.size() >= 4) {
            const double x1 = m.points[0] * s, y1 = m.points[1] * s, x2 = m.points[2] * s, y2 = m.points[3] * s;
            strokeLines(img, {{{x1, y1}, {x2, y2}}}, false, m.width * s, m.color, op);
            if (m.kind == "arrow") arrowHead(img, x1, y1, x2, y2, m.width * s, m.color, op);
        }
    } else if (m.kind == "ink" || (m.kind == "signature" && !m.strokes.empty())) {
        std::vector<Polygon> lines;
        for (const auto& st : m.strokes) {
            Polygon p;
            for (std::size_t k = 0; k + 1 < st.size(); k += 2) p.push_back({st[k] * s, st[k + 1] * s});
            lines.push_back(std::move(p));
        }
        strokeLines(img, lines, false, m.width * s, m.color, op);
    } else if (m.kind == "highlight" || m.kind == "underline" || m.kind == "strikeout") {
        for (const auto& b : m.boxes) {
            if (m.kind == "highlight") {
                fillShapes(img, {{{b[0] * s, b[1] * s}, {(b[0] + b[2]) * s, b[1] * s}, {(b[0] + b[2]) * s, (b[1] + b[3]) * s}, {b[0] * s, (b[1] + b[3]) * s}}},
                           m.color, 0.4 * op);
            } else {
                const double ly = (b[1] + b[3] * (m.kind == "underline" ? 0.92 : 0.55)) * s;
                strokeLines(img, {{{b[0] * s, ly}, {(b[0] + b[2]) * s, ly}}}, false, std::max(1.0, b[3] * 0.07 * s), m.color, op);
            }
        }
    } else if (m.kind == "note") {
        const double x = m.x * s, y = m.y * s, z = 18 * s;
        Polygon icon{{x, y}, {x + z, y}, {x + z, y + z * 0.75}, {x + z * 0.45, y + z * 0.75}, {x + z * 0.2, y + z}, {x + z * 0.25, y + z * 0.75}, {x, y + z * 0.75}};
        fillShapes(img, {icon}, m.color, 0.95);
        strokeLines(img, {icon}, true, std::max(1.0, s), {90, 70, 0, 255}, 0.9);
    } else if (m.kind == "image" || m.kind == "signature") {
        if (m.image && !m.image->pixels.empty()) {
            drawImageInto(img, m.image->pixels, m.x * s, m.y * s, m.w * s, m.h * s, op);
        } else if (!m.text.empty()) {
            Font f{Family::Serif, false, true};
            const double size = m.size;
            drawText(img, m.x * s, (m.y + m.h * 0.75) * s, m.text, f, size * s, faded(m.color, op));
        }
    }
}

raster::Image renderPage(const DocData& d, int page, double scale) {
    const PageEntry& p = d.pages.at(static_cast<std::size_t>(page));
    const PageGeom g = geometry(d, page);
    raster::Image img;
    if (p.file >= 0) {
        const PdfFile& f = *d.files[static_cast<std::size_t>(p.file)];
        // widgets of fields filled in here are drawn by us below
        std::set<int> changedWidgets;
        std::vector<std::pair<const FieldInfo::Widget*, const FieldInfo*>> widgetDraws;
        static thread_local std::vector<FieldInfo> fieldCache;
        static thread_local const PdfFile* fieldFile = nullptr;
        if (p.file == 0 && !d.values.empty()) {
            if (fieldFile != &f) {
                fieldCache = readFields(f);
                fieldFile = &f;
            }
            for (const auto& fi : fieldCache) {
                if (!d.values.count(fi.name)) continue;
                for (const auto& w : fi.widgets) {
                    if (w.page != p.index) continue;
                    changedWidgets.insert(w.object);
                    widgetDraws.emplace_back(&w, &fi);
                }
            }
        }
        RenderOptions opt;
        opt.content = p.content ? p.content.get() : nullptr;
        opt.skipAnnotation = [&](int obj, const Object&) { return obj && (p.dropAnnots.count(obj) || changedWidgets.count(obj)); };
        img = renderPdfPage(f, p.index, scale, opt);
        for (const auto& [w, fi] : widgetDraws) {
            double xs[2], ys[2];
            g.userToBase.apply(w->rect[0], w->rect[1], xs[0], ys[0]);
            g.userToBase.apply(w->rect[2], w->rect[3], xs[1], ys[1]);
            const double x0 = std::min(xs[0], xs[1]) * scale, x1 = std::max(xs[0], xs[1]) * scale;
            const double y0 = std::min(ys[0], ys[1]) * scale, y1 = std::max(ys[0], ys[1]) * scale;
            const std::string& v = d.values.at(fi->name);
            fillShapes(img, {{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}}, {232, 240, 255, 255});
            if (fi->type == "checkbox" || fi->type == "radio") {
                if (v != "Off" && !v.empty()) {
                    const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, r = std::min(x1 - x0, y1 - y0) * 0.35;
                    strokeLines(img, {{{cx - r, cy}, {cx - r * 0.3, cy + r * 0.7}, {cx + r, cy - r * 0.8}}}, false, std::max(1.5, r * 0.3), {0, 0, 0, 255});
                }
            } else {
                const double size = std::min(11.0 * scale, (y1 - y0) * 0.72);
                std::stringstream ss(v);
                std::string line;
                double by = fi->multiline ? y0 + size * 1.1 : (y0 + y1) / 2 + size * 0.35;
                while (std::getline(ss, line) && by < y1 + size) {
                    drawText(img, x0 + 2 * scale, by, line, Font{}, size, {0, 0, 0, 255});
                    by += size * 1.2;
                }
            }
        }
    } else {
        img = raster::Image(std::max(1, static_cast<int>(std::lround(p.width * scale))), std::max(1, static_cast<int>(std::lround(p.height * scale))),
                            {255, 255, 255, 255});
    }
    for (const auto& m : p.marks) drawMark(img, m, scale);
    if (g.turn) img = raster::rotate90(img, g.turn / 90);
    return img;
}

std::vector<TextChar> charsOf(const DocData& d, int page) {
    const PageEntry& p = d.pages.at(static_cast<std::size_t>(page));
    std::vector<TextChar> out;
    if (p.file >= 0) out = pageChars(*d.files[static_cast<std::size_t>(p.file)], p.index, p.content ? p.content.get() : nullptr);
    // text read off a scan here: each word's letters spread evenly over its box
    for (const auto& m : p.marks) {
        if (m.kind != "ocr") continue;
        std::size_t start = 0;
        for (std::size_t i = 0; i < m.boxes.size(); ++i) {
            const std::size_t nl = m.text.find('\n', start);
            const std::string word = m.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            start = nl == std::string::npos ? m.text.size() : nl + 1;
            if (word.empty() || 2 * i + 1 >= m.points.size()) continue;
            const auto& b = m.boxes[i];
            const double cw = b[2] / static_cast<double>(word.size()), base = m.points[2 * i], size = m.points[2 * i + 1];
            for (std::size_t k = 0; k <= word.size(); ++k) {
                TextChar c;
                c.text = k < word.size() ? std::string(1, word[k]) : std::string(" ");
                c.x0 = b[0] + cw * static_cast<double>(k), c.x1 = k < word.size() ? c.x0 + cw : c.x0 + cw * 0.5;
                c.y0 = base - size * 0.8, c.y1 = base + size * 0.2;
                c.ox = c.x0, c.oy = base, c.size = size;
                out.push_back(std::move(c));
            }
        }
    }
    return out;
}

// ---- the document ------------------------------------------------------------------------------

std::string PdfDoc::summary() const {
    std::size_t marks = 0;
    for (const auto& p : data.pages) marks += p.marks.size();
    std::string s = std::to_string(data.pages.size()) + " page" + (data.pages.size() == 1 ? "" : "s");
    if (marks) s += ", " + std::to_string(marks) + " edit" + (marks == 1 ? "" : "s");
    if (!data.values.empty()) s += ", " + std::to_string(data.values.size()) + " field" + (data.values.size() == 1 ? "" : "s") + " filled";
    if (!path.empty()) s += ", " + path;
    return s;
}

std::unique_ptr<Attachment> PdfDoc::preview() const {
    if (data.pages.empty()) return nullptr;
    const PageGeom g = geometry(data, 0);
    const double scale = std::min(2.0, 900.0 / std::max(g.width, g.height));
    auto a = std::make_unique<Attachment>();
    a->mime = "image/png";
    a->bytes = raster::encodePng(renderPage(data, 0, scale));
    a->name = "page-1.png";
    return a;
}

}  // namespace wetwaresuite::pdf::tool
