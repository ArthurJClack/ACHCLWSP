// app.view: look at any 3D document (a cad part, a mesh, a sculpt, a scan fit,
// a print) from any angle, the same camera the window's orbit view uses.
#include "view_commands.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#include "wetwaresuite/raster/codec.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::apps {

namespace {

struct Preset {
    const char* name;
    double yaw, pitch;
};
// matches mesh::View's fixed views
constexpr Preset kPresets[] = {{"iso", 45, 35.26}, {"front", 0, 0},  {"back", 180, 0},  {"left", -90, 0},
                               {"right", 90, 0},   {"top", 0, 89.9}, {"bottom", 0, -89.9}};

}  // namespace

bool viewPreset(const std::string& name, double& yaw, double& pitch) {
    for (const auto& p : kPresets) {
        if (name == p.name) {
            yaw = p.yaw;
            pitch = p.pitch;
            return true;
        }
    }
    return false;
}

void registerViewCommands(Session& session) {
    Session* s = &session;
    std::vector<std::string> views;
    for (const auto& p : kPresets) views.emplace_back(p.name);
    session.registry().add(
        {"app.view",
         "Look around in 3D",
         "Draw a 3D document (cad part, mesh, sculpt, scan fit, print) from any angle: yaw turns around the "
         "vertical axis, pitch tilts up or down (degrees), zoom and pan move in. view= starts from a fixed angle "
         "(iso, front, top...) and yaw/pitch are added to it. shading= draws it like Blender's viewport modes. Attaches a PNG; output= also saves it.",
         {{"doc", ParamType::String, "an open 3D document, e.g. mesh1 or cad1", true},
          {"view", ParamType::String, "start from this angle", false, views, std::string("iso")},
          {"yaw", ParamType::Number, "degrees to turn around the vertical axis (positive = the model turns left)", false, {}, 0.0},
          {"pitch", ParamType::Number, "degrees to tilt (positive = look from higher up)", false, {}, 0.0},
          {"zoom", ParamType::Number, "1 = fits, 2 = twice as big", false, {}, 1.0},
          {"pan_x", ParamType::Number, "shift right, in model radii", false, {}, 0.0},
          {"pan_y", ParamType::Number, "shift up, in model radii", false, {}, 0.0},
          {"width", ParamType::Int, "image width in px", false, {}, std::int64_t{800}},
          {"height", ParamType::Int, "image height in px", false, {}, std::int64_t{600}},
          {"shading", ParamType::String, "how surfaces are drawn: material, solid, matcap (clay), wireframe or xray (see-through)", false,
           {"material", "solid", "matcap", "wireframe", "xray"}, std::string("material")},
          {"output", ParamType::String, "also save the PNG here", false}},
         false, true},
        [s](const Params& p) {
            return std::make_unique<FunctionAction>([s, p] {
                const std::string id = stringParam(p, "doc");
                auto doc = s->workspace().get(id);
                if (!doc) return Result::failure("no open document " + id);
                OrbitView v;
                if (!viewPreset(stringParam(p, "view", "iso"), v.yaw, v.pitch)) return Result::failure("unknown view");
                v.yaw += numberParam(p, "yaw", 0.0);
                v.pitch = std::clamp(v.pitch + numberParam(p, "pitch", 0.0), -89.9, 89.9);
                v.zoom = numberParam(p, "zoom", 1.0);
                if (!(v.zoom > 0.01 && v.zoom < 1000)) return Result::failure("zoom must be between 0.01 and 1000");
                v.panX = numberParam(p, "pan_x", 0.0);
                v.panY = numberParam(p, "pan_y", 0.0);
                v.shading = stringParam(p, "shading", "material");
                const auto w = intParam(p, "width", 800), h = intParam(p, "height", 600);
                if (w < 16 || h < 16 || w > 4096 || h > 4096) return Result::failure("width and height must be 16..4096");
                std::vector<std::uint8_t> rgba;
                if (!doc->renderView(v, static_cast<int>(w), static_cast<int>(h), rgba)) {
                    return Result::failure(id + " is a " + doc->kind() + ", not a 3D document (app.view works on cad parts, meshes, sculpts, scan fits and prints)");
                }
                raster::Image img(static_cast<int>(w), static_cast<int>(h));
                std::copy(rgba.begin(), rgba.end(), img.data());
                std::string png = raster::encodePng(img);
                Result r = Result::success(id + " from yaw " + std::to_string(static_cast<int>(std::lround(v.yaw))) + ", pitch " +
                                           std::to_string(static_cast<int>(std::lround(v.pitch))) +
                                           (v.zoom != 1 ? ", zoom " + std::to_string(v.zoom).substr(0, 4) : std::string()));
                r.with("doc", id).with("yaw", v.yaw).with("pitch", v.pitch).with("zoom", v.zoom);
                if (p.count("output")) {
                    const std::string out = stringParam(p, "output");
                    std::ofstream f(out, std::ios::binary);
                    if (!f.write(png.data(), static_cast<std::streamsize>(png.size()))) return Result::failure("can't write " + out);
                    r.with("output", out);
                }
                r.attach({"image/png", std::move(png), "view.png"});
                return r;
            });
        });
}

}  // namespace wetwaresuite::apps
