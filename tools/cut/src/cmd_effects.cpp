// Transitions (pictures and audio crossfades), colour correction, effects,
// the chroma key, the transform (picture-in-picture) and keyframes.
#include <algorithm>
#include <cmath>

#include "internal.hpp"

namespace wetwaresuite::cut {

namespace {

const Clip* before(const Timeline& tl, const Track& t, const Clip& c) { return c.media() ? tl.previousMedia(t, c) : tl.previousClip(t, c); }

// Overlaps `id` with the clip before it by `d` seconds (moving it and what follows on its track).
// Returns how far it moved.
double applyTransition(Timeline& tl, const std::string& id, const std::string& type, double d) {
    Track* owner = trackOf(tl, id);
    Clip* c = tl.findClip(id);
    if (c->kind == "title") throw std::runtime_error("titles come and go with anim_in and anim_out (cut.set_clip), not transitions");
    const Clip* prev = before(tl, *owner, *c);
    if (!prev) throw std::runtime_error(c->id + " is the first clip on " + owner->id + "; there's nothing before it to blend with");
    if (type == "none") {
        c->transition.clear();
        c->transitionDuration = 0;
        return 0;
    }
    if (d <= 0.04) throw std::runtime_error("duration must be more than 0.04s");
    if (d > std::min(prev->length(), c->length()) - 1e-3) {
        throw std::runtime_error("a " + num(d) + "s transition is longer than " + (prev->length() < c->length() ? prev->id : c->id) + " (" +
                                 num(std::min(prev->length(), c->length())) + "s)");
    }
    const double newStart = prev->end() - d, oldStart = c->start, delta = newStart - oldStart;
    for (auto& x : owner->clips) {
        if (x.id != id && x.start > oldStart + kEps) x.start = std::max(0.0, x.start + delta);
    }
    c->start = newStart;
    c->transition = c->kind == "audio" ? std::string("crossfade") : type;
    c->transitionDuration = d;
    return delta;
}

std::vector<Clip*> targets(Timeline& tl, const Params& p, bool (*fits)(const Clip&), const std::string& what) {
    std::vector<Clip*> out;
    if (has(p, "clip")) {
        Clip* c = requireClip(tl, p);
        if (!fits(*c)) throw std::runtime_error(what);
        out.push_back(c);
        return out;
    }
    for (auto& t : tl.tracks) {
        if (t.locked) continue;
        for (auto& c : t.clips) {
            if (fits(c)) out.push_back(&c);
        }
    }
    if (out.empty()) throw std::runtime_error("there are no clips to change");
    return out;
}

bool picture(const Clip& c) { return c.media(); }
bool anyVisual(const Clip& c) { return c.visual(); }

// Applies only `names` from p (after an optional reset of them to their defaults) to each clip.
Result applyGroup(Timeline& tl, const Params& p, std::vector<Clip*> clips, const std::vector<std::string>& names, const std::string& verb) {
    const Clip d;
    for (Clip* c : clips) {
        Clip copy = *c;
        if (boolParam(p, "reset", false)) {
            Params defaults;
            for (const auto& n : names) {
                if (const double* f = propertyField(d, n)) defaults[n] = *f;
                copy.keys.erase(n);
            }
            if (std::string err = applyProps(copy, defaults); !err.empty()) return Result::failure(err);
            for (const auto& n : names) {
                if (n == "grayscale") copy.grayscale = false;
                if (n == "lut") copy.lut.clear();
                if (n == "curve") copy.curve.clear();
                if (n == "curve_r") copy.curveR.clear();
                if (n == "curve_g") copy.curveG.clear();
                if (n == "curve_b") copy.curveB.clear();
                if (n == "wheel_lift") copy.wheelLift = d.wheelLift;
                if (n == "wheel_gamma") copy.wheelGamma = d.wheelGamma;
                if (n == "wheel_gain") copy.wheelGain = d.wheelGain;
                if (n == "flip_h") copy.flipH = false;
                if (n == "flip_v") copy.flipV = false;
                if (n == "key_color") copy.keyColor.clear();
            }
        }
        Params only;
        for (const auto& k : names) {
            if (has(p, k)) only[k] = p.at(k);
        }
        if (std::string err = applyProps(copy, only); !err.empty()) return Result::failure(err);
        if (std::string err = validate(copy); !err.empty()) return Result::failure(copy.id + ": " + err);
        *c = copy;
    }
    const std::string first = clips.front()->id;
    Result r = clipResult(verb + " " + (clips.size() == 1 ? first : std::to_string(clips.size()) + " clips"), tl, p, first);
    r.with("clips", static_cast<std::int64_t>(clips.size()));
    return r;
}

std::vector<ParamSpec> withSpecs(std::vector<ParamSpec> head, const std::vector<std::string>& names, bool reset) {
    for (auto& x : propSpecs(names)) head.push_back(x);
    if (reset) head.push_back({"reset", ParamType::Bool, "set these back to their defaults first", false, {}, false});
    head.push_back(previewParam());
    return head;
}

double keyTime(const Timeline& tl, const Clip& c, const Params& p) {
    if (has(p, "t")) return numberParam(p, "t");
    return numberParam(p, "at", tl.cursor) - c.start;
}

void registerKeyframeCommands(Session& session) {
    auto& reg = session.registry();
    std::string props;
    for (const auto& n : animatableProperties()) props += (props.empty() ? "" : ", ") + n;
    reg.add({"cut.keyframe", "Add Keyframe",
             "Animate a clip property: set its value at a moment, and between keyframes it moves smoothly. t is "
             "seconds into the clip; or give at (timeline time, default the playhead). ease shapes the way to the "
             "next keyframe. Properties: " + props + ".",
             {docParam(), clipParam(), {"property", ParamType::String, "what to animate", true, animatableProperties()},
              {"value", ParamType::Number, "its value there", true}, {"t", ParamType::Number, "seconds into the clip", false},
              {"at", ParamType::Number, "or: timeline time (default the playhead)", false},
              {"ease", ParamType::String, "how it moves on to the next keyframe", false, easeNames(), std::string("linear")}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                const std::string prop = stringParam(p, "property");
                if (!propertyField(*c, prop)) return Result::failure("'" + prop + "' can't be animated");
                const std::string ease = stringParam(p, "ease", "linear");
                if (std::find(easeNames().begin(), easeNames().end(), ease) == easeNames().end()) return Result::failure("unknown ease '" + ease + "'");
                const double t = keyTime(tl, *c, p), v = numberParam(p, "value");
                if (t < -1e-3 || t > c->length() + 1e-3) return Result::failure("the keyframe must be inside the clip: t 0.." + num(c->length()) + "s");
                Clip probe = *c;
                *propertyField(probe, prop) = v;
                probe.keys.clear();
                if (std::string err = validate(probe); !err.empty()) return Result::failure(err);
                auto& ks = c->keys[prop];
                ks.erase(std::remove_if(ks.begin(), ks.end(), [&](const Key& k) { return std::abs(k.t - t) < 1e-4; }), ks.end());
                ks.push_back({std::clamp(t, 0.0, c->length()), v, ease});
                std::sort(ks.begin(), ks.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
                Result r = clipResult(prop + " of " + c->id + " is " + num(v) + " at " + num(t) + "s (" + std::to_string(ks.size()) + " keyframes)", tl, p, c->id);
                return r.with("keyframes", static_cast<std::int64_t>(ks.size()));
            }));

    reg.add({"cut.remove_keyframe", "Remove Keyframe",
             "Remove a property's keyframe at t (seconds into the clip) or at (timeline time, default the playhead), "
             "or every keyframe of the property (all=true; leave out property to clear them all). The property "
             "keeps its fixed value.",
             {docParam(), clipParam(), {"property", ParamType::String, "which property", false, animatableProperties()},
              {"t", ParamType::Number, "seconds into the clip", false}, {"at", ParamType::Number, "or: timeline time", false},
              {"all", ParamType::Bool, "remove all of them", false, {}, false}},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                const std::string prop = stringParam(p, "property");
                if (boolParam(p, "all", false)) {
                    if (prop.empty()) c->keys.clear();
                    else c->keys.erase(prop);
                    return Result::success("removed the keyframes of " + (prop.empty() ? std::string("every property") : prop) + " on " + c->id);
                }
                if (prop.empty()) return Result::failure("give the property (or all=true)");
                auto it = c->keys.find(prop);
                if (it == c->keys.end()) return Result::failure(prop + " of " + c->id + " has no keyframes");
                const double t = keyTime(tl, *c, p);
                const double tol = 0.5 / tl.settings.fps + 1e-4;
                auto& ks = it->second;
                const auto before = ks.size();
                ks.erase(std::remove_if(ks.begin(), ks.end(), [&](const Key& k) { return std::abs(k.t - t) <= tol; }), ks.end());
                if (ks.size() == before) return Result::failure("no keyframe of " + prop + " at " + num(t) + "s");
                if (ks.empty()) c->keys.erase(it);
                return Result::success("removed the keyframe of " + prop + " at " + num(t) + "s on " + c->id);
            }));

    reg.add({"cut.move_keyframe", "Move Keyframe",
             "Slide the keyframes at one moment of a clip (every property's, or just `property`) to another moment, "
             "keeping their values and eases (what dragging a diamond on the timeline does). from and to are seconds "
             "into the clip.",
             {docParam(), clipParam(), {"from", ParamType::Number, "the keyframes' time, seconds into the clip", true},
              {"to", ParamType::Number, "where they go, seconds into the clip", true},
              {"property", ParamType::String, "only this property's keyframe", false, animatableProperties()}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                const double from = numberParam(p, "from"), to = numberParam(p, "to");
                if (to < -1e-3 || to > c->length() + 1e-3) return Result::failure("to must be inside the clip: 0.." + num(c->length()) + "s");
                const std::string only = stringParam(p, "property");
                const double tol = 0.5 / tl.settings.fps + 1e-4;
                int moved = 0;
                for (auto& [prop, ks] : c->keys) {
                    if (!only.empty() && prop != only) continue;
                    auto hit = std::find_if(ks.begin(), ks.end(), [&](const Key& k) { return std::abs(k.t - from) <= tol; });
                    if (hit == ks.end()) continue;
                    Key k = *hit;
                    ks.erase(hit);
                    k.t = std::clamp(to, 0.0, c->length());
                    // a keyframe already at the new time gives way
                    ks.erase(std::remove_if(ks.begin(), ks.end(), [&](const Key& o) { return std::abs(o.t - k.t) < 1e-4; }), ks.end());
                    ks.push_back(k);
                    std::sort(ks.begin(), ks.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
                    ++moved;
                }
                if (!moved) return Result::failure("no keyframe on " + c->id + " at " + num(from) + "s (cut.keyframes lists them)");
                Result r = clipResult("moved " + std::to_string(moved) + " keyframe(s) on " + c->id + " from " + num(from) + "s to " + num(to) + "s", tl, p, c->id);
                return r.with("moved", static_cast<std::int64_t>(moved));
            }));

    reg.add({"cut.keyframes", "List Keyframes", "List a clip's keyframes: property, time into the clip, value and ease.",
             {docParam(), clipParam()}, false, true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                std::string text;
                std::int64_t n = 0;
                for (const auto& [prop, ks] : c->keys) {
                    for (const auto& k : ks) {
                        text += (text.empty() ? "" : "\n") + prop + " " + num(k.t) + "s (at " + num(c->start + k.t) + "s) = " + num(k.v) +
                                (k.ease == "linear" ? "" : " " + k.ease);
                        ++n;
                    }
                }
                Result r = Result::success(text.empty() ? c->id + " has no keyframes (cut.keyframe adds one)" : text);
                return r.with("clip", c->id).with("count", n);
            }, false));
}

}  // namespace

void registerEffectCommands(Session& session) {
    auto& reg = session.registry();

    reg.add({"cut.transition", "Transition",
             "Blend a clip with the clip before it on the same track: cross dissolve, dip to black or white, wipes, "
             "slides, pushes, zooms, circles and more. The clip moves so the two overlap by duration, and later clips "
             "on the track move with it. On an audio track (or for a video's linked sound) it's an equal-power "
             "crossfade. type=none removes it.",
             {docParam(), clipParam("the later of the two clips"),
              {"type", ParamType::String, "transition style", false, transitionNames(), std::string("crossfade")},
              {"duration", ParamType::Number, "seconds of overlap", false, {}, 1.0}, linkedParam(), previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                const std::string id = c->id;
                std::string type = stringParam(p, "type", "crossfade");
                if (!knownTransition(type)) return Result::failure("unknown transition '" + type + "' (cut.transition lists them)");
                const double d = type == "none" ? 0 : numberParam(p, "duration", 1.0);
                std::vector<std::string> partners;
                if (boolParam(p, "linked", true)) {
                    for (Clip* x : group(tl, *c, true))
                        if (x->id != id) partners.push_back(x->id);
                }
                const double startBefore = c->start;
                const double delta = applyTransition(tl, id, type, d);
                for (const auto& pid : partners) {
                    Track* t = trackOf(tl, pid);
                    Clip* x = tl.findClip(pid);
                    const Clip* prev = before(tl, *t, *x);
                    if (prev && type != "none" && prev->end() > x->start - 1e-3 && std::abs(x->start - startBefore) < 1e-3) {
                        applyTransition(tl, pid, "crossfade", d);
                    } else if (type == "none" && prev) {
                        x->transition.clear();
                        x->transitionDuration = 0;
                    } else if (std::abs(delta) > kEps) {
                        const double old = x->start;
                        x->start += delta;
                        rippleTrack(*t, old + 1e-3, delta, {pid});
                    }
                }
                if (type == "none") return clipResult("removed the transition into " + id, tl, p, id);
                const Clip* now = tl.findClip(id);
                Result r = Result::success(type + " into " + id + " over " + num(d) + "s (" + id + " now starts at " + num(now->start) + "s)", id);
                r.with("clip", id).with("start", now->start).with("duration", tl.duration());
                return maybePreview(r, tl, p, now->start + d / 2);
            }));

    reg.add({"cut.filter", "Colour Correction",
             "Grade a clip's colour, or every picture clip's when clip is left out: exposure, white balance "
             "(temperature, tint), lift/gamma/gain colour wheels, brightness, contrast, saturation, gamma, tone "
             "curves (master and per channel), grayscale, or a .cube 3D LUT. reset=true sets them back first.",
             withSpecs({docParam(), {"clip", ParamType::String, "clip id; leave out for every video, image and matte clip", false}}, kColourProps, true),
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                return applyGroup(tl, p, targets(tl, p, picture, "colour correction applies to video, image, matte and nested clips"), kColourProps, "graded");
            }));

    reg.add({"cut.effect", "Effects",
             "Blur, sharpen, vignette (darker corners) or mirror a clip, or every picture clip when clip is left out. "
             "reset=true removes them first.",
             withSpecs({docParam(), {"clip", ParamType::String, "clip id; leave out for every picture clip", false}},
                       {"blur", "sharpen", "vignette", "flip_h", "flip_v"}, true),
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                return applyGroup(tl, p, targets(tl, p, anyVisual, "effects apply to pictures"), {"blur", "sharpen", "vignette", "flip_h", "flip_v"}, "changed the effects of");
            }));

    reg.add({"cut.chroma_key", "Chroma Key",
             "Green screen: make one colour of a clip transparent so the tracks below show through, with a soft edge "
             "and spill suppression (removes the green glow on hair and edges). Put the clip on a track above the "
             "background. off=true removes the key.",
             {docParam(), clipParam(), {"color", ParamType::String, "the screen's colour", false, {}, std::string("#00ff00")},
              {"similarity", ParamType::Number, "how close a colour must be to vanish, 0..1", false, {}, 0.4},
              {"softness", ParamType::Number, "how soft the edge is, 0..1", false, {}, 0.1},
              {"spill", ParamType::Number, "how much of the screen's glow to remove, 0..1", false, {}, 0.5},
              {"off", ParamType::Bool, "remove the key", false, {}, false}, previewParam()},
             true},
            cutCommand(session, [](Timeline& tl, const Params& p) {
                Clip* c = requireClip(tl, p);
                if (!c->media()) return Result::failure("the chroma key works on video, image and nested clips");
                Clip copy = *c;
                if (boolParam(p, "off", false)) {
                    copy.keyColor.clear();
                } else {
                    copy.keyColor = stringParam(p, "color", "#00ff00");
                    copy.keySimilarity = numberParam(p, "similarity", copy.keySimilarity);
                    copy.keySoftness = numberParam(p, "softness", copy.keySoftness);
                    copy.keySpill = numberParam(p, "spill", copy.keySpill);
                }
                if (std::string err = validate(copy); !err.empty()) return Result::failure(err);
                *c = copy;
                return clipResult(copy.keyColor.empty() ? "removed the key from " + copy.id : "keyed " + copy.keyColor + " out of " + copy.id, tl, p, copy.id);
            }));

    {
        std::vector<ParamSpec> s{docParam(), clipParam(),
                                 {"pip", ParamType::String, "place it as a picture-in-picture in a corner, or back to full frame", false,
                                  {"top-left", "top-right", "bottom-left", "bottom-right", "center", "full"}},
                                 {"size", ParamType::Number, "with pip: its width as a part of the frame's", false, {}, 0.3},
                                 {"margin", ParamType::Number, "with pip: gap to the frame's edge in px", false, {}, 40.0}};
        reg.add({"cut.transform", "Transform",
                 "Place, size, turn and crop a clip's picture: the box (x, y, width, height), scale and rotation "
                 "around its centre, edge crops, opacity and blend mode. pip=top-right (with size) makes it a "
                 "picture-in-picture in one step. Every number here can be animated with cut.keyframe.",
                 withSpecs(s, {"x", "y", "width", "height", "fit", "scale", "rotation", "crop_left", "crop_right", "crop_top", "crop_bottom",
                               "opacity", "blend", "flip_h", "flip_v"}, false),
                 true},
                cutCommand(session, [](Timeline& tl, const Params& p) {
                    Clip* c = requireClip(tl, p);
                    if (!c->visual()) return Result::failure("only pictures have a transform");
                    Params q = p;
                    if (has(p, "pip")) {
                        const std::string where = stringParam(p, "pip");
                        const double W = tl.settings.width, H = tl.settings.height;
                        if (where == "full") {
                            q["x"] = 0.0, q["y"] = 0.0, q["width"] = 0.0, q["height"] = 0.0, q["scale"] = 1.0, q["rotation"] = 0.0;
                        } else {
                            const double k = numberParam(p, "size", 0.3), m = numberParam(p, "margin", 40);
                            if (k <= 0.01 || k > 1) return Result::failure("size is a part of the frame, 0.01..1");
                            // the box takes the picture's own shape (a 4:3 webcam stays 4:3), so the margin is exact
                            const double aspect = c->sourceWidth > 0 && c->sourceHeight > 0 && c->cropW <= 0
                                                      ? static_cast<double>(c->sourceWidth) / c->sourceHeight
                                                      : c->cropW > 0 && c->cropH > 0 ? c->cropW / c->cropH
                                                      : W / H;
                            const double w = std::round(W * k), h = std::round(w / aspect);
                            double x = (W - w) / 2, y = (H - h) / 2;
                            if (where.find("left") != std::string::npos) x = m;
                            if (where.find("right") != std::string::npos) x = W - w - m;
                            if (where.find("top") != std::string::npos) y = m;
                            if (where.find("bottom") != std::string::npos) y = H - h - m;
                            if (!has(p, "x")) q["x"] = x;
                            if (!has(p, "y")) q["y"] = y;
                            if (!has(p, "width")) q["width"] = w;
                            if (!has(p, "height")) q["height"] = h;
                        }
                    }
                    Clip copy = *c;
                    Params only;
                    for (const char* k : {"x", "y", "width", "height", "fit", "scale", "rotation", "crop_left", "crop_right", "crop_top", "crop_bottom",
                                          "opacity", "blend", "flip_h", "flip_v"}) {
                        if (has(q, k)) only[k] = q.at(k);
                    }
                    if (only.empty()) return Result::failure("give something to change (x, y, width, scale, rotation, ... or pip)");
                    if (std::string err = applyProps(copy, only); !err.empty()) return Result::failure(err);
                    if (copy.kind == "title" && (has(only, "x") || has(only, "y"))) copy.position = "custom";
                    if (std::string err = validate(copy); !err.empty()) return Result::failure(err);
                    *c = copy;
                    return clipResult("transformed " + copy.id, tl, p, copy.id);
                }));
    }
    registerKeyframeCommands(session);
}

}  // namespace wetwaresuite::cut
