// The pipeline builder's model: a list of blocks (one suite command each, with
// its settings), optionally run for every file matching a pattern. It turns
// into the JSON pipeline automate.pipeline runs, and reads back the pipelines it
// wrote (or any pipeline that only uses command steps). Window-free, unit tested.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace wetwaresuite::gui {

struct PipeBlock {
    std::string command;                                    // e.g. convert.file
    std::vector<std::pair<std::string, std::string>> args;  // name, value ($file, $stem... allowed)
    std::string ext;                                        // only for these extensions (png,jpg), empty = all
    bool keepGoing = false;                                 // carry on if this one fails
    bool off = false;                                       // switched off: kept, not run
};

struct PipeModel {
    std::string name, description;
    bool forEach = false;  // run the blocks once per file matching `glob`
    std::string glob = "in/*";
    std::vector<PipeBlock> blocks;
};

// The pipeline as JSON (pretty, with a trailing newline).
std::string pipelineJson(const PipeModel& model);

// Reads a JSON or simple pipeline back. False (with a reason) when it uses steps the
// builder can't show (run: lines, nested loops, conditions other than ext:).
bool parsePipelineJson(const std::string& text, PipeModel& out, std::string& error);

// Moves block `from` to sit at `to` (drag and drop). Returns the block's new index.
int moveBlock(PipeModel& model, int from, int to);

}  // namespace wetwaresuite::gui
