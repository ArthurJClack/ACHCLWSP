// The open-image document photo and paint both edit, plus the helpers their
// commands share: dirty-region undo, doc/layer params, previews.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/ora.hpp"

namespace wetwaresuite::raster {

class ImageEdit;

class ImageDoc : public Document {
public:
    LayeredImage image;
    int active = 0;  // index of the layer edits go to
    // The selection: pixel edits only change what's inside it. Empty = nothing selected, so
    // edits touch the whole layer.
    Mask selection;
    bool hasSelection() const { return !selection.empty(); }
    // An edit that handles the selection itself (moving selected pixels) sets this while it runs,
    // so its changes outside the selection are kept.
    bool ignoreSelection = false;

    // Edits made with ImageEdit::Mode::Declared call this before writing pixels so undo keeps a copy
    // of exactly that area (clipped to the layer). Outside such an edit it only records the change.
    void touch(int layer, int x, int y, int width, int height);

    // What changed since the last takeChanges(), for viewers that redraw only that part
    // (the GUI canvas). `all` means "redraw everything" (new document, resize, layer changes).
    struct Changes {
        bool all = false;
        PixelRect rect;
        bool any() const { return all || !rect.empty(); }
    };
    Changes takeChanges();
    void markChanged(const PixelRect& r) { changes_.rect.add(r); }
    void markAllChanged() { changes_.all = true; }

    // What the file it came from held: 16 for 16-bit PNG/TIFF/PSD (saves keep 16 bits unless told
    // otherwise; edits themselves run at 8 bits per channel), and its embedded colour profile, which
    // was converted to sRGB on open ("" = none).
    int sourceBits = 8;
    std::string colorProfile;

    std::string kind() const override { return "image"; }
    std::string summary() const override;
    std::unique_ptr<Attachment> preview() const override;

    Image flatten() const { return composite(image.layers, image.width, image.height); }
    Layer& activeLayer() { return image.layers.at(static_cast<std::size_t>(active)); }

private:
    friend class ImageEdit;
    Changes changes_{true, {}};
    ImageEdit* recording_ = nullptr;  // the declared edit running now, if any
};

std::shared_ptr<ImageDoc> newImage(int width, int height, Color background);
// .ora keeps layers; anything else stb reads opens as one "Background" layer.
std::shared_ptr<ImageDoc> openImage(const std::filesystem::path& path);
// .ora keeps everything; .psd keeps the layers (`notes` says what it couldn't keep: adjustment
// layers, editable text...); other formats are flattened. .psd opens layer by layer when it can.
// bits: 8 or 16 per channel for .png/.tif; 0 = what the source file had (doc.sourceBits).
void saveImage(const ImageDoc& doc, const std::filesystem::path& path, int quality = 90, std::vector<std::string>* notes = nullptr,
               int bits = 0);
bool canOpenImage(const std::filesystem::path& path);
bool canSaveImage(const std::filesystem::path& path);

// Undoable edit that keeps only the pixels it changed, so long histories on huge images stay small.
//
// Mode::Diff (the default, works for any edit): takes a quick copy before running `fn`, then
// compares 64x64 tiles and keeps just the old tiles that differ; the full copy is dropped at once.
// If the edit changes the canvas or layer sizes or the number of layers (resize, crop, add layer)
// it keeps the whole old image instead.
// Mode::Declared (for brushes and other local edits): `fn` calls doc.touch() for every area before it
// writes there, and only those areas are copied, with no full copy at all. Such edits must not
// change sizes or the layer list.
// Undo restores the saved pixels; redo runs `fn` again, as before.
class ImageEdit : public Action {
public:
    enum class Mode { Diff, Declared };
    static constexpr int kTile = 64;

    ImageEdit(std::shared_ptr<ImageDoc> doc, std::function<Result(ImageDoc&)> fn, Mode mode = Mode::Diff)
        : doc_(std::move(doc)), fn_(std::move(fn)), mode_(mode) {}
    ~ImageEdit() override;
    Result apply() override;
    void revert() override;

    std::size_t undoBytes() const { return bytes_; }  // memory this step holds for undo
    bool keptWholeImage() const { return whole_ != nullptr; }

private:
    friend class ImageDoc;
    struct Patch {
        int layer, x, y, w, h;
        std::vector<std::uint8_t> px;  // the old pixels, row by row
    };
    using LayerMeta = LayerInfo;  // everything but the pixels and mask
    void save(int layer, const Image& from, int x, int y, int w, int h);
    void diff(const LayeredImage& before);
    void clear();
    void account(std::size_t bytes);

    std::shared_ptr<ImageDoc> doc_;
    std::function<Result(ImageDoc&)> fn_;
    Mode mode_;
    std::vector<Patch> patches_;
    std::vector<LayerMeta> meta_;
    std::unique_ptr<LayeredImage> whole_;
    std::vector<std::pair<int, Mask>> masks_;  // old layer masks that changed
    std::optional<Mask> selection_;           // the old selection, when it changed
    int beforeActive_ = 0;
    int beforeWidth_ = 0, beforeHeight_ = 0;
    std::size_t bytes_ = 0;
};

// Memory held for undo by every image edit alive right now (all documents), in bytes.
std::size_t undoMemoryBytes();

// Shared param specs.
ParamSpec docParam();
ParamSpec layerParam();    // optional index, 0 = bottom; defaults to the active layer
ParamSpec previewParam();  // optional: attach a PNG preview to the result
ParamSpec colorParam(const std::string& name, const std::string& description, bool required, const std::string& fallback);

// Builds an action factory for a command that edits one image document. `edit` runs inside an
// ImageEdit (or plain, when undoable is false); missing docs become a failed result, not a crash.
ActionFactory imageCommand(Session& session, std::function<Result(ImageDoc&, const Params&)> edit, bool undoable = true,
                           ImageEdit::Mode mode = ImageEdit::Mode::Diff);

// Resolves the "layer" param; returns -1 and sets error when out of range.
int layerIndex(const ImageDoc& doc, const Params& p, std::string& error);
// For commands that change pixels: false (with a readable error) for groups and adjustment layers,
// which have none. Text and shape layers become plain pixel layers (their settings are dropped),
// like rasterizing them in other editors.
bool pixelLayer(ImageDoc& doc, int index, std::string& error);
// layerIndex + pixelLayer: the layer a pixel edit goes to, or -1 with an error.
int pixelLayerIndex(ImageDoc& doc, const Params& p, std::string& error);

// Puts a new layer just above the active one (at the top of the group when a group is active),
// in the same group, and makes it active. Returns its index.
int insertLayer(ImageDoc& doc, Layer layer);

// A geometry change for the whole document (resize, crop, rotate, warp): every layer's pixels and
// mask and the selection go through fn and the canvas takes the size fn returns. Groups and
// adjustment layers stay pixel-less; text and shape layers become plain pixels.
void transformDocument(ImageDoc& doc, const std::function<Image(const Image&)>& fn);

// Adds a preview attachment when the "preview" param is true.
Result& maybePreview(Result& r, const ImageDoc& doc, const Params& p);

// Parses a colour param or produces a readable error.
bool colorArg(const Params& p, const std::string& key, const std::string& fallback, Color& out, std::string& error);

}  // namespace wetwaresuite::raster
