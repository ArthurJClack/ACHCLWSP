// The timeline under video edits and audio projects: tracks of clips you drag
// to move (also onto another track of the same kind), drag by an edge to trim,
// split at the cursor (S) and delete (Delete). Video edits add track switches (lock,
// hide, mute, solo), razor, ripple delete, markers, in/out, transitions and
// keyframe diamonds. The document describes its
// tracks (Document::timeline) and names the command for each edit
// (Document::timelineCommand), so every drag is one ordinary, undoable command.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>

#include "shell.hpp"
#include "wetwaresuite/core/cli.hpp"

namespace wetwaresuite::gui {

namespace {

constexpr float kEdge = 6.0f;  // px at each end of a clip that trim instead of move

// A clip's gain line: -24 dB at the bottom, +12 dB at the top, 0 dB a third of the way down.
float gainY(double db, float top, float bottom) {
    const double f = (std::clamp(db, -24.0, 12.0) + 24.0) / 36.0;
    return bottom - static_cast<float>(f) * (bottom - top);
}
double gainAt(float y, float top, float bottom) { return std::clamp(-24.0 + 36.0 * (bottom - y) / std::max(1.0f, bottom - top), -24.0, 12.0); }

std::string clockText(double s) {
    char buf[32];
    const int m = static_cast<int>(s / 60);
    std::snprintf(buf, sizeof buf, "%d:%04.1f", m, s - m * 60);
    return buf;
}

double niceStep(double secondsPerPixel) {
    const double want = secondsPerPixel * 90;  // a label every ~90 px
    for (double s : {0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0}) {
        if (s >= want) return s;
    }
    return 1200.0;
}

// Rows are taller when clips show thumbnails or waveforms.
float mediaBinHeight() { return ImGui::GetFontSize() * 2.8f + ImGui::GetStyle().ScrollbarSize + 6; }

float rowHeight(const TimelineLayout& layout) {
    for (const auto& t : layout.tracks)
        for (const auto& c : t.clips)
            if (c.picture || c.sound) return ImGui::GetFontSize() * 3.6f;
    return ImGui::GetFontSize() * 2.4f;
}

}  // namespace

float Shell::timelineHeight(const TimelineLayout& layout) const {
    const float row = rowHeight(layout);
    float chrome = ImGui::GetFrameHeightWithSpacing() + ImGui::GetFontSize() * 1.6f + 8;
    if (!exportJob_.id.empty()) chrome += ImGui::GetFrameHeightWithSpacing();  // the export's progress bar
    if (layout.tools && !layout.media.empty()) chrome += mediaBinHeight() + ImGui::GetStyle().ItemSpacing.y;
    return chrome + row * static_cast<float>(std::max<std::size_t>(1, layout.tracks.size()));
}

const ClipStrip* Shell::clipStrip(const std::string& clipId) {
    const std::string key = currentDoc_ + "|" + clipId;
    if (auto it = strips_.find(key); it != strips_.end()) return &it->second;
    auto doc = session_.workspace().get(currentDoc_);
    MediaStrip m;
    if (!doc || !doc->clipMedia(clipId, m)) return nullptr;
    if (m.pending) {  // ask again next frame
        stripsPending_ = true;
        return nullptr;
    }
    if (strips_.size() > 300) {  // forget the lot now and then (closed documents, deleted clips)
        for (auto& [k, v] : strips_)
            for (auto& t : v.frames) t.reset();
        strips_.clear();
    }
    ClipStrip& strip = strips_[key];
    strip.frames.resize(m.frames.size());
    for (std::size_t i = 0; i < m.frames.size(); ++i) {
        if (m.frames[i].size() >= static_cast<std::size_t>(m.frameWidth) * m.frameHeight * 4)
            strip.frames[i].upload(renderer_, m.frames[i].data(), m.frameWidth, m.frameHeight);
    }
    m.frames.clear();  // the textures have them now
    strip.media = std::move(m);
    return &strip;
}

void Shell::runTimelineEdit(const TimelineLayout& layout, const TimelineEdit& edit) {
    auto doc = session_.workspace().get(currentDoc_);
    if (!doc) return;
    const std::string line = doc->timelineCommand(edit);
    if (line.empty()) return;
    std::vector<std::string> tokens = cli::split(line);
    if (tokens.empty()) return;
    const std::string id = tokens.front();
    const CommandInfo* info = find(id);
    if (!info) return;
    tokens.erase(tokens.begin());
    tokens.push_back(layout.docParam + "=" + currentDoc_);
    Result r;
    try {
        r = run(id, cli::parseArgs(*info, tokens));
    } catch (const std::exception& e) {
        r = Result::failure(e.what());
    }
    if (edit.kind == TimelineEditKind::Cursor && r.ok) return;  // moving the playhead isn't worth a message
    resultFor_ = info->title;
    r.message = r.ok ? sentence(r.message) : friendlyError(r.message);
    result_ = std::move(r);
}

void Shell::timelineView(const TimelineLayout& layout) {
    TimelineState& st = timeline_;
    if (st.doc != currentDoc_) st = TimelineState{currentDoc_};
    ImGuiIO& io = ImGui::GetIO();
    // a document with its own playhead keeps the cursor (the window follows it, and moves it with cut.set_cursor)
    if (layout.cursor >= 0 && st.drag != TimelineState::Drag::Cursor) st.cursor = layout.cursor;

    stripsPending_ = false;
    if (!exportJob_.id.empty()) exportProgress();
    // toolbar
    if (layout.tools) {
        if (ImGui::Button("+ Media")) showAddMediaDialog();
        ImGui::SetItemTooltip("Put videos, pictures or sound on the timeline (or drop files on the window)");
        ImGui::SameLine();
    }
    const TimelineClip* sel = nullptr;
    const TimelineTrack* selTrack = nullptr;
    double end = 1;
    for (const auto& t : layout.tracks) {
        for (const auto& c : t.clips) {
            end = std::max(end, c.start + c.length);
            if (t.id == st.selTrack && c.id == st.selClip) sel = &c, selTrack = &t;
        }
    }
    const bool canSplit = sel && st.cursor > sel->start + 0.01 && st.cursor < sel->start + sel->length - 0.01;
    ImGui::BeginDisabled(!canSplit);
    if (ImGui::Button("Split") || (canSplit && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl)) {
        runTimelineEdit(layout, {TimelineEditKind::Split, sel->id, selTrack->id, {}, 0, 0, st.cursor});
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Cut the selected clip at the cursor (S)");
    ImGui::SameLine();
    ImGui::BeginDisabled(!sel);
    if (ImGui::Button("Delete") || (sel && !io.WantTextInput && !(layout.tools && io.KeyShift) && ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
        runTimelineEdit(layout, {TimelineEditKind::Delete, sel->id, selTrack->id});
        st.selClip.clear();
        sel = nullptr;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Remove the selected clip (Delete)");
    if (layout.tools) {
        // editor tools: the document answers these edit kinds (cut.razor, cut.add_marker, ...)
        const bool keys = !io.WantTextInput && !io.KeyCtrl;
        auto tool = [&](const char* label, const char* tip, bool enabled, bool pressed) {
            ImGui::SameLine();
            ImGui::BeginDisabled(!enabled);
            const bool hit = ImGui::Button(label) || (enabled && pressed);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("%s", tip);
            return hit;
        };
        if (tool("Ripple delete", "Remove the selected clip and close the gap (Shift+Delete)", sel != nullptr,
                 !io.WantTextInput && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Delete, false))) {
            runTimelineEdit(layout, {TimelineEditKind::RippleDelete, sel->id, selTrack->id});
            st.selClip.clear();
            sel = nullptr;
        }
        if (tool("Razor", "Cut every unlocked track at the cursor (Ctrl+K)", true,
                 !io.WantTextInput && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false))) {
            runTimelineEdit(layout, {TimelineEditKind::Razor, {}, {}, {}, 0, 0, st.cursor});
        }
        if (tool("Marker", "Add a marker at the cursor (M); on the selected clip it moves with the clip", true, keys && ImGui::IsKeyPressed(ImGuiKey_M, false))) {
            // over the selected clip it rides along with it (a clip marker)
            runTimelineEdit(layout, {TimelineEditKind::Marker, st.selClip, st.selTrack, {}, 0, 0, st.cursor});
        }
        if (tool("In", "Mark the in point at the cursor (I)", true, keys && ImGui::IsKeyPressed(ImGuiKey_I, false))) {
            runTimelineEdit(layout, {TimelineEditKind::InPoint, {}, {}, {}, 0, 0, st.cursor});
        }
        if (tool("Out", "Mark the out point at the cursor (O)", true, keys && ImGui::IsKeyPressed(ImGuiKey_O, false))) {
            runTimelineEdit(layout, {TimelineEditKind::OutPoint, {}, {}, {}, 0, 0, st.cursor});
        }
        if (tool("Transition", "Crossfade into the selected clip from the one before it (T)", sel != nullptr,
                 keys && ImGui::IsKeyPressed(ImGuiKey_T, false))) {
            TimelineEdit e{TimelineEditKind::Transition, sel->id, selTrack->id};
            e.name = "crossfade";
            runTimelineEdit(layout, e);
        }
    }
    // stepping the playhead like other editors: Left/Right one frame (Shift: one second), Home/End
    if (layout.cursor >= 0 && !io.WantTextInput && !io.KeyCtrl && !timelinePlaying()) {
        double to = -1;
        const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow), right = ImGui::IsKeyPressed(ImGuiKey_RightArrow);
        if (left || right) {
            auto doc = session_.workspace().get(currentDoc_);
            auto player = doc ? doc->player() : nullptr;
            const double frame = player && player->fps() > 0 ? 1.0 / player->fps() : 1.0 / 30;
            to = layout.cursor + (right ? 1 : -1) * (io.KeyShift ? 1.0 : frame);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) to = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) to = end;
        if (to >= 0 || left || right) {
            to = std::clamp(to, 0.0, end);
            if (std::fabs(to - layout.cursor) < 1e-9) to = -1;
        }
        if (to >= 0) {
            st.cursor = to;
            runTimelineEdit(layout, {TimelineEditKind::Cursor, {}, {}, {}, 0, 0, to});
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) st.zoom = 0;
    ImGui::SetItemTooltip("Show the whole timeline");
    ImGui::SameLine();
    {
        // the tips only when they fit, else they hover over the cursor time
        const std::string at = "cursor " + clockText(st.cursor);
        const char* tips = "drag clips to move, edges to trim, arrows step a frame, Ctrl+scroll to zoom";
        const bool room = ImGui::CalcTextSize((at + "   " + tips).c_str()).x <= ImGui::GetContentRegionAvail().x;
        ImGui::TextDisabled("%s", room ? (at + "   " + tips).c_str() : at.c_str());
        if (!room) ImGui::SetItemTooltip("%s", tips);
    }

    // the media bin: a row of the files brought in, dragged from here onto a track
    if (layout.tools && !layout.media.empty()) {
        const float unit = ImGui::GetFontSize();
        const float tileH = unit * 2.8f, tileW = unit * 8.5f;
        ImGui::BeginChild("##bin", {0, mediaBinHeight()}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        ImDrawList* bdl = ImGui::GetWindowDrawList();
        for (std::size_t i = 0; i < layout.media.size(); ++i) {
            const std::string& path = layout.media[i];
            if (i) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(i));
            ImGui::InvisibleButton("##tile", {tileW, tileH});
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const bool hot = ImGui::IsItemHovered();
            // a thumbnail from a clip that plays it, else a kind-coloured block
            const ClipStrip* thumb = nullptr;
            bool sound = false;
            for (const auto& t : layout.tracks)
                for (const auto& c : t.clips)
                    if (!thumb && c.source == path) {
                        if (c.picture) thumb = clipStrip(c.id);
                        sound = sound || (c.sound && !c.picture);
                    }
            std::string ext = std::filesystem::path(path).extension().string();
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            static const char* sounds[] = {".wav", ".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wma", ".aif", ".aiff"};
            sound = sound || std::find(std::begin(sounds), std::end(sounds), ext) != std::end(sounds);
            bdl->AddRectFilled(a, b, ImGui::GetColorU32(hot ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 4);
            const float thumbW = tileH * 16 / 9;
            const ImVec2 ta{a.x + 3, a.y + 3}, tb{a.x + 3 + thumbW - 6, b.y - 3};
            if (thumb && !thumb->frames.empty() && thumb->frames[thumb->frames.size() / 3].tex) {
                bdl->AddImage(thumb->frames[thumb->frames.size() / 3].id(), ta, tb);
            } else {
                bdl->AddRectFilled(ta, tb, sound ? IM_COL32(60, 140, 90, 255) : IM_COL32(70, 100, 160, 255), 3);
                bdl->AddText({ta.x + 4, ta.y + 2}, IM_COL32(255, 255, 255, 220), sound ? "sound" : "picture");
            }
            std::string name = std::filesystem::path(path).filename().string();
            const float textRoom = b.x - tb.x - 8;
            if (ImGui::CalcTextSize(name.c_str()).x > textRoom) {
                while (name.size() > 3 && ImGui::CalcTextSize((name + "...").c_str()).x > textRoom) name.pop_back();
                name += "...";
            }
            bdl->AddText({tb.x + 5, a.y + (tileH - unit) / 2}, ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
            ImGui::SetItemTooltip("%s\nDrag onto a track to put it on the timeline", path.c_str());
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("WW_MEDIA", path.c_str(), path.size() + 1);
                ImGui::TextUnformatted(std::filesystem::path(path).filename().string().c_str());
                ImGui::EndDragDropSource();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    // geometry
    std::size_t toggleCount = 0;
    for (const auto& t : layout.tracks) toggleCount = std::max(toggleCount, t.toggles.size());
    const float toggleW = ImGui::GetFontSize() * 1.35f;
    const float nameW = ImGui::GetFontSize() * (toggleCount ? 3.5f : 6.5f) + toggleW * static_cast<float>(toggleCount);
    const float rulerH = ImGui::GetFontSize() * 1.4f;
    const float rowH = rowHeight(layout);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size{std::max(avail.x, 64.0f), rulerH + rowH * static_cast<float>(std::max<std::size_t>(1, layout.tracks.size()))};
    ImGui::InvisibleButton("timeline", size, ImGuiButtonFlags_MouseButtonLeft);
    const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
    const ImVec2 o = ImGui::GetItemRectMin();
    const float laneW = std::max(16.0f, size.x - nameW);
    const double fitPps = laneW / (end * 1.05);
    if (st.zoom <= 0) {
        st.zoom = fitPps;
        st.scroll = 0;
    }
    if (hovered && io.MouseWheel != 0) {
        if (io.KeyCtrl) {
            const double at = st.scroll + (io.MousePos.x - o.x - nameW) / st.zoom;
            st.zoom = std::clamp(st.zoom * std::pow(1.2, static_cast<double>(io.MouseWheel)), fitPps * 0.5, 2000.0);
            st.scroll = at - (io.MousePos.x - o.x - nameW) / st.zoom;
        } else {
            st.scroll -= io.MouseWheel * 60.0 / st.zoom;
        }
    }
    st.scroll = std::clamp(st.scroll, 0.0, std::max(0.0, end * 1.05 - laneW / st.zoom));
    auto toX = [&](double t) { return o.x + nameW + static_cast<float>((t - st.scroll) * st.zoom); };
    auto toT = [&](float x) { return st.scroll + (x - o.x - nameW) / st.zoom; };
    // a file dragged from the media bin: lands on the track and time under the mouse
    bool binDrop = false;
    std::string binPath;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("WW_MEDIA", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
            binPath = static_cast<const char*>(pl->Data);
            binDrop = pl->IsDelivery();
        }
        ImGui::EndDragDropTarget();
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool dark = darkLook();
    const ImU32 bg = dark ? IM_COL32(32, 34, 38, 255) : IM_COL32(226, 228, 232, 255);
    const ImU32 lane = dark ? IM_COL32(40, 42, 47, 255) : IM_COL32(236, 238, 242, 255);
    const ImU32 line = dark ? IM_COL32(60, 63, 70, 255) : IM_COL32(200, 202, 208, 255);
    const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    const ImU32 muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRectFilled(o, {o.x + size.x, o.y + size.y}, bg);
    dl->PushClipRect(o, {o.x + size.x, o.y + size.y}, true);

    // ruler
    const double step = niceStep(1.0 / st.zoom);
    for (double t = std::floor(st.scroll / step) * step; toX(t) < o.x + size.x; t += step) {
        if (t < 0) continue;
        const float x = toX(t);
        if (x < o.x + nameW) continue;
        dl->AddLine({x, o.y + rulerH * 0.55f}, {x, o.y + size.y}, line);
        dl->AddText({x + 3, o.y + 1}, muted, clockText(t).c_str());
    }

    // which row and clip the mouse is over
    const auto rowAt = [&](float y) { return static_cast<int>(std::floor((y - o.y - rulerH) / rowH)); };
    const int hoverRow = rowAt(io.MousePos.y);
    const double hoverT = toT(io.MousePos.x);
    if (!binPath.empty()) {
        // where it would land: the row under the mouse (the model picks a track of the right kind), at that time
        const double at = std::max(0.0, hoverT);
        const float x = toX(at);
        const int row = layout.tracks.empty() ? 0 : std::clamp(hoverRow, 0, static_cast<int>(layout.tracks.size()) - 1);
        const float y0 = o.y + rulerH + rowH * static_cast<float>(std::max(0, row));
        dl->AddRectFilled({x, y0 + 3}, {x + 3, y0 + rowH - 3}, ImGui::GetColorU32(ImGuiCol_DragDropTarget));
        if (binDrop) {
            TimelineEdit e;
            e.kind = TimelineEditKind::AddFile;
            e.name = binPath;
            e.track = layout.tracks.empty() ? std::string() : layout.tracks[static_cast<std::size_t>(std::max(0, row))].id;
            e.place = at;
            runTimelineEdit(layout, e);
        }
    }
    // the track switches sit right-aligned in the name column, one square each
    const auto toggleRect = [&](std::size_t row, std::size_t i, std::size_t count) {
        const float y0 = o.y + rulerH + rowH * static_cast<float>(row);
        const float x0 = o.x + nameW - 4 - toggleW * static_cast<float>(count - i);
        return std::pair<ImVec2, ImVec2>{{x0 + 1, y0 + (rowH - toggleW) / 2}, {x0 + toggleW - 1, y0 + (rowH + toggleW) / 2}};
    };
    const auto toggleAt = [&](ImVec2 m) -> std::pair<const TimelineTrack*, const TimelineToggle*> {
        const int row = rowAt(m.y);
        if (row < 0 || row >= static_cast<int>(layout.tracks.size()) || m.x >= o.x + nameW) return {nullptr, nullptr};
        const auto& t = layout.tracks[static_cast<std::size_t>(row)];
        for (std::size_t i = 0; i < t.toggles.size(); ++i) {
            const auto [a, b] = toggleRect(static_cast<std::size_t>(row), i, t.toggles.size());
            if (m.x >= a.x && m.x <= b.x && m.y >= a.y && m.y <= b.y) return {&t, &t.toggles[i]};
        }
        return {nullptr, nullptr};
    };

    // start a drag: on a clip body (move), an edge (trim), or the ruler / empty lane (cursor)
    if (ImGui::IsItemActivated()) {
        st.drag = TimelineState::Drag::None;
        if (const auto [tt, tg] = toggleAt(io.MousePos); tg) {
            TimelineEdit e{TimelineEditKind::Toggle, {}, tt->id};
            e.name = tg->name;
            runTimelineEdit(layout, e);
        } else if (io.MousePos.y < o.y + rulerH || io.MousePos.x < o.x + nameW) {
            st.drag = TimelineState::Drag::Cursor;
        } else if (hoverRow >= 0 && hoverRow < static_cast<int>(layout.tracks.size())) {
            const auto& t = layout.tracks[static_cast<std::size_t>(hoverRow)];
            st.drag = TimelineState::Drag::Cursor;
            for (const auto& c : t.clips) {
                const float x0 = toX(c.start), x1 = toX(c.start + c.length);
                if (io.MousePos.x < x0 - 1 || io.MousePos.x > x1 + 1) continue;
                st.selClip = c.id;
                st.selTrack = t.id;
                st.grabT = hoverT;
                st.origStart = c.start;
                st.origLength = c.length;
                st.headroom = c.headroom;
                st.tailroom = c.tailroom;
                const bool narrow = x1 - x0 < kEdge * 3;
                // fade handles sit on the clip's top edge, the gain line across it
                const float cy0 = o.y + rulerH + rowH * static_cast<float>(hoverRow) + 3, cy1 = cy0 + rowH - 8;
                const float fiX = toX(c.start + std::max(0.0, c.fadeIn)), foX = toX(c.start + c.length - std::max(0.0, c.fadeOut));
                const bool top = io.MousePos.y < cy0 + kEdge * 1.5f;
                // a keyframe diamond along the clip's bottom edge: slide it
                const float keyY = o.y + rulerH + rowH * static_cast<float>(hoverRow) + rowH - 9;
                bool onKey = false;
                for (double k : c.keyframes) {
                    if (std::fabs(io.MousePos.x - toX(k)) < 6 && std::fabs(io.MousePos.y - keyY) < 7) {
                        st.drag = TimelineState::Drag::Key;
                        st.origValue = st.dragValue = k;
                        onKey = true;
                        break;
                    }
                }
                if (onKey) break;
                if (c.fadeIn >= 0 && top && std::fabs(io.MousePos.x - fiX) < kEdge) {
                    st.drag = TimelineState::Drag::FadeIn;
                    st.origValue = st.dragValue = c.fadeIn;
                    break;
                }
                if (c.fadeOut >= 0 && top && std::fabs(io.MousePos.x - foX) < kEdge) {
                    st.drag = TimelineState::Drag::FadeOut;
                    st.origValue = st.dragValue = c.fadeOut;
                    break;
                }
                if (c.gainHandle && std::fabs(io.MousePos.y - gainY(c.gainDb, cy0, cy1)) < 4 && io.MousePos.x > x0 + kEdge && io.MousePos.x < x1 - kEdge) {
                    st.drag = TimelineState::Drag::Gain;
                    st.origValue = st.dragValue = c.gainDb;
                    break;
                }
                if (!narrow && io.MousePos.x < x0 + kEdge && c.trimStart) st.drag = TimelineState::Drag::TrimStart;
                else if (!narrow && io.MousePos.x > x1 - kEdge) st.drag = TimelineState::Drag::TrimEnd;
                else st.drag = TimelineState::Drag::Move;
                break;
            }
            if (st.drag == TimelineState::Drag::Cursor) st.selClip.clear();
        }
    }
    if (active && st.drag == TimelineState::Drag::Cursor) {
        st.cursor = std::max(0.0, hoverT);
        // the picture follows the mouse (video_play.cpp) instead of waiting for the button to come up
        if (st.cursor != scrubTime_) scrubMovedAt_ = SDL_GetTicks();
        if (!timelinePlaying()) scrubTime_ = st.cursor;
    }
    if (ImGui::IsItemDeactivated() && st.drag == TimelineState::Drag::Cursor) {
        scrubTime_ = -1;
        if (layout.cursor >= 0) runTimelineEdit(layout, {TimelineEditKind::Cursor, {}, {}, {}, 0, 0, st.cursor});
        if (timelinePlaying()) seekTo(st.cursor);  // the picture and sound jump there too
        st.drag = TimelineState::Drag::None;
    }

    // fade and gain handles: the value follows the mouse, released = one clip_set
    const bool handleDrag = st.drag == TimelineState::Drag::FadeIn || st.drag == TimelineState::Drag::FadeOut || st.drag == TimelineState::Drag::Gain;
    if (handleDrag && active) {
        if (st.drag == TimelineState::Drag::FadeIn) st.dragValue = std::clamp(hoverT - st.origStart, 0.0, st.origLength);
        else if (st.drag == TimelineState::Drag::FadeOut) st.dragValue = std::clamp(st.origStart + st.origLength - hoverT, 0.0, st.origLength);
        else if (hoverRow >= 0) {
            const float cy0 = o.y + rulerH + rowH * static_cast<float>(hoverRow) + 3, cy1 = cy0 + rowH - 8;
            st.dragValue = std::round(gainAt(io.MousePos.y, cy0, cy1) * 2) / 2;  // half-dB steps
        }
        ImGui::SetTooltip(st.drag == TimelineState::Drag::Gain ? "gain %+.1f dB" : "fade %.2f s", st.dragValue);
    }
    if (handleDrag && ImGui::IsItemDeactivated()) {
        if (std::fabs(st.dragValue - st.origValue) > 1e-4) {
            TimelineEdit e{TimelineEditKind::SetProperty, st.selClip, st.selTrack};
            e.name = st.drag == TimelineState::Drag::FadeIn ? "fade_in" : st.drag == TimelineState::Drag::FadeOut ? "fade_out" : "gain";
            e.at = st.dragValue;
            runTimelineEdit(layout, e);
        }
        st.drag = TimelineState::Drag::None;
    }

    // a keyframe diamond: slides along its clip (snapping to the cursor), released = one move_keyframe
    if (st.drag == TimelineState::Drag::Key && active) {
        double t = std::clamp(hoverT, st.origStart, st.origStart + st.origLength);
        if (std::fabs(t - st.cursor) * st.zoom < 8) t = std::clamp(st.cursor, st.origStart, st.origStart + st.origLength);
        st.dragValue = t;
        ImGui::SetTooltip("keyframe at %.2f s into the clip", st.dragValue - st.origStart);
    }
    if (st.drag == TimelineState::Drag::Key && ImGui::IsItemDeactivated()) {
        if (std::fabs(st.dragValue - st.origValue) > 1e-4) {
            TimelineEdit e{TimelineEditKind::MoveKey, st.selClip, st.selTrack};
            e.at = st.origValue;
            e.start = st.dragValue;
            runTimelineEdit(layout, e);
        }
        st.drag = TimelineState::Drag::None;
    }

    // snapping: to 0, the cursor and other clips' edges within 8 px
    auto snap = [&](double t, const std::string& skipClip) {
        double best = t, bestPx = 8.0 / st.zoom;
        auto consider = [&](double c) {
            if (std::fabs(c - t) < bestPx) best = c, bestPx = std::fabs(c - t);
        };
        consider(0);
        consider(st.cursor);
        for (const auto& m : layout.markers) consider(m.t);
        for (const auto& tr : layout.tracks)
            for (const auto& c : tr.clips)
                if (c.id != skipClip || tr.id != st.selTrack) consider(c.start), consider(c.start + c.length);
        return best;
    };

    // the clip being dragged, as it would end up
    double dragStart = st.origStart, dragLength = st.origLength;
    std::string dropTrack = st.selTrack;
    const bool dragging = active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) &&
                          (st.drag == TimelineState::Drag::Move || st.drag == TimelineState::Drag::TrimStart || st.drag == TimelineState::Drag::TrimEnd);
    if (dragging || (ImGui::IsItemDeactivated() && st.drag != TimelineState::Drag::None && st.drag != TimelineState::Drag::Cursor)) {
        const double delta = hoverT - st.grabT;
        if (st.drag == TimelineState::Drag::Move) {
            dragStart = std::max(0.0, st.origStart + delta);
            const double snappedStart = snap(dragStart, st.selClip), snappedEnd = snap(dragStart + st.origLength, st.selClip);
            if (snappedStart != dragStart) dragStart = snappedStart;
            else if (snappedEnd != dragStart + st.origLength) dragStart = std::max(0.0, snappedEnd - st.origLength);
            const TimelineTrack* from = nullptr;
            for (const auto& t : layout.tracks) if (t.id == st.selTrack) from = &t;
            if (from && hoverRow >= 0 && hoverRow < static_cast<int>(layout.tracks.size()) &&
                layout.tracks[static_cast<std::size_t>(hoverRow)].kind == from->kind) {
                dropTrack = layout.tracks[static_cast<std::size_t>(hoverRow)].id;
            }
        } else if (st.drag == TimelineState::Drag::TrimStart) {
            double lo = st.headroom >= 0 ? st.origStart - st.headroom : 0.0;
            lo = std::max(0.0, lo);
            dragStart = std::clamp(snap(st.origStart + delta, st.selClip), lo, st.origStart + st.origLength - 0.05);
            dragLength = st.origStart + st.origLength - dragStart;
        } else {
            const double hi = st.tailroom >= 0 ? st.origLength + st.tailroom : 1e9;
            dragLength = std::clamp(snap(st.origStart + st.origLength + delta, st.selClip) - st.origStart, 0.05, hi);
        }
    }
    if (ImGui::IsItemDeactivated() && st.drag != TimelineState::Drag::None && st.drag != TimelineState::Drag::Cursor) {
        const bool moved = std::fabs(dragStart - st.origStart) > 1e-4 || std::fabs(dragLength - st.origLength) > 1e-4 || dropTrack != st.selTrack;
        if (moved) {
            TimelineEdit e;
            e.clip = st.selClip;
            e.track = st.selTrack;
            if (st.drag == TimelineState::Drag::Move) {
                e.kind = TimelineEditKind::Move;
                e.start = dragStart;
                e.toTrack = dropTrack;
            } else if (st.drag == TimelineState::Drag::TrimStart) {
                e.kind = TimelineEditKind::TrimStart;
                e.start = dragStart;
            } else {
                e.kind = TimelineEditKind::TrimEnd;
                e.length = dragLength;
            }
            runTimelineEdit(layout, e);
            // a moved clip can change ids (audio projects number clips per track): select nothing rather than the wrong one
            if (dropTrack != st.selTrack) st.selClip.clear();
        }
        st.drag = TimelineState::Drag::None;
    }

    // lanes and clips
    std::string selLink;  // clips linked to the selection (video and its sound) are outlined too
    for (const auto& t : layout.tracks)
        for (const auto& c : t.clips)
            if (c.id == st.selClip && t.id == st.selTrack) selLink = c.link;
    const ImU32 clipCols[] = {IM_COL32(86, 140, 220, 255), IM_COL32(90, 170, 120, 255)};
    for (std::size_t ri = 0; ri < layout.tracks.size(); ++ri) {
        const auto& t = layout.tracks[ri];
        const float y0 = o.y + rulerH + rowH * static_cast<float>(ri), y1 = y0 + rowH - 2;
        bool off = false, locked = false;
        for (const auto& tg : t.toggles) {
            if (tg.on && (tg.name == "muted" || tg.name == "hidden")) off = true;
            if (tg.on && tg.name == "locked") locked = true;
        }
        dl->AddRectFilled({o.x + nameW, y0}, {o.x + size.x, y1}, lane);
        dl->AddText({o.x + 6, y0 + (rowH - ImGui::GetFontSize()) / 2}, off ? muted : text, t.name.c_str());
        for (std::size_t i = 0; i < t.toggles.size(); ++i) {
            const auto& tg = t.toggles[i];
            const auto [a, b] = toggleRect(ri, i, t.toggles.size());
            const bool over = hovered && io.MousePos.x >= a.x && io.MousePos.x <= b.x && io.MousePos.y >= a.y && io.MousePos.y <= b.y;
            const ImU32 fill = tg.on ? ImGui::ColorConvertFloat4ToU32(accent_) : (over ? line : lane);
            dl->AddRectFilled(a, b, fill, 3);
            dl->AddRect(a, b, line, 3);
            const ImVec2 ts = ImGui::CalcTextSize(tg.label.c_str());
            dl->AddText({(a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2}, tg.on ? ImGui::ColorConvertFloat4ToU32(accentText_) : muted, tg.label.c_str());
            if (over) ImGui::SetTooltip("%s%s", tg.tooltip.c_str(), tg.on ? " (on)" : "");
        }
        ImU32 col = clipCols[t.kind == "audio" ? 1 : 0];
        if (off || locked) col = (col & 0x00FFFFFF) | (off ? 0x70000000u : 0xB0000000u);
        for (const auto& c : t.clips) {
            const bool isSel = c.id == st.selClip && t.id == st.selTrack;
            const bool isDragged = isSel && dragging;
            const float x0 = toX(c.start), x1 = std::max(x0 + 2, toX(c.start + c.length));
            dl->AddRectFilled({x0, y0 + 3}, {x1, y1 - 3}, isDragged ? (col & 0x60FFFFFF) : col, 4);
            if ((c.picture || c.sound) && !isDragged && x1 > o.x + nameW && x0 < o.x + size.x) {
                if (const ClipStrip* strip = clipStrip(c.id)) {
                    const float cy0 = y0 + 4, cy1 = y1 - 4, ch = cy1 - cy0;
                    const float vx0 = std::max(x0 + 1, o.x + nameW), vx1 = std::min(x1 - 1, o.x + size.x);
                    dl->PushClipRect({vx0, cy0}, {vx1, cy1}, true);
                    const MediaStrip& m = strip->media;
                    // where in the source a timeline time falls
                    auto sourceAt = [&](double t) { return c.sourceIn + (t - c.start) * c.speed; };
                    if (!strip->frames.empty() && m.frameHeight > 0) {
                        const float tileW = std::max(8.0f, ch * static_cast<float>(m.frameWidth) / static_cast<float>(m.frameHeight));
                        const float first = x0 + std::floor(std::max(0.0f, vx0 - x0) / tileW) * tileW;
                        for (float xs = first; xs < vx1; xs += tileW) {
                            const double src = sourceAt(toT(xs + tileW / 2));
                            auto it = std::lower_bound(m.frameTimes.begin(), m.frameTimes.end(), src);
                            std::size_t i = static_cast<std::size_t>(it - m.frameTimes.begin());
                            if (i == m.frameTimes.size() || (i > 0 && src - m.frameTimes[i - 1] < m.frameTimes[i] - src)) i = i > 0 ? i - 1 : 0;
                            if (i < strip->frames.size() && strip->frames[i].tex) dl->AddImage(strip->frames[i].id(), {xs, cy0}, {xs + tileW, cy1});
                        }
                    }
                    if (!m.peaks.empty() && m.peaksPerSecond > 0 && c.speed != 0) {
                        // the waveform: over the thumbnails' lower half, or the whole clip for sound
                        const bool under = !strip->frames.empty();
                        const float mid = under ? cy1 - ch * 0.2f : (cy0 + cy1) / 2, half = under ? ch * 0.18f : ch * 0.45f;
                        const double perPx = std::fabs(c.speed) / st.zoom * m.peaksPerSecond;
                        const ImU32 wave = under ? IM_COL32(255, 255, 255, 170) : IM_COL32(20, 60, 35, 200);
                        for (float px = std::floor(vx0); px < vx1; px += 1.0f) {
                            const double a = sourceAt(toT(px)) * m.peaksPerSecond;
                            const long i0 = static_cast<long>(std::floor(std::min(a, a + (c.speed < 0 ? -perPx : perPx))));
                            const long i1 = std::max(i0, static_cast<long>(std::floor(i0 + perPx)));
                            float amp = 0;
                            for (long i = std::max(0L, i0); i <= i1 && i < static_cast<long>(m.peaks.size()); ++i) amp = std::max(amp, m.peaks[static_cast<std::size_t>(i)]);
                            if (amp > 0.01f) dl->AddLine({px, mid - amp * half}, {px, mid + amp * half}, wave);
                        }
                    }
                    dl->PopClipRect();
                }
            }
            if (c.fadeIn >= 0 || c.gainHandle) {
                // fades as shaded wedges with a handle on top, gain as a line (dragged: the new value)
                const bool editing = isSel && active && handleDrag;
                const double fi = editing && st.drag == TimelineState::Drag::FadeIn ? st.dragValue : std::max(0.0, c.fadeIn);
                const double fo = editing && st.drag == TimelineState::Drag::FadeOut ? st.dragValue : std::max(0.0, c.fadeOut);
                const double g = editing && st.drag == TimelineState::Drag::Gain ? st.dragValue : c.gainDb;
                const float cy0 = y0 + 3, cy1 = y1 - 5;
                const float fx = std::min(x1, toX(c.start + fi)), gx = std::max(x0, toX(c.start + c.length - fo));
                const ImU32 shade = IM_COL32(0, 0, 0, 80), edgeCol = IM_COL32(255, 255, 255, 200), knob = IM_COL32(255, 255, 255, 235);
                if (fi > 0) {
                    dl->AddTriangleFilled({x0, cy0}, {fx, cy0}, {x0, cy1}, shade);
                    dl->AddLine({x0, cy1}, {fx, cy0}, edgeCol, 1.5f);
                }
                if (fo > 0) {
                    dl->AddTriangleFilled({gx, cy0}, {x1, cy0}, {x1, cy1}, shade);
                    dl->AddLine({gx, cy0}, {x1, cy1}, edgeCol, 1.5f);
                }
                if (isSel || x1 - x0 > 40) {
                    dl->AddRectFilled({fx - 3, cy0}, {fx + 3, cy0 + 6}, knob, 1);
                    dl->AddRectFilled({gx - 3, cy0}, {gx + 3, cy0 + 6}, knob, 1);
                }
                if (c.gainHandle && (isSel || std::fabs(g) > 0.01)) {
                    const float gy = gainY(g, cy0, cy1);
                    dl->AddLine({fx, gy}, {gx, gy}, IM_COL32(255, 230, 120, isSel ? 230 : 120), isSel ? 1.5f : 1.0f);
                }
            }
            if (c.transitionIn > 0) {
                // the blend with the clip before: a darker wedge over its start
                const float xt = std::min(x1, toX(c.start + c.transitionIn));
                dl->AddTriangleFilled({x0, y1 - 3}, {xt, y0 + 3}, {xt, y1 - 3}, IM_COL32(0, 0, 0, 90));
                dl->AddLine({x0, y1 - 3}, {xt, y0 + 3}, IM_COL32(255, 255, 255, 160), 1.5f);
            }
            if (isSel && c.curve.size() > 1) {
                // the animated value across the clip, so the shape between keyframes shows
                const float cy0 = y0 + 8, cy1 = y1 - 12;
                std::vector<ImVec2> pts;
                for (std::size_t i = 0; i < c.curve.size(); ++i) {
                    const double t = c.start + c.length * static_cast<double>(i) / static_cast<double>(c.curve.size() - 1);
                    pts.push_back({toX(t), cy1 - (cy1 - cy0) * c.curve[i]});
                }
                dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), IM_COL32(255, 220, 90, 200), ImDrawFlags_None, 1.5f);
                dl->AddText({std::max(x0, o.x + nameW) + 4, y1 - 12 - ImGui::GetFontSize()}, IM_COL32(255, 220, 90, 230), c.curveName.c_str());
            }
            for (double k : c.keyframes) {
                const bool sliding = isSel && st.drag == TimelineState::Drag::Key && active && std::fabs(k - st.origValue) < 1e-6;
                const float kx = toX(sliding ? st.dragValue : k), ky = y1 - 7;
                if (kx < x0 - 1 || kx > x1 + 1) continue;
                const float r = sliding ? 5.5f : 4.0f;
                dl->AddQuadFilled({kx, ky - r}, {kx + r, ky}, {kx, ky + r}, {kx - r, ky}, IM_COL32(255, 220, 90, 255));
            }
            const bool linked = !isSel && !c.link.empty() && !selLink.empty() && c.link == selLink;
            if (isSel) dl->AddRect({x0, y0 + 3}, {x1, y1 - 3}, ImGui::ColorConvertFloat4ToU32(accent_), 4, 0, 2.5f);
            else if (linked) dl->AddRect({x0, y0 + 3}, {x1, y1 - 3}, ImGui::ColorConvertFloat4ToU32(accent_), 4, 0, 1.2f);
            dl->PushClipRect({std::max(x0, o.x + nameW), y0}, {x1, y1}, true);
            if (c.picture || c.sound) {
                // over thumbnails the name sits in the top corner, on a dark tab so it reads
                const ImVec2 ts = ImGui::CalcTextSize(c.label.c_str());
                const float lx = std::max(x0, o.x + nameW) + 4;
                dl->AddRectFilled({lx - 2, y0 + 4}, {lx + ts.x + 4, y0 + 5 + ts.y}, IM_COL32(0, 0, 0, 140), 3);
                dl->AddText({lx + 1, y0 + 4}, IM_COL32(255, 255, 255, 240), c.label.c_str());
            } else {
                dl->AddText({x0 + 6, y0 + (rowH - ImGui::GetFontSize()) / 2}, IM_COL32(255, 255, 255, 235), c.label.c_str());
            }
            dl->PopClipRect();
        }
        if (dragging && t.id == dropTrack) {
            // ghost of where it will land
            const float x0 = toX(dragStart), x1 = std::max(x0 + 2, toX(dragStart + dragLength));
            dl->AddRect({x0, y0 + 2}, {x1, y1 - 2}, ImGui::ColorConvertFloat4ToU32(accent_), 4, 0, 2.0f);
            dl->AddText({x0 + 4, y1 - ImGui::GetFontSize() - 2}, text, clockText(st.drag == TimelineState::Drag::TrimEnd ? dragStart + dragLength : dragStart).c_str());
        }
    }
    if (layout.tracks.empty()) {
        dl->AddText({o.x + nameW + 8, o.y + rulerH + 8}, muted,
                    layout.tools ? "No clips yet: drop videos, pictures or sound here, or press + Media" : "No tracks yet: add one with Clip (or Audio, Title) on the ribbon.");
    }

    // in/out range and markers on the ruler
    if (layout.inPoint >= 0 || layout.outPoint >= 0) {
        const float a = std::max(o.x + nameW, toX(std::max(0.0, layout.inPoint))), b = toX(layout.outPoint >= 0 ? layout.outPoint : end);
        if (b > a) {
            dl->AddRectFilled({a, o.y}, {b, o.y + rulerH}, (ImGui::ColorConvertFloat4ToU32(accent_) & 0x00FFFFFF) | 0x50000000u);
            dl->AddRectFilled({a, o.y + rulerH}, {b, o.y + size.y}, (ImGui::ColorConvertFloat4ToU32(accent_) & 0x00FFFFFF) | 0x14000000u);
        }
    }
    for (const auto& m : layout.markers) {
        const float mx = toX(m.t);
        if (mx < o.x + nameW) continue;
        const ImU32 mc = m.color == "red" ? IM_COL32(230, 70, 60, 255) : m.color == "green" ? IM_COL32(80, 200, 110, 255)
                         : m.color == "blue" ? IM_COL32(80, 140, 240, 255) : IM_COL32(245, 190, 60, 255);
        dl->AddTriangleFilled({mx - 5, o.y + 1}, {mx + 5, o.y + 1}, {mx, o.y + rulerH - 2}, mc);
        dl->AddLine({mx, o.y + rulerH}, {mx, o.y + size.y}, (mc & 0x00FFFFFF) | 0x60000000u);
        if (hovered && std::fabs(io.MousePos.x - mx) < 5 && io.MousePos.y < o.y + rulerH && !m.name.empty()) ImGui::SetTooltip("%s", m.name.c_str());
    }

    // the cursor, and where playback has got to
    const float cx = toX(st.cursor);
    if (cx >= o.x + nameW) dl->AddLine({cx, o.y}, {cx, o.y + size.y}, IM_COL32(240, 80, 70, 255), 2.0f);
    if (timelinePlaying()) {
        const float px = toX(transport_.position);
        if (px >= o.x + nameW) {
            dl->AddLine({px, o.y}, {px, o.y + size.y}, IM_COL32(255, 255, 255, 230), 2.0f);
            dl->AddTriangleFilled({px - 6, o.y}, {px + 6, o.y}, {px, o.y + 8}, IM_COL32(255, 255, 255, 230));
        }
        // keep it in view while it plays
        if (transport_.playing && (px > o.x + size.x - 20 || px < o.x + nameW)) st.scroll = std::max(0.0, transport_.position - 1.0);
    }

    // cursor shape over edges
    if (hovered && hoverRow >= 0 && hoverRow < static_cast<int>(layout.tracks.size())) {
        for (const auto& c : layout.tracks[static_cast<std::size_t>(hoverRow)].clips) {
            const float x0 = toX(c.start), x1 = toX(c.start + c.length);
            if (x1 - x0 >= kEdge * 3 && ((std::fabs(io.MousePos.x - x0) < kEdge && c.trimStart) || std::fabs(io.MousePos.x - x1) < kEdge)) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            }
        }
    }
    dl->PopClipRect();
}

}  // namespace wetwaresuite::gui
