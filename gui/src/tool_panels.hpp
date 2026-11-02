// Internal: the state of the tool panels that fill the middle of the window
// for the calculator, code editor, file manager, terminal, system monitor and
// clock. Every button in them runs a command through the Session, so undo,
// scripts and MCP clients see the same steps as the window.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "shell.hpp"
#include "wetwaresuite/gui/pipeline.hpp"

namespace wetwaresuite::gui {

namespace tp {

using Rows = std::vector<std::vector<std::string>>;

std::string text(const Result& r, const std::string& key);
double number(const Result& r, const std::string& key, double fallback = 0);
std::vector<double> numbers(const Result& r, const std::string& key);
Rows rows(const Result& r, const std::string& key = "rows");
Rows splitRows(const std::string& text);
std::string baseName(const std::string& path);
std::string parentOf(const std::string& path);
std::string joinPath(const std::string& folder, const std::string& name);
std::string lowerAscii(std::string s);
std::string humanSize(double bytes);
double seconds();  // a steady clock, for polling
std::filesystem::path pathOf(const std::string& utf8);
void muted(const std::string& text);
void title(const std::string& text, float scale = 1.25f);
void errorText(const std::string& text);
// Decodes the first PNG/JPEG attachment of `r` into `tex`; false when there is none.
bool attachmentTexture(SDL_Renderer* renderer, Texture& tex, const Result& r);
// Draws a texture scaled to fit (w, h), keeping its shape.
void fitImage(const Texture& tex, float w, float h);
// Puts the next button on this line when it fits, else on a new one.
void flow(const char* label, bool first = false);

}  // namespace tp

// ---- code editor widget (code_editor.cpp) ----------------------------------------

struct TextPos {
    int line = 0, col = 0;  // col is a byte offset in the line
    bool operator==(const TextPos& o) const { return line == o.line && col == o.col; }
    bool operator!=(const TextPos& o) const { return !(*this == o); }
    bool operator<(const TextPos& o) const { return line < o.line || (line == o.line && col < o.col); }
};

struct CodeEditor {
    std::string doc;  // the code document it shows
    std::vector<std::string> lines{std::string()};
    TextPos cursor, anchor;  // the selection runs between them
    bool dirty = false;      // typed but not yet sent to the document
    double lastEdit = 0;
    std::uint64_t seen = ~0ull;  // Shell change count the text was loaded at
    std::vector<int> spans;      // flat: line, col, len, kind (from code.highlight)
    std::vector<int> spanStart;  // index into spans of each line's first span
    bool spansStale = true;
    double lastHighlight = 0;
    std::string language;
    std::string comment = "//";  // line comment for Ctrl+/
    int scrollTo = -1;           // line to bring into view
    bool focusNext = false;      // take the keyboard on the next frame
    bool active = false;
    bool dragging = false;
    float wantX = -1;  // Up/Down keep this x
    std::vector<std::pair<int, bool>> marks;  // problem lines (0-based) and whether an error
    std::vector<TextPos> found;               // find matches (start); length foundLen
    int foundLen = 0;
    bool readOnly = false;
    bool tabs = false;  // the file indents with tabs
    int widest = 0;     // longest line in columns, for the scroll width
    // more carets (Alt+click, Ctrl+D, Ctrl+Alt+Up/Down); every edit and move happens at each one
    struct Caret {
        TextPos cursor, anchor;
    };
    std::vector<Caret> extra;
    bool wrap = false;         // long lines wrap at the window edge (Alt+Z)
    std::vector<int> folded;   // first lines of folded blocks, sorted
    // screen rows: a line can wrap over several, a folded block takes none
    struct Row {
        int line = 0, from = 0, to = 0;  // the bytes of the line on this row
    };
    std::vector<Row> rows;
    std::vector<int> lineRow;  // each line's first row, -1 inside a folded block
    std::uint64_t version = 0, rowsVersion = ~0ull;
    float rowsWidth = -1;
    std::size_t lastLineCount = 0;
    // set by the widget for the panel to act on
    bool wantUndo = false, wantRedo = false;
    bool wantComplete = false;    // Ctrl+Space
    bool wantDefinition = false;  // F12 or Ctrl+click
    ImVec2 caretScreen{0, 0};     // where the caret was drawn, for popups
    float lineHeight = 0;
    bool menuOpen = false;        // a completion list has Up/Down/Enter/Tab/Esc
    bool mouseOnText = false;     // the mouse is over a character (for hover notes)
    TextPos mousePos;
};

std::string editorText(const CodeEditor& ed);
void setEditorText(CodeEditor& ed, const std::string& text);  // keeps the cursor where it can
void editorSelect(CodeEditor& ed, TextPos from, TextPos to);
std::string editorSelection(const CodeEditor& ed);
void editorFoldAll(CodeEditor& ed, bool fold);   // fold every block that can be, or open them all
void editorAddNextMatch(CodeEditor& ed);         // Ctrl+D: a caret at the next copy of the selection
// Replaces the bytes [from, to) of the caret's line with `text` (a completion), at the primary caret.
void editorReplaceAtCaret(CodeEditor& ed, int fromCol, const std::string& text);
// Draws the editor filling `size`; returns true when the text changed.
bool drawEditor(CodeEditor& ed, ImFont* mono, bool dark, ImVec2 size);

// ---- per tool state ----------------------------------------------------------------

struct CalcPanel {
    std::string input;
    std::string shown = "0";  // big answer line
    std::string detail;       // exact / hex forms
    std::string asked;        // the calculation the answer is for
    bool error = false;
    bool justAnswered = false;
    bool focusInput = true;
    tp::Rows history;
    bool historyStale = true;
    std::string angle = "deg";
    bool exact = true;
    std::string mem;
    // graph
    std::string graphExpr = "sin(x); x^2/10";
    double xmin = -10, xmax = 10;
    Texture graph;
    bool graphStale = true;
    std::string graphError;
    float graphW = 0, graphH = 0;
    // converter
    std::string convValue = "1", convFrom = "km", convTo = "mi", convAnswer;
    std::vector<std::string> unitNames;
    // statistics / solve / programmer
    std::string statsData = "4, 8, 15, 16, 23, 42", statsAnswer;
    std::string solveText = "x^2 - 2 = 0", solveAnswer;
    std::string baseValue = "255", baseAnswer;
    int tab = 0, pickTab = -1;
};

struct CodePanel {
    CodeEditor editor;
    std::uint64_t filesSeen = ~0ull;
    tp::Rows files;  // id, path, language, modified
    std::string project;
    tp::Rows tree;  // depth, kind, path, size
    std::set<std::string> openFolders;
    bool treeStale = true;
    tp::Rows outline;  // line, kind, depth, name
    std::string outlineFor;
    std::uint64_t outlineSeen = ~0ull;
    int side = 0, pickSide = -1;  // files, outline, search, git
    // find bar
    bool findOpen = false, focusFind = false, matchCase = false, wholeWord = false, regex = false;
    std::string findText, replaceText, findInfo;
    int findIndex = -1;
    // go to line
    bool gotoOpen = false;
    int gotoLine = 1;
    // search in files
    std::string searchText, searchReplace, searchInfo;
    tp::Rows searchHits;  // file, line, col, len, text
    // jobs
    std::string job;
    bool jobRunning = false;
    std::string output;
    tp::Rows problems;  // file, line, col, severity, message
    std::string jobInput;
    double lastPoll = 0;
    int bottom = 0;  // output, problems
    // git
    bool gitStale = true;
    bool isRepo = false;
    std::string branch, gitMessage, gitInfo;
    tp::Rows gitFiles;  // x, y, path
    tp::Rows gitLog;
    std::vector<std::string> branches;
    std::string newBranch;
    // new file / open folder / save as
    std::string folderInput, newName = "untitled.py", saveAs;
    std::string status;
    std::string selectTab;  // tab to bring to the front on the next frame
    tp::Rows languages;     // id, name, extensions
    tp::Rows toolchains;    // name, languages, program
    // language server
    bool lsp = false;           // completion, hover, go to definition and live problems
    std::string lspHint;        // which server is installed, when lsp is off
    std::string lspHintFor;     // language the hint is about
    double lspPoll = 0;
    bool lspWaiting = false;    // the server hasn't looked at the latest text yet
    tp::Rows lspProblems;       // line, col, severity, message (for the file in the editor)
    std::string lspProblemsFor;
    bool compOpen = false;
    tp::Rows comps, compsAll;   // label, kind, detail, insert
    int compSel = 0, compFrom = 0, compLine = 0;
    TextPos hoverAt{-1, -1};
    double hoverSince = 0;
    std::string hoverText;
    bool hoverAsked = false;
};

struct ExplorerPanel {
    std::string folder;
    std::vector<std::string> back, forward;
    std::string pathInput;
    tp::Rows entries;  // kind, name, size, mtime, type, path
    std::set<std::string> selected;
    std::string lastClicked;
    std::uint64_t seen = ~0ull;
    bool stale = true;
    std::string filter;
    bool hidden = false;
    std::string sort = "name";
    bool descending = false;
    tp::Rows places;  // name, path
    // preview
    std::string previewFor;
    std::string previewKind, previewText, previewInfo;
    Texture previewImage;
    // clipboard
    std::vector<std::string> clip;
    bool cut = false;
    // popups
    std::string nameInput;
    std::string renaming;
    std::string error, status;
    // archive view and trash view
    std::string archive;
    tp::Rows archiveRows;
    std::string archiveText;
    bool trashView = false;
    tp::Rows trash;
    // deep search
    std::string searchName, searchContains;
    tp::Rows searchHits;
    bool searching = false;
};

struct TerminalTab {
    std::string doc;
    std::string scroll;  // everything printed
    bool running = false;
    std::string cwd;
    std::string input;
    std::vector<std::string> history;
    int historyPos = -1;
    bool scrollDown = true;
};

struct TerminalPanel {
    std::vector<TerminalTab> tabs;
    std::string current;
    double lastPoll = 0;
    bool focusInput = true;
    std::string error;
    std::string shell;
};

struct MonitorPanel {
    double lastSample = -100, lastProcs = -100, lastSlow = -100;
    std::deque<float> cpu, mem;
    std::vector<float> perCore;
    std::string cpuModel;
    double cpuPercent = 0;
    double memTotal = 0, memUsed = 0, swapTotal = 0, swapUsed = 0;
    tp::Rows disks;    // mount, total, used, free, fs, device
    tp::Rows procs;    // pid, ppid, name, cpu, mem, threads, user, state, command
    tp::Rows network;  // name, state, rx, tx, rxRate, txRate
    std::string system, uptime;
    std::string battery;
    std::string sort = "cpu";
    std::string filter;
    long long selectedPid = -1;
    std::string status;
    int tab = 0, pickTab = -1;
};

struct ClockPanel {
    double lastTick = -100, lastDue = -100;
    std::string time, date, zone, weekInfo;
    tp::Rows world;
    std::string worldPlaces = "London|New York|Los Angeles|Tokyo|Sydney|Delhi|Dubai|Paris|Shanghai";
    std::string addPlace;
    tp::Rows timers;  // id, left, total, state, label
    std::string timerText = "5m", timerLabel;
    double swSeconds = 0;
    bool swRunning = false;
    std::vector<double> laps;
    tp::Rows alarms;  // id, time, days, on, next, label
    std::string alarmTime = "07:00", alarmDays = "daily", alarmLabel;
    bool alarmsLoaded = false;
    std::string calendar;
    int calYear = 0, calMonth = 0;
    std::string diffFrom, diffTo, diffAnswer, addDate, addAmount = "30 days", addAnswer;
    std::string convTime = "9:00", convFrom = "New York", convTo = "London|Tokyo", convAnswer;
    std::vector<std::string> due;  // messages waiting to be seen
    std::string error;
    int tab = 0, pickTab = -1;
};

// The PDF editor's page view (pdf_view.cpp), shown for pdf documents.
struct PdfPanel {
    enum class Tool { Select, Text, EditText, Highlight, Underline, Strikeout, Note, Draw, Box, Ellipse, Line, Arrow, Whiteout, Redact, Sign, Picture };
    std::string doc;
    int page = 0;  // 0-based
    int pages = 0;
    float zoom = 0;  // screen px per point; 0 = fit the width on the next frame
    ImVec2 pan{0, 0};
    double lastZoom = 0;  // when the wheel last zoomed, so the page is redrawn sharp once it settles
    std::uint64_t seen = ~0ull;
    int shownPage = -1;
    float shownScale = 0;
    Texture tex;
    double pageW = 0, pageH = 0;  // points, as shown
    tp::Rows marks, lines, fields, widgets;
    std::map<int, Texture> thumbs;
    std::uint64_t thumbsSeen = ~0ull;

    Tool tool = Tool::Select;
    float color[4] = {0.9f, 0.1f, 0.1f, 1};
    float width = 2;
    float textSize = 12;
    bool dragging = false;
    ImVec2 dragFrom{0, 0}, dragTo{0, 0};  // points
    std::vector<double> stroke;          // points, x,y pairs
    int selected = -1;                   // edit (mark) number
    bool moving = false;

    // the text box popup: add text, edit text, notes, signatures, form fields
    std::string popup;  // what it is for; empty = closed
    bool openPopup = false;
    std::string input, original, field, fieldType;
    std::vector<std::string> options;
    double px = 0, py = 0, pw = 0, ph = 0;

    std::string findInput;
    tp::Rows found;  // page, x, y, w, h, text
    int foundAt = -1;
};

// automate's pipeline builder: blocks you add, fill in and drag into order
struct PipelinePanel {
    PipeModel model;
    int selected = -1;
    std::string search;              // the add-a-block list's filter
    bool focusSearch = false;
    std::string path = "pipeline.json";
    std::string output;              // what the last run, dry run or save said
    bool outputOk = true;
};

struct ToolPanels {
    PdfPanel pdf;
    PipelinePanel pipeline;
    CalcPanel calc;
    CodePanel code;
    ExplorerPanel explorer;
    TerminalPanel terminal;
    MonitorPanel monitor;
    ClockPanel clock;
    void resetTextures() {
        calc.graph.reset();
        explorer.previewImage.reset();
        pdf.tex.reset();
        for (auto& [page, t] : pdf.thumbs) t.reset();
        pdf.thumbs.clear();
    }
};

}  // namespace wetwaresuite::gui
