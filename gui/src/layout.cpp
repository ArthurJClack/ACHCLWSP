// The window's layout per app: which bars it shows, what goes in its menus and
// ribbon, and which command saves or closes a document. Window-free, so it is
// unit tested. Each app's ribbon follows the tool people know it from (Word,
// Onshape, Blender, Premiere, KiCad...), in plain words.
#include <algorithm>
#include <cctype>
#include <map>

#include "wetwaresuite/gui/logic.hpp"

namespace wetwaresuite::gui {

namespace {

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
bool endsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }

std::string suffix(const std::string& id) {
    auto dot = id.find('.');
    return dot == std::string::npos ? id : id.substr(dot + 1);
}

std::string prefix(const std::string& id) { return id.substr(0, id.find('.')); }

// ---- curated ribbons -------------------------------------------------------------
// Ids are relative to the tab's tool; ones a build doesn't carry are skipped.

struct GroupSpec {
    const char* title;
    std::vector<const char*> ids;
};

struct TabSpec {
    const char* title;
    const char* tool;
    const char* stem;  // documents opened by <tool>.<stem>_open / _new pick this tab; "" = the tool's own
    std::vector<GroupSpec> groups;
};

const std::map<std::string, std::vector<TabSpec>>& curated() {
    static const std::map<std::string, std::vector<TabSpec>> table = {
        {"image",
         {{"Photo", "photo", "",
           {{"Image", {"resize:Resize", "crop:Crop", "crop_selection:Crop to selection", "crop_perspective:Perspective crop", "canvas_size:Canvas size", "rotate:Rotate", "flip:Flip"}},
            {"Adjust", {"auto:Auto", "adjust", "levels", "curves", "color_balance", "apply_adjustment:Shadows, clarity, dehaze...", "adjustment_layer"}},
            {"Select", {"select", "select_subject", "select_modify", "refine_edge", "fill_selection", "delete_selected"}},
            {"Filters", {"blur", "sharpen", "noise", "effect", "stylize", "liquify", "lens_correction"}},
            {"Fix", {"heal", "remove_object", "clone_seamless"}}}},
          {"Paint", "paint", "",
           {{"Draw", {"stroke", "fill", "gradient", "shape", "text", "polygon"}},
            {"Layers", {"layer_add:New layer", "layer_duplicate:Duplicate", "layer_merge_down:Merge down", "merge_visible:Merge visible", "flatten:Flatten", "layer_style:Layer style"}}}}}},
        {"draw",
         {{"Vector", "vector", "",
           {{"Shapes", {"add_rect:Rectangle", "add_ellipse:Ellipse", "add_polygon:Polygon", "add_line:Line", "pen", "add_text:Text"}},
            {"Style", {"style", "gradient", "stroke_style", "text_style", "effect"}},
            {"Arrange", {"align", "distribute", "reorder", "group", "ungroup", "transform"}},
            {"Combine", {"boolean", "shape_builder", "outline_stroke", "to_path", "clip", "trace"}}}},
          {"Fonts", "type", "", {}},
          {"Whiteboard", "board", "", {}}}},
        {"model",
         {{"Model", "model", "",
           {{"Add", {"add", "duplicate_object", "join", "separate", "delete_object"}},
            {"Edit mesh", {"extrude", "inset", "bevel", "loop_cut", "knife", "subdivide", "merge", "bridge"}},
            {"Object", {"transform", "mirror", "origin", "apply_transform", "shade", "modifier_add"}},
            {"Look", {"material", "material_assign", "uv_unwrap", "render"}}}},
          {"Sculpt", "sculpt", "",
           {{"Brush", {"stroke", "smooth", "mask", "symmetrize"}}, {"Mesh", {"remesh", "subdivide", "decimate", "retopo", "displace"}}}},
          {"Render", "render", "",
           {{"Scene", {"model", "add", "light", "camera", "sky", "material"}}, {"Output", {"settings", "render", "turntable"}}}},
          {"Animate", "animate", "",
           {{"Keys", {"key_set", "key_list", "key_delete"}},
            {"Layers", {"layer_add", "element_add", "cel", "draw"}},
            {"Export", {"export_gif", "export_sheet", "export_video"}}}}}},
        {"game",
         {{"Scene", "scene", "",
           {{"Nodes", {"add:Add node", "instance:Instance scene", "add_level:Add level", "duplicate", "remove:Delete"}},
            {"Scene", {"script:Attach script", "connect:Connect signal", "project:Godot project", "save"}},
            {"Test", {"play:Play test (GIF)"}}}},
          {"Sprite", "forge", "sprite",
           {{"Frames", {"frame_add:New frame", "frame_remove:Delete frame", "frame_duration:Duration", "tag_add:Tag"}},
            {"Sprite", {"sprite_resize", "sprite_layer_add", "sprite_palette", "sprite_sheet", "sprite_export"}}}},
          {"Mesh", "forge", "mesh",
           {{"Mesh", {"mesh_lods", "mesh_decimate", "mesh_transform", "mesh_normals", "mesh_uv", "mesh_weld"}}}},
          {"Textures", "forge", "-",
           {{"Textures", {"seamless", "normal_map", "channel_pack", "upscale", "resize_pot", "trim"}}}},
          {"Level", "levels", "",
           {{"Tiles", {"paint", "fill_rect", "flood_fill", "erase", "autotile", "stamp"}},
            {"Generate", {"gen_caves", "gen_rooms", "gen_noise", "scatter"}},
            {"Layers", {"layer_add", "tileset_add", "object_add", "resize"}}}},
          {"World", "levels", "world", {{"World", {"world_add:Add level", "world_remove:Remove level", "world_preview:Preview"}}}}}},
        {"cad",
         {{"Part", "cad", "",
           {{"Sketch", {"sketch:New sketch", "sketch_rect", "sketch_circle", "sketch_line", "sketch_arc", "sketch_slot", "sketch_dimension:Dimension", "sketch_trim:Trim", "sketch_extend:Extend", "sketch_offset:Offset", "sketch_mirror:Mirror"}},
            {"Features", {"sketch_extrude:Extrude", "sketch_revolve:Revolve", "sweep", "loft", "hole", "thread"}},
            {"Modify", {"fillet", "chamfer", "shell", "draft", "mirror", "pattern_linear:Pattern", "pattern_circular:Circular pattern", "boolean"}},
            {"Shapes", {"box", "cylinder", "sphere", "cone", "torus"}},
            {"Inspect", {"measure_distance:Distance", "measure_mass:Mass", "measure_section:Section", "param_set:Parameter", "drawing:Make drawing"}}}},
          {"Assembly", "cad", "assembly",
           {{"Parts", {"assembly_insert:Insert", "assembly_move:Move", "assembly_remove:Remove", "assembly_update:Update"}},
            {"Mates", {"mate:Add mate", "mate_set:Edit", "mate_delete:Delete"}},
            {"Check", {"assembly_interference:Interference", "assembly_explode:Explode", "assembly_bom:Bill of materials"}}}},
          {"Drawing", "blueprint", "",
           {{"Draw", {"line", "polyline", "rect", "circle", "arc", "text"}},
            {"Modify", {"move", "copy", "rotate", "mirror", "offset", "scale"}},
            {"Annotate", {"dimension", "hatch", "titleblock", "page"}},
            {"Floor plan", {"wall", "door", "window", "room"}}}},
          {"3D print", "slicer", "",
           {{"Plate", {"add", "arrange", "lay_flat", "rotate", "scale", "move"}},
            {"Print", {"settings", "slice", "preview_layer", "export_gcode"}}}},
          {"Scan fit", "fit", "",
           {{"Clean", {"remove_floor", "keep_largest", "fill_holes", "smooth", "crop", "align"}},
            {"Reference", {"plane", "cylinder", "circle", "curve", "point"}},
            {"Parts", {"plate", "block", "clamp", "mount", "phone_mount", "gauge_pod", "hole"}},
            {"Check", {"check", "printability", "section", "export"}}}}}},
        {"electronics",
         {{"Schematic", "circuit", "",
           {{"Draw", {"place", "wire", "label", "power", "no_connect", "connect"}},
            {"Check", {"annotate", "erc", "sim_op", "sim_tran", "bom"}}}},
          {"Board", "circuit", "-",
           {{"Layout", {"board", "pcb_place", "route", "pour", "via", "fanout"}},
            {"Check", {"drc", "rules", "ratsnest"}},
            {"Make", {"gerber", "placement", "export"}}}},
          {"Flash", "flash", "",
           {{"Board", {"ports", "detect", "boards"}}, {"Firmware", {"compile", "flash", "monitor", "sketch"}}}}}},
        {"sound",
         {{"Audio", "audio", "",
           {{"Edit", {"cut", "copy", "paste", "trim", "insert_silence", "reverse"}},
            {"Fix", {"normalize", "denoise", "declick", "dehum", "remove_dc", "fade"}},
            {"Effects", {"eq", "compress", "reverb", "echo", "pitch", "speed", "effect"}},
            {"Analyze", {"measure", "loudness", "detect_silence", "markers"}}}},
          {"Project", "audio", "project",
           {{"Tracks", {"track_add", "track_set", "clip_add", "clip_split", "clip_trim", "crossfade"}},
            {"Mix", {"fx_add", "automate", "mixdown", "export", "export_stems"}}}},
          {"Beats", "beats", "",
           {{"Song", {"generate", "track_add", "pattern_add", "arrange_place", "set"}},
            {"Notes", {"note_add", "note_quantize", "note_transpose", "pattern_steps"}},
            {"Mix", {"fx_add", "automate", "render", "stems", "midi_export"}}}}}},
        {"video",
         {{"Edit", "cut", "",
           {{"Add", {"add_clip", "add_audio", "add_image", "add_title", "add_caption", "add_color"}},
            {"Cut", {"razor", "split_clip", "trim_clip", "ripple_trim", "delete_clip", "delete_gap"}},
            {"Look", {"transition", "filter", "effect", "transform", "chroma_key", "speed"}},
            {"Export", {"render", "export_frame", "gif", "export_captions"}}}},
          {"Capture", "capture", "",
           {{"Grab", {"screenshot", "record", "webcam"}}, {"Scene", {"scene_new", "add_source", "record_scene"}}}}}},
        {"office",
         {{"Document", "office", "doc",
           {{"Write", {"doc_add:Add", "doc_edit:Edit", "doc_delete:Delete", "doc_move:Move", "doc_find_replace:Find and replace"}},
            {"Format", {"doc_format:Format", "doc_styles:Styles", "doc_style_define:New style", "doc_setup:Page setup", "doc_header_footer:Header and footer"}},
            {"Insert", {"doc_table_insert:Table", "doc_footnote:Footnote", "doc_comment:Comment", "doc_toc:Contents"}},
            {"Review", {"doc_track:Track changes", "doc_changes:Changes", "doc_comments:Comments", "doc_word_count:Word count"}},
            {"Share", {"doc_export:Export"}}}},
          {"Sheet", "office", "sheet",
           {{"Cells", {"sheet_set", "sheet_fill", "sheet_clear", "sheet_format", "sheet_style", "sheet_merge"}},
            {"Data", {"sheet_sort:Sort", "sheet_filter:Filter", "sheet_autofilter:AutoFilter", "sheet_validate:Validation", "sheet_find:Find", "sheet_replace:Replace", "sheet_pivot:Pivot table"}},
            {"Charts", {"sheet_chart:Chart", "sheet_cond_format:Conditional format"}},
            {"Sheets", {"sheet_add:Add sheet", "sheet_rename:Rename", "sheet_freeze:Freeze panes", "sheet_calc:Recalculate"}}}},
          {"Slides", "office", "deck",
           {{"Slides", {"deck_add_slide:New slide", "deck_duplicate_slide:Duplicate", "deck_delete_slide:Delete", "deck_move_slide:Move"}},
            {"Insert", {"deck_text_box:Text box", "deck_image:Picture", "deck_shape:Shape", "deck_line:Line"}},
            {"Design", {"deck_theme:Theme", "deck_master:Master", "deck_transition:Transition"}},
            {"Show", {"deck_present:Present", "deck_notes:Notes", "deck_export:Export"}}}},
          {"PDF", "pdf", "",
           {{"Edit", {"add_text", "replace_text", "image", "shape", "draw", "whiteout"}},
            {"Review", {"highlight", "note", "redact", "sign"}},
            {"Pages", {"page_insert:Insert", "page_delete:Delete", "page_move:Move", "page_rotate:Turn"}},
            {"Forms", {"fields", "fill"}},
            {"Tools", {"ocr:Read scanned text", "protect:Password", "sign_digital:Sign digitally", "signatures:Check signatures"}}}},
          {"Layout", "layout", "", {}},
          {"Notes", "notes", "", {}}}},
        {"maps",
         {{"Map", "maps", "",
           {{"Layers", {"import", "add_countries", "raster_add", "style", "layer_move"}},
            {"Find", {"search", "route", "reach", "measure", "bookmark"}},
            {"Analyze", {"buffer", "clip", "dissolve", "overlay", "heatmap", "filter"}},
            {"Tracks", {"track_stats", "track_smooth", "track_trim", "track_split", "profile"}},
            {"Export", {"render", "export"}}}}}},
        {"files",
         {{"Toolkit", "toolkit", "", {}},
          {"Convert", "convert", "", {}},
          {"Library", "library", "", {}},
          {"Automate", "automate", "", {}}}},
    };
    return table;
}

std::string titleCase(std::string s) {
    for (auto& c : s) {
        if (c == '_') c = ' ';
    }
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string lowerWord(std::string w) {
    for (auto& c : w) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return w;
}

}  // namespace

std::string shortLabel(const std::string& title, const std::string& group, const std::string& tab) {
    // "Set Cells" in the Cells group is "Set"; "Resize image" on the Image group is "Resize"
    auto same = [](const std::string& a, const std::string& b) { return a == b || a + "s" == b || a == b + "s"; };
    std::vector<std::string> words, kept;
    std::string w;
    for (char c : title + " ") {
        if (c == ' ') {
            if (!w.empty()) words.push_back(w);
            w.clear();
        } else {
            w += c;
        }
    }
    for (const auto& word : words) {
        const std::string l = lowerWord(word);
        if (same(l, lowerWord(group)) || same(l, lowerWord(tab))) continue;
        kept.push_back(word);
    }
    if (kept.empty() || kept.size() == words.size()) return title;
    std::string out;
    for (const auto& k : kept) out += (out.empty() ? "" : " ") + k;
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

AppLayout appLayout(const std::string& appId) {
    AppLayout l;
    // Photoshop, Illustrator, Aseprite and QGIS keep the top bar slim: the tool rail and
    // the tool's options do most of the work, so their ribbon is one compact row
    if (appId == "image" || appId == "draw" || appId == "game" || appId == "maps" || appId == "sound" || appId == "video")
        l.compactRibbon = true;
    // the system tools fill the window with their own panels: menus are enough
    if (appId == "system" || appId == "launcher") l.ribbon = false;
    return l;
}

bool fileCommand(const std::string& id) {
    const std::string s = suffix(id);
    for (const char* verb : {"open", "new", "save", "close", "save_as"}) {
        if (s == verb || endsWith(s, std::string("_") + verb)) return true;
    }
    return s == "export" || startsWith(s, "export_") || endsWith(s, "_export");
}

std::string groupTitle(const std::string& key) {
    static const std::map<std::string, std::string> names = {
        {"doc", "Document"}, {"deck", "Slides"}, {"param", "Parameters"}, {"fx", "Effects"}, {"pdf", "PDF"},
        {"sim", "Simulation"}, {"uv", "UV"}, {"pcb", "Board"}, {"gen", "Generate"}, {"cond", "Conditional"},
        {"dimstyle", "Dimension style"}, {"midi", "MIDI"}, {"git", "Git"}, {"key", "Keys"}, {"sel", "Selection"},
    };
    auto it = names.find(key);
    return it != names.end() ? it->second : titleCase(key);
}

std::vector<MenuGroup> menuGroups(const std::string& tool, const std::vector<CommandInfo>& commands) {
    std::vector<const CommandInfo*> mine;
    for (const auto* c : toolCommands(tool, commands)) {
        if (!fileCommand(c->id)) mine.push_back(c);
    }
    // the word before the first "_" groups commands: layer_add, layer_set... go in "Layer"
    std::map<std::string, std::vector<const CommandInfo*>> byKey;
    for (const auto* c : mine) {
        const std::string s = suffix(c->id);
        const auto us = s.find('_');
        if (us != std::string::npos) byKey[s.substr(0, us)].push_back(c);
    }
    auto keyOf = [&](const CommandInfo* c) -> std::string {
        const std::string s = suffix(c->id);
        const auto us = s.find('_');
        if (us != std::string::npos) return s.substr(0, us);
        // a bare word that names a group ("layers" beside "layer_add") joins it
        if (byKey.count(s)) return s;
        if (s.size() > 1 && s.back() == 's' && byKey.count(s.substr(0, s.size() - 1))) return s.substr(0, s.size() - 1);
        return {};
    };
    std::map<std::string, std::vector<const CommandInfo*>> groups;
    MenuGroup flat;
    for (const auto* c : mine) {
        const std::string k = keyOf(c);
        if (!k.empty()) groups[k].push_back(c);
    }
    std::vector<MenuGroup> out;
    for (const auto* c : mine) {
        const std::string k = keyOf(c);
        if (k.empty() || groups[k].size() < 3) flat.commands.push_back(c);
    }
    for (auto& [k, list] : groups) {
        if (list.size() >= 3) out.push_back({groupTitle(k), std::move(list)});
    }
    std::sort(out.begin(), out.end(), [](const MenuGroup& a, const MenuGroup& b) { return a.title < b.title; });
    if (!flat.commands.empty()) out.insert(out.begin(), std::move(flat));
    return out;
}

std::vector<RibbonTab> ribbonTabs(const std::string& appId, const std::vector<std::string>& tools, const std::vector<CommandInfo>& commands) {
    auto find = [&](const std::string& id) -> const CommandInfo* {
        for (const auto& c : commands) {
            if (c.id == id) return &c;
        }
        return nullptr;
    };
    std::vector<RibbonTab> out;
    std::vector<std::string> covered;
    auto it = curated().find(appId);
    if (it != curated().end()) {
        for (const auto& spec : it->second) {
            if (std::find(tools.begin(), tools.end(), spec.tool) == tools.end()) continue;
            RibbonTab tab{spec.title, spec.tool, spec.stem, {}};
            for (const auto& g : spec.groups) {
                RibbonGroup group{g.title, {}};
                for (const char* entry : g.ids) {
                    // "id" or "id:Short label"
                    const std::string e = entry;
                    const auto colon = e.find(':');
                    const CommandInfo* c = find(std::string(spec.tool) + "." + e.substr(0, colon));
                    if (!c) continue;
                    group.commands.push_back(c);
                    group.labels.push_back(colon == std::string::npos ? shortLabel(c->title, g.title, spec.title) : e.substr(colon + 1));
                }
                if (!group.commands.empty()) tab.groups.push_back(std::move(group));
            }
            if (spec.groups.empty()) {
                // a tool with no curated groups: its common actions
                RibbonGroup group{toolTitle(spec.tool), {}};
                for (const auto& id : beginnerCommands(spec.tool, commands, 14)) {
                    if (fileCommand(id)) continue;
                    if (const CommandInfo* c = find(id)) {
                        group.commands.push_back(c);
                        group.labels.push_back(c->title);
                    }
                }
                if (!group.commands.empty()) tab.groups.push_back(std::move(group));
            }
            if (!tab.groups.empty()) out.push_back(std::move(tab));
            covered.push_back(spec.tool);
        }
    }
    // tools without a curated tab (the all-in-one window, new tools) get one of common actions
    for (const auto& tool : tools) {
        if (std::find(covered.begin(), covered.end(), tool) != covered.end()) continue;
        RibbonTab tab{toolTitle(tool), tool, "", {}};
        RibbonGroup group{"Common", {}};
        for (const auto& id : beginnerCommands(tool, commands, 12)) {
            if (fileCommand(id)) continue;
            if (const CommandInfo* c = find(id)) {
                group.commands.push_back(c);
                group.labels.push_back(c->title);
            }
        }
        if (!group.commands.empty()) {
            tab.groups.push_back(std::move(group));
            out.push_back(std::move(tab));
        }
    }
    return out;
}

std::string openerStem(const std::string& opener) {
    const std::string s = suffix(opener);
    const auto us = s.rfind('_');
    if (us == std::string::npos) return {};
    const std::string verb = s.substr(us + 1);
    if (verb != "open" && verb != "new" && verb != "load" && verb != "import") return {};
    return s.substr(0, us);
}

std::string commandVerb(const std::string& id) {
    const std::string s = suffix(id);
    for (const char* verb : {"save_as", "open", "new", "load", "import", "save", "export"}) {
        const std::string v = verb;
        if (s == v) return v;
        if (s.size() > v.size() && s.compare(s.size() - v.size() - 1, v.size() + 1, "_" + v) == 0) return v;
    }
    return {};
}

void prefillMadeNames(Form& form, const std::vector<std::string>& made) {
    for (auto& f : form.fields) {
        if (f.kind != FieldKind::Text || f.hidden || !f.text.empty() || f.spec.name.empty()) continue;
        for (auto it = made.rbegin(); it != made.rend(); ++it) {
            const std::string& n = *it;
            if (n.size() <= f.spec.name.size() || n.compare(0, f.spec.name.size(), f.spec.name) != 0) continue;
            if (!std::all_of(n.begin() + static_cast<std::ptrdiff_t>(f.spec.name.size()), n.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) continue;
            setFieldText(f, n);
            break;
        }
    }
}

std::string readableDescription(const std::string& text, const std::vector<CommandInfo>& commands) {
    // "Add geometry with cad.sketch_line" reads "Add geometry with Sketch Line" in a window
    auto word = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (!word(text[i]) || (i > 0 && word(text[i - 1]))) {
            out += text[i++];
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && word(text[j])) ++j;
        if (j + 1 < text.size() && text[j] == '.' && word(text[j + 1])) {
            std::size_t k = j + 1;
            while (k < text.size() && word(text[k])) ++k;
            const std::string id = text.substr(i, k - i);
            auto it = std::find_if(commands.begin(), commands.end(), [&](const CommandInfo& c) { return c.id == id; });
            if (it != commands.end()) {
                out += it->title;
                i = k;
                continue;
            }
        }
        out += text.substr(i, j - i);
        i = j;
    }
    return out;
}

std::string firstSentence(const std::string& text) {
    for (std::size_t i = 0; i + 1 < text.size(); ++i) {
        const bool abbreviation = text[i] == '.' && i >= 2 && text[i - 2] == '.';  // e.g. i.e.
        if ((text[i] == '.' || text[i] == '!' || text[i] == '?') && text[i + 1] == ' ' && !abbreviation) return text.substr(0, i + 1);
    }
    return text;
}

void splitDescription(const std::string& text, std::string& lead, std::string& rest) {
    // sentences end at ". " before a capital letter ("e.g. cad1" and "x.y" don't)
    std::vector<std::string> sentences;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool end = (text[i] == '.' || text[i] == '!' || text[i] == '?') &&
                         (i + 1 == text.size() || (i + 2 < text.size() && text[i + 1] == ' ' && std::isupper(static_cast<unsigned char>(text[i + 2]))));
        if (!end) continue;
        sentences.push_back(text.substr(start, i + 1 - start));
        start = i + 2;
    }
    if (start < text.size()) sentences.push_back(text.substr(start));
    lead.clear();
    rest.clear();
    for (auto s : sentences) {
        if (s.rfind("Returns ", 0) == 0 || s.rfind("Return ", 0) == 0) continue;  // what a script gets back
        // "... and return its id (e.g. map1)." is for scripts too
        for (const char* tail : {" and return ", " and returns "}) {
            if (auto at = s.find(tail); at != std::string::npos) s = s.substr(0, at) + ".";
        }
        // the first sentence, and the next while the lead is still short
        std::string& to = rest.empty() && (lead.empty() || (lead.size() < 60 && lead.size() + s.size() < 140)) ? lead : rest;
        to += (to.empty() ? "" : " ") + s;
    }
}

bool needsDocument(const CommandInfo& c) {
    return std::any_of(c.params.begin(), c.params.end(), [&](const ParamSpec& p) {
        return p.required && kindFor(p, c.id) == FieldKind::Document;
    });
}

bool oneShotCommand(const std::string& id) { return !commandVerb(id).empty(); }

std::string runLabel(const std::string& id) {
    const std::string verb = commandVerb(id);
    if (verb == "new") return "Create";
    if (verb == "open" || verb == "load") return "Open";
    if (verb == "import") return "Import";
    if (verb == "save" || verb == "save_as") return "Save";
    if (verb == "export") return "Export";
    if (id == "cad.sketch") return "Start sketch";
    return "Apply";
}

int ribbonTabFor(const std::vector<RibbonTab>& tabs, const std::string& opener) {
    if (opener.empty()) return -1;
    const std::string tool = prefix(opener), stem = openerStem(opener);
    int loose = -1;
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].tool != tool) continue;
        if (tabs[i].stem == stem) return static_cast<int>(i);
        if (loose < 0 && tabs[i].stem.empty()) loose = static_cast<int>(i);
    }
    return loose;
}

std::string siblingCommand(const std::string& opener, const std::string& verb, const std::vector<CommandInfo>& commands) {
    if (opener.empty()) return {};
    auto has = [&](const std::string& id) {
        return std::any_of(commands.begin(), commands.end(), [&](const CommandInfo& c) { return c.id == id; });
    };
    const std::string tool = prefix(opener), stem = openerStem(opener);
    if (!stem.empty() && has(tool + "." + stem + "_" + verb)) return tool + "." + stem + "_" + verb;
    if (has(tool + "." + verb)) return tool + "." + verb;
    return {};
}

}  // namespace wetwaresuite::gui
