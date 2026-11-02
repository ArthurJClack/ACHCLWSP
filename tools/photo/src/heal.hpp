// Retouching: healing brush and seamless clone (Poisson image editing) and
// content-aware fill (exemplar-based inpainting). Pure pixel functions plus
// the photo.heal / photo.clone_seamless / photo.remove_object commands.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::photo {

// Which pixels an operation touches: 1 = inside the area.
struct Mask {
    int width = 0, height = 0;
    std::vector<std::uint8_t> bits;

    Mask() = default;
    Mask(int w, int h) : width(w), height(h), bits(static_cast<std::size_t>(w) * h, 0) {}
    bool at(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height && bits[static_cast<std::size_t>(y) * width + x]; }
    void set(int x, int y) {
        if (x >= 0 && y >= 0 && x < width && y < height) bits[static_cast<std::size_t>(y) * width + x] = 1;
    }
    std::size_t count() const;
    // Bounding box of the set pixels; false when the mask is empty.
    bool bounds(int& x0, int& y0, int& x1, int& y1) const;  // inclusive
};

Mask rectMask(int w, int h, int x, int y, int rw, int rh);
Mask ellipseMask(int w, int h, int x, int y, int rw, int rh);  // ellipse inside the box
// Round brush of `radius` along the polyline through points (x0, y0, x1, y1, ...).
Mask strokeMask(int w, int h, const std::vector<double>& points, double radius);

struct SolveStats {
    int iterations = 0;
    bool converged = false;
};

// Poisson image editing: replaces the masked pixels of `target` so their
// gradients follow `source` (sampled at target position + (dx, dy)) while the
// pixels around the area stay exactly as they were. With `source` == nullptr
// the area is filled smoothly from its surroundings instead. Returns false
// and sets `error` when the area is empty, the whole image, or too large.
bool poissonBlend(raster::Image& target, const raster::Image* source, const Mask& mask, int dx, int dy,
                  SolveStats& stats, std::string& error);

// Content-aware fill (Criminisi et al. 2004): fills the masked pixels with
// patches copied from the rest of the image, most structured edges first,
// so lines and textures continue into the hole. `patch` is the patch side
// (odd, 3..31). Areas over kMaxInpaintPixels are filled on a smaller copy first and
// refined back up to full size (PatchMatch); areas over kMaxRemovePixels are refused.
inline constexpr std::size_t kMaxInpaintPixels = 60000;
inline constexpr std::size_t kMaxRemovePixels = 4'000'000;
bool inpaint(raster::Image& img, const Mask& mask, int patch, std::string& error);

void registerHealCommands(Session& session);

}  // namespace wetwaresuite::photo
