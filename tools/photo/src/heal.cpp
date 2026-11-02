#include "heal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "wetwaresuite/raster/document.hpp"

namespace wetwaresuite::photo {

using raster::Image;

// ---------------------------------------------------------------- masks

std::size_t Mask::count() const {
    return static_cast<std::size_t>(std::count(bits.begin(), bits.end(), std::uint8_t{1}));
}

bool Mask::bounds(int& x0, int& y0, int& x1, int& y1) const {
    x0 = width, y0 = height, x1 = -1, y1 = -1;
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = bits.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            if (!row[x]) continue;
            x0 = std::min(x0, x), x1 = std::max(x1, x);
            y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
    }
    return x1 >= 0;
}

Mask rectMask(int w, int h, int x, int y, int rw, int rh) {
    Mask m(w, h);
    for (int yy = std::max(0, y); yy < std::min(h, y + rh); ++yy)
        for (int xx = std::max(0, x); xx < std::min(w, x + rw); ++xx) m.set(xx, yy);
    return m;
}

Mask ellipseMask(int w, int h, int x, int y, int rw, int rh) {
    Mask m(w, h);
    const double cx = x + rw / 2.0, cy = y + rh / 2.0, ax = rw / 2.0, ay = rh / 2.0;
    if (ax <= 0 || ay <= 0) return m;
    for (int yy = std::max(0, y); yy < std::min(h, y + rh); ++yy) {
        for (int xx = std::max(0, x); xx < std::min(w, x + rw); ++xx) {
            double u = (xx + 0.5 - cx) / ax, v = (yy + 0.5 - cy) / ay;
            if (u * u + v * v <= 1.0) m.set(xx, yy);
        }
    }
    return m;
}

Mask strokeMask(int w, int h, const std::vector<double>& points, double radius) {
    Mask m(w, h);
    if (points.size() < 2 || radius <= 0) return m;
    const std::size_t n = points.size() / 2;
    for (std::size_t i = 0; i < n; ++i) {
        // each segment (a single point when the stroke has one point)
        double ax = points[2 * i], ay = points[2 * i + 1];
        double bx = i + 1 < n ? points[2 * i + 2] : ax, by = i + 1 < n ? points[2 * i + 3] : ay;
        if (i + 1 == n && n > 1) break;
        int x0 = static_cast<int>(std::floor(std::min(ax, bx) - radius)), x1 = static_cast<int>(std::ceil(std::max(ax, bx) + radius));
        int y0 = static_cast<int>(std::floor(std::min(ay, by) - radius)), y1 = static_cast<int>(std::ceil(std::max(ay, by) + radius));
        x0 = std::max(0, x0), y0 = std::max(0, y0), x1 = std::min(w - 1, x1), y1 = std::min(h - 1, y1);
        const double vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                double px = x + 0.5 - ax, py = y + 0.5 - ay;
                double t = len2 > 0 ? std::clamp((px * vx + py * vy) / len2, 0.0, 1.0) : 0.0;
                double ex = px - t * vx, ey = py - t * vy;
                if (ex * ex + ey * ey <= radius * radius) m.set(x, y);
            }
        }
    }
    return m;
}

// ---------------------------------------------------------------- Poisson blending

namespace {

constexpr std::size_t kMaxPoissonPixels = 4'000'000;

struct Node {
    int nb[4];
    int nbCount = 0;
    int degree = 0;  // neighbours inside the image (Neumann at the image edge)
    float rhs[4] = {0, 0, 0, 0};
};

void sample(const Image* src, int x, int y, float out[4]) {
    if (!src) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    x = std::clamp(x, 0, src->width() - 1);
    y = std::clamp(y, 0, src->height() - 1);
    const std::uint8_t* p = src->pixel(x, y);
    for (int c = 0; c < 4; ++c) out[c] = p[c];
}

std::uint8_t toByte(float v) { return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 255)); }

// 2x2 box downsample (odd edges repeat).
Image half(const Image& img) {
    const int w = (img.width() + 1) / 2, h = (img.height() + 1) / 2;
    Image out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const std::uint8_t* p = img.pixel(std::min(2 * x + (k & 1), img.width() - 1), std::min(2 * y + (k >> 1), img.height() - 1));
                for (int c = 0; c < 4; ++c) sum[c] += p[c];
            }
            std::uint8_t* o = out.pixel(x, y);
            for (int c = 0; c < 4; ++c) o[c] = static_cast<std::uint8_t>((sum[c] + 2) / 4);
        }
    }
    return out;
}

Mask half(const Mask& m) {
    Mask out((m.width + 1) / 2, (m.height + 1) / 2);
    for (int y = 0; y < m.height; ++y)
        for (int x = 0; x < m.width; ++x)
            if (m.at(x, y)) out.set(x / 2, y / 2);
    return out;
}

// One level of the solve: the correction c = f - g for every masked pixel (in `px`
// order), which is harmonic with c = target - g on the area's boundary. Big areas
// start from the solution of a half-size copy of the problem, so SOR only has to
// remove fine detail (a cascadic multigrid).
void solveLevel(const Image& target, const Image* source, const Mask& mask, int dx, int dy, std::vector<int>& px,
                std::vector<float>& c, SolveStats& stats, bool top) {
    const int W = target.width(), H = target.height();
    std::vector<int> id(static_cast<std::size_t>(W) * H, -1);
    px.clear();
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (mask.bits[i]) {
            id[i] = static_cast<int>(px.size());
            px.push_back(static_cast<int>(i));
        }
    }
    const std::size_t n = px.size();
    std::vector<Node> nodes(n);
    double mean[4] = {0, 0, 0, 0};
    std::size_t boundary = 0;
    const int ox[4] = {-1, 1, 0, 0}, oy[4] = {0, 0, -1, 1};
    for (std::size_t k = 0; k < n; ++k) {
        const int x = px[k] % W, y = px[k] / W;
        Node& nd = nodes[k];
        float gp[4];
        sample(source, x + dx, y + dy, gp);
        for (int d = 0; d < 4; ++d) {
            const int qx = x + ox[d], qy = y + oy[d];
            if (qx < 0 || qy < 0 || qx >= W || qy >= H) continue;
            ++nd.degree;
            const int qi = id[static_cast<std::size_t>(qy) * W + qx];
            if (qi >= 0) {
                nd.nb[nd.nbCount++] = qi;
                continue;
            }
            float gq[4];
            sample(source, qx + dx, qy + dy, gq);
            const std::uint8_t* t = target.pixel(qx, qy);
            for (int ch = 0; ch < 4; ++ch) {
                const float v = t[ch] - gq[ch];
                nd.rhs[ch] += v;
                mean[ch] += v;
            }
            ++boundary;
        }
    }
    c.assign(n * 4, 0.0f);
    for (std::size_t k = 0; k < n; ++k)
        for (int ch = 0; ch < 4; ++ch) c[k * 4 + ch] = static_cast<float>(mean[ch] / static_cast<double>(std::max<std::size_t>(boundary, 1)));

    int bx0, by0, bx1, by1;
    mask.bounds(bx0, by0, bx1, by1);
    const int side = std::max(bx1 - bx0, by1 - by0) + 1;
    bool warm = false;
    if (side > 32) {
        Mask cm = half(mask);
        const std::size_t cn = cm.count();
        if (cn > 0 && cn < cm.bits.size()) {
            Image ct = half(target);
            Image cs;
            if (source) cs = half(*source);
            std::vector<int> cpx;
            std::vector<float> cc;
            SolveStats ignored;
            solveLevel(ct, source ? &cs : nullptr, cm, dx / 2, dy / 2, cpx, cc, ignored, false);
            std::vector<int> cid(cm.bits.size(), -1);
            for (std::size_t k = 0; k < cpx.size(); ++k) cid[static_cast<std::size_t>(cpx[k])] = static_cast<int>(k);
            for (std::size_t k = 0; k < n; ++k) {
                const int j = cid[static_cast<std::size_t>(px[k] / W / 2) * cm.width + px[k] % W / 2];
                if (j >= 0) std::copy_n(&cc[static_cast<std::size_t>(j) * 4], 4, &c[k * 4]);
            }
            warm = true;
        }
    }

    const double pi = 3.14159265358979323846;
    // from a coarse start only short-range error is left, which a milder over-relaxation removes fastest
    const int reach = warm ? std::min(side, 24) : side;
    const float omega = static_cast<float>(2.0 / (1.0 + std::sin(pi / (reach + 1))));
    // bounded work: about 3 sweeps per pixel of the area's side, and at most ~3e8 pixel updates
    int maxIter = std::clamp(3 * side + 50, 50, 5000);
    maxIter = std::min<int>(maxIter, std::max<int>(50, static_cast<int>(300'000'000 / n)));
    const float tolerance = top ? 0.02f : 0.05f;
    stats = {};
    for (int it = 0; it < maxIter; ++it) {
        float worst = 0;
        for (std::size_t k = 0; k < n; ++k) {
            const Node& nd = nodes[k];
            float* ck = &c[k * 4];
            float sum[4] = {nd.rhs[0], nd.rhs[1], nd.rhs[2], nd.rhs[3]};
            for (int j = 0; j < nd.nbCount; ++j) {
                const float* cq = &c[static_cast<std::size_t>(nd.nb[j]) * 4];
                sum[0] += cq[0], sum[1] += cq[1], sum[2] += cq[2], sum[3] += cq[3];
            }
            const float inv = 1.0f / static_cast<float>(nd.degree);
            for (int ch = 0; ch < 4; ++ch) {
                const float step = omega * (sum[ch] * inv - ck[ch]);
                ck[ch] += step;
                worst = std::max(worst, std::abs(step));
            }
        }
        stats.iterations = it + 1;
        if (worst < tolerance) {
            stats.converged = true;
            break;
        }
    }
}

}  // namespace

bool poissonBlend(Image& target, const Image* source, const Mask& mask, int dx, int dy, SolveStats& stats,
                  std::string& error) {
    const int W = target.width();
    if (mask.width != W || mask.height != target.height()) {
        error = "the area doesn't match the image size";
        return false;
    }
    if (source && source->empty()) {
        error = "the source image is empty";
        return false;
    }
    const std::size_t n = mask.count();
    if (n == 0) {
        error = "the area is empty (it lies outside the image or has no size)";
        return false;
    }
    if (n == mask.bits.size()) {
        error = "the area covers the whole image; leave some picture around it to blend into";
        return false;
    }
    if (n > kMaxPoissonPixels) {
        error = "the area is " + std::to_string(n) + " pixels; the limit is " + std::to_string(kMaxPoissonPixels) +
                " (about 2000x2000). Work on a smaller area.";
        return false;
    }
    std::vector<int> px;
    std::vector<float> c;
    // source and target may be the same picture: the solve only reads, the write comes after
    solveLevel(target, source, mask, dx, dy, px, c, stats, true);
    std::vector<float> g(n * 4);
    for (std::size_t k = 0; k < n; ++k) sample(source, px[k] % W + dx, px[k] / W + dy, &g[k * 4]);
    for (std::size_t k = 0; k < n; ++k) {
        std::uint8_t* t = target.data() + static_cast<std::size_t>(px[k]) * 4;
        for (int ch = 0; ch < 4; ++ch) t[ch] = toByte(g[k * 4 + ch] + c[k * 4 + ch]);
    }
    return true;
}

// ---------------------------------------------------------------- exemplar-based inpainting

namespace {

struct Inpainter {
    Image& img;
    int W, H, r, side;
    std::vector<std::uint8_t> known;
    std::vector<float> conf, lum;
    std::vector<std::uint8_t> validSource;  // patch centred here is fully inside and untouched
    std::vector<int> from;                  // filled pixels: the pixel they were copied from (-1: never masked)
    int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;

    Inpainter(Image& image, const Mask& mask, int patch)
        : img(image), W(image.width()), H(image.height()), r(patch / 2), side(patch) {
        const std::size_t N = static_cast<std::size_t>(W) * H;
        known.resize(N);
        from.assign(N, -1);
        conf.resize(N);
        lum.resize(N);
        for (std::size_t i = 0; i < N; ++i) {
            known[i] = mask.bits[i] ? 0 : 1;
            conf[i] = known[i];
            lum[i] = luminance(static_cast<int>(i));
        }
        // integral image of the mask: a centre is a valid source when its patch holds no masked pixel
        std::vector<int> sat(static_cast<std::size_t>(W + 1) * (H + 1), 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                sat[static_cast<std::size_t>(y + 1) * (W + 1) + x + 1] = mask.bits[static_cast<std::size_t>(y) * W + x] +
                                                                         sat[static_cast<std::size_t>(y) * (W + 1) + x + 1] +
                                                                         sat[static_cast<std::size_t>(y + 1) * (W + 1) + x] -
                                                                         sat[static_cast<std::size_t>(y) * (W + 1) + x];
        validSource.assign(N, 0);
        for (int y = r; y + r < H; ++y) {
            for (int x = r; x + r < W; ++x) {
                const auto at = [&](int xx, int yy) { return sat[static_cast<std::size_t>(yy) * (W + 1) + xx]; };
                int s = at(x + r + 1, y + r + 1) - at(x - r, y + r + 1) - at(x + r + 1, y - r) + at(x - r, y - r);
                validSource[static_cast<std::size_t>(y) * W + x] = s == 0;
            }
        }
        mask.bounds(bx0, by0, bx1, by1);
    }

    float luminance(int i) const {
        const std::uint8_t* p = img.data() + static_cast<std::size_t>(i) * 4;
        return 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
    }
    bool isKnown(int x, int y) const { return x >= 0 && y >= 0 && x < W && y < H && known[static_cast<std::size_t>(y) * W + x]; }

    // Strongest image gradient over the known part of the patch around (x, y).
    void isophote(int x, int y, float& gx, float& gy) const {
        gx = gy = 0;
        float best = -1;
        for (int yy = std::max(1, y - r); yy <= std::min(H - 2, y + r); ++yy) {
            for (int xx = std::max(1, x - r); xx <= std::min(W - 2, x + r); ++xx) {
                if (!isKnown(xx, yy) || !isKnown(xx - 1, yy) || !isKnown(xx + 1, yy) || !isKnown(xx, yy - 1) || !isKnown(xx, yy + 1)) continue;
                const std::size_t i = static_cast<std::size_t>(yy) * W + xx;
                float ax = (lum[i + 1] - lum[i - 1]) * 0.5f, ay = (lum[i + W] - lum[i - W]) * 0.5f;
                float m = ax * ax + ay * ay;
                if (m > best) best = m, gx = ax, gy = ay;
            }
        }
    }

    float priority(int x, int y) const {
        float c = 0;
        for (int yy = std::max(0, y - r); yy <= std::min(H - 1, y + r); ++yy)
            for (int xx = std::max(0, x - r); xx <= std::min(W - 1, x + r); ++xx) c += conf[static_cast<std::size_t>(yy) * W + xx];
        c /= static_cast<float>(side * side);
        // front normal from the known/unknown boundary, data term from the isophote across it
        // Sobel on the known/unknown map gives a smoother normal than plain differences
        const auto k = [&](int xx, int yy) { return static_cast<float>(isKnown(xx, yy)); };
        float nx = (k(x + 1, y - 1) + 2 * k(x + 1, y) + k(x + 1, y + 1)) - (k(x - 1, y - 1) + 2 * k(x - 1, y) + k(x - 1, y + 1));
        float ny = (k(x - 1, y + 1) + 2 * k(x, y + 1) + k(x + 1, y + 1)) - (k(x - 1, y - 1) + 2 * k(x, y - 1) + k(x + 1, y - 1));
        float nn = std::sqrt(nx * nx + ny * ny);
        float gx, gy;
        isophote(x, y, gx, gy);
        float d = nn > 0 ? std::abs(gx * ny - gy * nx) / (nn * 255.0f) : 0.0f;
        return c * (d + 0.001f);
    }

    struct Pick {
        int x = -1, y = -1;
        float conf = 0;
    };

    Pick nextTarget() const {
        Pick best;
        float bestP = -1;
        for (int y = by0; y <= by1; ++y) {
            for (int x = bx0; x <= bx1; ++x) {
                if (known[static_cast<std::size_t>(y) * W + x]) continue;
                if (!isKnown(x - 1, y) && !isKnown(x + 1, y) && !isKnown(x, y - 1) && !isKnown(x, y + 1)) continue;
                float p = priority(x, y);
                if (p > bestP) bestP = p, best.x = x, best.y = y;
            }
        }
        if (best.x >= 0) {
            float c = 0;
            for (int yy = std::max(0, best.y - r); yy <= std::min(H - 1, best.y + r); ++yy)
                for (int xx = std::max(0, best.x - r); xx <= std::min(W - 1, best.x + r); ++xx) c += conf[static_cast<std::size_t>(yy) * W + xx];
            best.conf = c / static_cast<float>(side * side);
        }
        return best;
    }

    // Best fully-known source patch for the known pixels around (tx, ty); searches a
    // window that doubles until it holds at least one candidate.
    bool bestSource(int tx, int ty, int& sx, int& sy) const {
        struct Off {
            int dx, dy;
            std::uint8_t v[4];
        };
        std::vector<Off> offs;
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                if (!isKnown(tx + dx, ty + dy)) continue;
                const std::uint8_t* p = img.pixel(tx + dx, ty + dy);
                offs.push_back({dx, dy, {p[0], p[1], p[2], p[3]}});
            }
        }
        long long bestCost = std::numeric_limits<long long>::max();
        long long bestDist = 0;
        for (int R = 3 * side + 16;; R *= 2) {
            const int x0 = std::max(r, tx - R), x1 = std::min(W - 1 - r, tx + R);
            const int y0 = std::max(r, ty - R), y1 = std::min(H - 1 - r, ty + R);
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    if (!validSource[static_cast<std::size_t>(y) * W + x]) continue;
                    long long cost = 0;
                    for (const Off& o : offs) {
                        const std::uint8_t* q = img.pixel(x + o.dx, y + o.dy);
                        for (int ch = 0; ch < 4; ++ch) {
                            const int d = static_cast<int>(q[ch]) - o.v[ch];
                            cost += d * d;
                        }
                        if (cost > bestCost) break;
                    }
                    const long long dist = static_cast<long long>(x - tx) * (x - tx) + static_cast<long long>(y - ty) * (y - ty);
                    // equal matches: prefer the nearest, so texture comes from close by
                    if (cost < bestCost || (cost == bestCost && dist < bestDist)) bestCost = cost, bestDist = dist, sx = x, sy = y;
                }
            }
            if (bestCost != std::numeric_limits<long long>::max()) return true;
            if (x0 == r && y0 == r && x1 == W - 1 - r && y1 == H - 1 - r) return false;
        }
    }

    // Copies the source patch into the unknown pixels of the target patch; returns how many.
    std::size_t paste(int tx, int ty, int sx, int sy, float c) {
        std::size_t filled = 0;
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                const int x = tx + dx, y = ty + dy;
                if (x < 0 || y < 0 || x >= W || y >= H) continue;
                const std::size_t i = static_cast<std::size_t>(y) * W + x;
                if (known[i]) continue;
                const std::uint8_t* q = img.pixel(sx + dx, sy + dy);
                std::uint8_t* p = img.pixel(x, y);
                std::copy(q, q + 4, p);
                from[i] = (sy + dy) * W + sx + dx;
                known[i] = 1;
                conf[i] = c;
                lum[i] = luminance(static_cast<int>(i));
                ++filled;
            }
        }
        return filled;
    }
};

}  // namespace

namespace {

// Criminisi at one level; `from` gets where each filled pixel came from.
bool inpaintLevel(Image& img, const Mask& mask, int patch, std::vector<int>* from, std::string& error) {
    Inpainter ip(img, mask, patch);
    if (std::none_of(ip.validSource.begin(), ip.validSource.end(), [](std::uint8_t v) { return v != 0; })) {
        error = "there isn't enough untouched picture around the area to copy from; select a smaller area or use a smaller patch size";
        return false;
    }
    std::size_t left = mask.count();
    while (left > 0) {
        auto pick = ip.nextTarget();
        if (pick.x < 0) break;  // cannot happen while pixels are left: the image is never fully masked here
        int sx = 0, sy = 0;
        if (!ip.bestSource(pick.x, pick.y, sx, sy)) {
            error = "found nothing to copy from";
            return false;
        }
        left -= ip.paste(pick.x, pick.y, sx, sy, pick.conf);
    }
    if (from) *from = std::move(ip.from);
    return true;
}

// Sources whose (2r+1)^2 patch is inside the image and untouched by the mask.
std::vector<std::uint8_t> validCentres(const Mask& mask, int r) {
    const int W = mask.width, H = mask.height;
    std::vector<int> sat(static_cast<std::size_t>(W + 1) * (H + 1), 0);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            sat[static_cast<std::size_t>(y + 1) * (W + 1) + x + 1] = mask.bits[static_cast<std::size_t>(y) * W + x] + sat[static_cast<std::size_t>(y) * (W + 1) + x + 1] +
                                                                     sat[static_cast<std::size_t>(y + 1) * (W + 1) + x] - sat[static_cast<std::size_t>(y) * (W + 1) + x];
    std::vector<std::uint8_t> ok(static_cast<std::size_t>(W) * H, 0);
    auto at = [&](int xx, int yy) { return sat[static_cast<std::size_t>(yy) * (W + 1) + xx]; };
    for (int y = r; y + r < H; ++y)
        for (int x = r; x + r < W; ++x) ok[static_cast<std::size_t>(y) * W + x] = at(x + r + 1, y + r + 1) - at(x - r, y + r + 1) - at(x + r + 1, y - r) + at(x - r, y - r) == 0;
    return ok;
}

// Big holes: fill a small copy with Criminisi, then carry where each pixel came from up the
// pyramid and refine it with a few PatchMatch passes per level (Wexler/Barnes style), so a
// whole person or car fills in seconds instead of hours.
bool inpaintPyramid(Image& img, const Mask& mask, int patch, std::string& error) {
    std::vector<Image> imgs{img};
    std::vector<Mask> masks{mask};
    while (masks.back().count() > kMaxInpaintPixels / 2 && imgs.back().width() > 48 && imgs.back().height() > 48) {
        imgs.push_back(half(imgs.back()));
        masks.push_back(half(masks.back()));
    }
    if (masks.back().count() > kMaxInpaintPixels) {
        error = "the area covers too much of the picture to fill from what's left";
        return false;
    }
    std::vector<int> from;
    if (!inpaintLevel(imgs.back(), masks.back(), std::min(patch, 7), &from, error)) return false;
    const int pr = 2;  // 5x5 patches when refining
    std::uint32_t seed = 12345;
    auto rnd = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    for (int level = static_cast<int>(imgs.size()) - 2; level >= 0; --level) {
        Image& I = imgs[static_cast<std::size_t>(level)];
        const Mask& M = masks[static_cast<std::size_t>(level)];
        const int W = I.width(), H = I.height();
        const int cW = imgs[static_cast<std::size_t>(level) + 1].width();
        const std::vector<std::uint8_t> valid = validCentres(M, pr);
        std::vector<int> pix;
        for (int i = 0; i < W * H; ++i)
            if (M.bits[static_cast<std::size_t>(i)]) pix.push_back(i);
        std::vector<int> map(static_cast<std::size_t>(W) * H, -1);
        // nearest valid source to a wanted spot, by a small spiral
        auto snap = [&](int x, int y) {
            x = std::clamp(x, pr, W - 1 - pr), y = std::clamp(y, pr, H - 1 - pr);
            for (int rad = 0; rad < std::max(W, H); rad = rad ? rad * 2 : 1) {
                for (int k = 0; k < 16; ++k) {
                    const int xx = std::clamp(x + (rad ? static_cast<int>(rnd() % (2 * rad + 1)) - rad : 0), pr, W - 1 - pr);
                    const int yy = std::clamp(y + (rad ? static_cast<int>(rnd() % (2 * rad + 1)) - rad : 0), pr, H - 1 - pr);
                    if (valid[static_cast<std::size_t>(yy) * W + xx]) return yy * W + xx;
                }
            }
            return -1;
        };
        for (int t : pix) {
            const int x = t % W, y = t / W;
            const int c = (y / 2) * cW + x / 2;
            const int f = from.empty() || c >= static_cast<int>(from.size()) ? -1 : from[static_cast<std::size_t>(c)];
            int s = -1;
            if (f >= 0) s = snap(2 * (f % cW) + (x & 1), 2 * (f / cW) + (y & 1));
            if (s < 0) s = snap(x, y);
            if (s < 0) {
                error = "found nothing to copy from";
                return false;
            }
            map[static_cast<std::size_t>(t)] = s;
            std::copy(I.pixel(s % W, s / W), I.pixel(s % W, s / W) + 4, I.pixel(x, y));
        }
        auto dist = [&](int t, int s, long long cap) {
            const int tx = t % W, ty = t / W, sx = s % W, sy = s / W;
            long long d = 0;
            for (int dy = -pr; dy <= pr; ++dy) {
                const int y1 = ty + dy;
                if (y1 < 0 || y1 >= H) continue;
                for (int dx = -pr; dx <= pr; ++dx) {
                    const int x1 = tx + dx;
                    if (x1 < 0 || x1 >= W) continue;
                    const std::uint8_t* a = I.pixel(x1, y1);
                    const std::uint8_t* b = I.pixel(sx + dx, sy + dy);
                    for (int ch = 0; ch < 3; ++ch) {
                        const int e = static_cast<int>(a[ch]) - b[ch];
                        d += e * e;
                    }
                }
                if (d >= cap) return d;
            }
            return d;
        };
        const int passes = level == 0 ? 2 : 3;
        for (int pass = 0; pass < passes; ++pass) {
            const bool rev = pass & 1;
            for (std::size_t k = 0; k < pix.size(); ++k) {
                const int t = pix[rev ? pix.size() - 1 - k : k];
                const int tx = t % W, ty = t / W;
                int best = map[static_cast<std::size_t>(t)];
                long long bestD = dist(t, best, std::numeric_limits<long long>::max());
                auto tryS = [&](int s) {
                    if (s < 0 || s >= W * H || !valid[static_cast<std::size_t>(s)] || s == best) return;
                    const long long d = dist(t, s, bestD);
                    if (d < bestD) bestD = d, best = s;
                };
                // propagate from the neighbours already done this pass
                const int step = rev ? 1 : -1;
                for (const auto& [nx, ny] : {std::pair{tx + step, ty}, std::pair{tx, ty + step}}) {
                    if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                    const int n = map[static_cast<std::size_t>(ny) * W + nx];
                    if (n < 0) continue;
                    const int sx = n % W - step * (nx != tx), sy = n / W - step * (ny != ty);
                    if (sx >= 0 && sy >= 0 && sx < W && sy < H) tryS(sy * W + sx);
                }
                // random search around the current best, shrinking
                for (int rad = std::max(W, H) / 2; rad >= 1; rad /= 2) {
                    const int bx = best % W, by = best / W;
                    tryS(std::clamp(by + static_cast<int>(rnd() % (2 * rad + 1)) - rad, 0, H - 1) * W +
                         std::clamp(bx + static_cast<int>(rnd() % (2 * rad + 1)) - rad, 0, W - 1));
                }
                map[static_cast<std::size_t>(t)] = best;
                std::copy(I.pixel(best % W, best / W), I.pixel(best % W, best / W) + 4, I.pixel(tx, ty));
            }
        }
        from = std::move(map);
    }
    img = std::move(imgs.front());
    return true;
}

}  // namespace

bool inpaint(Image& img, const Mask& mask, int patch, std::string& error) {
    if (mask.width != img.width() || mask.height != img.height()) {
        error = "the area doesn't match the image size";
        return false;
    }
    if (patch < 3 || patch > 31) {
        error = "patch size must be between 3 and 31 pixels";
        return false;
    }
    if (patch % 2 == 0) ++patch;
    const std::size_t n = mask.count();
    if (n == 0) {
        error = "the area is empty (it lies outside the image or has no size)";
        return false;
    }
    if (n > kMaxRemovePixels) {
        error = "the area to remove is " + std::to_string(n) + " pixels; the limit is " + std::to_string(kMaxRemovePixels) +
                ". Select a smaller area, or remove the object in a few parts.";
        return false;
    }
    if (n > kMaxInpaintPixels) return inpaintPyramid(img, mask, patch, error);
    return inpaintLevel(img, mask, patch, nullptr, error);
}

// ---------------------------------------------------------------- commands

namespace {

using namespace raster;

std::vector<ParamSpec> areaParams(const std::string& what) {
    return {
        {"shape", ParamType::String, "shape of the area to " + what, false, {"rect", "ellipse", "stroke"}, std::string("rect")},
        {"x", ParamType::Int, "left edge of the area in px (rect and ellipse)", false},
        {"y", ParamType::Int, "top edge of the area in px (rect and ellipse)", false},
        {"width", ParamType::Int, "width of the area in px (rect and ellipse)", false},
        {"height", ParamType::Int, "height of the area in px (rect and ellipse)", false},
        {"points", ParamType::NumberList, "brush stroke as x1,y1,x2,y2,... in px (stroke)", false},
        {"radius", ParamType::Number, "brush radius in px (stroke)", false, {}, 10.0},
    };
}

bool buildMask(const Params& p, int w, int h, Mask& mask, std::string& error) {
    const std::string shape = stringParam(p, "shape", "rect");
    if (shape == "stroke") {
        auto pts = listParam(p, "points");
        if (pts.size() < 2 || pts.size() % 2 != 0) {
            error = "a stroke needs points as x,y pairs, e.g. [10,10,40,12]";
            return false;
        }
        double radius = numberParam(p, "radius", 10);
        if (radius <= 0 || radius > 1000) {
            error = "radius must be between 0 and 1000 px";
            return false;
        }
        mask = strokeMask(w, h, pts, radius);
    } else {
        if (!p.count("x") || !p.count("y") || !p.count("width") || !p.count("height")) {
            error = "give x, y, width and height of the area (or shape=stroke with points)";
            return false;
        }
        int x = static_cast<int>(intParam(p, "x")), y = static_cast<int>(intParam(p, "y"));
        int rw = static_cast<int>(intParam(p, "width")), rh = static_cast<int>(intParam(p, "height"));
        if (rw < 1 || rh < 1) {
            error = "width and height of the area must be at least 1";
            return false;
        }
        mask = shape == "ellipse" ? ellipseMask(w, h, x, y, rw, rh) : rectMask(w, h, x, y, rw, rh);
    }
    if (mask.count() == 0) {
        error = "the area is outside the image";
        return false;
    }
    return true;
}

Image* targetLayer(ImageDoc& doc, const Params& p, std::string& error) {
    int idx = pixelLayerIndex(doc, p, error);
    if (idx < 0) return nullptr;
    return &doc.image.layers[static_cast<std::size_t>(idx)].image;
}

Result editResult(const std::string& message, const ImageDoc& doc, const Params& p, std::size_t pixels) {
    Result r = Result::success(message);
    r.with("doc", stringParam(p, "doc"))
        .with("width", static_cast<std::int64_t>(doc.image.width))
        .with("height", static_cast<std::int64_t>(doc.image.height))
        .with("pixels", static_cast<std::int64_t>(pixels));
    return maybePreview(r, doc, p);
}

Result solveResult(const std::string& message, const ImageDoc& doc, const Params& p, std::size_t pixels, const SolveStats& st) {
    Result r = editResult(message, doc, p, pixels);
    return r.with("iterations", static_cast<std::int64_t>(st.iterations)).with("converged", st.converged);
}

Result heal(ImageDoc& doc, const Params& p) {
    std::string error;
    Image* img = targetLayer(doc, p, error);
    if (!img) return Result::failure(error);
    Mask mask;
    if (!buildMask(p, img->width(), img->height(), mask, error)) return Result::failure(error);
    const bool fromGiven = p.count("from_x") || p.count("from_y");
    if (fromGiven && !(p.count("from_x") && p.count("from_y"))) return Result::failure("give both from_x and from_y");
    int x0, y0, x1, y1;
    mask.bounds(x0, y0, x1, y1);
    int dx = 0, dy = 0;
    if (fromGiven) {
        dx = static_cast<int>(intParam(p, "from_x")) - (x0 + x1) / 2;
        dy = static_cast<int>(intParam(p, "from_y")) - (y0 + y1) / 2;
        if (x0 + dx < 0 || y0 + dy < 0 || x1 + dx >= img->width() || y1 + dy >= img->height()) {
            return Result::failure("the place to copy from runs off the image; pick a from_x/from_y further inside");
        }
    }
    const Image source = *img;
    SolveStats st;
    if (!poissonBlend(*img, fromGiven ? &source : nullptr, mask, dx, dy, st, error)) return Result::failure(error);
    return solveResult(std::string("healed ") + std::to_string(mask.count()) + " pixels" + (fromGiven ? "" : " (smooth fill)"), doc, p,
                       mask.count(), st);
}

Result cloneSeamless(Session& s, ImageDoc& doc, const Params& p) {
    std::string error;
    Image* img = targetLayer(doc, p, error);
    if (!img) return Result::failure(error);
    const std::string srcId = stringParam(p, "source_doc", stringParam(p, "doc"));
    auto srcDoc = s.workspace().getAs<ImageDoc>(srcId);
    if (!srcDoc) return Result::failure("no open image called '" + srcId + "' to copy from");
    Image source;
    if (srcDoc.get() == &doc) {
        source = *img;
        if (p.count("source_layer")) {
            Params lp{{"layer", p.at("source_layer")}};
            Image* l = targetLayer(doc, lp, error);
            if (!l) return Result::failure(error);
            source = *l;
        }
    } else {
        Params lp;
        if (p.count("source_layer")) lp["layer"] = p.at("source_layer");
        Image* l = targetLayer(*srcDoc, lp, error);
        if (!l) return Result::failure(error);
        source = *l;
    }
    const int fx = static_cast<int>(intParam(p, "from_x")), fy = static_cast<int>(intParam(p, "from_y"));
    const int w = static_cast<int>(intParam(p, "width")), h = static_cast<int>(intParam(p, "height"));
    const int tx = static_cast<int>(intParam(p, "to_x")), ty = static_cast<int>(intParam(p, "to_y"));
    if (w < 1 || h < 1) return Result::failure("width and height must be at least 1");
    if (fx < 0 || fy < 0 || fx + w > source.width() || fy + h > source.height()) {
        return Result::failure("the area to copy runs off the source image (it is " + std::to_string(source.width()) + "x" +
                               std::to_string(source.height()) + ")");
    }
    const bool ellipse = stringParam(p, "shape", "ellipse") == "ellipse";
    Mask mask = ellipse ? ellipseMask(img->width(), img->height(), tx, ty, w, h) : rectMask(img->width(), img->height(), tx, ty, w, h);
    if (mask.count() == 0) return Result::failure("the place to paste is outside the image");
    SolveStats st;
    if (!poissonBlend(*img, &source, mask, fx - tx, fy - ty, st, error)) return Result::failure(error);
    return solveResult("blended " + std::to_string(mask.count()) + " pixels from " + srcId, doc, p, mask.count(), st);
}

Result removeObject(ImageDoc& doc, const Params& p) {
    std::string error;
    Image* img = targetLayer(doc, p, error);
    if (!img) return Result::failure(error);
    Mask mask;
    if (!buildMask(p, img->width(), img->height(), mask, error)) return Result::failure(error);
    const int patch = static_cast<int>(intParam(p, "patch", 9));
    if (!inpaint(*img, mask, patch, error)) return Result::failure(error);
    return editResult("filled " + std::to_string(mask.count()) + " pixels from the surroundings", doc, p, mask.count());
}

}  // namespace

void registerHealCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    std::vector<ParamSpec> healParams{docParam()};
    for (auto& a : areaParams("heal")) healParams.push_back(a);
    healParams.push_back({"from_x", ParamType::Int,
                          "x of a clean spot to borrow texture from (the middle of the area copies from here); leave out "
                          "to fill smoothly from the surroundings",
                          false});
    healParams.push_back({"from_y", ParamType::Int, "y of the clean spot to borrow texture from", false});
    healParams.push_back(layerParam());
    healParams.push_back(previewParam());
    reg.add({"photo.heal",
             "Healing brush",
             "Remove a blemish, spot or scratch: the area takes its texture from a clean spot (from_x, from_y) and its "
             "colour and brightness from its own edges, so it blends in without a visible seam. Without a clean spot, "
             "the area is filled smoothly from its edges. Mark the area as a rectangle, an ellipse or a brush stroke.",
             healParams,
             true},
            imageCommand(session, heal));

    std::vector<ParamSpec> cloneParams{
        docParam(),
        {"source_doc", ParamType::String, "open image to copy from (default: the same image)", false},
        {"source_layer", ParamType::Int, "layer of the source image to copy (default: its active layer)", false},
        {"from_x", ParamType::Int, "left edge of the area to copy, in the source image", true},
        {"from_y", ParamType::Int, "top edge of the area to copy, in the source image", true},
        {"width", ParamType::Int, "width of the area to copy in px", true},
        {"height", ParamType::Int, "height of the area to copy in px", true},
        {"to_x", ParamType::Int, "left edge where it lands in this image", true},
        {"to_y", ParamType::Int, "top edge where it lands in this image", true},
        {"shape", ParamType::String, "shape of the pasted area", false, {"ellipse", "rect"}, std::string("ellipse")},
        layerParam(),
        previewParam(),
    };
    reg.add({"photo.clone_seamless",
             "Seamless clone",
             "Paste part of one picture into another (or elsewhere in the same one) so it blends in: the pasted area "
             "keeps its detail but takes on the colours and lighting around where it lands, with no hard edge.",
             cloneParams,
             true},
            imageCommand(session, [s](ImageDoc& doc, const Params& p) { return cloneSeamless(*s, doc, p); }));

    std::vector<ParamSpec> removeParams{docParam()};
    for (auto& a : areaParams("remove")) removeParams.push_back(a);
    removeParams.push_back({"patch", ParamType::Int,
                            "size of the pieces copied from the surroundings, 3..31 px (bigger suits coarse texture)", false,
                            {}, std::int64_t{9}});
    removeParams.push_back(layerParam());
    removeParams.push_back(previewParam());
    reg.add({"photo.remove_object",
             "Remove object",
             "Content-aware fill: erase something (a person, a sign, a power line) and fill the gap with picture from "
             "around it, continuing lines and textures into the hole. Small areas are filled patch by patch, most "
             "structured edges first; big ones (a whole person or car, up to 4 million pixels) on a smaller copy first "
             "and refined back up to full size.",
             removeParams,
             true},
            imageCommand(session, removeObject));
}

}  // namespace wetwaresuite::photo
