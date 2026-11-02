// Rendering (through the render tool's path tracer) and keyframe animation.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>

#include "cmd.hpp"
#include "wetwaresuite/animate/animate.hpp"
#include "wetwaresuite/raster/codec.hpp"

namespace wetwaresuite::model::cmd {

namespace fs = std::filesystem;

namespace {

std::vector<ParamSpec> renderParams(int width, int height, const std::string& mode, int samples) {
    return {{"width", ParamType::Int, "pixels", false, {}, std::int64_t{width}},
            {"height", ParamType::Int, "pixels", false, {}, std::int64_t{height}},
            {"mode", ParamType::String, "path tracer or fast preview rasterizer", false, {"path", "preview"}, mode},
            {"samples", ParamType::Int, "path tracer samples per pixel", false, {}, std::int64_t{samples}},
            {"camera", ParamType::String, "camera object, or view (orbit from yaw/pitch); default the scene camera", false},
            {"yaw", ParamType::Number, "camera=view: degrees around", false, {}, 45.0},
            {"pitch", ParamType::Number, "camera=view: degrees up", false, {}, 25.0},
            {"ground", ParamType::Bool, "add a ground plane", false, {}, false},
            {"denoise", ParamType::Bool, "path tracer denoiser", false, {}, true}};
}

RenderSetup setupFrom(const Params& p) {
    RenderSetup s;
    s.camera = stringParam(p, "camera");
    s.yaw = numberParam(p, "yaw", 45);
    s.pitch = numberParam(p, "pitch", 25);
    s.ground = boolParam(p, "ground", false);
    s.width = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "width", 640), 8, 8192));
    s.height = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "height", 480), 8, 8192));
    return s;
}

render::RenderOptions optionsFrom(const Params& p, const RenderSetup& s, const std::string& mode, int samples) {
    render::RenderOptions o;
    o.mode = stringParam(p, "mode", mode);
    o.width = s.width;
    o.height = s.height;
    o.samples = static_cast<int>(std::clamp<std::int64_t>(intParam(p, "samples", samples), 1, 4096));
    o.denoise = boolParam(p, "denoise", true);
    return o;
}

render::Scene sceneAt(const ModelDoc& d, int frame, bool useFrame, const RenderSetup& setup) {
    if (!useFrame) return toRenderScene(d, setup);
    ModelDoc copy = d;
    applyFrame(copy, frame);
    copy.refresh();
    return toRenderScene(copy, setup);
}

raster::Image renderAt(const ModelDoc& d, int frame, bool useFrame, const Params& p, const std::string& mode, int samples,
                       render::RenderStats* stats, const std::optional<render::Camera>& camera = std::nullopt) {
    const RenderSetup setup = setupFrom(p);
    render::RenderOptions o = optionsFrom(p, setup, mode, samples);
    o.camera = camera;
    return render::renderScene(sceneAt(d, frame, useFrame, setup), o, stats);
}

// Without a camera object the view is fitted to the scene; for an animation
// it's fitted once around every frame, so the motion shows.
std::optional<render::Camera> animationCamera(const ModelDoc& d, const Params& p, int start, int end, int step) {
    const RenderSetup setup = setupFrom(p);
    bool hasCamera = false;
    for (const auto& o : d.scene.objects) hasCamera = hasCamera || o.type == "camera";
    if (setup.camera != "view" && (hasCamera || !setup.camera.empty())) return std::nullopt;
    RenderSetup noGround = setup;
    noGround.ground = false;
    render::Scene all = sceneAt(d, start, true, noGround);
    for (int f = start + step; f <= end; f += step) {
        render::Scene s = sceneAt(d, f, true, noGround);
        all.objects.insert(all.objects.end(), s.objects.begin(), s.objects.end());
    }
    render::Camera cam = all.camera;
    render::fitCamera(all, cam);
    return cam;
}

std::vector<std::string> channelsFrom(const std::string& spec) {
    std::vector<std::string> out;
    for (const auto& c : splitList(spec.empty() ? "location,rotation,scale" : spec)) {
        if (c == "all") {
            out.insert(out.end(), channelNames().begin(), channelNames().end());
        } else if (c == "location" || c == "rotation" || c == "scale") {
            for (const char* a : {".x", ".y", ".z"}) out.push_back(c + a);
        } else if (std::find(channelNames().begin(), channelNames().end(), c) != channelNames().end()) {
            out.push_back(c);
        } else {
            throw std::runtime_error("unknown channel '" + c + "' (location, rotation, scale, all or e.g. location.z)");
        }
    }
    return out;
}

// "location.x" -> "location", "pose.arm.rotation.z" -> "pose.arm.rotation"
std::string channelGroup(const std::string& ch) {
    const auto dot = ch.rfind('.');
    return dot == std::string::npos ? ch : ch.substr(0, dot);
}

// Channels of `o` a dope-sheet row names: empty or "all" = every keyed channel, a group
// ("location", "pose.arm.rotation") or one channel ("location.z").
std::vector<std::string> rowChannels(const Object& o, const std::string& row) {
    std::vector<std::string> out;
    for (const auto& [ch, keys] : o.keys) {
        if (keys.empty()) continue;
        if (row.empty() || row == "all" || ch == row || channelGroup(ch) == row) out.push_back(ch);
    }
    return out;
}

}  // namespace

void registerRenderCommands(Session& session) {
    auto& reg = session.registry();
    Session* s = &session;

    reg.add({"model.render",
             "Render",
             "Render the scene with the render tool's path tracer (or mode=preview for a fast rasterized one): every "
             "visible object with its modifiers and materials, the scene's lights (a sun when there are none) and its "
             "camera (or camera=view to orbit from yaw/pitch). frame= renders that animation frame. The PNG is attached "
             "and written to path= when given.",
             withDoc([] {
                 auto v = renderParams(640, 480, "path", 16);
                 v.push_back({"path", ParamType::String, "image file to write", false});
                 v.push_back({"frame", ParamType::Int, "animation frame (default: the current pose)", false});
                 return v;
             }(),
                     false),
             false},
            query(session, [](ModelDoc& d, const Params& p) {
                d.refresh();
                render::RenderStats stats;
                const raster::Image img = renderAt(d, static_cast<int>(intParam(p, "frame", d.scene.frame)), has(p, "frame"), p, "path", 16, &stats);
                Result r = Result::success("rendered " + std::to_string(img.width()) + "x" + std::to_string(img.height()) + ", " +
                                           std::to_string(stats.triangles) + " triangles in " + num(stats.seconds) + " s");
                for (const auto& w : stats.warnings) r.message += "\nwarning: " + w;
                if (has(p, "path")) {
                    raster::save(img, stringParam(p, "path"));
                    r.with("path", stringParam(p, "path"));
                }
                r.attach({"image/png", raster::encodePng(img), "render.png"});
                return r;
            }));

    reg.add({"model.to_render",
             "Send to render",
             "Make a render-tool scene (render.*) from the model: its evaluated objects with materials, lights and "
             "camera, for render.set, render.light, render.camera and render.render to take further.",
             withDoc([] {
                 auto v = renderParams(640, 480, "path", 16);
                 v.erase(v.begin() + 2, v.begin() + 4);
                 v.pop_back();
                 return v;
             }(),
                     false),
             false,
             true},
            [s](const Params& p) {
                return std::make_unique<FunctionAction>([s, p] {
                    std::string id, error;
                    auto doc = findDoc(*s, p, id, error);
                    if (!doc) return Result::failure(error);
                    try {
                        doc->refresh();
                        auto sd = std::make_shared<render::SceneDoc>();
                        sd->scene = toRenderScene(*doc, setupFrom(p));
                        sd->scene.settings.width = static_cast<int>(intParam(p, "width", 640));
                        sd->scene.settings.height = static_cast<int>(intParam(p, "height", 480));
                        const std::string sid = s->workspace().add(sd, "scene");
                        return Result::success("made render scene " + sid + ": " + sd->summary()).with("scene", sid);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"model.keyframe",
             "Insert keyframe",
             "Key objects' transforms at a frame (I in Blender): channels location, rotation, scale (default all three), "
             "all, or single ones like location.z. location/rotation/scale= set the values first. delete=true removes "
             "the keys at that frame. Between keys values ease in and out (bezier), or interp=linear/constant.",
             withDoc({objectParam(),
                      {"objects", ParamType::String, "several objects (default: selected)", false},
                      {"frame", ParamType::Int, "frame (default: the current one)", false},
                      {"channels", ParamType::String, "what to key, comma separated", false},
                      {"location", ParamType::NumberList, "set the location first", false},
                      {"rotation", ParamType::NumberList, "set the rotation first", false},
                      {"scale", ParamType::NumberList, "set the scale first", false},
                      {"interp", ParamType::String, "interpolation to the next key", false, {"bezier", "linear", "constant"}, std::string("bezier")},
                      {"delete", ParamType::Bool, "remove keys instead", false, {}, false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                const int frame = static_cast<int>(intParam(p, "frame", d.scene.frame));
                auto chans = channelsFrom(stringParam(p, "channels"));
                if (!has(p, "channels")) {
                    if (has(p, "location") || has(p, "rotation") || has(p, "scale")) {
                        chans.clear();
                        for (const char* g : {"location", "rotation", "scale"})
                            if (has(p, g))
                                for (const char* a : {".x", ".y", ".z"}) chans.push_back(std::string(g) + a);
                    }
                }
                std::size_t n = 0;
                const auto objs = targets(d, p);
                for (Object* o : objs) {
                    o->location = vec3(p, "location", o->location);
                    o->rotation = vec3(p, "rotation", o->rotation);
                    o->scale = vec3(p, "scale", o->scale);
                    for (const auto& ch : chans) {
                        auto& keys = o->keys[ch];
                        std::erase_if(keys, [&](const Keyframe& k) { return k.frame == frame; });
                        if (!boolParam(p, "delete", false)) {
                            keys.push_back({frame, channelValue(*o, ch), stringParam(p, "interp", "bezier")});
                            std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
                        }
                        if (keys.empty()) o->keys.erase(ch);
                        ++n;
                    }
                }
                d.scene.frame = frame;
                return done((boolParam(p, "delete", false) ? "removed keys on " : "keyed ") + std::to_string(n) + " channels at frame " +
                                std::to_string(frame),
                            d, p);
            }));

    reg.add({"model.frame",
             "Go to frame",
             "Set the current frame (objects move to their keyed poses) and/or the scene's frame range and fps.",
             withDoc({{"frame", ParamType::Int, "go to this frame", false},
                      {"start", ParamType::Int, "first frame", false},
                      {"end", ParamType::Int, "last frame", false},
                      {"fps", ParamType::Number, "frames per second", false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                if (has(p, "start")) d.scene.frameStart = static_cast<int>(intParam(p, "start"));
                if (has(p, "end")) d.scene.frameEnd = static_cast<int>(intParam(p, "end"));
                if (d.scene.frameEnd < d.scene.frameStart) return Result::failure("end is before start");
                if (has(p, "fps")) {
                    if (numberParam(p, "fps") <= 0) return Result::failure("fps must be positive");
                    d.scene.fps = numberParam(p, "fps");
                }
                applyFrame(d, static_cast<int>(intParam(p, "frame", d.scene.frame)));
                return done("frame " + std::to_string(d.scene.frame) + " of " + std::to_string(d.scene.frameStart) + ".." +
                                std::to_string(d.scene.frameEnd),
                            d, p);
            }));


    reg.add({"model.keys",
             "List keys",
             "The dope sheet as data: for each keyed object a summary row and one row per channel group (location, "
             "rotation, scale, pose.<bone>.rotation ...) with the frames that hold keys. rows is one line per row: "
             "object, group (all for the summary) and the frames, separated by tabs.",
             withDoc({{"object", ParamType::String, "only this object", false}}, false),
             false},
            query(session, [](ModelDoc& d, const Params& p) {
                std::string rows, msg;
                std::int64_t count = 0;
                const std::string only = stringParam(p, "object");
                for (const auto& o : d.scene.objects) {
                    if (!only.empty() && o.name != only) continue;
                    std::map<std::string, std::set<int>> groups;
                    std::set<int> all;
                    for (const auto& [ch, keys] : o.keys)
                        for (const auto& k : keys) {
                            groups[channelGroup(ch)].insert(k.frame);
                            all.insert(k.frame);
                        }
                    if (all.empty()) continue;
                    auto frames = [](const std::set<int>& s) {
                        std::string out;
                        for (int f : s) out += (out.empty() ? "" : ",") + std::to_string(f);
                        return out;
                    };
                    rows += o.name + "\tall\t" + frames(all) + "\n";
                    msg += o.name + ": frames " + frames(all) + "\n";
                    for (const auto& [g, s] : groups) {
                        rows += o.name + "\t" + g + "\t" + frames(s) + "\n";
                        msg += "  " + g + ": " + frames(s) + "\n";
                    }
                    count += static_cast<std::int64_t>(all.size());
                }
                if (msg.empty()) msg = "no keys yet (model.keyframe adds them)\n";
                msg.pop_back();
                return Result::success(msg)
                    .with("rows", rows)
                    .with("frame", static_cast<std::int64_t>(d.scene.frame))
                    .with("start", static_cast<std::int64_t>(d.scene.frameStart))
                    .with("end", static_cast<std::int64_t>(d.scene.frameEnd))
                    .with("fps", d.scene.fps)
                    .with("keyed_frames", count);
            }));

    reg.add({"model.key_move",
             "Move keys",
             "Drag keys along the timeline (the dope sheet): the keys of object at frame move to frame `to`, for one "
             "channel group (row=location, rotation, scale, pose.<bone>.rotation, location.z ...) or all of them "
             "(row=all, the default). Keys already at `to` are replaced. copy=true leaves the originals (duplicate).",
             withDoc({objectParam(),
                      {"frame", ParamType::Int, "frame the keys are on", true},
                      {"to", ParamType::Int, "frame to move them to", true},
                      {"row", ParamType::String, "channel group, or all", false, {}, std::string("all")},
                      {"copy", ParamType::Bool, "copy instead of move", false, {}, false}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                const int from = static_cast<int>(intParam(p, "frame")), to = static_cast<int>(intParam(p, "to"));
                const bool copy = boolParam(p, "copy", false);
                std::size_t n = 0;
                for (const auto& ch : rowChannels(o, stringParam(p, "row", "all"))) {
                    auto& keys = o.keys[ch];
                    auto it = std::find_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.frame == from; });
                    if (it == keys.end()) continue;
                    Keyframe k = *it;
                    if (from == to) {
                        ++n;
                        continue;
                    }
                    if (!copy) keys.erase(it);
                    std::erase_if(keys, [&](const Keyframe& x) { return x.frame == to; });
                    k.frame = to;
                    keys.push_back(k);
                    std::sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.frame < b.frame; });
                    ++n;
                }
                if (n == 0) return Result::failure(o.name + " has no keys at frame " + std::to_string(from) + " in that row");
                applyFrame(d, d.scene.frame);
                return done(std::string(copy ? "copied " : "moved ") + std::to_string(n) + " keys of " + o.name + " from frame " +
                                std::to_string(from) + " to " + std::to_string(to),
                            d, p);
            }));

    reg.add({"model.key_delete",
             "Delete keys",
             "Remove the keys of object at frame, for one channel group (row=location, pose.<bone>.rotation ...) or "
             "all of them (the default).",
             withDoc({objectParam(),
                      {"frame", ParamType::Int, "frame the keys are on", true},
                      {"row", ParamType::String, "channel group, or all", false, {}, std::string("all")}}),
             true},
            edit(session, [](ModelDoc& d, const Params& p) {
                Object& o = target(d, p);
                const int frame = static_cast<int>(intParam(p, "frame"));
                std::size_t n = 0;
                for (const auto& ch : rowChannels(o, stringParam(p, "row", "all"))) {
                    auto& keys = o.keys[ch];
                    n += static_cast<std::size_t>(std::erase_if(keys, [&](const Keyframe& k) { return k.frame == frame; }));
                    if (keys.empty()) o.keys.erase(ch);
                }
                if (n == 0) return Result::failure(o.name + " has no keys at frame " + std::to_string(frame) + " in that row");
                return done("removed " + std::to_string(n) + " keys of " + o.name + " at frame " + std::to_string(frame), d, p);
            }));

    reg.add({"model.scrub",
             "Scrub",
             "Show another frame while playing or dragging the playhead: like model.frame frame= but not an undo step "
             "(Blender doesn't undo frame changes either). wrap=true loops past the end back to the start.",
             withDoc({{"frame", ParamType::Int, "frame to show", true},
                      {"wrap", ParamType::Bool, "loop within the frame range", false, {}, false}},
                     false),
             false},
            query(session, [](ModelDoc& d, const Params& p) {
                int f = static_cast<int>(intParam(p, "frame"));
                const int a = d.scene.frameStart, b = d.scene.frameEnd;
                if (boolParam(p, "wrap", false) && b >= a) f = a + (((f - a) % (b - a + 1)) + (b - a + 1)) % (b - a + 1);
                applyFrame(d, f);
                return Result::success("frame " + std::to_string(f)).with("doc", stringParam(p, "doc")).with("frame", static_cast<std::int64_t>(f));
            }));

    reg.add({"model.render_animation",
             "Render animation",
             "Render the keyed animation: an animated GIF (path ends .gif) or a numbered PNG sequence (shot.png -> "
             "shot_0001.png ...). Defaults to the fast preview renderer at 320x240; mode=path for path-traced frames.",
             withDoc([] {
                 auto v = renderParams(320, 240, "preview", 4);
                 v.push_back({"path", ParamType::String, "anim.gif, or frames/shot.png for a sequence", true});
                 v.push_back({"start", ParamType::Int, "first frame (default: the scene's)", false});
                 v.push_back({"end", ParamType::Int, "last frame (default: the scene's)", false});
                 v.push_back({"step", ParamType::Int, "render every nth frame", false, {}, std::int64_t{1}});
                 return v;
             }(),
                     false),
             false},
            query(session, [](ModelDoc& d, const Params& p) {
                const int start = static_cast<int>(intParam(p, "start", d.scene.frameStart));
                const int end = static_cast<int>(intParam(p, "end", d.scene.frameEnd));
                const int step = static_cast<int>(std::max<std::int64_t>(1, intParam(p, "step", 1)));
                if (end < start) return Result::failure("end is before start");
                if ((end - start) / step + 1 > 1000) return Result::failure("at most 1000 frames");
                const fs::path path = stringParam(p, "path");
                const bool gif = path.extension() == ".gif" || path.extension() == ".GIF";
                std::vector<raster::Image> frames;
                std::size_t written = 0;
                d.refresh();
                const auto camera = animationCamera(d, p, start, end, step);
                for (int f = start; f <= end; f += step) {
                    raster::Image img = renderAt(d, f, true, p, "preview", 4, nullptr, camera);
                    if (gif) {
                        frames.push_back(std::move(img));
                    } else {
                        char buf[16];
                        std::snprintf(buf, sizeof buf, "_%04d", f);
                        const fs::path out = path.parent_path() / (path.stem().string() + buf + path.extension().string());
                        raster::save(img, out);
                        ++written;
                    }
                }
                if (gif) {
                    const int delay = std::max(2, static_cast<int>(std::lround(100.0 * step / d.scene.fps)));
                    const std::string bytes = animate::encodeGif(frames, std::vector<int>(frames.size(), delay));
                    std::ofstream out(path, std::ios::binary);
                    if (!out) return Result::failure("can't write " + path.string());
                    out << bytes;
                    written = frames.size();
                }
                return Result::success("rendered " + std::to_string(written) + " frames to " + path.string())
                    .with("path", path.string())
                    .with("frames", static_cast<std::int64_t>(written));
            }));
}

}  // namespace wetwaresuite::model::cmd
