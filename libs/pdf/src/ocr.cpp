// Text recognition by letter shapes: binarize (Otsu), connected components, lines, letters,
// then nearest-template matching against the built-in faces with a little context afterwards.
#include "wetwaresuite/pdf/ocr.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <numeric>

#include "wetwaresuite/pdf/page.hpp"

namespace wetwaresuite::pdf {

namespace {

constexpr int kGrid = 16;  // letters are compared as 16 x 16 ink maps, keeping their shape
constexpr int kCells = kGrid * kGrid;

struct Bin {
    int w = 0, h = 0;
    std::vector<std::uint8_t> ink;
    bool at(int x, int y) const { return ink[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)] != 0; }
};

Bin binarize(const raster::Image& img) {
    Bin b;
    b.w = img.width(), b.h = img.height();
    std::vector<std::uint8_t> gray(static_cast<std::size_t>(b.w) * static_cast<std::size_t>(b.h));
    std::array<std::uint64_t, 256> hist{};
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const raster::Color c = img.at(x, y);
            const int lum = (c.r * 299 + c.g * 587 + c.b * 114) / 1000;
            const int v = (lum * c.a + 255 * (255 - c.a)) / 255;  // over white
            gray[static_cast<std::size_t>(y) * static_cast<std::size_t>(b.w) + static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(v);
            ++hist[static_cast<std::size_t>(v)];
        }
    // Otsu's threshold
    const double total = static_cast<double>(gray.size());
    double sum = 0;
    for (int i = 0; i < 256; ++i) sum += i * static_cast<double>(hist[static_cast<std::size_t>(i)]);
    double sumB = 0, wB = 0, best = -1;
    int threshold = 128;
    for (int t = 0; t < 256; ++t) {
        wB += static_cast<double>(hist[static_cast<std::size_t>(t)]);
        if (wB == 0) continue;
        const double wF = total - wB;
        if (wF == 0) break;
        sumB += t * static_cast<double>(hist[static_cast<std::size_t>(t)]);
        const double mB = sumB / wB, mF = (sum - sumB) / wF;
        const double between = wB * wF * (mB - mF) * (mB - mF);
        if (between > best) best = between, threshold = t;
    }
    b.ink.resize(gray.size());
    std::size_t dark = 0;
    for (std::size_t i = 0; i < gray.size(); ++i) dark += (b.ink[i] = gray[i] <= threshold ? 1 : 0);
    if (dark * 2 > gray.size())  // light text on a dark page
        for (auto& v : b.ink) v ^= 1;
    return b;
}

struct Comp {
    int x0, y0, x1, y1;  // inclusive
    int area = 0;
    int w() const { return x1 - x0 + 1; }
    int h() const { return y1 - y0 + 1; }
};

// 8-connected components; labels[] holds each ink pixel's component (+1), 0 for paper
std::vector<Comp> components(const Bin& b, std::vector<int>& labels) {
    labels.assign(b.ink.size(), 0);
    std::vector<int> parent{0};
    auto find = [&](int a) {
        while (parent[static_cast<std::size_t>(a)] != a) a = parent[static_cast<std::size_t>(a)] = parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(a)])];
        return a;
    };
    auto unite = [&](int a, int c) {
        a = find(a), c = find(c);
        if (a != c) parent[static_cast<std::size_t>(std::max(a, c))] = std::min(a, c);
    };
    const auto W = static_cast<std::size_t>(b.w);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x);
            if (!b.ink[i]) continue;
            int l = 0;
            const int nb[4][2] = {{-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
            for (const auto& d : nb) {
                const int nx = x + d[0], ny = y + d[1];
                if (nx < 0 || ny < 0 || nx >= b.w) continue;
                const int nl = labels[static_cast<std::size_t>(ny) * W + static_cast<std::size_t>(nx)];
                if (!nl) continue;
                if (!l) l = nl;
                else unite(l, nl);
            }
            if (!l) {
                l = static_cast<int>(parent.size());
                parent.push_back(l);
            }
            labels[i] = l;
        }
    std::vector<int> compOf(parent.size(), -1);
    std::vector<Comp> comps;
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * W + static_cast<std::size_t>(x);
            if (!labels[i]) continue;
            const int root = find(labels[i]);
            int& c = compOf[static_cast<std::size_t>(root)];
            if (c < 0) {
                c = static_cast<int>(comps.size());
                comps.push_back({x, y, x, y, 0});
            }
            Comp& k = comps[static_cast<std::size_t>(c)];
            k.x0 = std::min(k.x0, x), k.x1 = std::max(k.x1, x), k.y0 = std::min(k.y0, y), k.y1 = std::max(k.y1, y);
            ++k.area;
            labels[i] = c + 1;
        }
    return comps;
}

struct Features {
    std::array<float, kCells> cells{};
    float aspect = 0, top = 0, bottom = 0, height = 0;  // log(w/h); above/below the baseline and height in cap heights
    int parts = 1;  // separate pieces of ink (i and j have 2, % has 3)
};

// The ink of a box (only pixels of the given components when labels are given) as a 16 x 16 map:
// scaled so its longer side fills the map and centred, so a thin l stays thin.
Features featuresOf(const Bin& b, const std::vector<int>* labels, const std::vector<int>& comps, int x0, int y0, int x1, int y1, double baseline,
                    double cap) {
    Features f;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    const double side = std::max(w, h);
    const double ox = (side - w) / 2, oy = (side - h) / 2;
    std::array<float, kCells> count{};
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(b.w) + static_cast<std::size_t>(x);
            if (!b.ink[i]) continue;
            if (labels && std::find(comps.begin(), comps.end(), (*labels)[i] - 1) == comps.end()) continue;
            const int gx = std::min(kGrid - 1, static_cast<int>((x - x0 + ox + 0.5) * kGrid / side));
            const int gy = std::min(kGrid - 1, static_cast<int>((y - y0 + oy + 0.5) * kGrid / side));
            count[static_cast<std::size_t>(gy * kGrid + gx)] += 1;
        }
    const float area = static_cast<float>(side * side / kCells);
    for (int i = 0; i < kCells; ++i) f.cells[static_cast<std::size_t>(i)] = std::min(1.0f, count[static_cast<std::size_t>(i)] / std::max(area, 1e-3f));
    // a little blur, so a stroke one cell over still counts as close
    std::array<float, kCells> blurred{};
    for (int y = 0; y < kGrid; ++y)
        for (int x = 0; x < kGrid; ++x) {
            float sum = 0, wsum = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= kGrid || ny >= kGrid) continue;
                    const float wgt = dx == 0 && dy == 0 ? 4.0f : (dx == 0 || dy == 0) ? 2.0f : 1.0f;
                    sum += f.cells[static_cast<std::size_t>(ny * kGrid + nx)] * wgt, wsum += wgt;
                }
            blurred[static_cast<std::size_t>(y * kGrid + x)] = sum / wsum;
        }
    f.cells = blurred;
    f.aspect = static_cast<float>(std::log(static_cast<double>(w) / h));
    f.top = static_cast<float>((baseline - y0) / cap);
    f.bottom = static_cast<float>((y1 + 1 - baseline) / cap);
    f.height = static_cast<float>(h / cap);
    return f;
}

struct Template {
    char c;
    Features f;
};

const char* kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789.,;:!?'()-/&%$@#*+=[]<>_";

const std::vector<Template>& templates() {
    static std::vector<Template> all;
    static std::once_flag once;
    std::call_once(once, [] {
        const Font faces[] = {{Family::Sans, false}, {Family::Sans, true}, {Family::Serif, false}, {Family::Serif, true}, {Family::Mono, false}};
        // big clean letters and small ones (which look more like what a scanner sees)
        for (const double size : {64.0, 26.0})
        for (const Font& face : faces) {
            // the face's cap height from H
            auto inkBox = [&](char c, int& x0, int& y0, int& x1, int& y1, Bin& b) {
                raster::Image img(static_cast<int>(size * 2), static_cast<int>(size * 2), {255, 255, 255, 255});
                drawText(img, size * 0.4, size * 1.3, std::string(1, c), face, size, {0, 0, 0, 255});
                b = binarize(img);
                x0 = b.w, y0 = b.h, x1 = -1, y1 = -1;
                for (int y = 0; y < b.h; ++y)
                    for (int x = 0; x < b.w; ++x)
                        if (b.at(x, y)) x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                return x1 >= 0;
            };
            int hx0, hy0, hx1, hy1;
            Bin hb;
            inkBox('H', hx0, hy0, hx1, hy1, hb);
            const double baseline = size * 1.3, cap = baseline - hy0;
            for (const char* p = kAlphabet; *p; ++p) {
                int x0, y0, x1, y1;
                Bin b;
                if (!inkBox(*p, x0, y0, x1, y1, b)) continue;
                Features f = featuresOf(b, nullptr, {}, x0, y0, x1, y1, baseline, cap);
                std::vector<int> lab;
                int parts = 0;
                for (const auto& c : components(b, lab)) parts += c.area >= 3 ? 1 : 0;
                f.parts = std::max(1, parts);
                all.push_back({*p, f});
            }
        }
    });
    return all;
}

double distance(const Features& a, const Features& b) {
    double d = 0;
    for (int i = 0; i < kCells; ++i) {
        const double e = a.cells[static_cast<std::size_t>(i)] - b.cells[static_cast<std::size_t>(i)];
        d += e * e;
    }
    d /= kCells;
    const double da = a.aspect - b.aspect, dt = a.top - b.top, db = a.bottom - b.bottom, dh = a.height - b.height;
    // small marks (. , ' - :) are told apart by where they sit more than by their shape
    const double g = std::min(a.height, b.height) < 0.45f ? 4.0 : 1.0;
    const double parts = std::min(a.parts, 3) != std::min(b.parts, 3) ? 0.03 : 0.0;
    return d + parts + g * (0.06 * da * da + 0.5 * dt * dt + 0.5 * db * db + 0.25 * dh * dh);
}

struct Match {
    char c = '?';
    double cost = 1e9;
};
Match classify(const Features& f) {
    Match m;
    for (const auto& t : templates()) {
        const double d = distance(f, t.f);
        if (d < m.cost) m = {t.c, d};
    }
    return m;
}

struct Glyph {
    int x0, y0, x1, y1;
    std::vector<int> comps;
};

struct LineBuild {
    std::vector<int> comps;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    double sumCy = 0, sumH = 0;
    int n = 0;
    double cy() const { return sumCy / std::max(1, n); }
    double hh() const { return sumH / std::max(1, n); }
    void add(int id, const Comp& c) {
        comps.push_back(id);
        x0 = std::min(x0, c.x0), x1 = std::max(x1, c.x1), y0 = std::min(y0, c.y0), y1 = std::max(y1, c.y1);
        sumCy += (c.y0 + c.y1) * 0.5, sumH += c.h(), ++n;
    }
};

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(v.size() - 1))];
}

// Recognizes one glyph, cutting it in two (once or twice) when that reads much better: touching letters.
void readGlyph(const Bin& b, const std::vector<int>& labels, const Glyph& g, double baseline, double cap, int depth, std::string& out, double& cost, int& letters) {
    Features f = featuresOf(b, &labels, g.comps, g.x0, g.y0, g.x1, g.y1, baseline, cap);
    f.parts = depth == 0 ? static_cast<int>(g.comps.size()) : 1;
    const Match m = classify(f);
    const int w = g.x1 - g.x0 + 1, h = g.y1 - g.y0 + 1;
    if (depth < 2 && m.cost > 0.045 && w > h * 0.75 && w > cap * 0.55) {
        // the column with the least ink in the middle half is where two letters touch
        int bestX = -1, bestInk = 1 << 30;
        for (int x = g.x0 + w / 4; x <= g.x1 - w / 4; ++x) {
            int ink = 0;
            for (int y = g.y0; y <= g.y1; ++y) ink += b.at(x, y) ? 1 : 0;
            if (ink < bestInk) bestInk = ink, bestX = x;
        }
        if (bestX > g.x0 && bestX < g.x1) {
            auto shrink = [&](Glyph part) {  // tighten the box around the ink that's left
                int y0 = part.y1, y1 = part.y0;
                for (int y = part.y0; y <= part.y1; ++y)
                    for (int x = part.x0; x <= part.x1; ++x)
                        if (b.at(x, y)) y0 = std::min(y0, y), y1 = std::max(y1, y);
                if (y1 >= y0) part.y0 = y0, part.y1 = y1;
                return part;
            };
            const Glyph left = shrink({g.x0, g.y0, bestX - 1, g.y1, g.comps}), right = shrink({bestX + 1, g.y0, g.x1, g.y1, g.comps});
            std::string ls, rs;
            double lc = 0, rc = 0;
            int ln = 0, rn = 0;
            readGlyph(b, labels, left, baseline, cap, depth + 1, ls, lc, ln);
            readGlyph(b, labels, right, baseline, cap, depth + 1, rs, rc, rn);
            if ((lc + rc) / std::max(1, ln + rn) < m.cost * 0.6) {
                out += ls + rs;
                cost += lc + rc;
                letters += ln + rn;
                return;
            }
        }
    }
    out += m.c;
    cost += m.cost;
    ++letters;
}

bool isLower(char c) { return c >= 'a' && c <= 'z'; }
bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Letters that look alike, settled by the rest of the word.
std::string fixWord(std::string w) {
    int lower = 0, upper = 0, digits = 0;
    for (char c : w) {
        if (std::strchr("Il1|0Oo", c)) continue;  // the look-alikes don't get a vote
        lower += isLower(c), upper += isUpper(c), digits += isDigit(c);
    }
    const bool numeric = digits > 0 && digits >= lower + upper;
    for (std::size_t i = 0; i < w.size(); ++i) {
        char& c = w[i];
        if (numeric) {
            if (c == 'O' || c == 'o') c = '0';
            else if (c == 'l' || c == 'I') c = '1';
            else if (c == 'S' && digits > 1) c = '5';
        } else if (lower + upper > 0) {
            if (c == '0') c = upper > lower ? 'O' : 'o';
            if (c == '1' || c == 'I' || c == 'l' || c == '|') {
                if (upper > lower && upper >= 1) c = 'I';                    // CAPITALS
                else if (i > 0 && lower > 0) c = 'l';                        // inside a lowercase word
                else if (i == 0 && w.size() == 1) c = c == 'l' ? 'I' : c;    // a lone letter is the word I
                else if (i == 0 && w.size() > 1 && isLower(w[1]))           // l before a vowel (lazy), I before the rest (Invoice, It)
                    c = std::strchr("aeiouy", w[1]) ? 'l' : 'I';
            }
        }
    }
    // two apostrophes in a row are a double quote
    std::string out;
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (w[i] == '\'' && i + 1 < w.size() && w[i + 1] == '\'') {
            out += '"';
            ++i;
        } else {
            out += w[i];
        }
    }
    return out;
}

}  // namespace

OcrResult recognizeText(const raster::Image& image) {
    OcrResult res;
    if (image.width() < 8 || image.height() < 8) return res;
    const Bin b = binarize(image);
    std::vector<int> labels;
    std::vector<Comp> comps = components(b, labels);
    if (comps.empty()) return res;
    // typical letter height, then drop specks, pictures and ruled lines
    std::vector<double> hs;
    for (const auto& c : comps)
        if (c.h() >= 6 && c.area >= 8) hs.push_back(c.h());
    const double hMed = std::max(6.0, percentile(hs, 0.5));
    std::vector<int> big, small;
    for (int i = 0; i < static_cast<int>(comps.size()); ++i) {
        const Comp& c = comps[static_cast<std::size_t>(i)];
        if (c.area < 2 || c.h() > hMed * 4 || c.w() > hMed * 12 || (c.w() > hMed * 4 && c.h() < hMed * 0.25)) continue;
        if (c.h() >= hMed * 0.65) big.push_back(i);
        else if (c.area * 40 >= hMed * hMed * 0.3 || c.h() >= 2) small.push_back(i);
    }
    // lines: letters that sit on the same band, left to right
    std::sort(big.begin(), big.end(), [&](int a, int c) { return comps[static_cast<std::size_t>(a)].x0 < comps[static_cast<std::size_t>(c)].x0; });
    std::vector<LineBuild> lines;
    for (int id : big) {
        const Comp& c = comps[static_cast<std::size_t>(id)];
        const double ccy = (c.y0 + c.y1) * 0.5;
        int best = -1;
        double bestScore = 1e9;
        for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
            const LineBuild& L = lines[static_cast<std::size_t>(li)];
            if (c.x0 - L.x1 > L.hh() * 3) continue;
            const double core0 = L.cy() - L.hh() * 0.3, core1 = L.cy() + L.hh() * 0.3;
            if (c.y1 < core0 || c.y0 > core1) continue;
            const double score = std::abs(ccy - L.cy()) / L.hh();
            if (score < bestScore) bestScore = score, best = li;
        }
        if (best < 0) {
            lines.emplace_back();
            best = static_cast<int>(lines.size()) - 1;
        }
        lines[static_cast<std::size_t>(best)].add(id, c);
    }
    // lines of a few short pieces are punctuation that missed its line: hand them back as small pieces
    {
        std::vector<double> lh;
        for (const auto& L : lines) lh.push_back(L.hh());
        const double typical = percentile(lh, 0.5);
        std::vector<LineBuild> kept;
        for (auto& L : lines) {
            if (L.n <= 3 && L.hh() < typical * 0.75) small.insert(small.end(), L.comps.begin(), L.comps.end());
            else kept.push_back(std::move(L));
        }
        lines = std::move(kept);
    }
    for (int id : small) {  // dots, commas, hyphens, accents: the nearest line around them
        const Comp& c = comps[static_cast<std::size_t>(id)];
        const double ccy = (c.y0 + c.y1) * 0.5;
        int best = -1;
        double bestD = 1e9;
        for (int li = 0; li < static_cast<int>(lines.size()); ++li) {
            const LineBuild& L = lines[static_cast<std::size_t>(li)];
            const double pad = L.hh() * 0.6;
            if (ccy < L.y0 - pad || ccy > L.y1 + pad || c.x0 > L.x1 + L.hh() * 1.5 || c.x1 < L.x0 - L.hh() * 1.5) continue;
            const double d = std::abs(ccy - L.cy());
            if (d < bestD) bestD = d, best = li;
        }
        if (best >= 0) lines[static_cast<std::size_t>(best)].comps.push_back(id);
    }
    std::sort(lines.begin(), lines.end(), [](const LineBuild& a, const LineBuild& c) { return a.cy() < c.cy() || (std::abs(a.cy() - c.cy()) < 2 && a.x0 < c.x0); });

    double totalCost = 0;
    int totalLetters = 0;
    std::vector<double> caps;
    for (auto& L : lines) {
        std::sort(L.comps.begin(), L.comps.end(), [&](int a, int c) { return comps[static_cast<std::size_t>(a)].x0 < comps[static_cast<std::size_t>(c)].x0; });
        // letters: pieces stacked over each other belong together (i, j, :, ;, =, accents)
        std::vector<Glyph> glyphs;
        for (int id : L.comps) {
            const Comp& c = comps[static_cast<std::size_t>(id)];
            if (!glyphs.empty()) {
                Glyph& g = glyphs.back();
                const int overlap = std::min(g.x1, c.x1) - std::max(g.x0, c.x0) + 1;
                if (overlap * 2 >= std::min(g.x1 - g.x0 + 1, c.w())) {
                    g.x0 = std::min(g.x0, c.x0), g.x1 = std::max(g.x1, c.x1), g.y0 = std::min(g.y0, c.y0), g.y1 = std::max(g.y1, c.y1);
                    g.comps.push_back(id);
                    continue;
                }
            }
            glyphs.push_back({c.x0, c.y0, c.x1, c.y1, {id}});
        }
        if (glyphs.empty()) continue;
        // the line's baseline and cap height
        std::vector<double> bottoms, tops, heights;
        for (const auto& g : glyphs) heights.push_back(g.y1 - g.y0 + 1);
        const double hLine = percentile(heights, 0.5);
        for (const auto& g : glyphs)
            if (g.y1 - g.y0 + 1 >= hLine * 0.6) bottoms.push_back(g.y1 + 1), tops.push_back(g.y0);
        const double baseline = percentile(bottoms, 0.5);
        const double cap = std::max(4.0, baseline - percentile(tops, 0.2));
        caps.push_back(cap);

        OcrLine line;
        line.x = L.x0, line.y = L.y0, line.w = L.x1 - L.x0 + 1, line.h = L.y1 - L.y0 + 1;
        OcrWord word;
        double wordCost = 0;
        int wordLetters = 0;
        auto endWord = [&] {
            if (word.text.empty()) return;
            word.text = fixWord(word.text);
            word.confidence = std::exp(-wordCost / std::max(1, wordLetters) * 18);
            word.baseline = baseline;
            line.words.push_back(word);
            totalCost += wordCost;
            totalLetters += wordLetters;
            word = OcrWord{};
            wordCost = 0, wordLetters = 0;
        };
        // word spaces: the gaps split in two groups (between letters, between words), within sane bounds
        std::vector<double> gaps;
        for (std::size_t i = 1; i < glyphs.size(); ++i) gaps.push_back(glyphs[i].x0 - glyphs[i - 1].x1 - 1);
        double spaceAt = cap * 0.42;
        if (gaps.size() >= 4) {
            std::vector<double> sorted = gaps;
            std::sort(sorted.begin(), sorted.end());
            double bestScore = -1, bestT = spaceAt;
            for (std::size_t k = 1; k < sorted.size(); ++k) {  // Otsu on the gaps
                const double t = (sorted[k - 1] + sorted[k]) / 2;
                double m0 = 0, m1 = 0;
                for (std::size_t j = 0; j < k; ++j) m0 += sorted[j];
                for (std::size_t j = k; j < sorted.size(); ++j) m1 += sorted[j];
                m0 /= static_cast<double>(k), m1 /= static_cast<double>(sorted.size() - k);
                const double score = static_cast<double>(k) * static_cast<double>(sorted.size() - k) * (m1 - m0) * (m1 - m0);
                if (score > bestScore) bestScore = score, bestT = t;
            }
            spaceAt = std::clamp(bestT, cap * 0.3, cap * 0.6);
        }
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            const Glyph& g = glyphs[i];
            std::string letters;
            double cost = 0;
            int n = 0;
            readGlyph(b, labels, g, baseline, cap, 0, letters, cost, n);
            if (i > 0) {
                // punctuation hugs the word before it, and a 1 leaves a wide gap after itself
                double need = spaceAt;
                if (letters.size() == 1 && std::strchr(".,;:!?)]", letters[0])) need = std::max(need, cap * 0.75);
                if (!word.text.empty() && (word.text.back() == '1' || word.text.back() == '(' || word.text.back() == '$')) need += cap * 0.15;
                if (gaps[i - 1] > need) endWord();
            }
            if (word.text.empty()) word.x = g.x0, word.y = g.y0, word.w = 0, word.h = 0;
            word.text += letters;
            wordCost += cost;
            wordLetters += n;
            const double wx1 = std::max(word.x + word.w, static_cast<double>(g.x1 + 1)), wy1 = std::max(word.y + word.h, static_cast<double>(g.y1 + 1));
            word.y = std::min(word.y, static_cast<double>(g.y0));
            word.w = wx1 - word.x, word.h = wy1 - word.y;
        }
        endWord();
        if (!line.words.empty()) res.lines.push_back(std::move(line));
    }
    // the text: lines on the same row side by side, a blank line where a gap opens up
    double lastBottom = -1, lastH = 0, lastCy = -1e9;
    for (const auto& l : res.lines) {
        std::string t;
        for (const auto& w : l.words) t += (t.empty() ? "" : " ") + w.text;
        const double cy = l.y + l.h / 2;
        if (lastBottom >= 0 && std::abs(cy - lastCy) < lastH * 0.5) res.text += "   ";
        else if (lastBottom >= 0) res.text += l.y - lastBottom > lastH * 1.2 ? "\n\n" : "\n";
        res.text += t;
        lastBottom = l.y + l.h, lastH = l.h, lastCy = cy;
    }
    res.confidence = totalLetters ? std::exp(-totalCost / totalLetters * 18) : 0;
    res.textHeight = percentile(caps, 0.5);
    return res;
}

}  // namespace wetwaresuite::pdf
