// Camera raw: develops DNG files (what phones shoot in "raw"/ProRAW mode, what Adobe's free DNG
// Converter makes from any camera's raw, and what Leica, Pentax, Ricoh, DJI... write directly).
//
// Reads Bayer (CFA) and linear DNGs, uncompressed or lossless-JPEG compressed, with their black
// and white levels, linearisation table, as-shot white balance, colour matrix, crop and orientation.
// The develop is quick rather than fancy: gradient-corrected demosaic, matrix to sRGB, then the
// settings below.
#pragma once

#include <filesystem>
#include <string>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

struct RawDevelop {
    double exposure = 0;                    // stops, on top of the file's own baseline exposure
    std::string whiteBalance = "as_shot";   // as_shot | auto | daylight | cloudy | shade | tungsten | fluorescent | flash | custom
    double temperature = 5500;              // kelvin, for custom
    double tint = 0;                        // -100 (green) .. 100 (magenta), for custom
    double highlights = 0;                  // -100..100: negative pulls bright areas back
    double shadows = 0;                     // -100..100: positive lifts dark areas
    double contrast = 0;                    // -100..100
    double saturation = 0;                  // -100..100
    bool halfSize = false;                  // 2x2 pixels per output pixel: 4x faster, half the size
};

struct RawInfo {
    int width = 0, height = 0;   // developed size (after crop, before half size)
    int bits = 0;                // sensor bits per sample
    std::string camera;          // make + model when the file says
    std::string layout;          // "RGGB", "BGGR"... or "linear"
    double asShotNeutral[3] = {1, 1, 1};
    double baselineExposure = 0;
};

bool isDng(const std::string& bytes);
// Throws std::runtime_error with a readable reason (e.g. X-Trans or lossy DNGs aren't supported).
Image developDng(const std::string& bytes, const RawDevelop& settings = {});
RawInfo dngInfo(const std::string& bytes);
// Lowercase raw extensions this build develops.
const std::vector<std::string>& rawExtensions();

// Lossless JPEG (ITU T.81 process 14, used inside DNG): decodes into `out` (width * components samples a row).
// Throws on bad data. Exposed for tests.
void decodeLosslessJpeg(const std::string& data, std::vector<std::uint16_t>& out, int& width, int& height, int& components);

}  // namespace wetwaresuite::raster
