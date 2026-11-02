// The cell grid shown for spreadsheets: sheet tabs, a formula bar and a
// scrolling grid you click and type into. The document hands out only the
// cells on screen (Document::gridCell) and names the command for typing into a
// cell (Document::gridSetCommand), so every edit is one ordinary, undoable
// command, the same one a script or the MCP server would run.
#include <algorithm>
#include <cmath>

#include "shell.hpp"
#include "wetwaresuite/core/cli.hpp"

#include <imgui_stdlib.h>

namespace wetwaresuite::gui {

namespace {

constexpr int kMaxRows = 1048576;
constexpr int kMaxCols = 16384;

std::string columnName(int col) {
    std::string s;
    for (int c = col + 1; c > 0; c = (c - 1) / 26) s.insert(s.begin(), static_cast<char>('A' + (c - 1) % 26));
    return s;
}

std::string cellName(int row, int col) { return columnName(col) + std::to_string(row + 1); }

int caretToEnd(ImGuiInputTextCallbackData* data) {
    auto* st = static_cast<GridState*>(data->UserData);
    if (st && st->caretToEnd) {
        data->CursorPos = data->SelectionStart = data->SelectionEnd = data->BufTextLen;
        st->caretToEnd = false;
    }
    return 0;
}

ImU32 mix(ImU32 a, ImU32 b, float t) {
    const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t, 1});
}

}  // namespace

bool Shell::commitCell(const CellGrid& layout, int sheet, int row, int col, const std::string& input) {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return false;
    GridCell now;
    const bool had = doc->gridCell(sheet, row, col, now);
    if ((had ? now.input : std::string()) == input) return true;  // nothing changed
    std::vector<std::string> tokens = doc->gridSetCommand(sheet, row, col, input);
    if (tokens.empty()) return false;
    const std::string id = tokens.front();
    const CommandInfo* info = find(id);
    if (!info) return false;
    tokens.erase(tokens.begin());
    tokens.push_back(layout.docParam + "=" + currentDoc_);
    Result r;
    try {
        r = run(id, cli::parseArgs(*info, tokens));
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    grid_.formulaRow = -1;  // reload the formula bar
    if (!r.ok) {
        resultFor_ = info->title;
        r.message = friendlyError(r.message);
        result_ = std::move(r);
        scrollToResult_ = true;
    }
    return r.ok;
}

// The look buttons over the grid: bold, italic, underline, colours, alignment, number formats and
// freezing, each one office.sheet_style / sheet_format / sheet_freeze on the selected range.
void Shell::gridFormatBar(const CellGrid& layout) {
    GridState& st = grid_;
    if (!find("office.sheet_style") || !find("office.sheet_format")) return;
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    const int r1 = std::min(st.row, st.row2), r2 = std::max(st.row, st.row2);
    const int c1 = std::min(st.col, st.col2), c2 = std::max(st.col, st.col2);
    const std::string range = cellName(r1, c1) + (r1 == r2 && c1 == c2 ? "" : ":" + cellName(r2, c2));
    GridCell cur;
    doc->gridCell(st.sheet, st.row, st.col, cur);
    auto apply = [&](const char* cmd, Params p, const char* what) {
        p["doc"] = currentDoc_;
        p["range"] = range;
        p["sheet"] = std::to_string(st.sheet + 1);
        Result r = run(cmd, p);
        resultFor_ = what;
        r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
        result_ = std::move(r);
    };
    const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
    auto toggle = [&](const char* label, bool state, const char* tip) {
        if (state) ImGui::PushStyleColor(ImGuiCol_Button, on);
        const bool hit = ImGui::Button(label);
        if (state) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
        ImGui::SameLine(0, 2);
        return hit;
    };
    if (toggle("B", cur.bold, "Bold (Ctrl+B)") || (st.focusGrid && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_B, false)))
        apply("office.sheet_style", {{"bold", !cur.bold}}, "Bold");
    if (toggle("I", cur.italic, "Italic (Ctrl+I)") || (st.focusGrid && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_I, false)))
        apply("office.sheet_style", {{"italic", !cur.italic}}, "Italic");
    if (toggle("U", cur.underline, "Underline (Ctrl+U)") || (st.focusGrid && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_U, false)))
        apply("office.sheet_style", {{"underline", !cur.underline}}, "Underline");
    if (toggle("S", cur.strike, "Strike through")) apply("office.sheet_style", {{"strike", !cur.strike}}, "Strike");
    ImGui::SameLine(0, 8);
    // colours: pick, then it's put on when the picker closes
    auto colour = [&](const char* id, std::uint32_t now, const char* param, const char* tip, ImVec4& keep) {
        if (!ImGui::IsPopupOpen(id, ImGuiPopupFlags_AnyPopupId))
            keep = (now & 0x1000000) ? ImVec4{((now >> 16) & 255) / 255.0f, ((now >> 8) & 255) / 255.0f, (now & 255) / 255.0f, 1}
                                     : (std::string(param) == "fill" ? ImVec4{1, 1, 1, 1} : ImVec4{0, 0, 0, 1});
        ImGui::ColorEdit3(id, &keep.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        ImGui::SetItemTooltip("%s", tip);
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            char hex[16];
            std::snprintf(hex, sizeof hex, "#%02x%02x%02x", static_cast<int>(keep.x * 255 + 0.5f), static_cast<int>(keep.y * 255 + 0.5f),
                          static_cast<int>(keep.z * 255 + 0.5f));
            apply("office.sheet_style", {{param, std::string(hex)}}, std::string(param) == "fill" ? "Fill" : "Text colour");
        }
        ImGui::SameLine(0, 2);
    };
    colour("##fill", cur.fill, "fill", "Fill colour", st.fillPick);
    colour("##ink", cur.color, "color", "Text colour", st.inkPick);
    if (ImGui::Button("x")) apply("office.sheet_style", {{"fill", std::string("none")}, {"color", std::string("none")}}, "Colours");
    ImGui::SetItemTooltip("No fill or text colour");
    ImGui::SameLine(0, 8);
    if (toggle("<", cur.align == 'l', "Align left")) apply("office.sheet_style", {{"align", std::string(cur.align == 'l' ? "general" : "left")}}, "Align");
    if (toggle("=", cur.align == 'c', "Centre")) apply("office.sheet_style", {{"align", std::string(cur.align == 'c' ? "general" : "center")}}, "Align");
    if (toggle(">", cur.align == 'r', "Align right")) apply("office.sheet_style", {{"align", std::string(cur.align == 'r' ? "general" : "right")}}, "Align");
    ImGui::SameLine(0, 8);
    if (ImGui::Button("$")) apply("office.sheet_format", {{"format", std::string("currency")}}, "Number format");
    ImGui::SetItemTooltip("Currency");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("%")) apply("office.sheet_format", {{"format", std::string("percent")}}, "Number format");
    ImGui::SetItemTooltip("Percent");
    ImGui::SameLine(0, 2);
    if (ImGui::Button(".00")) apply("office.sheet_format", {{"format", std::string("number")}}, "Number format");
    ImGui::SetItemTooltip("Two decimals with thousands separators");
    ImGui::SameLine(0, 2);
    if (ImGui::Button("123")) apply("office.sheet_format", {{"format", std::string("general")}}, "Number format");
    ImGui::SetItemTooltip("General: numbers as typed");
    if (find("office.sheet_freeze")) {
        ImGui::SameLine(0, 8);
        const auto fz = st.sheet < static_cast<int>(layout.frozen.size()) ? layout.frozen[static_cast<std::size_t>(st.sheet)] : std::pair<int, int>{0, 0};
        const bool frozen = fz.first > 0 || fz.second > 0;
        if (toggle(frozen ? "Unfreeze" : "Freeze", frozen, frozen ? "Let every row and column scroll again"
                                                                  : "Keep the rows above and the columns left of the selected cell in view")) {
            Params p{{"doc", currentDoc_}, {"sheet", std::to_string(st.sheet + 1)}};
            if (frozen) p["none"] = true;
            else p["at"] = cellName(std::max(st.row, 1), st.col);
            Result r = run("office.sheet_freeze", p);
            resultFor_ = "Freeze";
            r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
            result_ = std::move(r);
        }
    }
    ImGui::NewLine();
}

void Shell::gridView(const CellGrid& layout) {
    GridState& st = grid_;
    if (st.doc != currentDoc_) st = GridState{currentDoc_};
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    const int sheets = static_cast<int>(layout.sheets.size());
    const bool firstFrame = st.sheet < 0;
    if (firstFrame) st.sheet = std::clamp(layout.active, 0, sheets - 1);
    st.sheet = std::clamp(st.sheet, 0, sheets - 1);
    ImGuiIO& io = ImGui::GetIO();
    const float unit = ImGui::GetFontSize();
    auto selectOne = [&](int r, int c) { st.row = st.row2 = r, st.col = st.col2 = c; };

    // sheet tabs
    if (ImGui::BeginTabBar("##sheets", ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < sheets; ++i) {
            ImGui::PushID(i);
            const ImGuiTabItemFlags flags = (firstFrame && i == st.sheet) ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(layout.sheets[static_cast<std::size_t>(i)].c_str(), nullptr, flags)) {
                if (i != st.sheet && !firstFrame) {
                    st.sheet = i;
                    st.editing = false;
                    selectOne(0, 0);
                    st.scrollToSel = true;
                }
                ImGui::EndTabItem();
            }
            ImGui::PopID();
        }
        ImGui::EndTabBar();
    }

    gridFormatBar(layout);

    // formula bar: the selected cell's name and what's typed in it
    if (st.formulaRow != st.row || st.formulaCol != st.col || st.formulaSheet != st.sheet) {
        GridCell c;
        st.formula = doc->gridCell(st.sheet, st.row, st.col, c) ? c.input : std::string();
        st.formulaRow = st.row, st.formulaCol = st.col, st.formulaSheet = st.sheet;
    }
    ImGui::AlignTextToFramePadding();
    const bool isRange = st.row != st.row2 || st.col != st.col2;
    const std::string here = cellName(st.row, st.col);
    const std::string shownName = isRange ? cellName(std::min(st.row, st.row2), std::min(st.col, st.col2)) + ":" +
                                                cellName(std::max(st.row, st.row2), std::max(st.col, st.col2))
                                          : here;
    ImGui::TextUnformatted(shownName.c_str());
    ImGui::SameLine(unit * 6.5f);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##formula", "type a value or =formula, Enter to put it in the cell", &st.formula,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        commitCell(layout, st.sheet, st.row, st.col, st.formula);
        selectOne(std::min(st.row + 1, kMaxRows - 1), st.col);
        st.scrollToSel = true;
    }
    ImGui::SetItemTooltip("Enter writes it into %s. Start with = for a formula, e.g. =SUM(A1:A9)", here.c_str());

    // grid geometry
    const float rowH = ImGui::GetFrameHeight();
    const float headH = rowH;
    const float headW = ImGui::CalcTextSize("0000000").x + 8;
    const float charW = ImGui::CalcTextSize("0").x;
    const int rows = std::min(kMaxRows, std::max({layout.usedRows + 100, st.row + 50, 200}));
    const int cols = std::min(kMaxCols, std::max({layout.usedCols + 8, st.col + 8, 26}));
    std::vector<float> colX(static_cast<std::size_t>(cols) + 1, 0.0f);
    for (int c = 0; c < cols; ++c) {
        const float w = std::max(unit * 1.5f, static_cast<float>(doc->gridColumnChars(st.sheet, c)) * charW + 10);
        colX[static_cast<std::size_t>(c) + 1] = colX[static_cast<std::size_t>(c)] + w;
    }
    const float totalW = headW + colX.back(), totalH = headH + rowH * static_cast<float>(rows);
    // frozen panes: these rows and columns stay put while the rest scrolls under them
    const auto fz = st.sheet < static_cast<int>(layout.frozen.size()) ? layout.frozen[static_cast<std::size_t>(st.sheet)] : std::pair<int, int>{0, 0};
    const int fr = std::clamp(fz.first, 0, rows - 1), fc = std::clamp(fz.second, 0, cols - 1);
    const float frozenH = rowH * static_cast<float>(fr), frozenW = colX[static_cast<std::size_t>(fc)];

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::BeginChild("##grid", {0, 0}, ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    const ImVec2 origin = ImGui::GetWindowPos();
    const ImVec2 size = ImGui::GetWindowSize();
    const float scrollX = ImGui::GetScrollX(), scrollY = ImGui::GetScrollY();
    const float viewW = size.x - headW - ImGui::GetStyle().ScrollbarSize;
    const float viewH = size.y - headH - ImGui::GetStyle().ScrollbarSize;
    auto cellMin = [&](int r, int c) {
        return ImVec2{origin.x + headW + colX[static_cast<std::size_t>(c)] - (c >= fc ? scrollX : 0.0f),
                      origin.y + headH + rowH * static_cast<float>(r) - (r >= fr ? scrollY : 0.0f)};
    };

    // keep the selection on screen after keyboard moves
    if (st.scrollToSel) {
        if (st.col >= fc) {
            const float x0 = colX[static_cast<std::size_t>(st.col)], x1 = colX[static_cast<std::size_t>(st.col) + 1];
            if (x0 < scrollX + frozenW) ImGui::SetScrollX(std::max(0.0f, x0 - frozenW));
            else if (x1 > scrollX + viewW) ImGui::SetScrollX(x1 - viewW);
        }
        if (st.row >= fr) {
            const float y0 = rowH * static_cast<float>(st.row), y1 = y0 + rowH;
            if (y0 < scrollY + frozenH) ImGui::SetScrollY(std::max(0.0f, y0 - frozenH));
            else if (y1 > scrollY + viewH) ImGui::SetScrollY(y1 - viewH);
        }
        st.scrollToSel = false;
    }

    // what scrolls into view (past the frozen panes)
    const int r0 = std::clamp(std::max(fr, static_cast<int>((scrollY + frozenH) / rowH)), 0, rows - 1);
    const int r1 = std::clamp(static_cast<int>((scrollY + size.y) / rowH) + 1, 0, rows);
    const int c0 = std::clamp(std::max(fc, static_cast<int>(std::upper_bound(colX.begin(), colX.end(), scrollX + frozenW) - colX.begin()) - 1), 0, cols - 1);
    int c1 = c0;
    while (c1 < cols && colX[static_cast<std::size_t>(c1)] < scrollX + size.x) ++c1;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const ImU32 line = mix(bg, text, 0.15f);
    const ImU32 headBg = ImGui::GetColorU32(ImGuiCol_TableHeaderBg);
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    const ImU32 headSel = mix(headBg, accent, 0.35f);
    const ImU32 errorCol = darkLook() ? IM_COL32(255, 120, 110, 255) : IM_COL32(190, 20, 10, 255);
    const ImVec2 body{origin.x + headW, origin.y + headH};
    const ImVec2 bodyEnd{origin.x + size.x, origin.y + size.y};
    dl->AddRectFilled(body, bodyEnd, bg);

    const float pad = 4;
    auto rgb = [](std::uint32_t c, int alpha = 255) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, alpha); };
    auto luma = [](std::uint32_t c) { return 0.299f * ((c >> 16) & 255) + 0.587f * ((c >> 8) & 255) + 0.114f * (c & 255); };
    const float bgLuma = darkLook() ? 40.0f : 230.0f;
    constexpr std::uint32_t kSet = 0x1000000;
    auto spanRect = [&](int r, int c, const GridCell& cell, ImVec2& a, ImVec2& b) {
        a = cellMin(r, c);
        const int ce = std::min(cols, c + std::max(1, cell.spanCols));
        b = {a.x + colX[static_cast<std::size_t>(ce)] - colX[static_cast<std::size_t>(c)], a.y + rowH * static_cast<float>(std::max(1, cell.spanRows))};
    };
    // one pane of cells: fills and data bars under the grid lines, text (and merged areas) over them
    struct Shown {
        int r, c;
        GridCell cell;
    };
    auto drawPane = [&](int ra, int rb, int ca, int cb, ImVec2 clipMin, ImVec2 clipMax) {
        if (ra >= rb || ca >= cb || clipMin.x >= clipMax.x || clipMin.y >= clipMax.y) return;
        dl->PushClipRect(clipMin, clipMax, true);
        std::vector<Shown> shown;
        for (int r = ra; r < rb; ++r) {
            for (int c = ca; c < cb; ++c) {
                GridCell cell;
                if (!doc->gridCell(st.sheet, r, c, cell) || cell.covered) continue;
                ImVec2 a, b;
                spanRect(r, c, cell, a, b);
                if (cell.fill & kSet) dl->AddRectFilled(a, b, rgb(cell.fill));
                if (cell.bar >= 0) {
                    const float bw = (b.x - a.x - 4) * std::clamp(cell.bar, 0.0f, 1.0f);
                    dl->AddRectFilled({a.x + 2, a.y + 3}, {a.x + 2 + bw, b.y - 3}, rgb(cell.barColor & kSet ? cell.barColor : 0x638EC6, 110));
                }
                if (!cell.text.empty() || cell.spanRows > 1 || cell.spanCols > 1) shown.push_back({r, c, std::move(cell)});
            }
        }
        for (int c = ca; c <= cb && c <= cols; ++c) {
            const float x = cellMin(0, c).x;
            dl->AddLine({x, clipMin.y}, {x, clipMax.y}, line);
        }
        for (int r = ra; r <= rb; ++r) {
            const float y = cellMin(r, 0).y;
            dl->AddLine({clipMin.x, y}, {clipMax.x, y}, line);
        }
        for (const auto& [r, c, cell] : shown) {
            ImVec2 a, b;
            spanRect(r, c, cell, a, b);
            const bool merged = cell.spanRows > 1 || cell.spanCols > 1;
            const ImVec4 clip{std::max(a.x, clipMin.x), std::max(a.y, clipMin.y), std::min(b.x - 1, clipMax.x), std::min(b.y, clipMax.y)};
            if (merged) dl->AddRectFilled({a.x + 1, a.y + 1}, {b.x, b.y}, cell.fill & kSet ? rgb(cell.fill) : bg);  // cover the lines inside
            if (cell.text.empty() || clip.x >= clip.z || clip.y >= clip.w) continue;
            ImU32 col = cell.error ? errorCol : text;
            if (!cell.error) {
                const float under = cell.fill & kSet ? luma(cell.fill) : bgLuma;
                if (cell.color & kSet) {
                    if (std::abs(luma(cell.color) - under) > 60) col = rgb(cell.color);
                    else if (cell.fill & kSet) col = under > 128 ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
                } else if (cell.fill & kSet) {
                    col = under > 128 ? IM_COL32(20, 20, 20, 255) : IM_COL32(240, 240, 240, 255);
                }
            }
            const float tw = ImGui::CalcTextSize(cell.text.c_str()).x;
            const char align = cell.align ? cell.align : merged ? 'c' : cell.number ? 'r' : 'l';
            const float x = align == 'r' ? b.x - pad - tw : align == 'c' ? (a.x + b.x - tw) * 0.5f : a.x + pad;
            const ImVec2 p{x, merged ? (a.y + b.y - unit) * 0.5f : a.y + (rowH - unit) * 0.5f};
            const int firstVtx = dl->VtxBuffer.Size;
            dl->AddText(nullptr, 0, p, col, cell.text.c_str(), nullptr, 0, &clip);
            if (cell.bold) dl->AddText(nullptr, 0, {p.x + 1, p.y}, col, cell.text.c_str(), nullptr, 0, &clip);
            if (cell.italic) {  // lean the letters over: shear what was just drawn about the baseline
                const float base = p.y + unit * 0.8f;
                for (int v = firstVtx; v < dl->VtxBuffer.Size; ++v) dl->VtxBuffer[v].pos.x += (base - dl->VtxBuffer[v].pos.y) * 0.2f;
            }
            dl->PushClipRect({clip.x, clip.y}, {clip.z, clip.w}, true);
            if (cell.underline) dl->AddLine({p.x, p.y + unit}, {p.x + tw, p.y + unit}, col);
            if (cell.strike) dl->AddLine({p.x, p.y + unit * 0.55f}, {p.x + tw, p.y + unit * 0.55f}, col);
            dl->PopClipRect();
        }
        dl->PopClipRect();
    };
    const ImVec2 split{body.x + frozenW, body.y + frozenH};  // where the scrolling part starts
    drawPane(r0, r1, c0, c1, split, bodyEnd);                // the part that scrolls
    drawPane(0, fr, c0, c1, {split.x, body.y}, {bodyEnd.x, split.y});  // frozen rows, scrolling sideways
    drawPane(r0, r1, 0, fc, {body.x, split.y}, {split.x, bodyEnd.y});  // frozen columns, scrolling up and down
    drawPane(0, fr, 0, fc, body, split);                               // the corner that never moves
    if (fr > 0) dl->AddLine({body.x, split.y}, {bodyEnd.x, split.y}, mix(line, text, 0.5f), 2);
    if (fc > 0) dl->AddLine({split.x, body.y}, {split.x, bodyEnd.y}, mix(line, text, 0.5f), 2);

    // selection: the range, the active cell, and the fill handle at its bottom right
    const int sr1 = std::min(st.row, st.row2), sr2 = std::max(st.row, st.row2);
    const int sc1 = std::min(st.col, st.col2), sc2 = std::max(st.col, st.col2);
    const ImVec2 selA = cellMin(sr1, sc1);
    const ImVec2 selB{cellMin(sr2, sc2).x + colX[static_cast<std::size_t>(sc2) + 1] - colX[static_cast<std::size_t>(sc2)], cellMin(sr2, sc2).y + rowH};
    const ImVec2 handle{selB.x, selB.y};
    dl->PushClipRect(body, bodyEnd, true);
    if (sr1 != sr2 || sc1 != sc2) {
        dl->AddRectFilled(selA, selB, (accent & 0x00FFFFFF) | (40u << 24));
        const ImVec2 a = cellMin(st.row, st.col);
        dl->AddRect(a, {a.x + colX[static_cast<std::size_t>(st.col) + 1] - colX[static_cast<std::size_t>(st.col)], a.y + rowH}, accent, 0, 0, 1.0f);
    }
    dl->AddRect(selA, {selB.x + 1, selB.y + 1}, accent, 0, 0, 2.0f);
    const bool canFill = find("office.sheet_fill") != nullptr && !st.editing;
    if (canFill) dl->AddRectFilled({handle.x - 3, handle.y - 3}, {handle.x + 4, handle.y + 4}, accent);
    if (st.filling) {  // where the fill will go
        const int tr = std::max(sr2, st.fillRow), tc = std::max(sc2, st.fillCol);
        const bool down = st.fillRow - sr2 >= st.fillCol - sc2;
        const ImVec2 e = down ? ImVec2{selB.x, cellMin(tr, sc2).y + rowH}
                              : ImVec2{cellMin(sr2, tc).x + colX[static_cast<std::size_t>(tc) + 1] - colX[static_cast<std::size_t>(tc)], selB.y};
        dl->AddRect(selA, e, accent, 0, 0, 1.0f);
    }
    dl->PopClipRect();

    // headers on top, so they stay put while the cells scroll under them
    dl->AddRectFilled(origin, {bodyEnd.x, body.y}, headBg);
    dl->AddRectFilled(origin, {body.x, bodyEnd.y}, headBg);
    auto colHeads = [&](int ca, int cb, float clipX0) {
        for (int c = ca; c < cb; ++c) {
            const float x = cellMin(0, c).x;
            const float w = colX[static_cast<std::size_t>(c) + 1] - colX[static_cast<std::size_t>(c)];
            const ImVec4 clip{std::max(x, clipX0), origin.y, x + w, body.y};
            if (clip.x >= clip.z) continue;
            if (c >= sc1 && c <= sc2) dl->AddRectFilled({clip.x, clip.y}, {clip.z, clip.w}, headSel);
            const std::string name = columnName(c);
            const float tw = ImGui::CalcTextSize(name.c_str()).x;
            dl->AddText(nullptr, 0, {x + (w - tw) * 0.5f, origin.y + (headH - unit) * 0.5f}, c == st.col ? text : muted, name.c_str(), nullptr, 0, &clip);
            if (x >= clipX0) dl->AddLine({x, origin.y}, {x, body.y}, line);
        }
    };
    auto rowHeads = [&](int ra, int rb, float clipY0) {
        for (int r = ra; r < rb; ++r) {
            const float y = cellMin(r, 0).y;
            const ImVec4 clip{origin.x, std::max(y, clipY0), body.x, y + rowH};
            if (clip.y >= clip.w) continue;
            if (r >= sr1 && r <= sr2) dl->AddRectFilled({clip.x, clip.y}, {clip.z, clip.w}, headSel);
            const std::string name = std::to_string(r + 1);
            const float tw = ImGui::CalcTextSize(name.c_str()).x;
            dl->AddText(nullptr, 0, {origin.x + headW - pad - tw, y + (rowH - unit) * 0.5f}, r == st.row ? text : muted, name.c_str(), nullptr, 0, &clip);
            if (y >= clipY0) dl->AddLine({origin.x, y}, {body.x, y}, line);
        }
    };
    colHeads(c0, c1, split.x);
    colHeads(0, fc, body.x);
    rowHeads(r0, r1, split.y);
    rowHeads(0, fr, body.y);
    dl->AddLine({origin.x, body.y}, {bodyEnd.x, body.y}, line);
    dl->AddLine({body.x, origin.y}, {body.x, bodyEnd.y}, line);

    auto move = [&](int dr, int dc, bool extend = false) {
        if (extend && !st.editing) {  // Shift+arrows grow the range from the active cell
            st.row2 = std::clamp(st.row2 + dr, 0, kMaxRows - 1);
            st.col2 = std::clamp(st.col2 + dc, 0, kMaxCols - 1);
        } else {
            selectOne(std::clamp(st.row + dr, 0, kMaxRows - 1), std::clamp(st.col + dc, 0, kMaxCols - 1));
        }
        st.scrollToSel = true;
    };
    auto startEdit = [&](std::string initial, bool typed) {
        st.editing = true;
        st.focusEdit = true;
        ++st.editCount;
        st.caretToEnd = true;
        st.row2 = st.row, st.col2 = st.col;
        if (typed) {
            st.buffer = std::move(initial);
        } else {
            GridCell c;
            st.buffer = doc->gridCell(st.sheet, st.row, st.col, c) ? c.input : std::string();
        }
        st.scrollToSel = true;
    };
    // the cell under a point on screen, frozen panes included
    auto cellAt = [&](ImVec2 m, int& r, int& c) {
        const float lx = m.x - body.x, ly = m.y - body.y;
        const float gx = lx < frozenW ? lx : lx + scrollX, gy = ly < frozenH ? ly : ly + scrollY;
        r = std::clamp(static_cast<int>(gy / rowH), 0, rows - 1);
        c = std::clamp(static_cast<int>(std::upper_bound(colX.begin(), colX.end(), gx) - colX.begin()) - 1, 0, cols - 1);
    };

    // mouse: click selects, drag selects a range, double-click edits, the corner square fills
    const bool focused = ImGui::IsWindowFocused();
    st.focusGrid = focused && !st.editing;
    // SDL only sends typed characters while text input is on, and ImGui turns it on just for
    // its own text boxes: keep it on while the grid has focus, so typing starts an edit.
    if (focused && !st.editing && !SDL_TextInputActive(window_)) {
        SDL_StartTextInput(window_);
        st.textInput = true;
    } else if (!focused && st.textInput) {
        if (!io.WantTextInput) SDL_StopTextInput(window_);  // unless a text box has it now
        st.textInput = false;
    }
    const ImVec2 m = io.MousePos;
    const bool inBody = m.x > body.x && m.y > body.y && m.x < bodyEnd.x - style.ScrollbarSize && m.y < bodyEnd.y - style.ScrollbarSize;
    const bool onHandle = canFill && std::abs(m.x - handle.x) <= 5 && std::abs(m.y - handle.y) <= 5;
    if (ImGui::IsWindowHovered() && (onHandle || st.filling)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    if (ImGui::IsWindowHovered() && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))) {
        if (onHandle && !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            st.filling = true;
            st.fillRow = sr2, st.fillCol = sc2;
        } else if (inBody) {
            int r, c;
            cellAt(m, r, c);
            if (st.editing && (r != st.row || c != st.col)) {
                commitCell(layout, st.sheet, st.row, st.col, st.buffer);
                st.editing = false;
            }
            if (io.KeyShift && !st.editing) {
                st.row2 = r, st.col2 = c;
            } else {
                selectOne(r, c);
                st.selecting = true;
            }
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) startEdit({}, false), st.selecting = false;
        }
    }
    if ((st.selecting || st.filling) && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        int r, c;
        cellAt({std::clamp(m.x, body.x + 1, bodyEnd.x - style.ScrollbarSize - 1), std::clamp(m.y, body.y + 1, bodyEnd.y - style.ScrollbarSize - 1)}, r, c);
        if (st.selecting) st.row2 = r, st.col2 = c;
        else st.fillRow = r, st.fillCol = c;
        // drag past the edge: scroll along
        if (m.y > bodyEnd.y - style.ScrollbarSize) ImGui::SetScrollY(scrollY + rowH);
        if (m.x > bodyEnd.x - style.ScrollbarSize) ImGui::SetScrollX(scrollX + charW * 4);
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        st.selecting = false;
        if (st.filling) {
            st.filling = false;
            const bool down = st.fillRow - sr2 >= st.fillCol - sc2;
            if ((down && st.fillRow > sr2) || (!down && st.fillCol > sc2)) {
                const std::string range = cellName(sr1, sc1) + ":" + (down ? cellName(st.fillRow, sc2) : cellName(sr2, st.fillCol));
                Result r = run("office.sheet_fill", {{"doc", currentDoc_},
                                                     {"range", range},
                                                     {"direction", std::string(down ? "down" : "right")},
                                                     {"source", static_cast<std::int64_t>(down ? sr2 - sr1 + 1 : sc2 - sc1 + 1)},
                                                     {"sheet", std::to_string(st.sheet + 1)}});
                resultFor_ = "Fill";
                r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
                if (r.ok) st.row = sr1, st.col = sc1, st.row2 = down ? st.fillRow : sr2, st.col2 = down ? sc2 : st.fillCol;
                result_ = std::move(r);
                st.formulaRow = -1;
            }
        }
    }

    // editing in the cell
    if (st.editing) {
        const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        const bool tab = ImGui::IsKeyPressed(ImGuiKey_Tab, false);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            st.editing = false;
        } else if ((enter || tab) && (!st.focusEdit || !st.buffer.empty())) {  // fast typists press Enter before the box is up
            commitCell(layout, st.sheet, st.row, st.col, st.buffer);
            st.editing = false;
            if (enter) move(io.KeyShift ? -1 : 1, 0);
            else move(0, io.KeyShift ? -1 : 1);
        } else {
            const ImVec2 a = cellMin(st.row, st.col);
            const float w = colX[static_cast<std::size_t>(st.col) + 1] - colX[static_cast<std::size_t>(st.col)];
            ImGui::SetCursorScreenPos({std::max(a.x, body.x), std::max(a.y, body.y)});
            ImGui::SetNextItemWidth(std::max(w, unit * 10));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {pad, (rowH - unit) * 0.5f});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
            if (st.focusEdit) {
                // the editor takes focus a frame or two late: keep what's typed meanwhile
                for (ImWchar ch : io.InputQueueCharacters) {
                    if (ch >= 32 && ch != 127) appendUtf8(st.buffer, ch);
                }
                io.InputQueueCharacters.resize(0);
                st.caretToEnd = true;
                ImGui::SetKeyboardFocusHere();
            }
            ImGui::PushID(st.editCount);
            ImGui::InputText("##cell", &st.buffer, ImGuiInputTextFlags_CallbackAlways, caretToEnd, &st);
            ImGui::PopID();
            if (ImGui::IsItemActive()) st.focusEdit = false;
            if (ImGui::IsItemDeactivated() && !st.focusEdit) {  // clicked away: keep what was typed
                commitCell(layout, st.sheet, st.row, st.col, st.buffer);
                st.editing = false;
            }
            ImGui::PopStyleVar(2);
        }
    } else if (focused && !io.KeyCtrl && !io.KeyAlt) {
        // keyboard on the grid
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(-1, 0, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(1, 0, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move(0, -1, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move(0, 1, io.KeyShift);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move(-static_cast<int>(viewH / rowH), 0);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move(static_cast<int>(viewH / rowH), 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) move(0, -st.col);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) selectOne(st.row, st.col);
        if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) move(io.KeyShift ? -1 : 1, 0);
        if (ImGui::IsKeyPressed(ImGuiKey_Tab)) move(0, io.KeyShift ? -1 : 1);
        if (ImGui::IsKeyPressed(ImGuiKey_F2)) startEdit({}, false);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            if ((sr1 != sr2 || sc1 != sc2) && find("office.sheet_clear")) {
                Result r = run("office.sheet_clear", {{"doc", currentDoc_}, {"range", cellName(sr1, sc1) + ":" + cellName(sr2, sc2)},
                                                      {"what", std::string("contents")}, {"sheet", std::to_string(st.sheet + 1)}});
                resultFor_ = "Clear";
                r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
                result_ = std::move(r);
                st.formulaRow = -1;
            } else {
                commitCell(layout, st.sheet, st.row, st.col, "");
            }
        }
        // typing starts an edit that replaces the cell, like every spreadsheet
        std::string typed;
        for (ImWchar ch : io.InputQueueCharacters) {
            if (ch >= 32 && ch != 127) appendUtf8(typed, ch);
        }
        if (!typed.empty()) startEdit(typed, true);
    } else if (focused && io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {  // the range as tab-separated rows, what other spreadsheets paste
            std::string out;
            for (int r = sr1; r <= sr2; ++r) {
                for (int c = sc1; c <= sc2; ++c) {
                    GridCell cell;
                    if (doc->gridCell(st.sheet, r, c, cell)) out += cell.input;
                    if (c < sc2) out += '\t';
                }
                if (r < sr2) out += '\n';
            }
            ImGui::SetClipboardText(out.c_str());
        }
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) st.row = st.col = 0, st.row2 = std::max(0, layout.usedRows - 1), st.col2 = std::max(0, layout.usedCols - 1);
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            if (const char* clip = ImGui::GetClipboardText()) {
                std::string s = clip;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
                if (!s.empty()) commitCell(layout, st.sheet, st.row, st.col, s);  // rows/tabs paste as a block
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            GridCell c;
            if (doc->gridCell(st.sheet, st.row, st.col, c)) {
                ImGui::SetClipboardText(c.input.c_str());
                commitCell(layout, st.sheet, st.row, st.col, "");
            }
        }
    }

    // the scroll area: the whole grid's size
    ImGui::SetCursorPos({totalW, totalH});
    ImGui::Dummy({1, 1});
    ImGui::EndChild();
}

}  // namespace wetwaresuite::gui
