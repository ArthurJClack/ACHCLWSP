// Embedded colour profiles (ICC) and bit depth of image files.
//
// Files tagged with a wide-gamut profile (phone photos in Display P3, Adobe RGB, ProPhoto from
// raw developers) look washed out when their numbers are shown as sRGB. load()/decode() find the
// embedded profile in PNG, JPEG, TIFF and WebP files and convert the pixels to sRGB, which is what
// every edit, preview and save here assumes. Matrix/curve RGB profiles and gray profiles convert;
// LUT-based (printer/CMYK) profiles are reported but left alone.
#pragma once

#include <string>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::raster {

struct ColorProfile {
    bool present = false;      // the file carried a profile
    bool convertible = false;  // matrix/TRC RGB or gray: we can turn it into sRGB
    bool gray = false;
    bool srgbLike = false;     // already (close enough to) sRGB, nothing to do
    std::string description;   // e.g. "Display P3", "Adobe RGB (1998)"
    float matrix[9] = {};      // device RGB (linear) -> XYZ D50
    // tone curves for R, G, B (gray uses [0]): 4096-entry tables from code value 0..1 to linear 0..1
    std::vector<float> curve[3];
};

// The raw ICC bytes embedded in a PNG (iCCP), JPEG (APP2), TIFF (tag 34675) or WebP (ICCP) file; empty if none.
std::string findIccProfile(const std::string& fileBytes);
ColorProfile parseIccProfile(const std::string& icc);
// Converts 8-bit pixels from `profile` to sRGB in place. False when nothing was done.
bool convertToSrgb(Image& img, const ColorProfile& profile);

// Bits per channel stored in the file (8 or 16, 32 for float TIFF/HDR); 8 when unknown.
int fileBitDepth(const std::string& fileBytes);

// What open() reports: bit depth and profile of a file.
struct FileColorInfo {
    int bits = 8;
    std::string profile;  // description, "" when the file has none
    bool converted = false;
};
FileColorInfo describeColor(const std::string& fileBytes);

}  // namespace wetwaresuite::raster
