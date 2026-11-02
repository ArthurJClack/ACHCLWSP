#include "apps.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>

namespace wetwaresuite::apps {

const std::vector<App>& all() {
    static const std::vector<App> table = {
        {"image", "edit photos, paint pictures and make textures", {"photo", "paint"}, 0xe0559a},
        {"draw", "vector art and logos, fonts, whiteboards and diagrams", {"vector", "type", "board"}, 0x2fb37a},
        {"model", "3D modeling and sculpting, rendered pictures, animation", {"model", "sculpt", "render", "animate"}, 0xf08c2e},
        {"game", "game scenes (Godot .tscn), assets (meshes, sprites, textures) and levels", {"scene", "forge", "levels"}, 0x5bbf3a},
        {"cad", "precise parts, technical drawings and floor plans, 3D printing, parts that fit 3D scans", {"cad", "blueprint", "slicer", "fit"}, 0x2f8fd6},
        {"electronics", "circuit diagrams, circuit boards and flashing microcontrollers", {"circuit", "flash"}, 0x14a89a},
        {"sound", "record and clean up audio, make music", {"audio", "beats"}, 0x9b6bff},
        {"video", "edit videos, take screenshots and record the screen", {"cut", "capture"}, 0xe5484d},
        {"office", "documents, spreadsheets, slides, a PDF editor, print layouts and notes", {"office", "pdf", "layout", "notes"}, 0x1f9bd1},
        {"maps", "offline maps, GPS tracks and map data", {"maps"}, 0x3aa76d},
        {"system", "calculator, code editor, file manager, terminal, system monitor and clock",
         {"calc", "code", "explorer", "terminal", "monitor", "clock"}, 0x6c7bd9},
        {"files", "open, convert, find and batch-process any file (always installed)",
         {"toolkit", "convert", "library", "automate"}, 0xc252d1, true},
        {"share", "share and live-sync folders with other devices", {"share"}, 0xd4a017},
    };
    return table;
}

const App* find(const std::string& id) {
    for (const auto& a : all())
        if (a.id == id) return &a;
    return nullptr;
}

const App* appOfTool(const std::string& tool) {
    for (const auto& a : all())
        if (std::find(a.tools.begin(), a.tools.end(), tool) != a.tools.end()) return &a;
    return nullptr;
}

std::string binaryName(const std::string& appId) { return "wetwaresuite-" + appId; }

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// extension -> app and the command that opens it there
const std::map<std::string, OpenRoute>& extensionTable() {
    static const std::map<std::string, OpenRoute> table = {
        // image
        {".png", {"image", "photo.open"}}, {".jpg", {"image", "photo.open"}}, {".jpeg", {"image", "photo.open"}},
        {".bmp", {"image", "photo.open"}}, {".tga", {"image", "photo.open"}}, {".ppm", {"image", "photo.open"}},
        {".pgm", {"image", "photo.open"}}, {".ora", {"image", "photo.open"}}, {".gif", {"image", "photo.open"}},
        {".webp", {"image", "photo.open"}}, {".psd", {"image", "photo.open"}}, {".tif", {"image", "photo.open"}}, {".tiff", {"image", "photo.open"}},
        // draw
        {".svg", {"draw", "vector.open"}}, {".ttf", {"draw", "type.open"}}, {".otf", {"draw", "type.open"}},
        {".woff", {"draw", "type.open"}}, {".ufo", {"draw", "type.open"}}, {".excalidraw", {"draw", "board.open"}},
        {".drawio", {"draw", "board.open"}}, {".mmd", {"draw", ""}}, {".board.json", {"draw", "board.open"}},
        {".ai", {"draw", "vector.open"}}, {".eps", {"draw", "vector.open"}},
        // model
        {".obj", {"model", "model.open"}}, {".ply", {"model", "sculpt.open"}}, {".off", {"model", "sculpt.open"}},
        {".glb", {"model", "sculpt.open"}}, {".gltf", {"model", "sculpt.open"}}, {".fbx", {"model", "model.open"}}, {".wwmodel", {"model", "model.open"}},
        {".scene.json", {"model", "render.open"}}, {".anim.json", {"model", "animate.open"}},
        // game
        {".tscn", {"game", "scene.open"}}, {".tmx", {"game", "levels.open"}}, {".tmj", {"game", "levels.open"}}, {".ldtk", {"game", "levels.open"}},
        {".world", {"game", "levels.world_open"}}, {".aseprite", {"game", "forge.sprite_open"}}, {".ase", {"game", "forge.sprite_open"}},
        // cad
        {".stl", {"cad", "slicer.load"}}, {".3mf", {"cad", "slicer.load"}}, {".dxf", {"cad", "blueprint.open"}},
        {".gcode", {"cad", ""}}, {".step", {"cad", "cad.open"}}, {".stp", {"cad", "cad.open"}}, {".wwcad", {"cad", "cad.open"}}, {".wwasm", {"cad", "cad.assembly_open"}}, {".wwfit", {"cad", "fit.open"}},
        {".achcad", {"cad", "cad.open"}},  // .ach*: files saved before the rename
        // electronics
        {".hex", {"electronics", ""}}, {".elf", {"electronics", ""}}, {".ino", {"electronics", ""}},
        {".gbr", {"electronics", ""}}, {".drl", {"electronics", ""}}, {".kicad_sch", {"electronics", "circuit.open"}},
        {".kicad_pcb", {"electronics", "circuit.open"}}, {".kicad_pro", {"electronics", "circuit.open"}},
        {".circuit.json", {"electronics", "circuit.open"}},
        // sound
        {".wav", {"sound", "audio.open"}}, {".mp3", {"sound", "audio.open"}}, {".flac", {"sound", "audio.open"}},
        {".ogg", {"sound", "audio.open"}}, {".opus", {"sound", "audio.open"}}, {".m4a", {"sound", "audio.open"}},
        {".aac", {"sound", "audio.open"}}, {".aiff", {"sound", "audio.open"}}, {".aif", {"sound", "audio.open"}},
        {".mid", {"sound", ""}}, {".midi", {"sound", ""}}, {".beats.json", {"sound", "beats.open"}},
        // video
        {".mp4", {"video", ""}}, {".mov", {"video", ""}}, {".mkv", {"video", ""}}, {".webm", {"video", ""}},
        {".avi", {"video", ""}}, {".srt", {"video", ""}}, {".wwcut", {"video", "cut.open"}}, {".wwscene", {"video", "capture.scene_open"}},
        {".achcut", {"video", "cut.open"}},
        // office
        {".docx", {"office", "office.doc_open"}}, {".odt", {"office", "office.doc_open"}}, {".md", {"office", "office.doc_open"}},
        {".txt", {"office", "office.doc_open"}}, {".rtf", {"office", "office.doc_open"}}, {".html", {"office", "office.doc_open"}},
        {".xlsx", {"office", "office.sheet_open"}}, {".ods", {"office", "office.sheet_open"}}, {".csv", {"office", "office.sheet_open"}},
        {".pptx", {"office", "office.deck_open"}}, {".odp", {"office", "office.deck_open"}}, {".pdf", {"office", "pdf.open"}},
        {".wwlay", {"office", "layout.open"}}, {".achlay", {"office", "layout.open"}},
        // maps
        {".geojson", {"maps", "maps.open"}}, {".kml", {"maps", "maps.open"}}, {".gpx", {"maps", "maps.open"}},
        {".shp", {"maps", "maps.open"}}, {".pmtiles", {"maps", ""}},
        {".mbtiles", {"maps", ""}}, {".wwmap", {"maps", "maps.open"}},
        {".achmap", {"maps", "maps.open"}}, {".fit", {"maps", "maps.open"}}, {".osm", {"maps", "maps.open"}},
        // system: source code and config open in the code editor; archives in the file manager
        {".c", {"system", "code.open"}}, {".h", {"system", "code.open"}}, {".cpp", {"system", "code.open"}},
        {".hpp", {"system", "code.open"}}, {".cc", {"system", "code.open"}}, {".cxx", {"system", "code.open"}},
        {".hh", {"system", "code.open"}}, {".hxx", {"system", "code.open"}}, {".m", {"system", "code.open"}},
        {".mm", {"system", "code.open"}}, {".rs", {"system", "code.open"}}, {".go", {"system", "code.open"}},
        {".zig", {"system", "code.open"}}, {".py", {"system", "code.open"}}, {".pyw", {"system", "code.open"}},
        {".js", {"system", "code.open"}}, {".mjs", {"system", "code.open"}}, {".cjs", {"system", "code.open"}},
        {".ts", {"system", "code.open"}}, {".tsx", {"system", "code.open"}}, {".jsx", {"system", "code.open"}},
        {".vue", {"system", "code.open"}}, {".java", {"system", "code.open"}}, {".kt", {"system", "code.open"}},
        {".kts", {"system", "code.open"}}, {".scala", {"system", "code.open"}}, {".cs", {"system", "code.open"}},
        {".fs", {"system", "code.open"}}, {".swift", {"system", "code.open"}}, {".dart", {"system", "code.open"}},
        {".lua", {"system", "code.open"}}, {".rb", {"system", "code.open"}}, {".php", {"system", "code.open"}},
        {".pl", {"system", "code.open"}}, {".r", {"system", "code.open"}}, {".jl", {"system", "code.open"}},
        {".ex", {"system", "code.open"}}, {".exs", {"system", "code.open"}}, {".hs", {"system", "code.open"}},
        {".sh", {"system", "code.open"}}, {".bash", {"system", "code.open"}}, {".zsh", {"system", "code.open"}},
        {".fish", {"system", "code.open"}}, {".ps1", {"system", "code.open"}}, {".bat", {"system", "code.open"}},
        {".cmd", {"system", "code.open"}}, {".css", {"system", "code.open"}}, {".scss", {"system", "code.open"}},
        {".less", {"system", "code.open"}}, {".json", {"system", "code.open"}}, {".jsonc", {"system", "code.open"}},
        {".yaml", {"system", "code.open"}}, {".yml", {"system", "code.open"}}, {".toml", {"system", "code.open"}},
        {".ini", {"system", "code.open"}}, {".cfg", {"system", "code.open"}}, {".conf", {"system", "code.open"}},
        {".xml", {"system", "code.open"}}, {".sql", {"system", "code.open"}}, {".cmake", {"system", "code.open"}},
        {".gradle", {"system", "code.open"}}, {".mk", {"system", "code.open"}}, {".log", {"system", "code.open"}},
        {".diff", {"system", "code.open"}}, {".patch", {"system", "code.open"}},
        {".zip", {"system", "explorer.archive_list"}}, {".tar", {"system", "explorer.archive_list"}},
        {".tgz", {"system", "explorer.archive_list"}}, {".tar.gz", {"system", "explorer.archive_list"}}, {".gz", {"system", "explorer.archive_list"}},
    };
    return table;
}

// A GeoTIFF is a TIFF with map tags (pixel scale, tie points or GeoKeys): those are elevation or
// satellite tiles for maps; every other TIFF is a picture.
bool isGeoTiff(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    unsigned char head[8];
    if (!in.read(reinterpret_cast<char*>(head), 8)) return false;
    const bool le = head[0] == 'I' && head[1] == 'I';
    if (!le && !(head[0] == 'M' && head[1] == 'M')) return false;
    auto u16 = [&](const unsigned char* b) { return le ? b[0] | b[1] << 8 : b[0] << 8 | b[1]; };
    auto u32 = [&](const unsigned char* b) {
        return le ? std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 | std::uint32_t(b[3]) << 24
                  : std::uint32_t(b[0]) << 24 | std::uint32_t(b[1]) << 16 | std::uint32_t(b[2]) << 8 | std::uint32_t(b[3]);
    };
    if (u16(head + 2) != 42) return false;
    in.seekg(u32(head + 4));
    unsigned char count[2];
    if (!in.read(reinterpret_cast<char*>(count), 2)) return false;
    for (int i = 0, n = std::min(u16(count), 512); i < n; ++i) {
        unsigned char entry[12];
        if (!in.read(reinterpret_cast<char*>(entry), 12)) return false;
        const int tag = u16(entry);
        if (tag == 33550 || tag == 33922 || tag == 34264 || tag == 34735) return true;
    }
    return false;
}

}  // namespace

OpenRoute routeForFile(const std::string& path) {
    std::string name = lower(std::filesystem::path(path).filename().string());
    const std::string ext = lower(std::filesystem::path(path).extension().string());
    if ((ext == ".tif" || ext == ".tiff") && isGeoTiff(path)) return {"maps", ""};
    // try the longest dotted suffix first: "song.beats.json" -> ".beats.json", then ".json"
    for (std::size_t dot = name.find('.'); dot != std::string::npos; dot = name.find('.', dot + 1)) {
        auto it = extensionTable().find(name.substr(dot));
        if (it != extensionTable().end()) return it->second;
    }
    return {"files", "toolkit.inspect"};
}

std::vector<std::pair<std::string, OpenRoute>> fileTypes() { return {extensionTable().begin(), extensionTable().end()}; }

const std::vector<SendRoute>& sendRoutes() {
    static const std::vector<SendRoute> table = {
        // pictures out of image
        {"image", "model", "photo.save", "path", ".png", "texture", "",
         "use it as a texture: render.material doc=<scene> name=<material> texture=FILE, or emboss it: sculpt.displace doc=<sculpt> image=FILE"},
        {"image", "game", "photo.save", "path", ".png", "texture", "",
         "make it tile: forge.seamless input=FILE output=<tile.png>; bumps: forge.normal_map input=FILE output=<normal.png>"},
        {"image", "video", "photo.save", "path", ".png", "still", "", "add it to a timeline: cut.add_image doc=<cut> path=FILE"},
        {"image", "office", "photo.save", "path", ".png", "picture", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"image", "draw", "photo.save", "path", ".png", "picture", "", "use it as a reference (draw files are vector; the picture is at FILE)"},
        // draw
        {"vector", "office", "vector.save", "path", ".svg", "artwork", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"vector", "image", "vector.save", "path", ".png", "picture", "photo.open", "opened as a picture to paint on"},
        {"vector", "video", "vector.save", "path", ".png", "still", "", "add it to a timeline: cut.add_image doc=<cut> path=FILE"},
        {"board", "office", "board.export", "path", ".svg", "diagram", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"board", "image", "board.export", "path", ".png", "picture", "photo.open", "opened as a picture"},
        {"font", "office", "type.export", "path", ".ttf", "font", "", "use it in a layout: layout.font_add doc=<layout> path=FILE"},
        // model
        {"model", "game", "model.export", "path", ".glb", "mesh", "forge.mesh_open", "opened as a game mesh: add LODs, fix scale, save"},
        {"model", "cad", "model.export", "path", ".stl", "mesh", "slicer.load", "loaded on the printer bed: slicer.export_gcode doc=<print> path=<part.gcode>; or put it in a part: cad.import doc=<cad> path=FILE"},
        {"model", "image", "model.render", "path", ".png", "picture", "photo.open", "the rendered picture"},
        {"sculpt", "game", "sculpt.export", "path", ".glb", "mesh", "forge.mesh_open", "opened as a game mesh: add LODs, fix scale, save"},
        {"sculpt", "cad", "sculpt.export", "path", ".stl", "mesh", "slicer.load", "loaded on the printer bed: slicer.export_gcode doc=<print> path=<part.gcode>; or put it in a part: cad.import doc=<cad> path=FILE"},
        {"sculpt", "image", "sculpt.export", "path", ".png", "picture", "photo.open", "a picture of the model"},
        {"scene", "image", "render.render", "output", ".png", "picture", "photo.open", "the rendered picture"},
        {"scene", "video", "render.render", "output", ".png", "still", "", "add it to a timeline: cut.add_image doc=<cut> path=FILE"},
        {"scene", "office", "render.render", "output", ".png", "picture", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"animation", "game", "animate.export_sheet", "output", ".png", "sprite sheet", "",
         "a sprite sheet; cut it into frames with forge.sprite_split input=FILE output_dir=<folder>"},
        {"animation", "video", "animate.export_gif", "output", ".gif", "clip", "", "add it to a timeline: cut.add_clip doc=<cut> path=FILE"},
        // game
        {"mesh", "model", "forge.mesh_save", "path", ".glb", "mesh", "sculpt.open", "opened for sculpting; or render it with render.model path=FILE"},
        {"mesh", "cad", "forge.mesh_save", "path", ".stl", "mesh", "slicer.load", "loaded on the printer bed: slicer.export_gcode doc=<print> path=<part.gcode>; or put it in a part: cad.import doc=<cad> path=FILE"},
        {"mesh", "image", "forge.mesh_save", "path", ".png", "picture", "photo.open", "a picture of the mesh"},
        {"level", "image", "levels.save", "path", ".png", "picture", "photo.open", "a picture of the level"},
        // cad
        {"cad", "model", "cad.save", "path", ".stl", "mesh", "sculpt.open", "opened for sculpting; or render it with render.model path=FILE"},
        {"cad", "game", "cad.save", "path", ".glb", "mesh", "forge.mesh_open", "opened as a game mesh"},
        {"cad", "image", "cad.save", "path", ".png", "picture", "photo.open", "a picture of the part"},
        {"blueprint", "draw", "blueprint.export", "path", ".svg", "drawing", "vector.open", "opened as vector art"},
        {"fit", "model", "fit.export", "path", ".stl", "mesh", "sculpt.open", "the fitted parts, opened for sculpting; or render them with render.model path=FILE"},
        {"fit", "game", "fit.export", "path", ".glb", "mesh", "forge.mesh_open", "the fitted parts as a game mesh"},
        {"fit", "image", "fit.export", "path", ".png", "picture", "photo.open", "a picture of the scan and its parts"},
        {"blueprint", "office", "blueprint.export", "path", ".svg", "drawing", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        // electronics
        {"circuit", "office", "circuit.export", "path", ".svg", "schematic", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
        {"circuit", "draw", "circuit.export", "path", ".svg", "schematic", "vector.open", "opened as vector art"},
        // sound
        {"audio", "video", "audio.save", "path", ".wav", "audio", "", "add it to a timeline: cut.add_audio doc=<cut> path=FILE"},
        {"audio project", "video", "audio.export", "path", ".wav", "audio", "", "add it to a timeline: cut.add_audio doc=<cut> path=FILE", "project"},
        {"song", "video", "beats.render", "path", ".wav", "music", "", "add it to a timeline: cut.add_audio doc=<cut> path=FILE", "song"},
        {"song", "game", "beats.render", "path", ".wav", "music", "", "game music: ship FILE with your level", "song"},
        // office and maps
        {"layout", "office", "layout.export", "path", ".pdf", "document", "", "the print-ready PDF (open it with pdf.open to mark it up)"},
        {"map", "image", "maps.render", "path", ".png", "picture", "photo.open", "a picture of the map"},
        {"map", "office", "maps.render", "path", ".svg", "map", "", "place it on a page: layout.place_image doc=<layout> path=FILE"},
    };
    return table;
}

std::optional<SendRoute> sendRoute(const std::string& docKind, const std::string& to) {
    for (const auto& r : sendRoutes())
        if (r.docKind == docKind && r.to == to) return r;
    return std::nullopt;
}

std::vector<std::string> sendTargets(const std::string& docKind) {
    std::vector<std::string> out;
    for (const auto& r : sendRoutes())
        if (r.docKind == docKind) out.push_back(r.to);
    return out;
}

std::string toolGuide(const std::string& tool) {
    static const std::map<std::string, std::string> guides = {
        {"photo", "photo_* edits pictures: photo_open or photo_new returns a doc id like img1; pass it as `doc`; photo_preview (or preview=true) shows the result; photo_save writes it. Coordinates are pixels from the top-left; colours accept #rrggbb, rgb(r,g,b) or names."},
        {"paint", "paint_* paints on image documents (paint_new returns img1): strokes take points as a flat [x1,y1,x2,y2,...] list with pressure brushes, plus fills, shapes, gradients and layers; save with photo_save."},
        {"vector", "vector_* draws shapes, paths (vector_pen, vector_nodes), text, layers, symbols and effects (vector_new returns vec1); vector_save writes SVG, PDF, EPS or PNG."},
        {"type", "type_* designs fonts and exports TTF/WOFF (type_new returns font1; type_proof shows a specimen)."},
        {"board", "board_* makes whiteboards and diagrams (board_from_mermaid turns Mermaid text into a laid-out diagram; board_export writes PNG/SVG/PDF)."},
        {"model", "model_* is polygon modeling like Blender, z up: model_new returns model1 (a cube, light and camera); model_add adds primitives; model_select (by index, box, rect on the view, normal), model_select_loop and model_pick choose vertices/edges/faces of the active mesh; model_extrude, model_inset, model_bevel, model_loop_cut, model_bisect, model_knife, model_subdivide, model_merge, model_dissolve, model_bridge, model_spin, model_mirror edit it (model_elements lists indices); model_modifier_add stacks mirror/array/subsurf/solidify/bevel/boolean/...; model_uv_unwrap, model_material; model_render path-traces; model_export writes OBJ/glTF/FBX/STL; preview=true on any edit shows the viewport."},
        {"sculpt", "sculpt_* sculpts meshes in mm with z up using brush strokes along point paths (sculpt_new returns sculpt1; strokes accept space=screen points on a preview; sculpt_export writes STL/OBJ/glTF)."},
        {"render", "render_* builds 3D scenes and path-traces them (render_new returns scene1; render_model renders a model file in one go; render_render without output returns the image)."},
        {"animate", "animate_* keyframes 2D layers, cels, bone rigs and 3D meshes and exports GIF, sprite sheets or video."},
        {"forge", "forge_* prepares game assets: meshes (forge_mesh_open, LODs, scale, up axis), sprite sheets and atlases, tileable textures and normal maps."},
        {"scene", "scene_* builds Godot 4 scenes (.tscn) like Godot's editor: scene_new returns scn1; scene_add adds nodes (scene_classes lists them), scene_set/scene_inspect edit properties, scene_transform moves/turns/scales, scene_instance and scene_add_level bring in scenes and tilemaps, scene_script attaches GDScript, scene_project writes project.godot; scene_preview shows the viewport and scene_hit turns a 2D point into a node."},
        {"levels", "levels_* edits tilemaps and generates caves and dungeons from a seed (Tiled TMX/TMJ, LDtk)."},
        {"cad", "cad_* builds parametric parts in mm with z up (cad_new returns cad1; numeric fields take expressions over cad_param_set parameters; cad_save exports STL/3MF)."},
        {"blueprint", "blueprint_* makes technical drawings and floor plans in real units (mm, m, ft) with y up, drawn to a scale such as 1:50 (blueprint_new returns bp1; blueprint_export writes PDF/SVG/PNG to scale)."},
        {"slicer", "slicer_* turns a model into G-code for a 3D printer (slicer_slice_file is the one-shot)."},
        {"fit", "fit_* designs car parts and mounts that fit a phone 3D scan, in mm with z up (fit_open returns fit1 and fixes metres and Y-up; clean with fit_remove_floor, fit_keep_largest, fit_fill_holes; fit_plane/fit_cylinder/fit_circle/fit_curve fit clean references to noisy patches; fit_plate, fit_block, fit_clamp, fit_phone_mount, fit_gauge_pod... build parts with a gap; fit_pick turns a u,v spot on a preview into a point; fit_check shows the gap; fit_export writes STL/3MF)."},
        {"circuit", "circuit_* draws schematics in mm and lays out PCBs (circuit_new returns ckt1; circuit_erc and circuit_drc check them; circuit_gerber writes fab files)."},
        {"flash", "flash_* finds boards on serial ports and flashes Arduino, ESP32 and STM32 firmware (flash_ports first; sim: boards let you try it without hardware)."},
        {"audio", "audio_* edits sound in seconds (audio_open returns aud1; audio_preview shows a waveform or spectrogram; audio_measure gives LUFS)."},
        {"beats", "beats_* makes music from step patterns, notes and chords (beats_generate makes a starter song; beats_render writes audio)."},
        {"cut", "cut_* edits video on a timeline (cut_new returns cut1; cut_preview shows a frame; cut_render exports; needs ffmpeg for video files)."},
        {"capture", "capture_screenshot without output returns an image of the screen; capture_* also records the screen."},
        {"office", "office_* handles spreadsheets with formulas, documents, slide decks and PDF tools."},
        {"pdf", "pdf_* edits any PDF like Acrobat: pdf_open returns pdf1; pdf_view shows a page (coordinates are points from its top-left); pdf_add_text, pdf_replace_text (edit the page's own text), pdf_highlight, pdf_note, pdf_draw, pdf_shape, pdf_redact, pdf_image, pdf_sign; pdf_fields and pdf_fill fill forms; pdf_page_* turns, moves, inserts and deletes pages; pdf_save writes the file."},
        {"layout", "layout_* builds print layouts (posters, flyers, booklets) exported to PDF."},
        {"notes", "notes_* works on an Obsidian-style folder of Markdown notes (notes_search, notes_backlinks, notes_tasks)."},
        {"maps", "maps_* handles GeoJSON/KML/GPX/FIT/OSM/Shapefile data in lon/lat and renders maps (maps_new returns map1): edit features and vertices, overlay and dissolve, track splits and cleaning, offline routing (maps_route on an .osm roads layer), place search, bookmarks; maps_hit turns a preview pixel into lat,lon."},
        {"calc", "calc_eval works out any expression: exact fractions and big whole numbers, units (\"5 km to miles\", \"3 ft + 20 cm\"), hex/bin/oct and bitwise maths, functions you define with calc_define, and ans/mem; calc_solve, calc_stats, calc_regression, calc_integrate and calc_graph (a PNG) do the rest."},
        {"code", "code_* edits source files (code_open returns code1; code_edit replaces an exact piece of text, code_read shows numbered lines; code_save keeps line ends), searches folders (code_search, code_replace_in_files), runs files and builds projects with the installed compilers (code_run, code_build; errors come back as file/line rows), and drives git (code_git_status, code_git_commit)."},
        {"explorer", "explorer_* manages files: list, search, preview, copy, move, rename and delete (to the suite's trash; every change is undoable), zip/tar archives (explorer_archive, explorer_extract), checksums, folder compare and duplicates. Several paths go in one paths= separated by | or new lines."},
        {"terminal", "terminal_open returns term1, a shell that keeps its folder and exported variables; terminal_run runs a command line and returns output and exit code; terminal_send types into a running program; terminal_exec runs one command without keeping a terminal."},
        {"monitor", "monitor_* reads processor (per core), memory, disks, processes (monitor_processes sort=cpu|memory), network and battery; monitor_kill ends a program; monitor_watch samples for some seconds and returns a chart."},
        {"clock", "clock_* gives the time anywhere (clock_now place=Tokyo, clock_world, clock_convert with summer time), timers (clock_timer_start duration=5m), a stopwatch, saved alarms, and date maths (clock_date_diff, clock_date_add \"10 working days\", clock_calendar). clock_due reports finished timers and alarms."},
        {"toolkit", "toolkit_inspect identifies any file and says which app opens it."},
        {"convert", "convert_file changes file formats; convert_formats shows what's available."},
        {"library", "library_* indexes and searches asset folders (by type, size, tags, duplicates)."},
        {"automate", "automate_* runs batch scripts, pipelines over folders and folder watches."},
    };
    auto it = guides.find(tool);
    return it == guides.end() ? std::string() : it->second;
}

std::string appInstructions(const std::string& appId) {
    const App* app = find(appId);
    if (!app) return {};
    std::string text = binaryName(appId) + " is the wetwaresuite " + appId + " app: " + app->summary +
                       ". Every edit is undoable with undo/redo, `documents` lists what's open, and opened or new "
                       "documents get short ids you pass as `doc`.";
    for (const auto& t : app->tools) {
        std::string g = toolGuide(t);
        if (!g.empty()) text += " " + g;
    }
    return text;
}

}  // namespace wetwaresuite::apps
