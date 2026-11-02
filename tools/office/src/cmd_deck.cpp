// office.deck_* commands and the Markdown outline format for slide decks.
#include <algorithm>
#include <cmath>

#include "wetwaresuite/office/color.hpp"
#include "wetwaresuite/raster/codec.hpp"
#include "deck_cmd.hpp"
#include "text.hpp"

namespace wetwaresuite::office {

// ---- Markdown outlines ----------------------------------------------------------------

namespace {

bool isBulletLine(const std::string& raw) {
    std::string t = trimCopy(raw);
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') return true;
    if (startsWith(t, "\xE2\x80\xA2")) return true;
    std::size_t d = 0;
    while (d < t.size() && std::isdigit(static_cast<unsigned char>(t[d]))) ++d;
    return d > 0 && d < 4 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') && t[d + 1] == ' ';
}

bool layoutKnown(const std::string& l) { return std::find(slideLayouts().begin(), slideLayouts().end(), l) != slideLayouts().end(); }

}  // namespace

Deck deckFromOutline(const std::string& markdown, const std::string& baseDir) {
    Deck deck;
    auto lines = splitLines(markdown);
    std::size_t i = 0;
    // front matter
    if (!lines.empty() && trimCopy(lines[0]) == "---") {
        std::size_t j = 1;
        bool meta = true;
        for (; j < lines.size() && trimCopy(lines[j]) != "---"; ++j) {
            if (trimCopy(lines[j]).empty()) continue;
            if (lines[j].find(':') == std::string::npos) meta = false;
        }
        if (meta && j < lines.size()) {
            for (std::size_t k = 1; k < j; ++k) {
                auto c = lines[k].find(':');
                if (c == std::string::npos) continue;
                std::string key = lower(trimCopy(lines[k].substr(0, c))), val = trimCopy(lines[k].substr(c + 1));
                if (val.size() >= 2 && (val.front() == '"' || val.front() == '\'') && val.back() == val.front()) val = val.substr(1, val.size() - 2);
                std::string err;
                if (key == "title") deck.title = val;
                else if (key == "author") deck.author = val;
                else if (key == "theme") parseTheme(val, deck.theme, err);
                else if (key == "font") {
                    std::string f = lower(val);
                    deck.theme.font = f == "serif" ? pdf::Family::Serif : f == "mono" ? pdf::Family::Mono : pdf::Family::Sans;
                } else if (key == "aspect" && val == "4:3") deck.width = 720;
            }
            i = j + 1;
        }
    }
    struct Draft {
        Slide s;
        std::string left, right, subtitle, notes;
        bool rightCol = false;
        bool explicitLayout = false;
        bool empty() const { return s.title.empty() && left.empty() && right.empty() && subtitle.empty() && notes.empty() && s.image.empty(); }
    };
    std::vector<Draft> drafts;
    auto cur = [&]() -> Draft& {
        if (drafts.empty()) drafts.emplace_back();
        return drafts.back();
    };
    bool inCode = false;
    for (; i < lines.size(); ++i) {
        const std::string& raw = lines[i];
        const std::string t = trimCopy(raw);
        if (startsWith(t, "```")) {
            inCode = !inCode;
            continue;
        }
        if (inCode) {
            Draft& d = cur();
            (d.rightCol ? d.right : d.left) += "- `" + t + "`\n";
            continue;
        }
        if (startsWith(t, "<!--")) {
            std::string inner = lower(trimCopy(t.substr(4, t.size() >= 7 && endsWith(t, "-->") ? t.size() - 7 : std::string::npos)));
            if (startsWith(inner, "layout:")) {
                if (!drafts.empty() && (cur().explicitLayout || !cur().empty())) drafts.emplace_back();
                Draft& d = cur();
                d.s.layout = trimCopy(inner.substr(7));
                if (!layoutKnown(d.s.layout)) d.s.layout = "content";
                d.explicitLayout = true;
            } else if (inner == "column" || inner == "right" || inner == "column 2") {
                cur().rightCol = true;
            } else if (inner == "slide") {
                drafts.emplace_back();
            }
            continue;
        }
        if (t == "---" || t == "***" || t == "___") {
            if (drafts.empty() || !cur().empty()) drafts.emplace_back();
            continue;
        }
        if (!t.empty() && t[0] == '#') {
            std::size_t h = 0;
            while (h < t.size() && t[h] == '#') ++h;
            if (h < t.size() && t[h] != ' ') {
                // not a heading ("#hashtag"): treat as text
            } else {
                if (drafts.empty() || !cur().s.title.empty() || !cur().left.empty() || !cur().right.empty() || !cur().s.image.empty()) {
                    drafts.emplace_back();
                }
                cur().s.title = trimCopy(t.substr(h));
                continue;
            }
        }
        if (t.empty()) continue;
        Draft& d = cur();
        if (isBulletLine(raw)) {
            (d.rightCol ? d.right : d.left) += raw + "\n";
        } else if (startsWith(t, "![")) {
            auto close = t.find("](");
            auto end = t.rfind(')');
            if (close != std::string::npos && end != std::string::npos && end > close) {
                std::string path = trimCopy(t.substr(close + 2, end - close - 2));
                auto sp = path.find(" \"");
                if (sp != std::string::npos) path = path.substr(0, sp);
                fs::path full = pathFromUtf8(path);
                if (full.is_relative() && !baseDir.empty()) full = pathFromUtf8(baseDir) / full;
                setSlideImage(d.s, readBytes(full.string()));
            }
        } else if (startsWith(t, ">")) {
            d.notes += (d.notes.empty() ? "" : "\n") + trimCopy(t.substr(1));
        } else if (startsWith(lower(t), "notes:")) {
            d.notes += (d.notes.empty() ? "" : "\n") + trimCopy(t.substr(6));
        } else if (!d.left.empty() && (raw.size() > 0 && (raw[0] == ' ' || raw[0] == '\t'))) {
            // a continuation line of the previous bullet
            std::string& col = d.rightCol ? d.right : d.left;
            if (!col.empty() && col.back() == '\n') col.pop_back();
            col += " " + t + "\n";
        } else {
            d.subtitle += (d.subtitle.empty() ? "" : "\n") + t;
        }
    }
    for (std::size_t k = 0; k < drafts.size(); ++k) {
        Draft& d = drafts[k];
        if (d.empty() && !d.explicitLayout) continue;
        d.s.bullets = parseBullets(d.left, &d.s.numbered);
        d.s.right = parseBullets(d.right);
        d.s.subtitle = d.subtitle;
        d.s.notes = d.notes;
        if (!d.explicitLayout) {
            const bool img = !d.s.image.empty();
            if (deck.slides.empty() && d.s.bullets.empty() && !img) d.s.layout = "title";
            else if (!d.s.right.empty()) d.s.layout = "two_column";
            else if (img && !d.s.bullets.empty()) d.s.layout = "image_right";
            else if (img) d.s.layout = "image";
            else if (d.s.bullets.empty() && !d.s.title.empty()) d.s.layout = "section";
            else d.s.layout = "content";
        }
        deck.slides.push_back(std::move(d.s));
    }
    if (deck.title.empty() && !deck.slides.empty()) deck.title = deck.slides.front().title;
    return deck;
}

std::string deckToOutline(const Deck& deck, const std::string& mediaPrefix, std::vector<MediaFile>& media) {
    std::string out = "---\n";
    if (!deck.title.empty()) out += "title: " + deck.title + "\n";
    if (!deck.author.empty()) out += "author: " + deck.author + "\n";
    out += "theme: " + (deck.theme.name.rfind("#", 0) == 0 ? "\"" + deck.theme.name + "\"" : deck.theme.name) + "\n";
    if (deck.theme.font != pdf::Family::Sans) out += std::string("font: ") + (deck.theme.font == pdf::Family::Serif ? "serif" : "mono") + "\n";
    if (deck.width < deck.height * 1.5) out += "aspect: 4:3\n";
    out += "---\n";
    int pictures = 0;
    for (const auto& s : deck.slides) {
        out += "\n<!-- layout: " + s.layout + " -->\n";
        if (!s.title.empty()) out += "# " + s.title + "\n";
        if (!s.subtitle.empty()) out += "\n" + s.subtitle + "\n";
        if (!s.bullets.empty()) out += "\n" + bulletText(s.bullets, s.numbered);
        if (!s.right.empty()) out += "\n<!-- column -->\n" + bulletText(s.right, s.numbered);
        if (!s.image.empty()) {
            const bool jpeg = s.image.size() > 2 && static_cast<unsigned char>(s.image[0]) == 0xFF;
            std::string name = mediaPrefix + "image" + std::to_string(++pictures) + (jpeg ? ".jpg" : ".png");
            media.push_back({name, s.image});
            out += "\n![](" + name + ")\n";
        }
        if (!s.notes.empty()) {
            out += "\n";
            for (const auto& l : splitLines(s.notes)) out += "> " + l + "\n";
        }
    }
    return out;
}

// ---- commands ----------------------------------------------------------------------------

namespace {

using Edit = std::function<Result(DeckDoc&, const Params&, const std::string&)>;

ActionFactory edit(Session& s, Edit fn) { return docCommand<DeckDoc>(s, std::move(fn), true); }
ActionFactory read(Session& s, Edit fn) { return docCommand<DeckDoc>(s, std::move(fn), false); }

ParamSpec doc() { return docSpec("slide deck", "deck1"); }

std::vector<ParamSpec> slideSpecs() {
    return {str("layout", "slide layout", false, {}, slideLayouts()),
            str("title", "slide title (inline Markdown)"),
            str("subtitle", "subtitle (title/section slides) or a line under the title"),
            str("bullets", "bullet points, one per line; indent 2 spaces for sub-bullets; - or 1. markers optional"),
            str("bullets_file", "read the bullets from this file"),
            str("right", "two_column: the right column's bullets"),
            str("image", "picture file (png/jpg) for image layouts"),
            str("notes", "speaker notes"),
            str("background", "this slide's background colour (#rrggbb, a name, accent; none = the layout's)"),
            flag("hidden", "hide the slide: left out of shows and PDF exports")};
}

std::vector<ParamSpec> join(std::vector<ParamSpec> a, const std::vector<ParamSpec>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

bool applySlide(Slide& s, const Params& p, std::string& error) {
    if (has(p, "layout")) s.layout = stringParam(p, "layout");
    if (has(p, "title")) s.title = stringParam(p, "title");
    if (hasText(p, "subtitle")) s.subtitle = textParam(p, "subtitle");
    if (hasText(p, "bullets")) s.bullets = parseBullets(textParam(p, "bullets"), &s.numbered);
    if (hasText(p, "right")) s.right = parseBullets(textParam(p, "right"));
    if (hasText(p, "notes")) s.notes = textParam(p, "notes");
    if (has(p, "background") && !parsePaint(stringParam(p, "background"), s.background)) {
        error = "background: not a colour: " + stringParam(p, "background");
        return false;
    }
    if (has(p, "hidden")) s.hidden = boolParam(p, "hidden", false);
    if (boolParam(p, "clear_image", false)) {
        s.image.clear();
        s.pixels.reset();
    }
    if (has(p, "image")) {
        try {
            setSlideImage(s, readBytes(stringParam(p, "image")));
        } catch (const std::exception& e) {
            error = std::string("image: ") + e.what();
            return false;
        }
    }
    return true;
}

bool slideArg(const Deck& d, const Params& p, const char* key, std::size_t& index, std::string& error) {
    const std::int64_t n = intParam(p, key, 0);
    if (n < 1 || n > static_cast<std::int64_t>(d.slides.size())) {
        error = d.slides.empty() ? std::string("the deck has no slides") : std::string(key) + " must be 1.." + std::to_string(d.slides.size());
        return false;
    }
    index = static_cast<std::size_t>(n - 1);
    return true;
}

std::string describe(const Deck& d, std::size_t i) {
    const Slide& s = d.slides[i];
    std::string t = std::to_string(i + 1) + ". " + s.layout + ": " + (s.title.empty() ? std::string("(no title)") : "\"" + s.title + "\"");
    if (!s.subtitle.empty()) t += " / " + s.subtitle.substr(0, 60);
    if (!s.bullets.empty()) t += ", " + std::to_string(s.bullets.size()) + " bullet(s)";
    if (!s.right.empty()) t += " + " + std::to_string(s.right.size()) + " right";
    if (!s.image.empty() && s.pixels) t += ", image " + std::to_string(s.pixels->pixels.width()) + "x" + std::to_string(s.pixels->pixels.height());
    if (!s.notes.empty()) t += ", notes";
    if (!s.objects.empty()) t += ", " + std::to_string(s.objects.size()) + " object(s)";
    if (!s.background.empty()) t += ", background " + s.background;
    if (!s.transition.empty()) t += ", " + s.transition + " transition";
    if (s.advanceAfter > 0) t += ", advances after " + textNumber(s.advanceAfter) + " s";
    if (s.hidden) t += " (hidden)";
    auto over = slideOverflow(d, i);
    if (!over.empty()) {
        std::string w;
        for (const auto& o : over) w += (w.empty() ? "" : ", ") + o;
        t += " - WARNING: too much text in " + w + " even at the smallest size; split the slide";
    }
    return t;
}

Attachment slidePng(const Deck& d, std::size_t i, double width) {
    return png(pdf::renderPage(layoutSlide(d, i), width / d.width), "slide" + std::to_string(i + 1) + ".png");
}

void slidePreview(Result& r, const Deck& d, std::size_t i, const Params& p) {
    if (r.ok && boolParam(p, "preview", false) && i < d.slides.size()) r.attach(slidePng(d, i, 960));
}

bool applyTheme(Deck& d, const Params& p, std::string& error) {
    if (has(p, "theme") && !parseTheme(stringParam(p, "theme"), d.theme, error)) return false;
    for (const char* key : {"accent", "background", "text_color"}) {
        if (!has(p, key)) continue;
        std::uint32_t c = 0;
        if (!parseColor(stringParam(p, key), c) || !hasColor(c)) {
            error = std::string(key) + ": not a colour: " + stringParam(p, key);
            return false;
        }
        const pdf::Color col{static_cast<std::uint8_t>(rgbOf(c) >> 16), static_cast<std::uint8_t>((rgbOf(c) >> 8) & 0xFF), static_cast<std::uint8_t>(rgbOf(c) & 0xFF), 255};
        if (std::string(key) == "accent") {
            d.theme.accent = col;
            d.theme.name = colorName(c);
        } else if (std::string(key) == "background") {
            d.theme.background = col;
        } else {
            d.theme.text = col;
        }
    }
    if (has(p, "font")) {
        const std::string f = lower(stringParam(p, "font"));
        if (f == "sans") d.theme.font = pdf::Family::Sans;
        else if (f == "serif") d.theme.font = pdf::Family::Serif;
        else if (f == "mono") d.theme.font = pdf::Family::Mono;
        else {
            error = "font must be sans, serif or mono";
            return false;
        }
    }
    return true;
}

std::string hexOf(pdf::Color c) {
    char b[8];
    std::snprintf(b, sizeof b, "%02x%02x%02x", c.r, c.g, c.b);
    return b;
}

std::string themeHelp() {
    std::string names;
    for (const auto& n : themeNames()) names += (names.empty() ? "" : ", ") + n;
    return "colour theme: " + names + ", or an accent colour like #0a7d5a";
}

}  // namespace

void registerDeckCommands(Session& session) {
    auto& reg = session.registry();
    std::string layouts;
    for (const auto& l : slideLayouts()) layouts += (layouts.empty() ? "" : ", ") + l;

    reg.add({"office.deck_new", "New Slide Deck",
             "Create a 16:9 slide deck and return its id (deck1, ...). outline= builds every slide from Markdown at once: each "
             "# heading starts a slide, - lines are bullets (indent for sub-bullets), other lines a subtitle, > lines speaker notes, "
             "![](pic.png) a picture, <!-- layout: two_column --> picks a layout and <!-- column --> starts the right column. A "
             "first slide without bullets becomes the title slide.",
             {str("title", "deck title (file metadata)"), str("author", "author"), str("theme", themeHelp(), false, std::string("blue")),
              str("font", "font family", false, std::string("sans"), {"sans", "serif", "mono"}), str("outline", "slides as a Markdown outline"), str("outline_file", "read the outline from this .md file"),
              str("aspect", "slide shape", false, std::string("16:9"), {"16:9", "4:3"})},
             false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p]() -> Result {
                    try {
                        auto d = std::make_shared<DeckDoc>();
                        if (hasText(p, "outline")) {
                            std::string base = fs::current_path().string();
                            if (!has(p, "outline") && has(p, "outline_file")) base = pathFromUtf8(stringParam(p, "outline_file")).parent_path().string();
                            d->data = deckFromOutline(textParam(p, "outline"), base);
                        }
                        if (stringParam(p, "aspect", "16:9") == "4:3") d->data.width = 720;
                        std::string error;
                        if (!applyTheme(d->data, p, error)) return Result::failure(error);
                        if (has(p, "title")) d->data.title = stringParam(p, "title");
                        if (has(p, "author")) d->data.author = stringParam(p, "author");
                        std::string warn;
                        for (std::size_t i = 0; i < d->data.slides.size(); ++i) {
                            if (!slideOverflow(d->data, i).empty()) warn += "; slide " + std::to_string(i + 1) + " has too much text";
                        }
                        return created(session, d, "created", warn);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"office.deck_open", "Open Slide Deck",
             "Open a deck: .pptx (titles, subtitles, bullets with levels, pictures, text boxes, shapes and lines, speaker notes, "
             "transitions, hidden slides, theme colours and font, the master's shapes, footer and slide numbers) or a Markdown "
             "outline (.md); .odp and .ppt through LibreOffice.",
             {str("path", "file to open", true)}, false, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([&session, p]() -> Result {
                    try {
                        auto d = std::make_shared<DeckDoc>();
                        d->path = stringParam(p, "path");
                        d->data = loadDeck(d->path);
                        return created(session, d, "opened");
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"office.deck_add_slide", "Add Slide",
             "Add a slide. Layouts: " + layouts + ". Without layout: the first slide of an empty deck with no bullets is a title "
             "slide, otherwise content (which puts a picture beside the bullets when image= is given). Long bullet lists shrink to "
             "fit; info warns when they no longer can.",
             join(join({doc()}, slideSpecs()), {integer("at", "insert at this position (default: last)"), previewSpec()}), true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                Slide s;
                if (!has(p, "layout")) s.layout = d.data.slides.empty() && !hasText(p, "bullets") && !has(p, "image") ? "title" : "content";
                std::string error;
                if (!applySlide(s, p, error)) return Result::failure(error);
                std::size_t at = d.data.slides.size();
                if (has(p, "at")) {
                    const std::int64_t a = intParam(p, "at");
                    if (a < 1 || a > static_cast<std::int64_t>(d.data.slides.size()) + 1) return Result::failure("at must be 1.." + std::to_string(d.data.slides.size() + 1));
                    at = static_cast<std::size_t>(a - 1);
                }
                d.data.slides.insert(d.data.slides.begin() + static_cast<std::ptrdiff_t>(at), std::move(s));
                Result r = Result::success("added slide " + describe(d.data, at), static_cast<std::int64_t>(at + 1));
                r.with("slide", static_cast<std::int64_t>(at + 1)).with("slides", static_cast<std::int64_t>(d.data.slides.size()));
                slidePreview(r, d.data, at, p);
                return r;
            }));

    reg.add({"office.deck_edit_slide", "Edit Slide", "Change a slide's layout, title, subtitle, bullets, right column, image or notes; fields left out stay.",
             join(join({doc(), integer("slide", "slide number", true)}, slideSpecs()), {flag("clear_image", "remove the picture", false), previewSpec()}), true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i;
                std::string error;
                if (!slideArg(d.data, p, "slide", i, error)) return Result::failure(error);
                if (!applySlide(d.data.slides[i], p, error)) return Result::failure(error);
                Result r = Result::success("slide " + describe(d.data, i));
                slidePreview(r, d.data, i, p);
                return r;
            }));

    reg.add({"office.deck_delete_slide", "Delete Slide", "Delete a slide.", {doc(), integer("slide", "slide number", true)}, true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i;
                std::string error;
                if (!slideArg(d.data, p, "slide", i, error)) return Result::failure(error);
                std::string what = describe(d.data, i);
                d.data.slides.erase(d.data.slides.begin() + static_cast<std::ptrdiff_t>(i));
                return Result::success("deleted slide " + what).with("slides", static_cast<std::int64_t>(d.data.slides.size()));
            }));

    reg.add({"office.deck_move_slide", "Move Slide", "Move a slide so it ends up at position `to`.",
             {doc(), integer("slide", "slide to move", true), integer("to", "its new position", true)}, true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i, j;
                std::string error;
                if (!slideArg(d.data, p, "slide", i, error) || !slideArg(d.data, p, "to", j, error)) return Result::failure(error);
                Slide s = d.data.slides[i];
                d.data.slides.erase(d.data.slides.begin() + static_cast<std::ptrdiff_t>(i));
                d.data.slides.insert(d.data.slides.begin() + static_cast<std::ptrdiff_t>(j), std::move(s));
                return Result::success("moved slide " + std::to_string(i + 1) + " to " + describe(d.data, j));
            }));

    reg.add({"office.deck_theme", "Deck Theme",
             "Change the colour theme and/or the font of the whole deck; accent=, background= and text_color= "
             "set single theme colours (after theme=). Title and section slides use the accent as their background.",
             {doc(), str("theme", themeHelp()), str("font", "font family", false, {}, {"sans", "serif", "mono"}), str("accent", "accent colour"),
              str("background", "background colour of content slides"), str("text_color", "body text colour"), previewSpec()},
             true},
            edit(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::string error;
                if (!has(p, "theme") && !has(p, "font") && !has(p, "accent") && !has(p, "background") && !has(p, "text_color"))
                    return Result::failure("give theme=, font=, accent=, background= or text_color=");
                if (!applyTheme(d.data, p, error)) return Result::failure(error);
                Result r = Result::success("theme " + d.data.theme.name + ": accent #" + lower(hexOf(d.data.theme.accent)) + ", background #" +
                                           lower(hexOf(d.data.theme.background)) + ", text #" + lower(hexOf(d.data.theme.text)));
                slidePreview(r, d.data, 0, p);
                return r;
            }));

    reg.add({"office.deck_hit", "Slide Part At Point",
             "What is at a spot on a slide (x, y in points from the top left; the slide is 960 x 540 at 16:9): the topmost text "
             "box, shape, line or picture (kind=object with its number), else the title, subtitle, bullets or right column "
             "(also where an empty one would go), with its box and its text the way deck_edit_slide / deck_object_edit take it. "
             "Lets a click on the slide edit that part.",
             {doc(), integer("slide", "slide number", true), number("x", "points from the left", true), number("y", "points from the top", true)},
             false, true},
            read(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                std::size_t i;
                std::string error;
                if (!slideArg(d.data, p, "slide", i, error)) return Result::failure(error);
                const SlideHit h = slideHit(d.data, i, numberParam(p, "x"), numberParam(p, "y"));
                if (h.kind.empty()) return Result::failure("nothing there on slide " + std::to_string(i + 1));
                Result r = Result::success(h.kind == "object" ? "object " + std::to_string(h.object) : h.kind);
                r.with("kind", h.kind).with("text", h.text).with("x", h.x).with("y", h.y).with("w", h.w).with("h", h.h);
                if (h.object) r.with("object", static_cast<std::int64_t>(h.object));
                // how big its words are and the slide's colours, so a window can type over the part in place
                double size = 0;
                for (const auto& op : layoutSlide(d.data, i).ops)
                    if (const auto* t = std::get_if<pdf::TextOp>(&op))
                        if (t->x >= h.x - 1 && t->x <= h.x + h.w + 1 && t->y >= h.y - 1 && t->y <= h.y + h.h + t->size) size = std::max(size, t->size);
                if (size <= 0) size = h.kind == "title" ? 40 : h.kind == "subtitle" ? 24 : 22;
                r.with("font_size", size).with("background", "#" + lower(hexOf(d.data.theme.background))).with("text_color", "#" + lower(hexOf(d.data.theme.text)));
                return r;
            }));

    reg.add({"office.deck_info", "Deck Info",
             "List the slides with their layouts, titles, bullet counts, pictures and notes, and warn about slides with too much text.",
             {doc(), fileSpec("slide deck"), flag("outline", "also return the whole deck as a Markdown outline", false)}, false, true},
            read(session, [](DeckDoc& d, const Params& p, const std::string& id) -> Result {
                std::string t = id + ": " + (d.data.title.empty() ? std::string("(no title)") : "\"" + d.data.title + "\"") + ", " +
                                std::to_string(d.data.slides.size()) + " slide(s), theme " + d.data.theme.name + ", " +
                                (d.data.width / d.data.height > 1.5 ? "16:9" : "4:3") + (d.path.empty() || d.path == id ? "" : ", " + d.path);
                if (!d.data.footer.empty() || !d.data.master.empty() || !d.data.slideNumbers) {
                    t += "\n  master: " + std::string(d.data.slideNumbers ? "slide numbers" : "no slide numbers") +
                         (d.data.footer.empty() ? "" : ", footer \"" + d.data.footer + "\"") +
                         (d.data.master.empty() ? "" : ", " + std::to_string(d.data.master.size()) + " object(s)") + (d.data.masterOnTitle ? ", also on title slides" : "");
                }
                for (std::size_t i = 0; i < d.data.slides.size(); ++i) t += "\n  " + describe(d.data, i);
                if (boolParam(p, "outline", false)) {
                    std::vector<MediaFile> media;
                    t += "\n\n" + deckToOutline(d.data, "media/", media);
                }
                return Result::success(t).with("doc", id).with("slides", static_cast<std::int64_t>(d.data.slides.size())).with("theme", d.data.theme.name);
            }));

    reg.add({"office.deck_save", "Save Slide Deck",
             "Save the deck: .pptx (PowerPoint, Keynote, Google Slides and LibreOffice open it), .pdf (one 16:9 page per slide), .md "
             "(outline; pictures go to <name>_media/); .odp/.ppt through LibreOffice. Without path it saves back to its own file.",
             {doc(), str("path", "output file (default: the deck's own path)")}, false, false},
            read(session, [](DeckDoc& d, const Params& p, const std::string& id) -> Result {
                const std::string path = has(p, "path") ? stringParam(p, "path") : d.path;
                if (path.empty()) return Result::failure(id + " has never been saved; give path=<file.pptx>");
                saveDeck(d.data, path);
                if (extOf(path) != "pdf") d.path = path;
                return Result::success("saved " + path + " (" + std::to_string(d.data.slides.size()) + " slides)", path).with("path", path);
            }));

    reg.add({"office.deck_export", "Export Slide Deck",
             "Export without changing the deck's own file: .pdf (notes=true: notes pages, the slide above its speaker notes), .pptx, "
             ".md, .html (a slide show to present from a browser, like office.deck_present), .odp/.ppt (LibreOffice), or .png/.jpg - one "
             "slide with slide=, otherwise every slide as name-1.png, name-2.png, ... Hidden slides are left out of .pdf and .html.",
             {doc(), fileSpec("slide deck"), str("path", "output file; the extension picks the format", true), integer("slide", "png: only this slide"),
              integer("width", "image width in pixels (png default 1280, html 1600)"),
              flag("notes", "pdf: notes pages with the speaker notes", false), flag("loop", "html: start again after the last slide", false)},
             false, false},
            read(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                const std::string path = stringParam(p, "path");
                const std::string ext = extOf(path);
                if (ext == "png" || ext == "jpg" || ext == "jpeg") {
                    if (d.data.slides.empty()) return Result::failure("the deck has no slides");
                    const double width = static_cast<double>(std::clamp<std::int64_t>(intParam(p, "width", 1280), 64, 7680));
                    auto encode = [&](std::size_t i) {
                        raster::Image img = pdf::renderPage(layoutSlide(d.data, i), width / d.data.width);
                        return ext == "png" ? raster::encodePng(img) : raster::encodeJpeg(img, 90);
                    };
                    if (has(p, "slide")) {
                        std::size_t i;
                        std::string error;
                        if (!slideArg(d.data, p, "slide", i, error)) return Result::failure(error);
                        writeBytes(path, encode(i));
                        return Result::success("exported slide " + std::to_string(i + 1) + " to " + path, path).with("path", path);
                    }
                    fs::path base = pathFromUtf8(path);
                    std::string first;
                    for (std::size_t i = 0; i < d.data.slides.size(); ++i) {
                        fs::path out = base.parent_path() / (base.stem().string() + "-" + std::to_string(i + 1) + base.extension().string());
                        writeBytes(out.string(), encode(i));
                        if (first.empty()) first = out.string();
                    }
                    return Result::success("exported " + std::to_string(d.data.slides.size()) + " slides as " + base.stem().string() + "-1" +
                                               base.extension().string() + " ... " + base.stem().string() + "-" + std::to_string(d.data.slides.size()) +
                                               base.extension().string(),
                                           first)
                        .with("files", static_cast<std::int64_t>(d.data.slides.size()));
                }
                if (ext == "html" || ext == "htm") {
                    writeBytes(path, writeSlideShow(d.data, static_cast<int>(has(p, "width") ? intParam(p, "width") : 1600), boolParam(p, "loop", false)));
                    return Result::success("exported a slide show to " + path + " (open it in a browser; F full screen, P presenter window, N notes)", path)
                        .with("path", path);
                }
                if (ext == "pdf" && boolParam(p, "notes", false)) {
                    auto pages = layoutNotesPages(d.data);
                    writeBytes(path, pdf::writePdf(pages, {d.data.title, d.data.author, "speaker notes"}));
                    return Result::success("exported notes pages to " + path + " (" + std::to_string(pages.size()) + " pages)", path).with("path", path);
                }
                saveDeck(d.data, path);
                return Result::success("exported " + path + " (" + std::to_string(d.data.slides.size()) + " slides)", path).with("path", path);
            }));

    reg.add({"office.deck_preview", "Preview Slides", "Render a slide (or all=true for every slide) as PNG, exactly as the PDF export draws it.",
             {doc(), fileSpec("slide deck"), integer("slide", "slide number", false, std::int64_t{1}), flag("all", "attach every slide", false),
              integer("width", "pixels", false, std::int64_t{960})},
             false, true},
            read(session, [](DeckDoc& d, const Params& p, const std::string&) -> Result {
                if (d.data.slides.empty()) return Result::failure("the deck has no slides");
                const double width = static_cast<double>(std::clamp<std::int64_t>(intParam(p, "width", 960), 64, 3840));
                Result r = Result::success("");
                if (boolParam(p, "all", false)) {
                    for (std::size_t i = 0; i < d.data.slides.size() && i < 60; ++i) r.attach(slidePng(d.data, i, width));
                    r.message = "previews of " + std::to_string(std::min<std::size_t>(d.data.slides.size(), 60)) + " slides";
                    return r.with("slides", static_cast<std::int64_t>(d.data.slides.size()));
                }
                std::size_t i;
                std::string error;
                Params q = p;
                if (!has(p, "slide")) q["slide"] = std::int64_t{1};
                if (!slideArg(d.data, q, "slide", i, error)) return Result::failure(error);
                r.message = "slide " + describe(d.data, i);
                r.attach(slidePng(d.data, i, width));
                // so a click on the picture can be turned into slide points (deck_hit)
                return r.with("slides", static_cast<std::int64_t>(d.data.slides.size())).with("points_wide", d.data.width);
            }));

    reg.add({"office.deck_close", "Close Slide Deck", "Close a deck without saving (undo reopens it).", {doc()}, true},
            [&session](const Params& p) -> std::unique_ptr<Action> {
                std::string id, error;
                if (!findDoc<DeckDoc>(session, p, id, error)) return std::make_unique<FunctionAction>([error] { return Result::failure(error); });
                return std::make_unique<CloseAction>(session, id);
            });

    deckcmd::registerDeckObjectCommands(session);
    deckcmd::registerDeckShowCommands(session);
}

namespace deckcmd {

ActionFactory edit(Session& s, Edit fn) { return office::edit(s, std::move(fn)); }
ActionFactory read(Session& s, Edit fn) { return office::read(s, std::move(fn)); }
ParamSpec doc() { return office::doc(); }
bool slideArg(const Deck& d, const Params& p, const char* key, std::size_t& index, std::string& error) {
    return office::slideArg(d, p, key, index, error);
}
std::string describe(const Deck& d, std::size_t index) { return office::describe(d, index); }
void slidePreview(Result& r, const Deck& d, std::size_t index, const Params& p) { office::slidePreview(r, d, index, p); }

}  // namespace deckcmd

}  // namespace wetwaresuite::office
