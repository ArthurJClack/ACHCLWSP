// Reading printed text out of a picture (a scanned page, a photo of a document), with nothing
// to install: the page is cut into lines, words and letters, and each letter is matched against
// the built-in sans, serif and mono faces. Quick and dependable on clean printed text at 200-300
// dpi in ordinary fonts; handwriting, decorative fonts and non-Latin scripts aren't read.
#pragma once

#include <string>
#include <vector>

#include "wetwaresuite/raster/image.hpp"

namespace wetwaresuite::pdf {

struct OcrWord {
    std::string text;            // ASCII
    double x = 0, y = 0, w = 0, h = 0;  // pixels: the word's box
    double baseline = 0;         // pixels down to the line's baseline
    double confidence = 0;       // 0..1
};

struct OcrLine {
    std::vector<OcrWord> words;
    double x = 0, y = 0, w = 0, h = 0;
};

struct OcrResult {
    std::vector<OcrLine> lines;
    std::string text;         // the lines joined with newlines (a blank line between blocks)
    double confidence = 0;    // average over the letters
    double textHeight = 0;    // typical capital height in pixels (tiny text reads badly: scan at more dpi)
};

OcrResult recognizeText(const raster::Image& image);

}  // namespace wetwaresuite::pdf
