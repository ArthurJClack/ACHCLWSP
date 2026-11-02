#include "wetwaresuite/gui/logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <sstream>

namespace wetwaresuite::gui {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

// True when `word` starts a word somewhere in `text` (after a space, '.', '_', ...).
bool wordStart(const std::string& text, const std::string& word) {
    for (std::size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1)) {
        if (pos == 0 || !isWordChar(text[pos - 1])) return true;
    }
    return false;
}

// True when `word` appears as a whole word in `text`.
bool wholeWord(const std::string& text, const std::string& word) {
    for (std::size_t pos = text.find(word); pos != std::string::npos; pos = text.find(word, pos + 1)) {
        std::size_t end = pos + word.size();
        bool before = pos == 0 || !isWordChar(text[pos - 1]);
        bool after = end >= text.size() || !isWordChar(text[end]);
        if (before && after) return true;
    }
    return false;
}

bool subsequence(const std::string& text, const std::string& word) {
    std::size_t i = 0;
    for (char c : text) {
        if (i < word.size() && c == word[i]) ++i;
    }
    return i == word.size();
}

std::vector<std::string> words(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::string suffixOf(const std::string& id) {
    auto dot = id.find('.');
    return dot == std::string::npos ? id : id.substr(dot + 1);
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string capitalise(std::string s) {
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

std::string formatNumber(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

bool parseNumber(const std::string& text, double& out) {
    std::string t = trim(text);
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

bool parseNumberList(const std::string& text, NumberList& out) {
    std::string t = text;
    for (auto& c : t) {
        if (c == ',' || c == ';' || c == '[' || c == ']') c = ' ';
    }
    out.clear();
    for (const auto& w : words(t)) {
        double v;
        if (!parseNumber(w, v)) return false;
        out.push_back(v);
    }
    return true;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

}  // namespace

// ---- search ----------------------------------------------------------------

int matchScore(const std::string& query, const CommandInfo& info) {
    const auto qs = words(lower(query));
    if (qs.empty()) return 1;
    const std::string title = lower(info.title), id = lower(info.id), desc = lower(info.description);
    const std::string verb = suffixOf(id);
    int total = 0;
    for (const auto& w : qs) {
        int best = 0;
        if (startsWith(title, w)) best = std::max(best, 100);
        else if (wordStart(title, w)) best = std::max(best, 80);
        else if (contains(title, w)) best = std::max(best, 60);
        if (startsWith(verb, w)) best = std::max(best, 70);
        else if (wordStart(id, w)) best = std::max(best, 50);
        else if (contains(id, w)) best = std::max(best, 40);
        if (wordStart(desc, w)) best = std::max(best, 25);
        else if (contains(desc, w)) best = std::max(best, 15);
        if (best == 0 && w.size() >= 2) {
            if (subsequence(title, w)) best = 10;
            else if (subsequence(id, w)) best = 5;
        }
        if (best == 0) return 0;
        total += best;
    }
    if (qs.size() > 1 && startsWith(title, lower(trim(query)))) total += 50;
    return total;
}

std::vector<SearchHit> search(const std::string& query, const std::vector<CommandInfo>& commands, std::size_t limit) {
    std::vector<SearchHit> hits;
    for (const auto& c : commands) {
        int s = matchScore(query, c);
        if (s > 0) hits.push_back({&c, s});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const SearchHit& a, const SearchHit& b) { return a.score > b.score; });
    if (limit && hits.size() > limit) hits.resize(limit);
    return hits;
}

// ---- forms -----------------------------------------------------------------

std::string labelFor(const std::string& name) {
    static const std::map<std::string, std::string> special = {
        {"doc", "Document"}, {"dir", "Folder"}, {"src", "Source"}, {"dpi", "DPI"}, {"id", "ID"},
        {"url", "Web address"}, {"bpm", "Tempo (BPM)"}, {"x", "X"}, {"y", "Y"}, {"z", "Z"},
    };
    if (auto it = special.find(name); it != special.end()) return it->second;
    std::string out;
    for (char c : name) out += (c == '_' || c == '-') ? ' ' : c;
    out = capitalise(trim(out));
    // keep well known abbreviations readable
    for (const char* abbr : {"dpi", "url", "svg", "pdf", "png", "jpg", "csv", "rgb", "fps", "id"}) {
        std::string a = abbr;
        for (std::size_t pos = 0; (pos = lower(out).find(a, pos)) != std::string::npos; pos += a.size()) {
            bool before = pos == 0 || out[pos - 1] == ' ';
            bool after = pos + a.size() >= out.size() || out[pos + a.size()] == ' ';
            if (before && after) {
                for (std::size_t i = 0; i < a.size(); ++i) {
                    out[pos + i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[pos + i])));
                }
            }
        }
    }
    return out;
}

bool parseRange(const std::string& description, double& min, double& max) {
    static const std::regex range(R"((-?\d+(?:\.\d+)?)\s*\.\.\s*(-?\d+(?:\.\d+)?))");
    std::smatch m;
    if (!std::regex_search(description, m, range)) return false;
    double a = std::strtod(m[1].str().c_str(), nullptr), b = std::strtod(m[2].str().c_str(), nullptr);
    if (!(a < b)) return false;
    min = a;
    max = b;
    return true;
}

FieldKind kindFor(const ParamSpec& spec, const std::string& commandId) {
    switch (spec.type) {
        case ParamType::Bool: return FieldKind::Bool;
        case ParamType::Int: return FieldKind::Integer;
        case ParamType::Number: return FieldKind::Number;
        case ParamType::NumberList: return FieldKind::NumberList;
        case ParamType::String: break;
    }
    if (!spec.choices.empty()) return FieldKind::Choice;
    const std::string name = lower(spec.name), desc = lower(spec.description);
    if (name == "doc" || startsWith(desc, "id of an open")) return FieldKind::Document;
    if (contains(desc, "#rrggbb") || name == "color" || name == "colour" || endsWith(name, "_color") ||
        endsWith(name, "_colour") || name == "background" || name == "fill" || name == "stroke") {
        if (contains(desc, "#") || contains(desc, "colour") || contains(desc, "color")) return FieldKind::Color;
    }
    if (name == "dir" || endsWith(name, "_dir") || name == "folder" || endsWith(name, "_folder") || startsWith(desc, "folder")) {
        return FieldKind::Folder;
    }
    const bool fileName = name == "path" || name == "file" || name == "input" || name == "output" || name == "src" ||
                          endsWith(name, "_path") || endsWith(name, "_file");
    if (!fileName) return FieldKind::Text;
    const std::string verb = suffixOf(lower(commandId));
    const bool writes = startsWith(name, "output") || contains(desc, "to write") || contains(desc, "output file") ||
                        contains(desc, "write to") || contains(desc, "save to") ||
                        (name == "path" && (startsWith(verb, "save") || startsWith(verb, "export") || startsWith(verb, "render")));
    return writes ? FieldKind::SaveFile : FieldKind::OpenFile;
}

Form buildForm(const CommandInfo& info, const std::vector<std::string>& docs, const std::string& currentDoc) {
    Form form;
    form.command = info.id;
    form.title = info.title;
    form.description = info.description;
    for (const auto& spec : info.params) {
        Field f;
        f.spec = spec;
        f.kind = kindFor(spec, info.id);
        f.label = labelFor(spec.name);
        f.help = capitalise(spec.description);
        f.required = spec.required;
        f.hidden = (spec.name == "preview" || spec.name == "background") && spec.type == ParamType::Bool;
        double lo, hi;
        if (parseRange(spec.description, lo, hi)) {
            f.min = lo;
            f.max = hi;
        }
        const Value& d = spec.defaultValue;
        if (auto b = std::get_if<bool>(&d)) {
            f.flag = *b;
            f.set = true;
        } else if (auto i = std::get_if<std::int64_t>(&d)) {
            f.number = static_cast<double>(*i);
            f.set = true;
        } else if (auto n = std::get_if<double>(&d)) {
            f.number = *n;
            f.set = true;
        } else if (auto s = std::get_if<std::string>(&d)) {
            f.text = *s;
            f.set = true;
        } else if (auto l = std::get_if<NumberList>(&d)) {
            for (std::size_t k = 0; k < l->size(); ++k) f.text += (k ? ", " : "") + formatNumber((*l)[k]);
            f.set = true;
        } else if (spec.required) {
            // nothing given: start from something that works instead of an empty box
            f.set = true;
            const std::string name = lower(spec.name);
            if (f.kind == FieldKind::Integer || f.kind == FieldKind::Number) {
                // pixel art starts small; new pictures, videos and renders start at full HD;
                // a shape drawn on a page (a rectangle, a frame) starts a size you can see whole
                const std::string& id = info.id;
                const bool small = id.find("sprite") != std::string::npos;
                const bool page = id.find("new") != std::string::npos || id.find("resize") != std::string::npos ||
                                  id.find("canvas") != std::string::npos || id.find("render") != std::string::npos ||
                                  id.find("export") != std::string::npos || id.find("capture") != std::string::npos;
                if (name == "width") f.number = small ? 32 : page ? 1920 : 200;
                else if (name == "height") f.number = small ? 32 : page ? 1080 : 120;
                else if (f.min) f.number = std::max(0.0, *f.min);
            } else if (f.kind == FieldKind::Choice) {
                f.text = spec.choices.front();
            } else if (f.kind == FieldKind::Color) {
                f.text = "#000000";
            }
        }
        if (f.kind == FieldKind::Document) {
            if (!currentDoc.empty() && std::find(docs.begin(), docs.end(), currentDoc) != docs.end()) f.text = currentDoc;
            else if (f.text.empty() && !docs.empty()) f.text = docs.front();
            f.set = !f.text.empty();
        }
        if (f.min && f.max && (f.kind == FieldKind::Integer || f.kind == FieldKind::Number) && f.set) {
            f.number = std::clamp(f.number, *f.min, *f.max);
        }
        form.fields.push_back(std::move(f));
    }
    return form;
}

bool toParams(const Form& form, Params& out, std::string& error) {
    out.clear();
    for (const auto& f : form.fields) {
        if (f.hidden) continue;
        const auto missing = [&] {
            error = "Please fill in \"" + f.label + "\".";
            return false;
        };
        switch (f.kind) {
            case FieldKind::Bool:
                if (f.set) out[f.spec.name] = f.flag;
                else if (f.required) return missing();
                break;
            case FieldKind::Integer:
                if (f.set) out[f.spec.name] = static_cast<std::int64_t>(std::llround(f.number));
                else if (f.required) return missing();
                break;
            case FieldKind::Number:
                if (f.set) out[f.spec.name] = f.number;
                else if (f.required) return missing();
                break;
            case FieldKind::NumberList: {
                if (trim(f.text).empty()) {
                    if (f.required) return missing();
                    break;
                }
                NumberList list;
                if (!parseNumberList(f.text, list)) {
                    error = "\"" + f.label + "\" should be numbers separated by commas, like 10, 20, 30.";
                    return false;
                }
                out[f.spec.name] = list;
                break;
            }
            default:
                if (f.text.empty()) {
                    if (f.required) return missing();
                    break;
                }
                out[f.spec.name] = f.text;
        }
    }
    return true;
}

bool setFieldText(Field& field, const std::string& text) {
    const std::string t = lower(trim(text));
    switch (field.kind) {
        case FieldKind::Bool:
            if (t == "yes" || t == "true" || t == "1" || t == "on") field.flag = true;
            else if (t == "no" || t == "false" || t == "0" || t == "off") field.flag = false;
            else return false;
            field.set = true;
            return true;
        case FieldKind::Integer:
        case FieldKind::Number: {
            double v;
            if (!parseNumber(t, v)) return false;
            field.number = field.kind == FieldKind::Integer ? std::round(v) : v;
            field.set = true;
            return true;
        }
        case FieldKind::Choice:
            if (std::find(field.spec.choices.begin(), field.spec.choices.end(), trim(text)) == field.spec.choices.end()) return false;
            field.text = trim(text);
            field.set = true;
            return true;
        default:
            field.text = text;
            field.set = !text.empty();
            return true;
    }
}

// ---- shortlist ---------------------------------------------------------------

std::vector<const CommandInfo*> toolCommands(const std::string& tool, const std::vector<CommandInfo>& commands) {
    std::vector<const CommandInfo*> out;
    const std::string prefix = tool + ".";
    for (const auto& c : commands) {
        if (startsWith(c.id, prefix)) out.push_back(&c);
    }
    return out;
}

std::vector<std::string> beginnerCommands(const std::string& tool, const std::vector<CommandInfo>& commands, std::size_t count) {
    static const std::vector<std::string> verbs = {
        "open", "new", "save", "export", "stroke", "fill", "resize", "crop", "adjust", "effect", "rotate", "erase", "text",
        "add", "shape", "levels", "render", "convert", "import", "generate", "play", "flip", "info",
    };
    const auto mine = toolCommands(tool, commands);
    std::vector<std::string> out;
    auto take = [&](const CommandInfo* c) {
        if (out.size() < count && std::find(out.begin(), out.end(), c->id) == out.end()) out.push_back(c->id);
    };
    for (const auto& v : verbs) {
        for (const auto* c : mine) {
            if (suffixOf(c->id) == v) take(c);
        }
        for (const auto* c : mine) {
            const std::string verb = suffixOf(c->id);
            // export_status, export_cancel: watching a job isn't a first thing to do
            if (endsWith(verb, "_status") || endsWith(verb, "_cancel")) continue;
            // open_url, or a family's own: office.doc_open, forge.sprite_new
            if (startsWith(verb, v + "_") || endsWith(verb, "_" + v)) {
                take(c);
                break;  // one variant per verb keeps the list short
            }
        }
    }
    for (const auto* c : mine) {
        const std::string verb = suffixOf(c->id);
        if (verb != "preview" && verb != "close" && !endsWith(verb, "_close") && !contains(verb, "batch")) take(c);
    }
    return out;
}

// ---- opening files -------------------------------------------------------------

std::vector<std::string> openCandidates(const std::string& path, const std::vector<std::string>& tools,
                                        const std::vector<CommandInfo>& commands) {
    std::string ext;
    auto dot = path.find_last_of('.');
    auto slash = path.find_last_of("/\\");
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) ext = lower(path.substr(dot + 1));
    std::vector<std::string> first, rest;
    // <tool>.open, then the tool's other openers (office.sheet_open, audio.project_open...)
    // that take a path; ones whose description names the extension go first.
    for (const auto& tool : tools) {
        std::vector<const CommandInfo*> openers;
        for (const auto& c : commands) {
            if (c.id.rfind(tool + ".", 0) != 0) continue;
            const bool plain = c.id == tool + ".open";
            const bool named = c.id.size() > 5 && c.id.compare(c.id.size() - 5, 5, "_open") == 0;
            const bool takesPath = std::any_of(c.params.begin(), c.params.end(), [](const ParamSpec& p) { return p.name == "path"; });
            // a named opener must need nothing but the path (forge.sprite_open, levels.world_open)
            const bool pathOnly = std::none_of(c.params.begin(), c.params.end(), [](const ParamSpec& p) { return p.required && p.name != "path"; });
            if ((plain || (named && pathOnly)) && takesPath) openers.insert(plain ? openers.begin() : openers.end(), &c);
        }
        for (const CommandInfo* it : openers) {
            const std::string desc = lower(it->description + " " + it->title);
            bool mentions = !ext.empty() && (wholeWord(desc, ext) || (ext == "jpeg" && wholeWord(desc, "jpg")) ||
                                             (ext == "jpg" && wholeWord(desc, "jpeg")));
            (mentions ? first : rest).push_back(it->id);
        }
    }
    first.insert(first.end(), rest.begin(), rest.end());
    return first;
}

// ---- colours ---------------------------------------------------------------------

std::optional<Rgba> parseHex(const std::string& text) {
    std::string t = lower(trim(text));
    if (t == "transparent" || t == "none") return Rgba{0, 0, 0, 0};
    // the names and rgb() the commands take too, so the swatch matches the text
    static const std::map<std::string, std::string> names = {
        {"white", "#ffffff"}, {"black", "#000000"}, {"red", "#ff0000"},    {"green", "#008000"}, {"lime", "#00ff00"},
        {"blue", "#0000ff"},  {"yellow", "#ffff00"}, {"orange", "#ffa500"}, {"purple", "#800080"}, {"pink", "#ffc0cb"},
        {"brown", "#a52a2a"}, {"gray", "#808080"},  {"grey", "#808080"},   {"cyan", "#00ffff"},  {"magenta", "#ff00ff"},
        {"navy", "#000080"},  {"teal", "#008080"},  {"silver", "#c0c0c0"}, {"gold", "#ffd700"},  {"maroon", "#800000"},
        {"olive", "#808000"}};
    if (auto it = names.find(t); it != names.end()) t = it->second;
    int r = 0, g = 0, b = 0;
    if (std::sscanf(t.c_str(), "rgb(%d,%d,%d)", &r, &g, &b) == 3 || std::sscanf(t.c_str(), "rgb( %d , %d , %d )", &r, &g, &b) == 3)
        return Rgba{std::clamp(r, 0, 255) / 255.0f, std::clamp(g, 0, 255) / 255.0f, std::clamp(b, 0, 255) / 255.0f, 1.0f};
    if (t.empty() || t[0] != '#') return std::nullopt;
    t = t.substr(1);
    if (t.size() == 3 || t.size() == 4) {
        std::string wide;
        for (char c : t) wide += std::string(2, c);
        t = wide;
    }
    if (t.size() != 6 && t.size() != 8) return std::nullopt;
    float ch[4] = {0, 0, 0, 1};
    for (std::size_t i = 0; i < t.size() / 2; ++i) {
        int hi = hexDigit(t[i * 2]), lo = hexDigit(t[i * 2 + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        ch[i] = static_cast<float>(hi * 16 + lo) / 255.0f;
    }
    return Rgba{ch[0], ch[1], ch[2], ch[3]};
}

std::string toHex(const Rgba& c) {
    auto byte = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    char buf[16];
    if (byte(c.a) == 255) std::snprintf(buf, sizeof buf, "#%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b));
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", byte(c.r), byte(c.g), byte(c.b), byte(c.a));
    return buf;
}

Rgba fromRgb24(std::uint32_t rgb) {
    return {static_cast<float>((rgb >> 16) & 0xff) / 255.0f, static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
            static_cast<float>(rgb & 0xff) / 255.0f, 1.0f};
}

// ---- recent files -------------------------------------------------------------------

std::vector<std::string> pushRecent(std::vector<std::string> recent, const std::string& path, std::size_t limit) {
    recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
    recent.insert(recent.begin(), path);
    if (recent.size() > limit) recent.resize(limit);
    return recent;
}

std::vector<std::string> parseRecent(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && std::find(out.begin(), out.end(), line) == out.end()) out.push_back(line);
    }
    return out;
}

std::string formatRecent(const std::vector<std::string>& recent) {
    std::string out;
    for (const auto& r : recent) out += r + "\n";
    return out;
}

// ---- messages ---------------------------------------------------------------------

std::string friendlyError(const std::string& message) {
    static const std::regex missing(R"(missing parameter '([^']+)' for \S+)");
    static const std::regex oneOf(R"(parameter '([^']+)' for \S+ must be one of: (.*))");
    static const std::regex mustBe(R"(parameter '([^']+)' for \S+ must be (\w+))");
    static const std::regex unknownParam(R"(unknown parameter '([^']+)' for \S+)");
    static const std::regex unknownCommand(R"(unknown command: (\S+))");
    std::smatch m;
    if (std::regex_match(message, m, missing)) return "Please fill in \"" + labelFor(m[1]) + "\".";
    if (std::regex_match(message, m, oneOf)) return "\"" + labelFor(m[1]) + "\" has to be one of: " + m[2].str() + ".";
    if (std::regex_match(message, m, mustBe)) {
        static const std::map<std::string, std::string> kinds = {
            {"integer", "a whole number"}, {"number", "a number"}, {"boolean", "yes or no"},
            {"string", "some text"},       {"array", "a list of numbers"},
        };
        auto it = kinds.find(m[2]);
        return "\"" + labelFor(m[1]) + "\" needs " + (it != kinds.end() ? it->second : m[2].str()) + ".";
    }
    if (std::regex_match(message, m, unknownParam)) return "This action doesn't use \"" + labelFor(m[1]) + "\".";
    if (std::regex_match(message, m, unknownCommand)) return "There is no action called " + m[1].str() + " in this app.";
    return sentence(message);
}

std::string sentence(const std::string& message) {
    std::string out = capitalise(trim(message));
    if (!out.empty() && out.back() != '.' && out.back() != '!' && out.back() != '?' && out.back() != ':') out += '.';
    return out;
}

std::vector<std::string> resultLines(const Result& result) {
    std::vector<std::string> out;
    for (const auto& [k, v] : result.data) out.push_back(labelFor(k) + ": " + toText(v));
    return out;
}

// ---- tools --------------------------------------------------------------------------

std::string toolBlurb(const std::string& tool) {
    static const std::map<std::string, std::string> blurbs = {
        {"toolkit", "Open any file and see what is inside it."},
        {"photo", "Fix and change photos: brightness, crop, resize, effects."},
        {"paint", "Draw and paint with brushes, layers and pen pressure."},
        {"vector", "Logos and drawings made of shapes that stay sharp at any size."},
        {"convert", "Change a file into another type: images, sound, video, documents."},
        {"blueprint", "Floor plans and technical drawings printed to scale."},
        {"cad", "Design exact parts to 3D print or make."},
        {"slicer", "Turn a 3D model into instructions for a 3D printer."},
        {"fit", "Make car parts and mounts that fit a 3D scan from your phone."},
        {"forge", "Make models, textures and sprites for games."},
        {"audio", "Record, clean up and mix sound."},
        {"cut", "Edit videos: trim, join, titles and music."},
        {"capture", "Take screenshots and record the screen."},
        {"office", "Documents, spreadsheets and slides."},
        {"pdf", "Open any PDF to mark it up, edit its text, fill in forms and sign it."},
        {"layout", "Posters, flyers and booklets ready to print."},
        {"library", "Find, tag and sort all your files."},
        {"automate", "Repeat jobs on many files at once."},
        {"model", "Build 3D models from boxes and loops, like Blender."},
        {"sculpt", "Shape 3D models like clay."},
        {"render", "Make pictures of 3D scenes with light and materials."},
        {"animate", "Make things move: 2D and 3D animation."},
        {"scene", "Godot-style scenes: a node tree, inspector and viewport, saved as .tscn for Godot."},
        {"levels", "Build maps and levels for games."},
        {"beats", "Make music with drums, synths and samples."},
        {"circuit", "Draw electronic circuits and design boards."},
        {"flash", "Put programs onto Arduino and other small boards."},
        {"maps", "Maps and places that work without the internet."},
        {"board", "A whiteboard for notes, diagrams and ideas."},
        {"notes", "Write and link notes."},
        {"type", "Design your own fonts."},
        {"calc", "A calculator for everything: exact fractions, units, graphs, statistics and solving."},
        {"code", "Write, run and build code in many languages, with git built in."},
        {"explorer", "Browse, copy, zip and tidy your files. Deleted things go to a trash you can undo."},
        {"terminal", "Type commands into your computer's shell."},
        {"monitor", "See what your computer is busy with, and end programs that hang."},
        {"clock", "World clock, timers, stopwatch, alarms and calendars."},
    };
    auto it = blurbs.find(tool);
    return it != blurbs.end() ? it->second : capitalise(tool) + " tools.";
}

std::string toolTitle(const std::string& tool) {
    if (tool == "forge") return "Asset forge";
    if (tool == "calc") return "Calculator";
    if (tool == "explorer") return "Files";
    if (tool == "monitor") return "System monitor";
    if (tool == "fit") return "Scan fit";
    if (tool == "pdf") return "PDF editor";
    return capitalise(tool);
}

// ---- retouching by painting -----------------------------------------------------

std::vector<double> thinStroke(const std::vector<double>& points, double minStep, std::size_t maxPoints) {
    const std::size_t n = points.size() / 2;
    if (n == 0) return {};
    std::vector<double> kept{points[0], points[1]};
    const double step2 = minStep * minStep;
    for (std::size_t i = 1; i < n; ++i) {
        const double x = points[2 * i], y = points[2 * i + 1];
        const double dx = x - kept[kept.size() - 2], dy = y - kept.back();
        if (dx * dx + dy * dy < step2 && i + 1 < n) continue;
        if (dx * dx + dy * dy < step2 && kept.size() > 2) {
            kept[kept.size() - 2] = x;  // the end is too close to the last kept point: move that one
            kept.back() = y;
            continue;
        }
        if (dx == 0 && dy == 0) continue;
        kept.push_back(x);
        kept.push_back(y);
    }
    const std::size_t m = kept.size() / 2;
    maxPoints = std::max<std::size_t>(maxPoints, 2);
    if (m <= maxPoints) return kept;
    std::vector<double> out;
    out.reserve(maxPoints * 2);
    for (std::size_t k = 0; k < maxPoints; ++k) {
        const std::size_t i = k * (m - 1) / (maxPoints - 1);
        out.push_back(kept[2 * i]);
        out.push_back(kept[2 * i + 1]);
    }
    return out;
}

void HealSource::pick(double px, double py) {
    picked = true;
    x = px;
    y = py;
    anchored = false;
    dx = dy = 0;
}

bool HealSource::offsetFor(double sx, double sy, double& ox, double& oy) {
    if (!picked) return false;
    if (!anchored) {
        dx = x - sx;
        dy = y - sy;
        anchored = true;
    }
    ox = dx;
    oy = dy;
    return true;
}

namespace {

struct StrokeBox {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // inclusive pixel bounds of the brushed area, clamped to the picture
};

// The common half of both commands: radius, thinned points, and the area the
// stroke covers (to check it's on the picture).
bool strokeArea(const std::string& doc, const std::vector<double>& points, double brushSize, int width, int height, Params& out,
                StrokeBox& box, std::string& error) {
    if (points.size() < 2) {
        error = "Paint over the area first.";
        return false;
    }
    const double radius = std::clamp(brushSize / 2, 1.0, 1000.0);
    std::vector<double> pts = thinStroke(points, std::max(1.0, radius / 4));
    double minX = pts[0], maxX = pts[0], minY = pts[1], maxY = pts[1];
    for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
        minX = std::min(minX, pts[i]), maxX = std::max(maxX, pts[i]);
        minY = std::min(minY, pts[i + 1]), maxY = std::max(maxY, pts[i + 1]);
    }
    box.x0 = std::max(0.0, std::floor(minX - radius));
    box.y0 = std::max(0.0, std::floor(minY - radius));
    box.x1 = std::min(width - 1.0, std::ceil(maxX + radius) - 1);
    box.y1 = std::min(height - 1.0, std::ceil(maxY + radius) - 1);
    if (width < 1 || height < 1 || box.x1 < box.x0 || box.y1 < box.y0) {
        error = "That stroke is outside the picture.";
        return false;
    }
    // a little rounding keeps the numbers short in the history and in scripts
    for (auto& v : pts) v = std::round(v * 10) / 10;
    out = Params{{"doc", doc}, {"shape", std::string("stroke")}, {"points", NumberList(pts)}, {"radius", radius}};
    return true;
}

}  // namespace

bool healStrokeParams(const std::string& doc, const std::vector<double>& points, double brushSize, int width, int height,
                      const double* offset, Params& out, std::string& error) {
    StrokeBox box;
    if (!strokeArea(doc, points, brushSize, width, height, out, box, error)) return false;
    if (!offset) return true;
    const double ox = std::round(offset[0]), oy = std::round(offset[1]);
    // the source area is the brushed area moved by the offset: it has to be on the picture
    if (box.x0 + ox < 0 || box.y0 + oy < 0 || box.x1 + ox > width - 1 || box.y1 + oy > height - 1) {
        error = "The spot to copy from is too close to the edge of the picture for this stroke. "
                "Alt+click (or use Pick source) on a spot further inside, or paint a shorter stroke.";
        return false;
    }
    // photo.heal copies the middle of the area from (from_x, from_y)
    const auto cx = static_cast<std::int64_t>((box.x0 + box.x1) / 2), cy = static_cast<std::int64_t>((box.y0 + box.y1) / 2);
    out["from_x"] = cx + static_cast<std::int64_t>(ox);
    out["from_y"] = cy + static_cast<std::int64_t>(oy);
    return true;
}

bool removeStrokeParams(const std::string& doc, const std::vector<double>& points, double brushSize, int width, int height,
                        Params& out, std::string& error) {
    StrokeBox box;
    return strokeArea(doc, points, brushSize, width, height, out, box, error);
}

bool brushToolParams(const std::string& tool, const std::string& doc, const std::vector<double>& points,
                     const std::vector<double>& pressures, const BrushToolSettings& settings, const double* cloneOffset,
                     std::string& command, Params& out, std::string& error) {
    out.clear();
    if (points.size() < 2 || points.size() % 2) {
        error = "Paint a stroke on the picture first.";
        return false;
    }
    out["doc"] = doc;
    out["points"] = NumberList(points.begin(), points.end());
    if (!pressures.empty() && pressures.size() == points.size() / 2) out["pressures"] = NumberList(pressures.begin(), pressures.end());
    out["brush"] = settings.brush;
    out["size"] = std::max(1.0, settings.size);
    out["opacity"] = std::clamp(settings.opacity, 0.0, 100.0);
    const double strength = std::clamp(settings.strength, 0.0, 100.0);
    if (tool == "clone") {
        if (!cloneOffset) {
            error = "Alt+click (Option+click on a Mac) the spot to copy from first, or press Pick source.";
            return false;
        }
        command = "paint.clone";
        out["source_x"] = points[0] + std::round(cloneOffset[0]);
        out["source_y"] = points[1] + std::round(cloneOffset[1]);
        return true;
    }
    if (tool == "dodge" || tool == "burn" || tool == "sponge") {
        command = "paint.dodge";
        out["tool"] = tool;
        out["exposure"] = strength;
        if (tool == "sponge") out["sponge_mode"] = std::string("desaturate");
        else out["range"] = settings.range;
        return true;
    }
    if (tool == "smudge") {
        command = "paint.smudge";
        out["strength"] = strength;
        return true;
    }
    if (tool == "blur" || tool == "sharpen") {
        command = "paint.blur_brush";
        out["mode"] = tool;
        out["strength"] = strength;
        return true;
    }
    error = "'" + tool + "' isn't a brush tool.";
    return false;
}

}  // namespace wetwaresuite::gui
