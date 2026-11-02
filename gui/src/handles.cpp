// Working on a part or a model right in the 3D view, as in Onshape, SolidWorks or Blender: the face
// under the mouse lights up, a click picks it, and arrows on it can be dragged: push/pull a flat
// face (cad.push_pull), move the body along X, Y or Z (cad.transform), or the selection of a model
// (model.drag). While an arrow is dragged a ghost shows where it goes, and when the mouse rests the
// real result is tried on the part (a Trial, preview.cpp); letting go keeps it as one undo step.
// Forms get arrows too: an open Extrude has one on its depth. Each arrow only runs commands, so
// everything stays scriptable and Claude sees it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "shell.hpp"

namespace wetwaresuite::gui {

namespace {

double dotc(const P3& a, const double* b) { return a.x * b[0] + a.y * b[1] + a.z * b[2]; }
double length(const P3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
P3 fromArray(const double* v) { return {v[0], v[1], v[2]}; }

NumberList numbers(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto l = v ? std::get_if<NumberList>(v) : nullptr;
    return l ? *l : NumberList{};
}
std::string words(const Result& r, const char* key) {
    const Value* v = r.field(key);
    return v ? toText(*v) : std::string();
}
bool yes(const Result& r, const char* key) {
    const Value* v = r.field(key);
    auto b = v ? std::get_if<bool>(v) : nullptr;
    return b && *b;
}
P3 point(const NumberList& l, std::size_t i = 0) { return l.size() >= i + 3 ? P3{l[i], l[i + 1], l[i + 2]} : P3{}; }

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4g", std::fabs(v) < 1e-9 ? 0.0 : v);
    return b;
}
std::string exact(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.12g", v);
    return b;
}
std::string triple(const P3& p) { return exact(p.x) + "," + exact(p.y) + "," + exact(p.z); }

double nowSeconds() { return static_cast<double>(SDL_GetTicks()) / 1000.0; }

// The smallest of 1, 2, 5 x 10^n model units that is at least `px` pixels long.
double niceStep(double pxPerUnit, double px) {
    for (int e = -4; e < 8; ++e)
        for (double m : {1.0, 2.0, 5.0}) {
            const double s = m * std::pow(10.0, e);
            if (s * pxPerUnit >= px) return s;
        }
    return 1e8;
}

float segDistance(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
    const float t = l2 > 0 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l2, 0.0f, 1.0f) : 0.0f;
    const float ex = a.x + dx * t - p.x, ey = a.y + dy * t - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

constexpr float kArrow = 78;  // arrow length on screen, px

}  // namespace

// ---- the camera ---------------------------------------------------------------------

void Shell::view3dCamera() {
    auto doc = session_.workspace().get(currentDoc_);
    if (view3d_.doc != currentDoc_) {
        view3d_ = View3d{};
        view3d_.doc = currentDoc_;
    }
    view3d_.camOk = doc && canvas_.orbitW > 0 && doc->viewCamera(canvas_.orbit, canvas_.orbitW, canvas_.orbitH, view3d_.cam);
    view3d_.camW = canvas_.orbitW;
    view3d_.camH = canvas_.orbitH;
}

void Shell::keepView(const ViewCamera& before) {
    auto doc = session_.workspace().get(currentDoc_);
    const int w = canvas_.orbitW, h = canvas_.orbitH;
    if (!doc || w < 8 || h < 8 || before.scale <= 0) return;
    OrbitView o = canvas_.orbit;
    o.camera.reset();
    o.zoom = 1;
    o.panX = o.panY = 0;
    ViewCamera c0, c1;
    if (!doc->viewCamera(o, w, h, c0)) return;
    o.panX = o.panY = 1;
    doc->viewCamera(o, w, h, c1);
    const double radius = c0.cx - c1.cx;
    if (c0.scale <= 0 || std::fabs(radius) < 1e-12) return;
    // the same angle shows the same picture with this zoom and pan
    canvas_.orbit.zoom = before.scale / c0.scale;
    canvas_.orbit.panX = (c0.cx - before.cx) / radius;
    canvas_.orbit.panY = (c0.cy - before.cy) / radius;
    canvas_.orbitDirty = true;
}

// ---- picking cad faces ----------------------------------------------------------------

View3d::Face Shell::cadPickRay(const P3& from, const P3& dir) {
    View3d::Face f;
    const Result r = query("cad.pick", {{"doc", currentDoc_}, {"ray", triple(from) + "," + triple(dir)}});
    if (!r.ok || !yes(r, "hit")) return f;
    f.hit = true;
    f.flat = yes(r, "flat");
    f.body = words(r, "body");
    f.face = words(r, "face");
    f.side = words(r, "side");
    f.point = point(numbers(r, "point"));
    f.normal = point(numbers(r, "normal"));
    f.centre = point(numbers(r, "centre"));
    if (const Value* a = r.field("area"); a && std::get_if<double>(a)) f.area = *std::get_if<double>(a);
    const NumberList b = numbers(r, "bounds");
    for (std::size_t i = 0; i < 6 && i < b.size(); ++i) f.bounds[i] = b[i];
    const NumberList o = numbers(r, "outline");
    for (std::size_t i = 0; i < o.size();) {
        const std::size_t n = static_cast<std::size_t>(o[i++]);
        std::vector<P3> loop;
        for (std::size_t j = 0; j < n && i + 2 < o.size() + 1; ++j, i += 3) loop.push_back(point(o, i));
        f.loops.push_back(std::move(loop));
    }
    return f;
}

View3d::Face Shell::cadPickAt(ImVec2 local) {
    const ViewCamera& c = view3d_.cam;
    if (!view3d_.camOk || c.scale <= 0) return {};
    const double a = (local.x - view3d_.camW / 2.0) / c.scale + c.cx;
    const double b = (view3d_.camH / 2.0 - local.y) / c.scale + c.cy;
    const P3 fwd = fromArray(c.forward);
    const P3 from = fromArray(c.right) * a + fromArray(c.up) * b - fwd * 1e6;
    return cadPickRay(from, fwd);
}

View3d::Edge Shell::cadEdgeAt(ImVec2 local) {
    View3d::Edge e;
    const ViewCamera& c = view3d_.cam;
    if (!view3d_.camOk || c.scale <= 0 || !find("cad.pick_edge")) return e;
    const double a = (local.x - view3d_.camW / 2.0) / c.scale + c.cx;
    const double b = (view3d_.camH / 2.0 - local.y) / c.scale + c.cy;
    const P3 fwd = fromArray(c.forward);
    const P3 from = fromArray(c.right) * a + fromArray(c.up) * b - fwd * 1e6;
    const Result r = query("cad.pick_edge", {{"doc", currentDoc_}, {"ray", triple(from) + "," + triple(fwd)}, {"tolerance", 7.0 / c.scale}});
    if (!r.ok || !yes(r, "hit")) return e;
    e.hit = true;
    e.outer = yes(r, "outer");
    e.body = words(r, "body");
    e.near = words(r, "near");
    e.mid = point(numbers(r, "midpoint"));
    e.out = point(numbers(r, "out"));
    const NumberList segs = numbers(r, "segments");
    for (std::size_t i = 0; i + 2 < segs.size(); i += 3) e.segments.push_back(point(segs, i));
    return e;
}

// ---- which arrows show -----------------------------------------------------------------

void Shell::cadHandles() {
    if (!view3d_.edges.empty()) {  // one arrow for the size of the round (or bevel) on all the picked edges
        const View3d::Edge& e = view3d_.edges.front();
        view3d_.handles.push_back({Handle::Kind::Blend, e.mid, length(e.out) > 0 ? e.out : P3{0, 0, 1}, ImGui::ColorConvertFloat4ToU32(accent_), "", 0, 0, 1,
                                   view3d_.chamfer ? "Drag to bevel the picked edges" : "Drag to round the picked edges"});
        return;
    }
    const View3d::Face& f = view3d_.picked;
    if (!f.hit) return;
    if (!view3d_.moveMode) {
        if (f.flat) view3d_.handles.push_back({Handle::Kind::PushPull, f.centre, f.normal, ImGui::ColorConvertFloat4ToU32(accent_), "", 0, 0, 1,
                                               "Drag to pull the face out or push it in"});
        else if (find("cad.offset_face"))  // a curved face moves along its own normals (a hole gets wider, a boss fatter)
            view3d_.handles.push_back({Handle::Kind::PushPull, f.point, f.normal, ImGui::ColorConvertFloat4ToU32(accent_), "", 0, 0, 1,
                                       "Drag to move this curved face out or in"});
        return;
    }
    const P3 mid{(f.bounds[0] + f.bounds[3]) / 2, (f.bounds[1] + f.bounds[4]) / 2, (f.bounds[2] + f.bounds[5]) / 2};
    view3d_.handles.push_back({Handle::Kind::Move, mid, {1, 0, 0}, IM_COL32(225, 70, 70, 255), "x", 0, 0, 1, "Drag to move it along X"});
    view3d_.handles.push_back({Handle::Kind::Move, mid, {0, 1, 0}, IM_COL32(60, 175, 85, 255), "y", 0, 0, 1, "Drag to move it along Y"});
    view3d_.handles.push_back({Handle::Kind::Move, mid, {0, 0, 1}, IM_COL32(60, 115, 230, 255), "z", 0, 0, 1, "Drag to move it along Z"});
}

void Shell::modelHandles() {
    if (canvas_.grab) return;
    if (view3d_.pivotSeen != changes_) {
        view3d_.pivotSeen = changes_;
        const Result r = query("model.pivot", {{"doc", currentDoc_}});
        const Value* n = r.ok ? r.field("selected") : nullptr;
        view3d_.pivot = n && std::get_if<std::int64_t>(n) && *std::get_if<std::int64_t>(n) > 0;
        view3d_.pivotAt = point(numbers(r, "centre"));
    }
    if (!view3d_.pivot) return;
    const P3 at = view3d_.pivotAt;
    view3d_.handles.push_back({Handle::Kind::Move, at, {1, 0, 0}, IM_COL32(225, 70, 70, 255), "x", 0, 0, 1, "Drag to move the selection along X (G then X)"});
    view3d_.handles.push_back({Handle::Kind::Move, at, {0, 1, 0}, IM_COL32(60, 175, 85, 255), "y", 0, 0, 1, "Drag to move the selection along Y (G then Y)"});
    view3d_.handles.push_back({Handle::Kind::Move, at, {0, 0, 1}, IM_COL32(60, 115, 230, 255), "z", 0, 0, 1, "Drag to move the selection along Z (G then Z)"});
}

void Shell::formHandles() {
    // an open Extrude: an arrow on the top of the profile sets its depth
    if (!form_ || form_->command != "cad.sketch_extrude") return;
    std::size_t depthField = form_->fields.size();
    std::string sketch, doc;
    bool reverse = false;
    for (std::size_t i = 0; i < form_->fields.size(); ++i) {
        const Field& f = form_->fields[i];
        if (f.spec.name == "depth") depthField = i;
        if (f.spec.name == "sketch") sketch = f.text;
        if (f.spec.name == "doc") doc = f.text;
        if (f.spec.name == "reverse") reverse = f.flag;
    }
    if (depthField == form_->fields.size() || sketch.empty() || (!doc.empty() && doc != currentDoc_)) return;
    P3& base = view3d_.formBase;
    P3& normal = view3d_.formNormal;
    bool& have = view3d_.formHave;
    const std::string key = currentDoc_ + "|" + sketch;
    if (key != view3d_.formKey) {  // the sketch's plane and the middle of its profiles
        view3d_.formKey = key;
        const Result g = query("cad.sketch_geometry", {{"doc", currentDoc_}, {"sketch", sketch}});
        const NumberList prof = numbers(g, "profiles"), o = numbers(g, "origin"), u = numbers(g, "u"), v = numbers(g, "v");
        have = g.ok && !prof.empty() && o.size() == 3;
        double sx = 0, sy = 0;
        std::size_t n = 0;
        view3d_.formLoops.clear();
        for (std::size_t i = 0; i < prof.size();) {
            const std::size_t c = static_cast<std::size_t>(prof[i++]);
            std::vector<P3> loop;
            for (std::size_t j = 0; j < c && i + 1 < prof.size(); ++j, i += 2) {
                sx += prof[i], sy += prof[i + 1], ++n;
                if (o.size() == 3 && u.size() == 3 && v.size() == 3) loop.push_back(point(o) + point(u) * prof[i] + point(v) * prof[i + 1]);
            }
            view3d_.formLoops.push_back(std::move(loop));
        }
        if (have && n) {
            sx /= static_cast<double>(n), sy /= static_cast<double>(n);
            base = point(o) + point(u) * sx + point(v) * sy;
            normal = point(numbers(g, "n"));
        }
    }
    if (!have) return;
    const Field& d = form_->fields[depthField];
    const double depth = d.set ? std::atof(d.text.c_str()) : 10;
    view3d_.handles.push_back({Handle::Kind::Field, base, reverse ? normal * -1 : normal, ImGui::ColorConvertFloat4ToU32(accent_), "", depthField,
                               std::isfinite(depth) ? depth : 10, 1, "Drag to set how far it extrudes"});
}

// ---- dragging -------------------------------------------------------------------------------

bool Shell::handleTrial(bool final) {
    View3d& v = view3d_;
    if (v.dragging < 0 || v.dragging >= static_cast<int>(v.handles.size())) return false;
    const Handle& h = v.handles[static_cast<std::size_t>(v.dragging)];
    std::string command;
    Params p{{"doc", currentDoc_}};
    if (h.kind == Handle::Kind::PushPull && !v.picked.flat) {
        command = "cad.offset_face";
        p["target"] = v.picked.body;
        p["face"] = v.picked.face;
        p["distance"] = exact(v.amount);
    } else if (h.kind == Handle::Kind::PushPull) {
        command = "cad.push_pull";
        p["face"] = v.picked.face;
        p["distance"] = v.amount;
    } else if (h.kind == Handle::Kind::Move && v.picked.hit) {
        command = "cad.transform";
        p["target"] = v.picked.body;
        p[h.axis] = exact(v.amount);
    } else if (h.kind == Handle::Kind::Blend && !v.edges.empty()) {
        if (v.amount <= 0) {  // a round has a size: dragging back in takes it off again
            dropTrial();
            return false;
        }
        command = v.chamfer ? "cad.chamfer" : "cad.fillet";
        p["target"] = v.edges.front().body;
        std::string near;
        for (const auto& e : v.edges) near += (near.empty() ? "" : ",") + e.near;
        p["near"] = near;
        p[v.chamfer ? "distance" : "radius"] = exact(v.amount);
    } else {
        return false;
    }
    if (std::fabs(v.amount) < 1e-12) {
        dropTrial();
        return false;
    }
    if (!final) {
        v.tryAmount = v.amount;
        return tryRun(command, p);
    }
    const char* what = h.kind == Handle::Kind::PushPull ? (v.amount > 0 ? "Pull" : "Push")
                       : h.kind == Handle::Kind::Blend ? (v.chamfer ? "Chamfer" : "Fillet")
                                                       : "Move";
    resultFor_ = what;
    if (keepTrial(command, p)) {
        result_ = trial_.result;
        return true;
    }
    Result r = run(command, p);
    const bool ok = r.ok;
    r.message = ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
    return ok;
}

void Shell::handleRelease() {
    View3d& v = view3d_;
    if (v.dragging < 0 || v.dragging >= static_cast<int>(v.handles.size())) {
        v.dragging = -1;
        canvas_.orbit.camera.reset();
        return;
    }
    const Handle h = v.handles[static_cast<std::size_t>(v.dragging)];
    const ViewCamera held = v.cam;
    bool done = false;
    if (h.kind != Handle::Kind::Field) done = handleTrial(true);
    v.dragging = -1;
    canvas_.orbit.camera.reset();
    keepView(held);
    if (done && h.kind == Handle::Kind::Blend) v.edges.clear();  // the edges are round now
    if (done && v.picked.hit) {  // keep working on the face (or body) where it went
        const P3 moved = h.kind == Handle::Kind::PushPull ? v.picked.point + v.picked.normal * v.amount : v.picked.point + h.dir * v.amount;
        const P3 fwd = fromArray(held.forward);
        const bool mode = v.moveMode;
        v.picked = cadPickRay(moved - fwd * 1e6, fwd);
        v.moveMode = mode;
    }
    v.amount = 0;
}

bool Shell::view3dInput(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active) {
    (void)dl;
    (void)active;
    View3d& v = view3d_;
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || v.doc != currentDoc_) return false;
    const bool cad = doc->kind() == "cad" && find("cad.pick");
    const bool model = doc->kind() == "model" && find("model.pivot");
    if (!cad && !model) return false;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 local{io.MousePos.x - v0.x, io.MousePos.y - v0.y};

    // the arrows for now
    if (v.dragging < 0) {
        v.handles.clear();
        if (cad) {
            if (form_ && form_->command == "cad.sketch_extrude") formHandles();
            else if (!(trial_.applied && form_ && form_->command == trial_.command)) cadHandles();  // not over a form's preview
        }
        if (model) modelHandles();
    }
    const ViewCamera& c = v.cam;
    auto screenOf = [&](const P3& p) {
        return ImVec2{v0.x + static_cast<float>((dotc(p, c.right) - c.cx) * c.scale + v.camW / 2.0),
                      v0.y + static_cast<float>(v.camH / 2.0 - (dotc(p, c.up) - c.cy) * c.scale)};
    };
    auto tipOf = [&](const Handle& h, ImVec2& a, ImVec2& b) {
        const P3 from = h.kind == Handle::Kind::Field ? h.base + h.dir * h.start : h.base;
        a = screenOf(from);
        const ImVec2 far = screenOf(from + h.dir);
        float dx = far.x - a.x, dy = far.y - a.y;
        const float l = std::sqrt(dx * dx + dy * dy);
        if (l < 1e-6f) dx = 0, dy = -1;
        else dx /= l, dy /= l;
        const float len = l < 0.25f * static_cast<float>(c.scale) ? kArrow * 0.45f : kArrow;  // pointing at you: short
        b = {a.x + dx * len, a.y + dy * len};
    };

    if (v.dragging < 0) {
        v.hot = -1;
        if (hovered && v.camOk && !canvas_.grab) {
            float best = 9;
            for (std::size_t i = 0; i < v.handles.size(); ++i) {
                ImVec2 a, b;
                tipOf(v.handles[i], a, b);
                const float d = segDistance(io.MousePos, a, b);
                if (d < best) best = d, v.hot = static_cast<int>(i);
            }
        }
        if (v.hot >= 0) ImGui::SetTooltip("%s", v.handles[static_cast<std::size_t>(v.hot)].tip);
        if (v.hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const Handle& h = v.handles[static_cast<std::size_t>(v.hot)];
            if (model) {  // Blender's G with the axis locked, until the mouse is let go
                canvas_.grab = 'G';
                canvas_.grabApplied = false;
                canvas_.grabStart = io.MousePos;
                canvas_.grabAxis = h.axis;
                canvas_.grabByHandle = true;
                return true;
            }
            v.dragging = v.hot;
            v.dragFrom = io.MousePos;
            v.amount = 0;
            v.tryAmount = 0;
            v.movedAt = nowSeconds();
            if (v.camOk) canvas_.orbit.camera = v.cam;  // the view holds still while the part changes
        }
    }
    if (v.dragging >= 0) {
        const Handle& h = v.handles[static_cast<std::size_t>(v.dragging)];
        // how far along the arrow the mouse went, in model units, snapped to a round step (Alt: free)
        const ImVec2 a = screenOf(h.base), b = screenOf(h.base + h.dir);
        const double sx = b.x - a.x, sy = b.y - a.y, l2 = sx * sx + sy * sy;
        double amount = 0;
        if (l2 > 1e-9) amount = ((io.MousePos.x - v.dragFrom.x) * sx + (io.MousePos.y - v.dragFrom.y) * sy) / l2;
        const double perUnit = std::sqrt(l2);
        if (!io.KeyAlt && perUnit > 0) {
            const double step = niceStep(perUnit, 6);
            amount = std::round(amount / step) * step;
        }
        if (h.kind == Handle::Kind::Field && h.start + amount < 1e-6) amount = 1e-3 - h.start;  // depth stays positive
        if (amount != v.amount) {
            v.amount = amount;
            v.movedAt = nowSeconds();
            if (h.kind == Handle::Kind::Field && form_ && h.field < form_->fields.size()) setFieldText(form_->fields[h.field], num(h.start + amount));
        }
        // resting: try the real thing on the part
        if (h.kind != Handle::Kind::Field && nowSeconds() - v.movedAt > 0.18 && (!trial_.applied || v.tryAmount != v.amount)) handleTrial(false);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {  // put it back
            dropTrial();
            v.amount = 0;
            v.dragging = -1;
            canvas_.orbit.camera.reset();
            keepView(v.cam);
        } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            handleRelease();
        }
        return true;
    }

    // cad: the face under the mouse lights up; a click picks it, Esc lets it go
    if (cad) {
        const bool moved = std::fabs(local.x - v.hoverAt.x) + std::fabs(local.y - v.hoverAt.y) > 1.5f;
        if (hovered && v.hot < 0 && !ImGui::IsAnyMouseDown() && (moved || v.hoverSeen != changes_)) {
            v.hoverAt = local;
            v.hoverSeen = changes_;
            // an edge near the mouse wins over the face it borders, as in SolidWorks
            v.hoverEdge = cadEdgeAt(local);
            v.hover = v.hoverEdge.hit ? View3d::Face{} : cadPickAt(local);
        }
        if (!hovered) v.hover = {}, v.hoverEdge = {};
        if (!v.edges.empty() && v.pickedSeen != changes_ && !trial_.applied) v.edges.clear();  // the part changed under them
        if (v.picked.hit && v.pickedSeen != changes_ && !trial_.applied) {  // the part changed under it: pick it again
            v.pickedSeen = changes_;
            const P3 fwd = fromArray(c.forward);
            const bool mode = v.moveMode;
            v.picked = cadPickRay(v.picked.point - fwd * 1e6, fwd);
            v.moveMode = mode;
        }
        const bool click = hovered && v.hot < 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                           io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < 16 && !io.KeyShift && size.x >= 8;
        if (click && v.hoverEdge.hit) {
            // a click picks the edge (and those carrying on from it); Ctrl+click adds or takes away, on one body
            auto same = [&](const View3d::Edge& e) { return e.body == v.hoverEdge.body && e.near == v.hoverEdge.near; };
            if (!io.KeyCtrl || (!v.edges.empty() && v.edges.front().body != v.hoverEdge.body)) v.edges.clear();
            auto it = std::find_if(v.edges.begin(), v.edges.end(), same);
            if (it != v.edges.end()) v.edges.erase(it);
            else v.edges.push_back(v.hoverEdge);
            v.picked = {};
            v.pickedSeen = changes_;
        } else if (click) {
            const bool mode = v.moveMode;
            v.picked = cadPickAt(local);
            v.pickedSeen = changes_;
            v.moveMode = mode && v.picked.hit;
            v.edges.clear();
        }
        if ((v.picked.hit || !v.edges.empty()) && ImGui::IsKeyPressed(ImGuiKey_Escape) && !io.WantTextInput) v.picked = {}, v.edges.clear();
    }
    return v.hot >= 0;
}

// ---- drawing ------------------------------------------------------------------------------

void Shell::view3dDraw(ImDrawList* dl, ImVec2 v0, ImVec2 size) {
    (void)size;
    View3d& v = view3d_;
    if (!v.camOk || v.doc != currentDoc_) return;
    const ViewCamera& c = v.cam;
    auto screenOf = [&](const P3& p) {
        return ImVec2{v0.x + static_cast<float>((dotc(p, c.right) - c.cx) * c.scale + v.camW / 2.0),
                      v0.y + static_cast<float>(v.camH / 2.0 - (dotc(p, c.up) - c.cy) * c.scale)};
    };
    const ImVec4 ac = accent_;
    const ImU32 accent = ImGui::ColorConvertFloat4ToU32(ac);
    auto face = [&](const View3d::Face& f, const P3& shift, float fill, float thick, ImU32 line) {
        for (const auto& loop : f.loops) {
            std::vector<ImVec2> pts;
            for (const P3& p : loop) pts.push_back(screenOf(p + shift));
            if (pts.size() < 3) continue;
            if (fill > 0) dl->AddConcavePolyFilled(pts.data(), static_cast<int>(pts.size()), ImGui::ColorConvertFloat4ToU32({ac.x, ac.y, ac.z, fill}));
            dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), line, ImDrawFlags_Closed, thick);
        }
    };
    const bool dragging = v.dragging >= 0 && v.dragging < static_cast<int>(v.handles.size());
    const bool formTrial = trial_.applied && form_ && form_->command == trial_.command && form_->command != "cad.sketch_extrude";
    // the face under the mouse, and the picked one
    if (v.hover.hit && !dragging && !(v.picked.hit && v.hover.face == v.picked.face) && v.hot < 0)
        face(v.hover, {}, 0.10f, 1.5f, ImGui::ColorConvertFloat4ToU32({ac.x, ac.y, ac.z, 0.8f}));
    if (v.picked.hit && !(dragging && trial_.applied) && !formTrial) {  // a form's preview has changed the part under it
        if (v.moveMode) {  // the body's box
            const double* b = v.picked.bounds;
            for (int e = 0; e < 12; ++e) {
                const int axis = e / 4, k = e % 4;
                P3 p, q;
                double lo[3] = {b[0], b[1], b[2]}, hi[3] = {b[3], b[4], b[5]};
                double s[3], t[3];
                const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
                s[axis] = lo[axis], t[axis] = hi[axis];
                s[o1] = t[o1] = (k & 1) ? hi[o1] : lo[o1];
                s[o2] = t[o2] = (k & 2) ? hi[o2] : lo[o2];
                p = fromArray(s), q = fromArray(t);
                dl->AddLine(screenOf(p), screenOf(q), ImGui::ColorConvertFloat4ToU32({ac.x, ac.y, ac.z, 0.7f}), 1.5f);
            }
        } else {
            face(v.picked, {}, 0.14f, 2.5f, accent);
        }
    }
    // edges: the one under the mouse, and the picked ones (thicker)
    auto edgeLines = [&](const View3d::Edge& e, float thick, ImU32 col) {
        for (std::size_t i = 0; i + 1 < e.segments.size(); i += 2) {
            dl->AddLine(screenOf(e.segments[i]), screenOf(e.segments[i + 1]), IM_COL32(255, 255, 255, 220), thick + 3);
            dl->AddLine(screenOf(e.segments[i]), screenOf(e.segments[i + 1]), col, thick);
        }
    };
    if (!(dragging && trial_.applied)) {
        if (v.hoverEdge.hit && v.hot < 0 && !dragging) edgeLines(v.hoverEdge, 2.5f, ImGui::ColorConvertFloat4ToU32({ac.x, ac.y, ac.z, 0.75f}));
        for (const auto& e : v.edges) edgeLines(e, 4.0f, accent);
    }
    // a ghost of where it goes while dragging (until the real thing is tried on the part)
    std::string label;
    if (dragging) {
        const Handle& h = v.handles[static_cast<std::size_t>(v.dragging)];
        const bool tried = trial_.applied && v.tryAmount == v.amount;
        if (h.kind == Handle::Kind::PushPull && !tried) {
            const P3 shift = h.dir * v.amount;
            const ImU32 ghost = v.amount >= 0 ? IM_COL32(40, 170, 90, 230) : IM_COL32(220, 70, 60, 230);
            face(v.picked, shift, 0.18f, 2.0f, ghost);
            for (const auto& loop : v.picked.loops)
                for (const P3& p : loop) dl->AddLine(screenOf(p), screenOf(p + shift), ghost, 1.5f);
        } else if (h.kind == Handle::Kind::Field) {  // the extrude's outline at the depth being dragged to
            const P3 shift = h.dir * (h.start + v.amount);
            for (const auto& loop : v.formLoops) {
                std::vector<ImVec2> pts;
                for (const P3& p : loop) {
                    pts.push_back(screenOf(p + shift));
                    dl->AddLine(screenOf(p), pts.back(), IM_COL32(40, 170, 90, 200), 1.5f);
                }
                if (pts.size() >= 3) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(40, 170, 90, 230), ImDrawFlags_Closed, 2.0f);
            }
        } else if (h.kind == Handle::Kind::Move && !tried && v.picked.hit) {
            const double* b = v.picked.bounds;
            const P3 shift = h.dir * v.amount;
            for (int e = 0; e < 12; ++e) {
                const int axis = e / 4, k = e % 4, o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
                double s[3], t[3];
                s[axis] = b[axis], t[axis] = b[axis + 3];
                s[o1] = t[o1] = (k & 1) ? b[o1 + 3] : b[o1];
                s[o2] = t[o2] = (k & 2) ? b[o2 + 3] : b[o2];
                dl->AddLine(screenOf(fromArray(s) + shift), screenOf(fromArray(t) + shift), h.colour, 2.0f);
            }
        }
        const double shown = h.kind == Handle::Kind::Field ? h.start + v.amount : v.amount;
        if (h.kind == Handle::Kind::Blend) label = (v.chamfer ? "chamfer " : "R ") + num(std::max(0.0, shown));
        else label = (h.kind != Handle::Kind::Field && v.amount > 0 ? "+" : "") + num(shown);
    }
    // the arrows
    for (std::size_t i = 0; i < v.handles.size(); ++i) {
        const Handle& h = v.handles[i];
        const bool hot = static_cast<int>(i) == v.hot || static_cast<int>(i) == v.dragging;
        const double along = h.kind == Handle::Kind::Field ? h.start + (static_cast<int>(i) == v.dragging ? v.amount : 0)
                                                           : (static_cast<int>(i) == v.dragging ? v.amount : 0);
        const ImVec2 a = screenOf(h.base + h.dir * along);
        const ImVec2 far = screenOf(h.base + h.dir * (along + 1));
        float dx = far.x - a.x, dy = far.y - a.y;
        const float l = std::sqrt(dx * dx + dy * dy);
        const float len = l < 0.25f * static_cast<float>(c.scale) ? kArrow * 0.45f : kArrow;
        if (l < 1e-6f) dx = 0, dy = -1;
        else dx /= l, dy /= l;
        const ImVec2 b{a.x + dx * len, a.y + dy * len};
        const float w = hot ? 4.5f : 3.2f, head = hot ? 16.0f : 14.0f;
        const ImU32 col = h.colour;
        const ImU32 halo = IM_COL32(255, 255, 255, 200);  // the 3D view's background is always light
        const ImVec2 neck{b.x - dx * head, b.y - dy * head};
        dl->AddLine(a, neck, halo, w + 3);
        dl->AddLine(a, neck, col, w);
        const ImVec2 side{-dy * head * 0.55f, dx * head * 0.55f};
        dl->AddTriangleFilled(b, {neck.x + side.x, neck.y + side.y}, {neck.x - side.x, neck.y - side.y}, col);
        dl->AddCircleFilled(a, hot ? 5.0f : 4.0f, col);
        if (static_cast<int>(i) == v.dragging && !label.empty())
            dl->AddText({b.x + 10, b.y - ImGui::GetFontSize() / 2}, IM_COL32(20, 22, 26, 255), label.c_str());
    }
}

// ---- the toolbar -------------------------------------------------------------------------------

void Shell::sketchOnFace(const View3d::Face& f) {
    if (!f.hit || !f.flat) return;
    Result r = run("cad.sketch", {{"doc", currentDoc_}, {"on", f.body}, {"face", f.face}});
    resultFor_ = "Sketch";
    if (!r.ok) {
        result_ = Result::failure(friendlyError(r.message));
        return;
    }
    sketch_.face3d.clear();
    for (const auto& loop : f.loops) {
        sketch_.face3d.push_back(static_cast<double>(loop.size()));
        for (const P3& p : loop) sketch_.face3d.insert(sketch_.face3d.end(), {p.x, p.y, p.z});
    }
    sketch_.faceEdges.clear();
    sketch_.onBody = f.body;
    view3d_.picked = {};
    r.message = sentence(r.message);
    result_ = std::move(r);
}

void Shell::sketchOnPlane(const char* plane) {
    Result r = run("cad.sketch", {{"doc", currentDoc_}, {"plane", std::string(plane)}});
    resultFor_ = "Sketch";
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::view3dToolbar() {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc || doc->kind() != "cad" || !find("cad.pick")) return;
    View3d& v = view3d_;
    if (ImGui::Button("New sketch")) ImGui::OpenPopup("newsketch");
    ImGui::SetItemTooltip("Start a sketch on a plane or on a face of the part, then draw on it with the mouse");
    if (ImGui::BeginPopup("newsketch")) {
        if (ImGui::MenuItem("On the clicked face", nullptr, false, v.picked.hit && v.picked.flat)) sketchOnFace(v.picked);
        if (!v.picked.hit) ImGui::TextDisabled("(click a flat face of the part first)");
        ImGui::Separator();
        if (ImGui::MenuItem("Top plane (XY)")) sketchOnPlane("xy");
        if (ImGui::MenuItem("Front plane (XZ)")) sketchOnPlane("xz");
        if (ImGui::MenuItem("Right plane (YZ)")) sketchOnPlane("yz");
        ImGui::EndPopup();
    }
    // a second row, the same height either way so the view doesn't jump: what to do, or the picked face's actions
    ImGui::AlignTextToFramePadding();
    if (!v.edges.empty()) {
        edgeToolbar();
        return;
    }
    if (!v.picked.hit) {
        ImGui::TextDisabled("%s", form_ && form_->command == "cad.sketch_extrude"
                                      ? "Drag the arrow to set the depth (or type it on the right), then Apply."
                                      : "Click a face to push/pull it, move it or sketch on it, or an edge to round it. Drag to turn, right-drag to move, scroll to zoom.");
        return;
    }
    ImGui::TextUnformatted((v.picked.body + (v.moveMode ? std::string(":") : v.picked.side == "slanted" ? ", a face:" : ", " + v.picked.side + " face:")).c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(!v.picked.flat);
    if (ImGui::Button("Sketch on it")) sketchOnFace(v.picked);
    ImGui::SetItemTooltip(v.picked.flat ? "Start a sketch right on this face" : "Only flat faces take a sketch");
    ImGui::EndDisabled();
    ImGui::SameLine();
    auto toggle = [&](const char* label, bool on, const char* tip) {
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, accent_);
            ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
        }
        const bool pressed = ImGui::Button(label);
        if (on) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", tip);
        ImGui::SameLine();
        return pressed;
    };
    if (toggle("Push/Pull", !v.moveMode, "Arrow on the face: drag it out to add, in to cut")) v.moveMode = false;
    if (toggle("Move", v.moveMode, "X, Y and Z arrows: drag to move the whole body")) v.moveMode = true;
    if (ImGui::Button("Fillet...") && find("cad.fillet")) {
        select("cad.fillet");
        if (form_)
            for (auto& f : form_->fields) {
                if (f.spec.name == "target") setFieldText(f, v.picked.body);
                if (f.spec.name == "edges" && (v.picked.side == "top" || v.picked.side == "bottom")) setFieldText(f, v.picked.side);  // the edges around this face
            }
    }
    ImGui::SetItemTooltip("Round the edges around this face, with a live preview");
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) v.picked = {};
    ImGui::SetItemTooltip("Let go of the face (Esc)");
}

void Shell::edgeToolbar() {
    View3d& v = view3d_;
    std::size_t pieces = 0;
    for (const auto& e : v.edges) pieces += e.segments.size() / 2;
    const std::string what = v.edges.front().body + ", " + std::to_string(v.edges.size()) + (v.edges.size() == 1 ? " edge" : " edges") +
                             (pieces > v.edges.size() ? " (" + std::to_string(pieces) + " pieces)" : std::string()) + ":";
    ImGui::TextUnformatted(what.c_str());
    ImGui::SameLine();
    auto toggle = [&](const char* label, bool on, const char* tip) {
        if (on) {
            ImGui::PushStyleColor(ImGuiCol_Button, accent_);
            ImGui::PushStyleColor(ImGuiCol_Text, accentText_);
        }
        const bool pressed = ImGui::Button(label);
        if (on) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", tip);
        ImGui::SameLine();
        return pressed;
    };
    if (toggle("Fillet", !v.chamfer, "The arrow rounds the edges: drag it out for a bigger radius")) v.chamfer = false;
    if (toggle("Chamfer", v.chamfer, "The arrow bevels the edges: drag it out for a bigger bevel")) v.chamfer = true;
    const char* command = v.chamfer ? "cad.chamfer" : "cad.fillet";
    if (ImGui::Button("Type a size...") && find(command)) {
        std::string near;
        for (const auto& e : v.edges) near += (near.empty() ? "" : ",") + e.near;
        const std::string body = v.edges.front().body;
        select(command);
        if (form_)
            for (auto& f : form_->fields) {
                if (f.spec.name == "target") setFieldText(f, body);
                if (f.spec.name == "near") setFieldText(f, near);
            }
    }
    ImGui::SetItemTooltip("Open the form for these edges, with a live preview");
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) v.edges.clear();
    ImGui::SetItemTooltip("Let go of the edges (Esc). Ctrl+click adds or takes away edges");
}

}  // namespace wetwaresuite::gui
