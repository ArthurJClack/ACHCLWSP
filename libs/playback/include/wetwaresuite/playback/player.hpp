// Live audio playback: one output device for the whole process, shared by the
// audio editor, beats, cut and the desktop window. It plays a Buffer from a
// start time, can loop a range, pause, seek and change volume while playing,
// and reports where the playhead is. Built on miniaudio (public domain), which
// picks WASAPI, CoreAudio, PulseAudio or ALSA at runtime. With no sound card
// (servers, CI, docker) or WETWARESUITE_AUDIO=null it falls back to a silent
// device that still moves the playhead in real time, so scripts behave the same.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/audio/buffer.hpp"

namespace wetwaresuite::playback {

struct Status {
    bool playing = false;  // sound is going out (false when paused or stopped)
    bool paused = false;
    bool loop = false;
    double position = 0;  // playhead in seconds, from the start of the buffer
    double start = 0;     // the range being played (loop range when looping)
    double end = 0;
    double length = 0;  // whole buffer
    double volume = 1;  // linear gain
    int rate = 0;
    int channels = 0;
    std::string source;  // what's playing ("aud1", "song.wav", ...)
    std::string device;  // output device name, or "silent (no sound device)"
    bool silent = false;
};

struct PlayRequest {
    std::shared_ptr<const audio::Buffer> buffer;
    double start = 0;
    double end = -1;  // < 0 = to the end
    bool loop = false;
    std::string source;
};

class Player {
public:
    // The process-wide player. The device opens on first play.
    static Player& instance();

    ~Player();
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // Starts playing (replacing whatever was playing). Returns false with error set
    // when the buffer is empty or no device (not even the silent one) could open.
    bool play(const PlayRequest& request, std::string& error);
    // Swaps in a new version of what's playing (an edit was made) without stopping: the
    // playhead keeps its time, and a range that ran to the old end runs to the new one.
    // Returns false when nothing is playing or paused.
    bool replace(std::shared_ptr<const audio::Buffer> buffer, std::string& error);
    void pause();
    void resume();
    void stop();
    void seek(double seconds);
    void setVolume(double gain);  // 0..4
    void setLoop(bool loop);
    Status status() const;
    // Blocks until playback stops or timeout seconds pass (< 0 = forever).
    // Returns true when playback has stopped. A looping play only ends by timeout
    // (or stop), so waiting forever on one returns false at once.
    bool wait(double timeoutSeconds);
    // Output devices the system offers ("default" first).
    std::vector<std::string> devices();
    // Picks an output device by name (or "default"); takes effect on the next play.
    bool selectDevice(const std::string& name, std::string& error);

    struct Impl;

private:
    Player();
    std::unique_ptr<Impl> impl_;
};

// Live input recording from a microphone or line in, one at a time per process.
// With no sound card it records silence in real time, so scripts behave the same;
// input "tone" records a 440 Hz test tone instead of a device (for checking levels).
struct RecordStatus {
    bool recording = false;
    double seconds = 0;    // recorded so far
    double peakDb = -200;  // loudest sample of the last ~50 ms, for a level meter
    double maxPeakDb = -200;  // loudest since the start: 0 means it clipped
    int rate = 0, channels = 0;
    std::string device;
    bool silent = false;
};

class Recorder {
public:
    static Recorder& instance();
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // device: "default", a name from devices(), or "tone". maxSeconds caps the take.
    bool start(int rate, int channels, const std::string& device, double maxSeconds, std::string& error);
    // Ends the take and hands it over (empty when nothing was recording).
    audio::Buffer stop();
    RecordStatus status() const;
    std::vector<std::string> devices();

    struct Impl;

private:
    Recorder();
    std::unique_ptr<Impl> impl_;
};

// For one-shot command lines: lets a non-looping play finish before the process
// exits, so `wetwaresuite audio.play path=a.wav` is heard instead of cut off.
void finishBeforeExit();

}  // namespace wetwaresuite::playback
