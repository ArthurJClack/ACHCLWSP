// Push-and-shove: a new trace pushes other nets' traces on its layer aside (they bend around it
// with 45-degree jogs and stay connected) instead of refusing to go where they are. Traces that
// move push the next ones in turn, a few levels deep; pads and vias don't move, so a push that
// would run into one is undone and reported. Also net lengths and length tuning (meanders).
#include <algorithm>
#include <cmath>
#include <deque>

#include "wetwaresuite/circuit/circuit.hpp"
#include "geom.hpp"

namespace wetwaresuite::circuit {

namespace {

Pt add(Pt a, Pt b) { return {a.x + b.x, a.y + b.y}; }
Pt sub(Pt a, Pt b) { return {a.x - b.x, a.y - b.y}; }
Pt mul(Pt a, double k) { return {a.x * k, a.y * k}; }
double len(Pt a) { return std::hypot(a.x, a.y); }
Pt lerp(Pt a, Pt b, double t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; }

// the gap a pair of traces needs, centre line to centre line
double needed(const Trace& a, const Trace& b, double clearance) { return a.width / 2 + b.width / 2 + clearance; }

bool clearOfFixed(const Trace& t, const std::vector<WorldPad>& pads, const std::vector<Via>& vias, double clearance) {
    const geom::Shape s = geom::capsule(t.a, t.b, t.width / 2);
    for (const auto& p : pads) {
        if (!(p.layers & layerBit(t.layer)) || (!p.net.empty() && p.net == t.net)) continue;
        if (geom::distance(s, geom::padShape(p)) < clearance - 1e-6) return false;
    }
    for (const auto& v : vias) {
        if (!v.net.empty() && v.net == t.net) continue;
        if (geom::distance(s, geom::capsule(v.pos, v.pos, v.diameter / 2)) < clearance - 1e-6) return false;
    }
    return true;
}

// The pushed trace's new path: it leaves its old line before the pusher, runs alongside it far
// enough away, and comes back after it, with 45-degree jogs.
std::vector<Pt> detour(const Trace& t, const Trace& pusher, double need) {
    const Pt d = sub(t.b, t.a);
    const double L = len(d);
    if (L < 1e-9) return {t.a, t.b};
    const Pt u = mul(d, 1 / L);
    // which side of the pusher the trace is on, and the push direction (normal to the pusher)
    const Pt pd = sub(pusher.b, pusher.a);
    const double pl = len(pd);
    Pt n = pl > 1e-9 ? Pt{-pd.y / pl, pd.x / pl} : Pt{-u.y, u.x};
    const Pt mid = lerp(t.a, t.b, 0.5);
    double side = (mid.x - pusher.a.x) * n.x + (mid.y - pusher.a.y) * n.y;
    if (std::abs(side) < 1e-9) side = 1;
    if (side < 0) n = mul(n, -1);
    // how far: step out until the moved middle part clears the pusher
    const double gap0 = geom::segSeg(t.a, t.b, pusher.a, pusher.b);
    double k = std::max(0.02, need - gap0 + 0.01);
    // the stretch of the trace beside the pusher (its ends projected, with room for the jogs)
    auto param = [&](Pt q) { return ((q.x - t.a.x) * u.x + (q.y - t.a.y) * u.y) / L; };
    std::vector<Pt> path;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Pt off = mul(n, k);
        const double margin = (need + k) / L, jog = k / L;
        const double lo = std::min(param(pusher.a), param(pusher.b)) - margin, hi = std::max(param(pusher.a), param(pusher.b)) + margin;
        const double j0 = std::clamp(lo, 0.0, 1.0), j1 = std::clamp(hi, 0.0, 1.0);
        double m0 = std::clamp(lo + jog, 0.0, 1.0), m1 = std::clamp(hi - jog, 0.0, 1.0);
        if (m0 > m1) m0 = m1 = (m0 + m1) / 2;
        path = {t.a, lerp(t.a, t.b, j0), add(lerp(t.a, t.b, m0), off), add(lerp(t.a, t.b, m1), off), lerp(t.a, t.b, j1), t.b};
        // drop repeated points
        std::vector<Pt> clean;
        for (const auto& p : path)
            if (clean.empty() || len(sub(p, clean.back())) > 1e-6) clean.push_back(p);
        path = clean;
        bool ok = true;
        for (std::size_t i = 1; i < path.size() && ok; ++i) ok = geom::segSeg(path[i - 1], path[i], pusher.a, pusher.b) >= need - 1e-6;
        if (ok) return path;
        k *= 1.4;
    }
    return {};
}

}  // namespace

ShoveReport addTraceShoving(Circuit& c, const Netlist& nl, const std::vector<Trace>& segs, double maxDepthIn) {
    ShoveReport rep;
    const double clearance = c.board.rules.clearance;
    const int maxDepth = std::max(1, static_cast<int>(maxDepthIn));
    const auto pads = worldPads(c, nl);
    std::vector<Trace>& traces = c.board.traces;
    std::vector<int> fixed(traces.size(), 0);  // 1: may not move (the new trace and what this push already moved)
    // pieces of one path (the new trace, or one pushed trace's detour) don't push each other
    std::vector<int> group(traces.size());
    for (std::size_t i = 0; i < group.size(); ++i) group[i] = static_cast<int>(i) + 1;
    struct Push {
        Trace t;
        int depth, group;
    };
    std::deque<Push> todo;
    for (const auto& s : segs) {
        traces.push_back(s);
        fixed.push_back(1);
        group.push_back(0);
        todo.push_back({s, 0, 0});
    }
    int guard = 0;
    while (!todo.empty() && ++guard < 400) {
        const Push p = todo.front();
        todo.pop_front();
        for (std::size_t i = 0; i < traces.size(); ++i) {
            Trace& t = traces[i];
            if (group[i] == p.group || t.layer != p.t.layer || (!t.net.empty() && t.net == p.t.net)) continue;
            const double need = needed(p.t, t, clearance);
            if (geom::segSeg(t.a, t.b, p.t.a, p.t.b) >= need - 1e-6) continue;
            const std::string who = t.net.empty() ? "a trace" : t.net;
            if (fixed[i] || p.depth >= maxDepth) {
                rep.blocked.push_back(who + " (couldn't move further)");
                continue;
            }
            const std::vector<Pt> path = detour(t, p.t, need);
            std::vector<Trace> repl;
            for (std::size_t k = 1; k < path.size(); ++k) repl.push_back({path[k - 1], path[k], t.width, t.layer, t.net});
            // the first and last pieces lie on the old trace, so only the moved middle needs checking
            bool ok = !repl.empty();
            for (std::size_t k = 1; k + 1 < repl.size(); ++k) ok = ok && clearOfFixed(repl[k], pads, c.board.vias, clearance);
            if (!ok) {
                rep.blocked.push_back(who + " (a pad or via is in the way)");
                continue;
            }
            // swap in the detour; its pieces push what they now touch
            const int g = group[i];
            traces[i] = repl[0];
            fixed[i] = 1;
            for (std::size_t k = 1; k < repl.size(); ++k) {
                traces.push_back(repl[k]);
                fixed.push_back(1);
                group.push_back(g);
            }
            ++rep.moved;
            for (const auto& r : repl) todo.push_back({r, p.depth + 1, g});
        }
    }
    std::sort(rep.blocked.begin(), rep.blocked.end());
    rep.blocked.erase(std::unique(rep.blocked.begin(), rep.blocked.end()), rep.blocked.end());
    return rep;
}

double netLength(const Circuit& c, const std::string& net) {
    double total = 0;
    for (const auto& t : c.board.traces)
        if (t.net == net) total += len(sub(t.b, t.a));
    return total;
}

TuneReport tuneLength(Circuit& c, const Netlist& nl, const std::string& net, double target, double amplitude, double spacing) {
    TuneReport rep;
    rep.before = rep.after = netLength(c, net);
    const double need = target - rep.before;
    if (need <= 0.01) {
        rep.note = need < -0.01 ? "already longer than that (meanders only add length)" : "already that long";
        return rep;
    }
    const double clearance = c.board.rules.clearance;
    const auto pads = worldPads(c, nl);
    // the straight segments of the net, longest first
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < c.board.traces.size(); ++i)
        if (c.board.traces[i].net == net) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return len(sub(c.board.traces[a].b, c.board.traces[a].a)) > len(sub(c.board.traces[b].b, c.board.traces[b].a));
    });
    if (order.empty()) {
        rep.note = "the net has no traces to stretch";
        return rep;
    }
    double left = need;
    for (std::size_t idx : order) {
        if (left <= 0.01) break;
        const Trace t = c.board.traces[idx];
        const Pt d = sub(t.b, t.a);
        const double L = len(d);
        const double pitch = std::max(spacing, t.width + clearance) ;  // along the trace per half bump
        const double margin = pitch;
        const int bumps = static_cast<int>(std::floor((L - 2 * margin) / (2 * pitch)));
        if (bumps < 1) continue;
        const Pt u = mul(d, 1 / L), n{-u.y, u.x};
        // each bump (out A, along, back A, along) adds 2A; the last one takes what's left
        for (double sideSign : {1.0, -1.0}) {
            const int use = std::min(bumps, static_cast<int>(std::ceil(left / (2 * amplitude))));
            const double a = std::min(amplitude, left / (2 * use));
            std::vector<Pt> path{t.a};
            const double start = (L - use * 2 * pitch) / 2;
            Pt at = add(t.a, mul(u, start));
            path.push_back(at);
            for (int b = 0; b < use; ++b) {
                const Pt out = add(at, mul(n, a * sideSign));
                const Pt along = add(out, mul(u, pitch));
                const Pt back = add(along, mul(n, -a * sideSign));
                path.insert(path.end(), {out, along, back});
                at = add(back, mul(u, pitch));
                path.push_back(at);
            }
            path.push_back(t.b);
            std::vector<Trace> repl;
            for (std::size_t k = 1; k < path.size(); ++k)
                if (len(sub(path[k], path[k - 1])) > 1e-6) repl.push_back({path[k - 1], path[k], t.width, t.layer, t.net});
            // the meander mustn't touch other nets (its first and last pieces lie on the old trace, so
            // they keep whatever the old one touched)
            bool ok = true;
            for (std::size_t k = 1; k + 1 < repl.size(); ++k) {
                const Trace& r = repl[k];
                ok = ok && clearOfFixed(r, pads, c.board.vias, clearance);
                for (std::size_t j = 0; j < c.board.traces.size() && ok; ++j) {
                    const Trace& o = c.board.traces[j];
                    if (j == idx || o.layer != r.layer || o.net == r.net) continue;
                    ok = geom::segSeg(r.a, r.b, o.a, o.b) >= needed(r, o, clearance) - 1e-6;
                }
                ok = ok && (c.board.outline.size() < 3 || (insideBoard(c.board, r.a) && insideBoard(c.board, r.b)));
            }
            if (!ok) continue;
            c.board.traces[idx] = repl[0];
            for (std::size_t k = 1; k < repl.size(); ++k) c.board.traces.push_back(repl[k]);
            left -= 2 * a * use;
            ++rep.segments;
            break;
        }
    }
    rep.after = netLength(c, net);
    if (target - rep.after > 0.05) rep.note = "ran out of room: " + formatValue(target - rep.after) + "mm short (longer straight runs or a bigger amplitude help)";
    return rep;
}

}  // namespace wetwaresuite::circuit
