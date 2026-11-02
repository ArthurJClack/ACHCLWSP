// Internal: the window's state and panels. Only window.cpp and the panel
// files include this; apps use gui.hpp.
#pragma once

#include <algorithm>
#include <SDL3/SDL.h>

#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "wetwaresuite/gui/gui.hpp"
#include "wetwaresuite/gui/handoff.hpp"
#include "wetwaresuite/gui/logic.hpp"
#include "wetwaresuite/gui/palette.hpp"

namespace wetwaresuite::gui {

// Pushes the default font at exactly `px` screen pixels (the UI's own font scale undone), so text
// typed over a page or slide is the size the page draws it. Pop with ImGui::PopFont().
inline void pushFontPx(float px) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float scale = st.FontScaleMain * st.FontScaleDpi;
    ImGui::PushFont(nullptr, scale > 0 ? px / scale : px);
}

// Adds one typed character (a code point) to UTF-8 text.
inline void appendUtf8(std::string& out, unsigned c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// An options-bar label in front of its control, the way Photoshop's bar reads ("Size [12 px]").
// Call it before SetNextItemWidth, and give the control a "##" label.
inline void barLabel(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(0, ImGui::GetStyle().ItemInnerSpacing.x);
}

// An RGBA texture on the renderer, remade when its picture changes.
struct Texture {
    SDL_Texture* tex = nullptr;
    int width = 0, height = 0;       // texture size
    int srcWidth = 0, srcHeight = 0;  // size of the picture it shows (can be larger than the texture)
    void reset();
    bool upload(SDL_Renderer* renderer, const std::uint8_t* rgba, int w, int h);
    ImTextureID id() const { return static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(tex)); }
};

enum class CanvasTool { Move, Select, Transform, Type, Brush, Eraser, Fill, Heal, Remove, Clone, Dodge, Burn, Sponge, Smudge, Blur, Pick, Anchor, VecRect, VecEllipse, VecLine, VecPen, VecText, VecGradient, VecBuilder,
                        BoardSelect, BoardNote, BoardShape, BoardEllipse, BoardText, BoardConnect, BoardPen, GlyphEdit, GlyphDraw };
enum class SelectShape { Rect, Ellipse, Lasso, Wand };

// Click tools for game levels (levels.*) and pixel-art sprites (forge.sprite_*), game_view.cpp.
enum class GameTool { Move, Paint, Rect, Erase, Fill, Object, Pick, Pencil, Eraser, Line, Box, Ellipse, Bucket, Select, Translate, Rotate, Scale, Terrain, Stamp, ObjSelect };

// A Godot-style scene open on the canvas (scene_view.cpp): the tree, the selected node's
// inspector, and the fixed 2D view (or 3D orbit) the picture was drawn with.
struct SceneCanvas {
    std::vector<std::string> paths, types;
    int dims = 2;
    std::string selected = ".";
    bool viewSet = false;               // 2D: the world view stays put once fitted, so dragging doesn't jump
    double originX = 0, originY = 0, scale = 1;
    int viewW = 0, viewH = 0;
    bool haveBox = false;               // the selected node's world box
    double box[4] = {0, 0, 0, 0};
    double angle = 35, tilt = 25;       // 3D orbit
    bool camera = false, helpers = true;
    // inspector
    std::string inspectedFor;           // node + change count it was read at
    std::string type, script, groups;
    std::vector<std::string> names, values, kinds, sections, choices, edits;
    char rename[128] = {};
    int addClass = 0, scriptKind = 0;
    std::vector<std::string> classes2, classes3;
};

struct GameCanvas {
    std::string doc, kind;       // the document the state is for; kind "level" or "sprite"
    std::uint64_t seen = ~0ull;  // Shell change count the info was read at
    GameTool tool = GameTool::Move;
    bool dragging = false;
    ImVec2 dragFrom{0, 0}, dragTo{0, 0};  // picture px
    std::vector<double> points;           // level: cells x,y; sprite: pixels x,y
    std::string error;
    // level
    int mapW = 0, mapH = 0, tileW = 16, tileH = 16;
    std::vector<std::string> layers, layerKinds, tilesets;
    int layer = 0, tileset = 0, tile = 0;
    Texture tilesetTex;
    std::string tilesetFor;  // doc/tileset/change the texture shows
    int tsColumns = 1, tsTiles = 0, tsTileW = 16, tsTileH = 16;
    bool grid = true;
    std::string orientation = "orthogonal";  // isometric, staggered and hexagonal maps pick cells through levels.pick
    int pixW = 0, pixH = 0;                  // the whole map in px
    std::vector<std::vector<double>> outlines;  // non-square maps: outlines of the cells a stroke has touched
    std::vector<double> hoverOutline;
    int hoverX = -1, hoverY = -1;
    ImVec2 hoverAt{-1, -1};
    int brushW = 1, brushH = 1;  // a block of tiles picked in the tileset: Paint stamps it
    bool tsDragging = false;
    int tsFrom = 0;
    bool haveStamp = false;      // Stamp: the map region copied by the first drag
    int stampX = 0, stampY = 0, stampW = 1, stampH = 1;
    std::string stampLayer;
    struct LevelObject {
        int id = 0;
        double box[4] = {0, 0, 0, 0};  // px on the map
        std::string name, type, layer;
    };
    std::vector<LevelObject> objects;
    int selObject = 0;               // object id, 0 = none
    int objDrag = 0;                 // 1 move, 2 resize
    ImVec2 objFrom{0, 0};
    char objName[96] = {}, objType[96] = {}, objKey[64] = {}, objValue[128] = {};
    int objFor = -1;
    // sprite
    int frame = 0, frames = 1, spriteLayer = 0, width = 0, height = 0;
    std::vector<std::string> spriteLayers, palette, tags, tagRanges;
    std::vector<bool> layerShown;
    std::vector<int> durations;
    bool onion = false, playing = false, filled = false;
    double playClock = 0;
    float color[4] = {0, 0, 0, 1};
    std::vector<std::uint8_t> picture;  // the frame on the canvas, RGBA, for the colour picker
    int pictureW = 0, pictureH = 0;
    // scene
    SceneCanvas scene;
};

// A cad sketch open for drawing with the mouse (sketch_view.cpp). Geometry comes from
// cad.sketch_geometry in sketch units; the view is a centre and pixels per unit.
struct SketchEdit {
    enum class Tool { Line, Rect, Circle, Select, Dimension };
    std::string doc, sketch;
    bool active = false;
    std::uint64_t seen = ~0ull;
    std::string status, units = "mm";
    NumberList lines, construction, points, free, profiles;
    Tool tool = Tool::Line;
    double cx = 0, cy = 0, ppm = 8;
    bool fit = true;
    bool drawing = false;
    double ax = 0, ay = 0;          // where the shape started
    double chainX = 0, chainY = 0;  // a line chain's first point: clicking it closes the shape
    ImVec2 pressAt{0, 0};
    // where the sketch sits in the part (sketch x, y = origin + x*u + y*v), so the part can be drawn
    // behind it, seen straight on
    double origin[3] = {0, 0, 0}, u[3] = {1, 0, 0}, v[3] = {0, 1, 0}, n[3] = {0, 0, 1};
    Texture backdrop;
    std::string backdropKey;  // view and change count it was drawn for
    // the outline of the face it was started on, in sketch units (x1,y1,x2,y2,...): drawn faintly and snapped to
    NumberList faceEdges;
    std::vector<double> face3d;  // the same outline in the part, x,y,z per point, count first per loop
    std::string onBody;          // the body whose face it is on: what its extrude joins or cuts
    // for picking: points by id, entities and their outlines, and the dimensions written on the sketch
    std::vector<std::string> pointIds;
    NumberList pointXy;
    std::vector<std::pair<std::string, std::string>> entities;  // id, kind
    std::vector<NumberList> outlines;                           // x,y pairs per entity
    struct Dim {
        std::string id, type, value;
        double x = 0, y = 0, measured = 0;
        double line[4] = {0, 0, 0, 0};
        ImVec2 boxMin{0, 0}, boxMax{0, 0};  // where its label was drawn, for clicks
    };
    std::vector<Dim> dims;
    // Select: dragging a point (cad.sketch_move tried on the sketch as it goes)
    std::string dragPoint;
    double dragX = 0, dragY = 0, triedX = 1e300, triedY = 1e300, movedAt = 0;
    // Dimension: the first point clicked, and the dimension being typed (a new one, or an edit)
    std::string dimFirst;
    std::string editId, editType, editRefs;
    char editBuf[128] = {0};
    bool editOpen = false;
};

// A point or direction in a 3D view (handles.cpp).
struct P3 {
    double x = 0, y = 0, z = 0;
    P3 operator+(const P3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    P3 operator-(const P3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    P3 operator*(double s) const { return {x * s, y * s, z * s}; }
};

// An arrow drawn on the 3D view: drag it to slide something along dir (handles.cpp).
struct Handle {
    // Blend: fillet/chamfer size on picked edges; Ring / Scale: a model selection's rotate rings and scale boxes
    enum class Kind { PushPull, Move, Field, Blend, Ring, Scale } kind = Kind::PushPull;
    P3 base, dir;           // world: where it starts, which way it points (unit length)
    ImU32 colour = 0;
    std::string axis;       // Move: x, y or z
    std::size_t field = 0;  // Field: the form field it sets
    double start = 0;       // Field: the field's value when the drag began
    double scale = 1;       // Field: value per model unit along dir (-1 for a field that grows the other way)
    const char* tip = "";
};

// Working on a cad part or a model in the 3D view (handles.cpp): the camera of the last picture,
// the face under the mouse and the one clicked, and the arrows on them.
struct View3d {
    std::string doc;
    ViewCamera cam;
    bool camOk = false;
    int camW = 0, camH = 0;
    // cad: the face under the mouse, and the face clicked (its body, outline, normal)
    struct Face {
        bool hit = false, flat = false;
        std::string body, face, side;
        P3 point, normal, centre;
        double area = 0;
        double bounds[6] = {0, 0, 0, 0, 0, 0};
        std::vector<std::vector<P3>> loops;
    };
    Face hover, picked;
    // cad: a sharp edge under the mouse (with the edges carrying on from it), and the ones clicked
    struct Edge {
        bool hit = false, outer = true;
        std::string body, near;     // near: x,y,z of each piece's middle, for cad.fillet near=
        std::vector<P3> segments;   // pairs of ends
        P3 mid, out;                // out: away from the part, between the two faces
    };
    Edge hoverEdge;
    std::vector<Edge> edges;
    bool chamfer = false;  // the picked edges' arrow bevels instead of rounding
    ImVec2 hoverAt{-1e9f, -1e9f};
    std::uint64_t hoverSeen = ~0ull;
    bool moveMode = false;  // the clicked face's arrows move its body instead of pushing/pulling the face
    // model: the selection's middle
    std::uint64_t pivotSeen = ~0ull;
    bool pivot = false;
    P3 pivotAt;
    // the arrows shown, the one under the mouse and the one being dragged
    std::vector<Handle> handles;
    int hot = -1, dragging = -1;
    ImVec2 dragFrom{0, 0};
    double amount = 0;       // how far it has been dragged along dir, in model units
    double tryAmount = 0;    // what the trial on the file shows (see Trial)
    double movedAt = 0;      // seconds: when the amount last changed
    std::uint64_t pickedSeen = ~0ull;  // changes_ when the face was picked
    std::string formKey;  // the form's sketch its arrow was placed for
    P3 formBase, formNormal;
    bool formHave = false;
    std::vector<std::vector<P3>> formLoops;  // the profiles being extruded, in the part
};

// A command run on the file only to show what it would do: undone again when its settings change,
// when something else is done, or when the form closes; kept when Apply is pressed (preview.cpp).
struct Trial {
    bool applied = false;
    std::string command, doc, key;  // key: the command and its settings
    std::size_t depth = 0;          // history steps right after it ran, so only it gets undone
    bool wasUnsaved = false;
    Result result;
    bool running = false;           // inside its own run()
};

// Live previews of forms: the settings run on the file as they change (preview.cpp).
struct LivePreview {
    bool on = true;
    std::string pendingKey;
    double changedAt = 0;
    std::string note;  // why the last preview failed
    std::set<std::string> slow;  // commands that took too long to preview by themselves
    bool runNow = false;
};

// Layout pages (layout_view.cpp): the page shown, its frames (mm boxes), the picked one, a drag
// and the text frame being typed into.
struct LayoutView {
    std::string doc;
    int page = 1, pages = 1;
    double pageW = 210, pageH = 297;
    std::uint64_t seen = ~0ull;
    std::vector<std::string> ids, types;
    std::vector<ImVec4> boxes;
    int selected = -1, handle = -1;
    bool dragging = false;
    ImVec2 pressAt{0, 0};
    ImVec4 start{0, 0, 0, 0}, live{0, 0, 0, 0};
    bool editing = false, focus = false;
    std::string text, original, editFrame;
    int generation = 0;
};

// Slides (deck_view.cpp): which slide is shown big, the strip of thumbnails, the part of the
// slide being typed into and an object being dragged. Boxes are in slide points (x, y, w, h).
struct DeckView {
    std::string doc;
    int slide = 1, slides = 0;
    double pointsWide = 960;
    std::uint64_t seen = ~0ull;  // Shell change count the thumbnails were drawn at
    std::string thumbsFor;
    std::vector<Texture> thumbs;
    bool scrollToSlide = false;
    // hover
    ImVec2 hoverAt{-1, -1};
    std::string hoverKind;
    int hoverObject = 0;
    ImVec4 hover{0, 0, 0, 0};
    bool hoverEmpty = false;
    // press / drag
    std::string pressKind, pressText;
    int pressObject = 0;
    float pressFont = 0;
    ImVec4 pressBg{1, 1, 1, 1}, pressFg{0, 0, 0, 1};
    ImVec4 press{0, 0, 0, 0};
    ImVec2 pressAt{0, 0};
    bool moving = false;
    // the part being edited
    bool editing = false, focus = false;
    std::string kind, text, original;
    int object = 0, generation = 0;
    ImVec4 box{0, 0, 0, 0};
    float fontSize = 24;               // slide points
    ImVec4 bg{1, 1, 1, 1}, fg{0, 0, 0, 1};  // the slide's colours, so the box looks like the slide
};

// A retouch command waiting to run: it runs on the frame after it is queued, so the
// window first shows "Working..." over the canvas.
struct PendingRetouch {
    std::string command;
    std::string title;
    Params params;
    bool shown = false;  // a frame with the busy sign has been drawn
};

struct Canvas {
    Texture texture;
    std::string shownDoc;
    bool dirty = true;
    bool fit = true;  // fit to the view on the next frame
    float zoom = 1.0f;
    ImVec2 pan{0, 0};       // offset of the image's top-left from the view's top-left, in screen px
    ImVec2 viewMin{0, 0};   // last frame's view rectangle
    ImVec2 viewSize{0, 0};
    ImVec2 waveSize{0, 0};  // the view size a waveform was last drawn for
    std::string summary;  // for documents with no picture

    CanvasTool tool = CanvasTool::Move;
    std::string brush = "round";
    float color[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    float size = 12;
    float opacity = 100;
    bool stroking = false;
    bool penErasing = false;
    bool usedPen = false;
    std::vector<double> points;     // image px, x,y pairs
    std::vector<double> pressures;  // one per point when a pen is used

    // retouching (retouch.cpp)
    HealSource healSource;       // Alt+click spot the Heal tool copies from
    bool pickingSource = false;  // "Pick source" is on: the next click picks the spot
    std::vector<double> removeMark;  // painted area waiting for "Remove", picture px
    float removeMarkSize = 0;
    std::string removeMarkDoc;
    bool autoRemove = false;  // remove as soon as the stroke ends
    std::optional<PendingRetouch> pending;
    // retouch brushes (brush_tools.cpp): clone, dodge/burn/sponge, smudge, blur/sharpen
    HealSource cloneSource;   // Alt+click spot the clone stamp copies from
    float strength = 50;      // exposure / strength 0..100
    std::string toneRange = "midtones";
    bool sharpen = false;     // the Blur tool sharpens instead

    // the 3D view (orbit.cpp), for documents that can draw themselves from any angle
    bool is3d = false;
    std::string shown3dDoc;
    OrbitView orbit;
    bool orbitDirty = true;
    bool orbitDragging = false;
    bool orbitSharpPending = false;
    // G / R / S in a model: move, rotate or scale the selection with the mouse (model.drag)
    char grab = 0;             // 'G', 'R', 'S' while dragging, 0 otherwise
    bool grabApplied = false;  // a model.drag of this grab is in the history (undone before the next one)
    ImVec2 grabStart{0, 0};
    std::string grabAxis;      // "", "x", "y" or "z"
    bool grabEndClick = false; // the click that kept it isn't also a click to select
    bool grabByHandle = false; // started by dragging an arrow: letting go keeps it
    // started from a rotate ring or a scale box: the pivot and the axis's direction on screen
    bool grabExact = false;
    ImVec2 grabCentre{0, 0}, grabDir{1, 0};
    float grabFacing = 1;      // rings: +1 when the axis points at the viewer
    bool grabCtrl = false, grabAlt = false;  // snapping keys last frame
    // B in a model's edit mode: the next drag draws a box that selects
    bool boxArmed = false, boxDragging = false;
    ImVec2 boxFrom{0, 0};
    int orbitW = 0, orbitH = 0;

    // selecting (selection_view.cpp)
    SelectShape selectShape = SelectShape::Rect;
    float tolerance = 32;
    bool contiguous = true;
    bool selecting = false;
    ImVec2 selectStart{0, 0};          // picture px
    std::vector<double> lasso;         // picture px, x,y pairs
    int version = 0;                   // goes up whenever the picture is refreshed
    std::string antsDoc;
    int antsVersion = -1;
    int antsStep = 1;                  // picture px per mask cell the outline was traced on
    std::vector<float> ants;           // selection outline, x0,y0,x1,y1 in picture px

    // vector drawings (vector_view.cpp): the picked shape, its box and anchors, in drawing units
    std::string vecSel;
    std::uint64_t vecLoaded = ~0ull;  // changes_ when the box and anchors were read
    bool vecHasBox = false;
    float vecBox[4] = {0, 0, 0, 0};  // x, y, w, h
    std::vector<float> vecNodes;      // per anchor: x, y, in x, in y, out x, out y
    int vecDrag = 0;                  // 0 none, 1 move, 2 scale handle, 3 anchor part, 4 drawing a new shape
    int vecHandle = 0;                // scale: handle 0..7; anchor: node * 3 + part (0 anchor, 1 in, 2 out)
    ImVec2 vecStart{0, 0}, vecNow{0, 0};
    std::vector<ImVec2> penPoints;   // Pen: the anchors clicked so far, in drawing units
    std::vector<ImVec2> penIn, penOut;  // Pen: each anchor's handles (equal to the anchor = a corner)
    std::vector<float> guideBoxes;    // smart guides: the other shapes' boxes (x, y, w, h), read when a move starts
    float guideX = NAN, guideY = NAN; // the guide lines shown while dragging (drawing units), NAN = none
    std::vector<double> builderPts;   // Shape builder: the points the drag crossed
    float color2[4] = {1, 1, 1, 1};   // Gradient: the end colour
    bool textOpen = false;           // Text: a box is waiting for words at textAt
    ImVec2 textAt{0, 0};
    std::string textDraft;

    GameCanvas game;
};

// What the in-place document editor's text callback reads and writes (canvas.cpp).
struct DocEditAccess {
    int* caret;     // caret position to set once, -1 none
    char* pending;  // 'b', 'i', 'u' formatting to apply to the selection
    int* cursor;    // where the caret is now
};

// A whiteboard on the canvas (board_view.cpp), in board units.
struct BoardView {
    std::string doc;
    std::uint64_t seen = ~0ull;
    double x = 0, y = 0, w = 1, h = 1, scale = 1;  // what the picture shows (board.view)
    bool known = false;
    std::string sel;              // picked element
    std::string selType;
    float box[4] = {0, 0, 0, 0};  // its box
    int drag = 0;                 // 1 move, 2 resize (corner in handle), 3 new shape box, 4 connector, 5 ink
    int handle = 0;
    ImVec2 start{0, 0}, now{0, 0};
    std::string from;             // connector start element
    std::vector<double> ink;      // ink points
    bool editing = false;         // typing on sel
    std::string text;
    bool focus = false;
    bool fillChosen = false;      // new shapes take the theme's colours until a colour is picked
};

// The glyph editor for fonts (glyph_view.cpp), in font units (y up).
struct GlyphPoint {
    float x = 0, y = 0;
    char type = 'o';  // o on-curve, c cubic control, q quadratic control
};
struct GlyphView {
    std::string doc;
    std::string glyph = "A";
    std::string input = "A";
    std::uint64_t seen = ~0ull;
    bool exists = false;
    float advance = 600, ascender = 800, descender = -200, xHeight = 500, capHeight = 700;
    std::vector<std::vector<GlyphPoint>> contours;
    int dragC = -1, dragP = -1;
    ImVec2 dragStart{0, 0}, dragNow{0, 0};
    std::vector<ImVec2> drawing;  // Draw tool: corners clicked so far
    bool cut = false;
    float scale = 1, ox = 0, oy = 0;  // screen = (ox + x * scale, oy - y * scale)
};

// A timeline's moving picture while it plays (video_play.cpp): frames drawn on a worker
// thread by the document's FramePlayer, newest request first.
class VideoPlayback {
public:
    VideoPlayback(std::string doc, std::unique_ptr<FramePlayer> player, int maxWidth);
    ~VideoPlayback();  // stops the worker
    VideoPlayback(const VideoPlayback&) = delete;
    VideoPlayback& operator=(const VideoPlayback&) = delete;
    const std::string& doc() const { return doc_; }
    // the frame wanted now; fast = a quick, possibly nearby frame (dragging the playhead)
    void request(double seconds, bool fast = false);
    // A frame drawn since the last call: true with its pixels.
    bool take(std::vector<std::uint8_t>& rgba, int& width, int& height);
    bool pending();  // a frame is wanted or drawn but not taken yet

private:
    void work();
    std::string doc_;
    std::unique_ptr<FramePlayer> player_;
    int maxWidth_;
    std::mutex lock_;
    std::condition_variable wake_;
    double want_ = -1, drawn_ = -1;
    bool wantFast_ = false, drawnFast_ = false;
    bool stop_ = false, fresh_ = false;
    std::vector<std::uint8_t> rgba_;
    int width_ = 0, height_ = 0;
    std::thread worker_;
};

// Thumbnails and a waveform for one timeline clip, from Document::clipMedia (timeline_view.cpp).
struct ClipStrip {
    MediaStrip media;
    std::vector<Texture> frames;
};

// An export running in the background (<tool>.render background=true), polled for progress.
struct ExportJob {
    std::string tool, id, output;
    double progress = 0;
    std::uint64_t polledAt = 0;  // SDL ticks
};

// The play bar's view of playback (transport.cpp), read back from <tool>.playhead.
struct Transport {
    std::string tool;  // audio, beats or cut
    std::string doc;   // the document the window last started
    std::string source;
    bool playing = false, paused = false, loop = false, silent = false;
    double position = 0, length = 0, start = 0, end = 0;
    double cue = 0;  // where Play starts when nothing is playing
    float volumeDb = 0;
};

// The timeline panel's own state (timeline_view.cpp).
struct TimelineState {
    std::string doc;
    double zoom = 0;    // px per second, 0 = fit
    double scroll = 0;  // seconds at the left edge
    double cursor = 0;  // where Split cuts
    std::string selTrack, selClip;
    enum class Drag { None, Cursor, Move, TrimStart, TrimEnd } drag = Drag::None;
    double grabT = 0, origStart = 0, origLength = 0, headroom = -1, tailroom = -1;
};

// The cell grid shown for spreadsheet documents (grid_view.cpp).
struct GridState {
    std::string doc;
    int sheet = -1;  // -1: start on the document's active sheet
    int row = 0, col = 0;
    int row2 = 0, col2 = 0;   // the other corner of the selected range (== row, col for one cell)
    bool selecting = false;   // dragging out a range
    bool filling = false;     // dragging the fill handle
    int fillRow = 0, fillCol = 0;  // where the fill handle is dragged to
    bool focusGrid = false;        // the grid has the keyboard (Ctrl+B/I/U style the range)
    ImVec4 fillPick{1, 1, 1, 1}, inkPick{0, 0, 0, 1};  // the format bar's colour pickers while open
    bool editing = false;
    bool focusEdit = false;   // give the cell editor keyboard focus next frame
    bool caretToEnd = false;  // started by typing: keep the typed text, caret after it
    bool scrollToSel = false;
    bool textInput = false;  // we turned SDL text input on
    int editCount = 0;       // each edit gets a fresh editor, so the last one's text can't come back
    std::string buffer;   // the cell editor's text
    std::string formula;  // the formula bar's text
    int formulaRow = -1, formulaCol = -1, formulaSheet = -1;
};

// Clicking on a circuit document (panel_circuit.cpp): the tool and a wire/trace being drawn.
struct CircuitCanvas {
    std::string doc, view = "schematic";  // schematic, pcb or 3d
    std::string tool = "select";          // select, place, wire, move, delete, trace, via
    std::string symbol = "resistor", value, layer = "F";
    bool hasStart = false;  // the first click of a wire or trace
    double startX = 0, startY = 0;  // model point (grid units or mm)
    ImVec2 startPx{0, 0};           // picture px
    std::string moving;             // part picked up by the move tool
};

// Clicking on a map document (maps_view.cpp): the tool, points being drawn, a feature being moved.
struct MapsCanvas {
    std::string doc;
    std::string tool = "identify";  // pan, identify, measure, point, line, area, move, delete, route
    std::string layer = "drawn", props, profile = "car", search, bookmark;
    std::vector<double> points;     // lat,lon pairs clicked so far
    std::vector<ImVec2> pointsPx;   // the same, picture px
    std::string movingLayer;        // move tool: what was picked up
    std::int64_t movingIndex = -1, movingVertex = -1;
    ImVec2 movingFrom{0, 0};
};

// Where a native file dialog's answer goes.
struct ToolPanels;  // tool_panels.hpp

struct DialogTarget {
    enum Kind { OpenDocument, FormField, PanelFolder, SaveDocument, AddMedia } kind = OpenDocument;
    std::string command;  // FormField: the form it belongs to; PanelFolder: what the folder is for; SaveDocument: the save command; AddMedia: the timeline
    std::size_t field = 0;
    std::string doc;  // SaveDocument: the file to save
};

class Shell {
public:
    Shell(Session& session, const AppInfo& app, SDL_Window* window, SDL_Renderer* renderer);
    ~Shell();

    void applyStyle(float dpiScale);
    void frame();                         // one ImGui frame of every panel
    void onEvent(const SDL_Event& event);  // raw input for strokes, before ImGui sees it
    void openFile(const std::string& path);
    void followWikiLink(const std::string& name);
    // a command's form, or "settings" for the Aa menu (text size and colours)
    void showAction(const std::string& commandId) {
        if (commandId == "settings") { openSettings_ = true; return; }
        select(commandId);
        openPanelFor(commandId);
    }
    bool wantsQuit() const { return quit_; }
    // the window's close button or Ctrl+Q: true when nothing unsaved stands in the way, else it asks
    bool requestQuit();
    int tickMs() const;  // > 0: wake the window this often (timers, running programs, live graphs)
    bool animating() const { return animating_; }  // something on screen moves (the selection outline)
    // true once after the window's state changed (a file made, opened, closed or shown), so the
    // loop draws a few more frames: new tabs and views take a frame or two to settle
    bool settling();

    static Uint32 wakeEvent;  // pushed from dialog threads to wake the event loop
    static ImFont* monoFont;  // a fixed-width font for code and terminals (null: the UI font)

private:
    // the window's frame (chrome.cpp): menu bar, ribbon, file tabs, tool rail,
    // properties tabs and status bar, laid out per app like the tool it replaces
    void menuBar();
    void fileMenu();
    void editMenu();
    void viewMenu();
    void toolMenu(const std::string& tool);
    void helpMenu();
    void searchBox();
    void ribbon();
    void docTabs();
    void toolRail();
    bool railItem(const char* label, const char* key, const char* tip, bool on);  // a rail button; its letter picks it too
    void railTools();     // the current document's click tools (canvas.cpp)
    void canvasTools();
    void circuitTools();  // panel_circuit.cpp
    void mapsTools();     // maps_view.cpp
    void gameTools();     // game_view.cpp
    void propertiesPanel();
    void statusBar();
    void shortcuts();
    void shortcutsPopup();
    void rootPopups();
    void pick(const std::string& commandId);  // a menu or ribbon button: runs at once, or shows its settings first
    bool runOnDoc(const std::string& commandId, const std::string& doc);
    void saveDoc(bool askWhere);
    void askSavePath(const std::string& command, const std::string& doc, const std::string& start);
    void writeBar();
    ImVec2 vectorDrawEnd() const;
    void drawVectorDraft(ImDrawList* dl, ImVec2 p0) const;
    void penFinish(bool closed);
    void vectorPenKeys();
    void vectorTextBox(ImVec2 p0);
    void vectorDrawRelease();
    void saveDocTo(const std::string& command, const std::string& doc, const std::string& path);
    void closeDoc(const std::string& id);  // asks first when it has unsaved changes
    void closeDocNow(const std::string& id);
    void askSavePopup();
    void passwordPopup();
    struct PasswordAsk {
        bool open = false;
        std::string path, command, text, error;
        bool focus = false;
    } passwordAsk_;
    void noteChange(const std::string& id, const Params& params, const Result& r);
    void showDoc(const std::string& id);
    void nextTab(int step);
    std::string docOpener(const std::string& id) const;
    bool hasLayers() const;   // layers_view.cpp
    bool hasEffects() const;  // sound_view.cpp

    // panels (panels.cpp)
    void rightPanel();
    void homeView();
    void documentView();
    void formPanel();
    void historyPanel();
    void resultView();
    void searchPopup();
    void newPopup();
    void settingsPopup();
    void coloursSettings();  // colours.cpp
    void updatesSettings();  // updates.cpp, through the app.update_* commands
    void readUpdates(bool now);
    void runUpdate(const std::string& command, const Params& params);

    // the shared colour palette (colours.cpp), through the app.palette_* commands
    bool loadPalette();  // true when the colours changed
    void savePaletteNow();
    void runPalette(const std::string& command, const Params& params);
    bool darkLook() const { return custom_ ? theme_.dark : dark_; }
    void field(Field& f, std::size_t index);

    // canvas (canvas.cpp)
    void canvasView();
    void canvasArea();
    void canvasToolbar();
    void refreshCanvas();
    void beginStroke(ImVec2 screen, float pressure, bool pen, bool eraser);
    void addStrokePoint(ImVec2 screen, float pressure);
    void endStroke();
    ImVec2 toImage(ImVec2 screen) const;
    bool toolAvailable(CanvasTool tool) const;
    // circuit documents (panel_circuit.cpp): schematic / board / 3D, and click tools that run circuit.* commands
    bool circuitDoc() const;
    bool circuitEditing() const;  // a click tool is on, so a left drag doesn't pan
    void circuitToolbar();
    void circuitCanvas(ImDrawList* dl, ImVec2 origin, bool hovered, bool clicked);
    void circuitClick(ImVec2 picture);
    // map documents (maps_view.cpp): click tools that run maps.* commands, zoom, bookmarks and search
    bool mapsDoc() const;
    bool mapsEditing() const;  // a click tool is on, so a left drag doesn't pan
    void mapsToolbar();
    void mapsCanvas(ImDrawList* dl, ImVec2 origin, bool hovered, bool clicked);
    void mapsClick(ImVec2 picture);
    void mapsFinish();

    // the 3D view (orbit.cpp)
    bool probe3d(const Document& doc) const;
    void orbitToolbar();
    void orbitCanvas(ImDrawList* dl, ImVec2 origin, ImVec2 size, bool hovered, bool active);

    // retouching by painting (retouch.cpp): Heal and Remove
    void retouchSettings();
    bool retouchPress(ImVec2 screen);  // true when the click picked a heal source instead of starting a stroke
    void retouchRelease();
    void drawRetouch(ImDrawList* dl, ImVec2 origin, bool hovered);
    void applyRemove();
    void queueRetouch(const std::string& command, const std::string& title, Params params);
    void runPendingRetouch();
    bool pictureSize(int& width, int& height) const;
    // the retouch brushes (brush_tools.cpp) and the layers panel (layers_view.cpp)
    static bool brushTool(CanvasTool tool);
    static bool vectorDrawTool(CanvasTool tool) {
        return tool == CanvasTool::VecRect || tool == CanvasTool::VecEllipse || tool == CanvasTool::VecLine || tool == CanvasTool::VecPen ||
               tool == CanvasTool::VecText || tool == CanvasTool::VecGradient || tool == CanvasTool::VecBuilder;
    }
    static bool glyphTool(CanvasTool tool) { return tool == CanvasTool::GlyphEdit || tool == CanvasTool::GlyphDraw; }
    GlyphView glyph_;
    bool fontDoc() const;
    void glyphSync();
    void glyphSettings();
    void glyphCanvas(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active);
    static bool boardTool(CanvasTool tool) { return tool >= CanvasTool::BoardSelect && tool <= CanvasTool::BoardPen; }
    // whiteboards on the canvas (board_view.cpp): pick, move, resize, type on shapes, connect, ink
    BoardView board_;
    bool boardDoc() const;
    void boardSettings();
    void boardCanvas(ImDrawList* dl, ImVec2 p0, bool hovered, bool active);
    void boardSync();
    ImVec2 boardAt(ImVec2 screen) const;
    ImVec2 boardScreen(ImVec2 p0, float x, float y) const;
    void boardRun(const std::string& id, Params p, const char* title);
    void boardEditText(ImVec2 p0);
    void vectorGuides(float& dx, float& dy);
    void brushToolSettings();
    bool brushToolPress(ImVec2 screen);  // true when the click picked a clone source
    void runBrushTool();
    void drawBrushTool(ImDrawList* dl, ImVec2 origin, bool hovered);
    void layersPanel();
    void soundPanel();     // effects rack and envelope of audio projects and songs (sound_view.cpp)
    void pianoRollView();  // the notes of a song pattern (sound_view.cpp)

    // the play bar over sound documents (transport.cpp)
    bool playTarget(std::string& tool, Params& target) const;
    void transportBar();
    void pollTransport();
    void startPlaying(double from);
    void togglePlay();
    void seekTo(double seconds);
    // the timeline under video edits and audio projects (timeline_view.cpp)
    float timelineHeight(const TimelineLayout& layout) const;
    void timelineView(const TimelineLayout& layout);
    void runTimelineEdit(const TimelineLayout& layout, const TimelineEdit& edit);
    // spreadsheet grid (grid_view.cpp)
    void gridView(const CellGrid& layout);
    // the PDF editor (pdf_view.cpp)
    void pdfView();
    void pdfToolbar();
    void pdfThumbs(float width);
    void pdfPage();
    void pdfPopup();
    void pdfLoad(bool force);
    void pdfRun(const std::string& command, Params params, const std::string& title);
    void pdfRelease();
    void gridFormatBar(const CellGrid& layout);
    bool commitCell(const CellGrid& layout, int sheet, int row, int col, const std::string& input);
    // selections on the canvas (selection_view.cpp)
    void selectSettings();
    void selectPress(ImVec2 screen);
    void selectDrag(ImVec2 screen);
    void selectRelease(ImVec2 screen);
    void drawSelection(ImDrawList* dl, ImVec2 origin, bool hovered);
    void runSelect(Params params, const char* title);
    // vector drawings on the canvas (vector_view.cpp): pick, move, scale, anchors, align
    bool vectorDoc() const;
    void vectorSettings();
    void vectorPress(ImVec2 screen);
    void vectorDrag(ImVec2 screen);
    void vectorRelease();
    void vectorKeys();
    void drawVector(ImDrawList* dl, ImVec2 origin);

    // game levels and sprites on the canvas (game_view.cpp)
    bool gameActive() const;  // the current document is a level or sprite with its commands here
    void gameSync();          // re-read the document's layers, tilesets, frames, palette
    std::string gamePicture();  // PNG for the canvas, empty when not a game document
    void gameToolbar();
    void gameSide();
    void gameInput(ImDrawList* dl, ImVec2 origin, bool hovered, bool active);
    bool gamePainting() const;
    void gameRun(const std::string& id, Params params, const std::string& title);
    void gameLevelSide();
    void gameSpriteSide();
    void gameLevelRelease();
    void gameObjectInput(ImDrawList* dl, ImVec2 origin, bool hovered, bool active, float sx, float sy);
    void gameObjectPanel();
    void gameSpriteRelease();
    // Godot-style scenes on the canvas (scene_view.cpp)
    void sceneSync(bool newDoc);
    std::string scenePicture();
    void sceneTools();
    void sceneToolbar();
    void sceneSide();
    void sceneInput(ImDrawList* dl, ImVec2 origin, bool hovered, bool active);
    void sceneSelect(const std::string& path);

    // actions (shell.cpp)
    Result run(const std::string& id, const Params& params);
    void select(const std::string& commandId);
    void fillPicked(bool replace);  // the shape picked on the canvas goes into the form's shape box
    void runForm();
    void undo();
    void redo();
    void showOpenDialog();
    void showAddMediaDialog();  // files to put on the open timeline
    // A file dropped on the window: onto the open timeline when it takes it, else opened.
    void dropFile(const std::string& path);
    // the moving picture while a timeline plays (video_play.cpp); true when it drew the canvas
    bool videoPlaybackFrame();
    bool timelinePlaying() const;  // the current timeline is playing or paused
    // a timeline's still picture at its playhead, drawn on the worker too (false: not a timeline)
    bool timelineCanvas(const std::shared_ptr<Document>& doc);
    // the selected timeline clip's everyday settings as sliders (timeline_view.cpp); false when none
    bool clipPanel();
    void exportProgress();  // the bar and Cancel button of a background export
    void pollExport();
    const ClipStrip* clipStrip(const std::string& clipId);
    void showFieldDialog(std::size_t index);
    void drainDialogs();
    void loadRecent();
    void saveRecent();
    void syncDocs();
    std::vector<std::string> docIds() const;
    const CommandInfo* find(const std::string& id) const;
    bool accentButton(const char* label, ImVec2 size = {0, 0});

    // tool panels (tool_panels.cpp, panel_*.cpp): the calculator, code editor,
    // file manager, terminal, system monitor and clock fill the middle
    bool hasPanel(const std::string& tool) const;
    std::string panelForDoc(const std::string& id) const;
    void openPanel(const std::string& tool);
    void openPanelFor(const std::string& commandId);
    bool openFolder(const std::string& path);
    void toolPanelView();
    void backgroundTicks();
    void beforeHistoryStep();
    void askFolder(const std::string& purpose);
    void folderChosen(const std::string& purpose, const std::string& path);
    Result query(const std::string& id, const Params& params);  // polls: no history, no switching documents
    void calcPanel();
    void calcKeypad();
    void calcSide();
    void calcEvaluate();
    void calcGraph();
    void codePanel();
    void codeToolbar();
    void codeSidebar();
    void codeTree();
    void codeOutline();
    void codeSearch();
    void codeGit();
    void codeTabs();
    void codeFindBar();
    void codeOutput();
    void codeSync();
    void codeCommit();
    void codeOpenAt(const std::string& path, int line);
    void codeRun(const std::string& what);
    void codePoll();
    void codeFind(int step);
    void explorerPanel();
    void explorerToolbar();
    void explorerSide();
    void explorerList();
    void explorerPreview();
    void explorerPopups();
    void explorerTrash();
    void explorerArchive();
    void explorerGo(const std::string& folder, bool remember = true);
    void explorerOpen(const std::string& path, bool folder);
    void explorerRefresh();
    void terminalPanel();
    void terminalNew(const std::string& folder);
    void terminalPoll(bool force);
    void monitorPanel();
    void monitorSample();
    void clockPanel();
    void clockTick();
    void clockTimers();
    void clockStopwatch();
    void clockAlarms();
    void clockDates();
    void dueBanner();

    // handing work to other apps (handoff_panels.cpp), all through the app.* commands
    void refreshInbox();
    void sendPopup();
    void inboxBannerView();
    void suggestionBannerView();
    void launchOfferView();
    void suggestOtherApp(const std::string& path, bool openedHere);
    void sendTo(const std::string& app);
    void launchApp(const std::string& app, const std::string& path);

    // the launcher (launcher_view.cpp): a grid of apps, each started as its own program
    void launcherFrame();
    void launcherOpen(const std::string& path);
    void launcherStart(const std::string& app, const std::string& path);

    Session& session_;
    AppInfo app_;
    SDL_Window* window_;
    SDL_Renderer* renderer_;
    std::vector<std::string> tools_;
    std::vector<CommandInfo> commands_;  // this app's commands

    bool pro_ = false;
    bool dark_ = true;  // the Dark/Light choice for the app's own colours
    bool custom_ = false;  // a palette was picked: every colour comes from theme_
    Palette palette_;
    Theme theme_;
    bool paletteStale_ = false;    // re-read the palette on the next frame (the window got focus)
    bool palettePending_ = false;  // a picker changed it: save once the mouse is let go
    std::string paletteNote_;      // the last palette command's answer or error
    bool paletteNoteOk_ = true;
    bool openSettings_ = false;
    struct Updates {
        std::string version, status, note, why;
        bool on = true, canAuto = false;
    } updates_;
    double updatesRead_ = -10;
    ImVec4 accent_{0.24f, 0.49f, 1, 1}, accentHover_, accentActive_, accentText_{1, 1, 1, 1}, title_;
    float textScale_ = 1.0f;
    float dpi_ = 1.0f;
    bool quit_ = false;
    std::set<std::string> unsaved_;  // files changed since they were opened or saved
    std::string askClose_;           // the file the "save changes?" question is about
    std::string closeAfterSave_;     // close this file once its save goes through
    bool askQuit_ = false, openAskSave_ = false;
    bool animating_ = false;
    std::string settledKey_;
    std::string propsForm_;
    std::string writeText_, writeKind_ = "paragraph";  // the text document's write bar
    std::vector<std::string> madeNames_;  // features and layers the last actions made (sketch1...), newest last  // the form the properties tab last showed
    bool showHome_ = true;
    std::string currentDoc_;
    std::map<std::string, std::string> docTool_;  // doc id -> tool that made it
    std::map<std::string, std::string> docOpener_;  // doc id -> command that made it (office.sheet_open)

    // the frame (chrome.cpp)
    AppLayout layout_;
    std::vector<RibbonTab> ribbonTabs_;
    int ribbonPick_ = -1;          // select this ribbon tab on the next frame
    std::string ribbonFor_;        // the document the ribbon last picked its tab for
    bool showRibbon_ = true, showRail_ = true, showProps_ = true, showStatus_ = true;
    bool focusMode_ = false;       // Ctrl+Space: only the canvas
    int railCount_ = 0;            // rail buttons last frame (0: no rail)
    bool railWas_ = false;
    bool railDraw_ = false;        // railItem draws (false: only its shortcut letter works)
    std::string tabShown_;         // what the file tabs last showed: "" start, "panel:<tool>" or a doc id
    int tabForce_ = 0;             // frames to ignore the tab bar's own pick after we picked for it
    bool pickProps_ = false;       // bring the Properties tab forward
    bool pickHistory_ = false;
    bool openNew_ = false, openSend_ = false, openShortcuts_ = false;

    std::string query_;
    bool focusSearch_ = false;
    int searchPick_ = 0;
    ImVec2 searchMin_{0, 0}, searchMax_{0, 0};
    bool searchActive_ = false;
    bool searchHovered_ = false;
    std::string expandTool_;

    std::optional<Form> form_;
    std::optional<Result> result_;
    std::string resultFor_;
    bool scrollToResult_ = false;

    Canvas canvas_;
    Transport transport_;
    std::unique_ptr<VideoPlayback> video_;           // a timeline's picture: playing, scrubbed or still
    double scrubTime_ = -1;                          // where the playhead is being dragged (-1: it isn't)
    std::uint64_t scrubMovedAt_ = 0;                 // SDL ticks of the drag's last move
    bool liveVideo_ = false;                         // last frame was playing or scrubbing
    std::map<std::string, float> clipEdits_;         // clip panel sliders being dragged, applied on release
    std::map<std::string, ClipStrip> strips_;        // by doc + clip id
    bool stripsPending_ = false;                     // some clip's thumbnails are still being read
    ExportJob exportJob_;                            // an export running in the background
    float playheadFraction_ = -1;  // where to draw the playhead over a waveform, 0..1
    // a time range dragged over a waveform, as in Audacity: forms with start/end get it
    struct AudioSelection {
        std::string doc;
        double a = -1, b = -1;    // seconds; a < 0 = none
        bool dragging = false;
        double length = 0;        // the clip's length in seconds
        std::uint64_t seen = ~0ull;
        std::string lengthDoc;
        bool any() const { return a >= 0 && b > a; }
    } audioSel_;
    void audioCanvas(ImDrawList* dl, ImVec2 p0, ImVec2 p1, bool hovered, bool active);
    void fillAudioRange(bool replace);
    // documents: click a paragraph (or list, table...) on the page to change it in place
    struct DocBlockEdit {
        std::string doc;
        int block = 0;  // 0: nothing open
        std::string kind, text, original;
        float top = 0, bottom = 0;  // in the page picture's pixels
        float left = 0, right = 0;  // the text column, picture px
        float fontPx = 14;          // the block's text size in picture px
        bool focus = false;
        int caret = -1;             // where the caret goes when the box opens (-1: the end)
        char pending = 0;           // 'b', 'i', 'u': wrap the selection on the next frame
        int split = -1;             // Enter: the caret position to split the paragraph at
        int cursor = 0;             // the caret as of the last frame (Enter splits there)
        int generation = 0;         // bumps on every open so each block gets its own text box
    } docEdit_;
    void docCommit(bool close);
    void deckCommit();
    void docCanvas(ImDrawList* dl, ImVec2 p0, bool hovered);
    // the 3D view's arrows and picking (handles.cpp)
    View3d view3d_;
    // brushing in the 3D view (brush3d.cpp): sculpting a mesh, or painting a bone's weights on a model
    struct Brush3d {
        std::string tool;  // "" off; a sculpt brush (draw, inflate, smooth, ...); "weight" in a model
        float radiusPx = 40, strength = 0.5f;
        bool invert = false;
        std::string symmetry = "none";
        std::string bone, weightMode = "add";
        bool stroking = false;
        NumberList rays;
        std::vector<double> pressures;
        double lastTry = 0;
        std::size_t tried = 0;  // rays in the stroke the last trial ran
    } brush3d_;
    // the dope sheet under a model's 3D view (dopesheet.cpp)
    struct DopeSheet {
        std::string doc;
        bool open = true, playing = false, hovered = false;
        struct Row {
            std::string object, group;  // group "all" = the object's summary row
            std::vector<int> frames;
        };
        std::vector<Row> rows;
        int frame = 1, start = 1, end = 48;
        double fps = 24, lastQuery = -1, lastStep = 0;
        std::string selObject, selGroup;
        int selFrame = -1;
        bool dragging = false, dragCopy = false, scrubbing = false;
        int dragRow = -1, dragTo = 0;
    } dope_;
    // free transform handles on a picture (transform_view.cpp)
    struct FreeTransform {
        bool active = false;
        std::string doc;
        ImVec2 base[4], quad[4], startQuad[4];  // picture px: where the box was, where its corners are now
        ImVec2 startMouse;
        float startAngle = 0;
        int drag = -1;
        double lastTry = 0;
    } xform_;
    // typing a text layer on a picture (transform_view.cpp)
    struct ImageText {
        bool open = false, focus = false, bold = false;
        std::string doc, words;
        ImVec2 at;
        float size = 32;
    } imgText_;
    void imageTextInput(ImVec2 p0, bool hovered);
    Params imageTextParams() const;
    void imageTextFinish(bool keep);
    Params transformParams() const;
    void transformStart();
    void transformFinish(bool keep);
    void transformToolbar();
    void transformInput(ImDrawList* dl, ImVec2 p0, bool hovered, bool active);
    void dopeSheetView();
    float dopeSheetHeight() const;
    void dopeSheetRefresh(bool force);
    void dopeSheetScrub(int frame, bool wrap);
    void brush3dToolbar();
    bool brush3dInput(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered);
    Params brush3dParams() const;
    void view3dCamera();  // read the camera the picture was drawn with
    bool view3dInput(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active);  // true: the mouse is on an arrow
    void view3dDraw(ImDrawList* dl, ImVec2 v0, ImVec2 size);
    void view3dToolbar();
    void cadHandles();
    void modelHandles();
    void formHandles();
    View3d::Face cadPickAt(ImVec2 local);  // the face under a point of the view
    View3d::Face cadPickRay(const P3& from, const P3& dir);
    View3d::Edge cadEdgeAt(ImVec2 local);  // the sharp edge near a point of the view (cad.pick_edge)
    void edgeToolbar();
    void handleRelease();
    bool handleTrial(bool final);  // runs the dragged arrow's edit: as a trial, or for good
    void keepView(const ViewCamera& before);  // the part changed size: keep showing what was shown
    void sketchOnFace(const View3d::Face& f);
    void sketchOnPlane(const char* plane);
    // live previews (preview.cpp)
    Trial trial_;
    LivePreview live_;
    bool previewable(const std::string& command) const;
    bool tryRun(const std::string& command, const Params& params);  // run it as a trial (dropping the last one)
    void dropTrial();
    bool keepTrial(const std::string& command, const Params& params);  // true: the trial already shows exactly that
    static std::string trialKey(const std::string& command, const Params& params);
    void previewTick();
    void previewControls();
    // cad: drawing a sketch with the mouse (sketch_view.cpp)
    SketchEdit sketch_;
    bool sketchActive() const;
    std::string docTitle(const std::string& id) const;  // file name, or "Untitled part 1"
    void enterSketch(const std::string& doc, const std::string& name);
    void leaveSketch();
    void refreshSketch();
    void sketchToolbar();
    void sketchCanvas(ImDrawList* dl, ImVec2 v0, ImVec2 size, bool hovered, bool active);
    // slides: thumbnails on the left, click a part of the slide to type, drag objects (deck_view.cpp)
    DeckView deck_;
    bool deckDoc() const;
    std::string deckPicture();  // the shown slide as PNG for the canvas
    void showSlide(int n);
    void deckStrip();
    void deckCanvas(ImDrawList* dl, ImVec2 p0, bool hovered, bool active);
    // layout: drag frames, resize by handles, double-click text to retype (layout_view.cpp)
    LayoutView page_;
    bool layoutDoc() const;
    std::string layoutPicture();
    void layoutRefreshFrames();
    void layoutShowPage(int n);
    void layoutCanvas(ImDrawList* dl, ImVec2 p0, bool hovered, bool active);
    void layoutCommit();
    TimelineState timeline_;
    GridState grid_;
    CircuitCanvas circuit_;
    MapsCanvas maps_;
    float penPressure_ = 1.0f;
    bool penDown_ = false;
    bool penEraser_ = false;
    std::vector<std::string> recent_;
    std::string recentFile_;

    bool handoff_ = false;  // the session has app.send / app.inbox / ...
    std::string self_;      // this app's id from app.installed ("suite" for the all-in-one)
    InboxBanner inbox_;
    OpenSuggestion suggestion_;
    std::vector<SendChoice> sendChoices_;
    std::string launchOffer_;  // app to offer to start after a send
    std::string installed_;      // the launcher: comma list from app.installed
    bool installedStale_ = true;  // re-read it (the window got focus: the hub may have added an app)
    std::string launcherNote_;   // the launcher's last answer
    bool launcherNoteOk_ = true;

    std::unique_ptr<ToolPanels> panels_;
    std::string panel_;          // tool whose panel fills the middle; empty = home or a document
    std::uint64_t changes_ = 0;  // goes up on every change, so panels know to reload
    bool hasClock_ = false;
};

}  // namespace wetwaresuite::gui
