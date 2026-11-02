// Transport commands for live playback, registered under each tool that plays
// sound (audio.*, beats.*, cut.*) so every app has them. They all drive the one
// process-wide Player, so audio.stop also stops a song started by beats.play.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/playback/player.hpp"

namespace wetwaresuite::playback {

// Renders what a source (a document id given to <tool>.play) sounds like now, for
// live playback: an edit made while it plays is heard on the next <tool>.refresh.
using Renderer = std::function<std::shared_ptr<const audio::Buffer>(const std::string& source, std::string& error)>;

// <tool>.pause, .resume, .stop, .seek, .playhead, .volume, .loop, .wait, .devices, .output_device,
// and .refresh when a renderer is given
void registerTransportCommands(Session& session, const std::string& tool, Renderer render = {});

// The params every <tool>.play takes after its own: start, end, loop, wait, volume.
std::vector<ParamSpec> playParams();
// Starts playing with those params and builds the result (fields: source, start, end,
// length, device, silent; waits first when wait=true).
Result startPlayback(std::shared_ptr<const audio::Buffer> buffer, const std::string& source, const Params& p);
// Adds playhead fields (playing, paused, position, ...) to a result.
void statusFields(Result& r, const Status& s);

}  // namespace wetwaresuite::playback
