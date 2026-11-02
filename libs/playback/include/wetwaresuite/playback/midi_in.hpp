// MIDI input from a keyboard or pad controller, one device at a time per process.
// Notes arrive on the device's own thread and wait in a queue until read. Built on
// what each system ships: ALSA raw MIDI device files on Linux, WinMM on Windows,
// CoreMIDI on macOS, so there's nothing to install. The "keyboard" device is a
// virtual one fed by press() (the window's computer-keyboard piano, scripts, tests).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wetwaresuite::playback {

struct MidiEvent {
    bool on = false;   // note on (velocity > 0) or off
    int pitch = 60;    // MIDI note number
    int velocity = 0;  // 1..127 for note on
    double time = 0;   // seconds since the device was opened
};

class MidiInput {
public:
    static MidiInput& instance();
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // "keyboard" first, then the system's MIDI inputs.
    std::vector<std::string> devices();
    // Opens a device by name ("" or "default": the first hardware input, else "keyboard").
    bool open(const std::string& name, std::string& error);
    void close();
    bool isOpen() const;
    std::string device() const;
    // The events since the last read, oldest first.
    std::vector<MidiEvent> read();
    // A note on the virtual keyboard (works whichever device is open); time < 0 = now.
    void press(int pitch, int velocity, bool on, double time = -1);
    // Seconds since open() (the clock event times use).
    double now() const;

    struct Impl;

private:
    MidiInput();
    std::unique_ptr<Impl> impl_;
};

}  // namespace wetwaresuite::playback
