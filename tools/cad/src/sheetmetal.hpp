// Sheet metal (internal to cad): sheet_base / sheet_flange features and the flat pattern.
#pragma once

#include <string>
#include <vector>

#include "build.hpp"

namespace wetwaresuite {
class Session;
}

namespace wetwaresuite::cad {

namespace detail {

struct SheetInfo {
    struct Flange {
        std::string name;
        std::size_t edge = 0;  // index into outline: the edge from outline[edge] to the next point
        double length = 0;
        double angle = 90;
        bool up = true;
        double radius = 0;
    };
    bool ok = false;
    std::string base;
    std::vector<mesh::Vec2> outline;  // counter-clockwise
    double thickness = 0;
    double radius = 0;  // default inside bend radius
    double k = 0.44;
    std::vector<Flange> flanges;  // in feature order
};

bool isSheetMetal(const std::string& type);
// The sheet part a feature belongs to (a sheet_base or a flange of one); ok=false when it isn't one.
SheetInfo sheetInfo(const CadDoc& doc, const std::string& featureName);
Built buildSheetMetal(const Ctx& c, const Done& done);

}  // namespace detail

void registerSheetMetalCommands(Session& session);

}  // namespace wetwaresuite::cad
