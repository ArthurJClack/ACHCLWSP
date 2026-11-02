// Pieces the compositor files share: colours, the per-clip colour grade and
// effects, title drawing and the frame sources.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "wetwaresuite/cut/render.hpp"
#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::cut {

// "white", "#rrggbb", "red@0.5" (ffmpeg style alpha), "0xRRGGBB".
std::optional<raster::Color> parseColour(const std::string& text);
raster::Color colourOr(const std::string& text, raster::Color fallback);

// Colour correction and the picture effects that work on the clip's own pixels, at `local`
// seconds into the clip. `pixelScale` is how many of this image's pixels make one timeline pixel
// (for blur radii). Returns false when nothing needed doing.
bool applyLook(raster::Image& img, const Clip& clip, double local, double pixelScale, const Lut3d* lut);
// The chroma key alone (alpha and spill), part of applyLook.
void chromaKey(raster::Image& img, raster::Color key, double similarity, double softness, double spill);

// A title (or lower third) drawn at `scale` output px per timeline px. x, y, w, h: where the
// image goes, in timeline px. `chars` limits how many characters show (typewriter), -1 = all.
struct TitleImage {
    raster::Image image;
    double x = 0, y = 0, w = 0, h = 0;
};
TitleImage drawTitle(const Clip& clip, const Settings& settings, double scale, int chars);
std::size_t titleLength(const Clip& clip);  // characters, for the typewriter

// A video file's frames at one size, read through ffmpeg in order (re-seeking when needed).
class VideoReader {
public:
    VideoReader(std::string path, int width, int height, double rate);
    ~VideoReader();
    // The frame at a source time. False when it can't be decoded.
    bool frame(double sourceTime, raster::Image& out, std::string& error);

private:
    bool restart(double at, std::string& error);
    struct Proc;
    std::string path_;
    int w_, h_;
    double rate_;
    bool big_ = false;  // HD or bigger frames: decoded on the graphics card when there is one
    std::unique_ptr<Proc> proc_;
    double t0_ = 0;
    long next_ = 0;  // index of the frame the process gives next
    long lastIndex_ = -1;
    raster::Image last_;
    std::map<long, raster::Image> cache_;  // frames read ahead for backwards playback
};

struct FrameContext::Impl {
    std::mutex lock;
    std::map<std::string, std::shared_ptr<const raster::Image>> stills;
    std::map<std::string, std::unique_ptr<VideoReader>> readers;
    std::map<std::string, long> readerUse;
    long useCounter = 0;
    bool preview = false;      // the previews' context: reads proxies of big sources when they're there
    bool approximate = false;  // scrubbing: a nearby frame from the scrub cache beats waiting for the exact one
    std::map<std::string, std::shared_ptr<const TitleImage>> titles;
    std::map<std::string, std::shared_ptr<const Lut3d>> luts;

    std::shared_ptr<const raster::Image> still(const std::string& path);
    bool video(const std::string& path, double sourceTime, int width, int height, double rate, raster::Image& out);
    std::shared_ptr<const TitleImage> title(const Clip& clip, const Settings& settings, double scale, int chars);
    const Lut3d* lut(const std::string& path);
};

// The shared context previews use.
FrameContext& previewContext();
// Forgets the decoded sound of every source (part of clearCaches).
void clearSoundCache();
// Forgets the thumbnails and waveforms read for the timeline (part of clearCaches).
void clearStrips();
// The scrub cache's frame of a video source nearest to sourceTime, resized to width x height.
// False while nothing has been read yet (the first ask starts reading it in the background).
bool scrubFrame(const std::string& path, double sourceTime, int width, int height, raster::Image& out);

}  // namespace wetwaresuite::cut
