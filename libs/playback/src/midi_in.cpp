#include "wetwaresuite/playback/midi_in.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <CoreMIDI/CoreMIDI.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#endif

namespace wetwaresuite::playback {

namespace {

// Turns a MIDI byte stream (with running status) into note events.
struct Parser {
    std::uint8_t status = 0;
    std::uint8_t data[2] = {0, 0};
    int have = 0;
    template <class Emit>
    void feed(std::uint8_t b, Emit&& emit) {
        if (b >= 0xF8) return;  // real-time bytes can sit anywhere
        if (b & 0x80) {
            status = b >= 0xF0 ? 0 : b;  // system messages end running status
            have = 0;
            return;
        }
        if (!status) return;
        data[have++] = b;
        const std::uint8_t kind = status & 0xF0;
        const int need = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
        if (have < need) return;
        have = 0;
        if (kind == 0x90 || kind == 0x80) emit(kind == 0x90 && data[1] > 0, data[0], data[1]);
    }
};

}  // namespace

struct MidiInput::Impl {
    mutable std::mutex mutex;
    std::vector<MidiEvent> queue;
    std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now();
    std::string name;
    bool open = false;
    Parser parser;

    void push(bool on, int pitch, int velocity, double time = -1) {
        std::lock_guard lock(mutex);
        if (queue.size() > 100000) return;  // nobody is reading
        queue.push_back({on, std::clamp(pitch, 0, 127), on ? std::clamp(velocity, 1, 127) : 0,
                         time >= 0 ? time : std::chrono::duration<double>(std::chrono::steady_clock::now() - opened).count()});
    }
    void feed(const std::uint8_t* bytes, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) parser.feed(bytes[i], [this](bool on, int p, int v) { push(on, p, v); });
    }

#if defined(_WIN32)
    HMIDIIN handle = nullptr;
    static void CALLBACK callback(HMIDIIN, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR) {
        if (msg != MIM_DATA) return;
        auto* self = reinterpret_cast<Impl*>(user);
        const std::uint8_t bytes[3] = {static_cast<std::uint8_t>(p1 & 0xFF), static_cast<std::uint8_t>((p1 >> 8) & 0xFF),
                                       static_cast<std::uint8_t>((p1 >> 16) & 0xFF)};
        self->feed(bytes, 3);
    }
    std::vector<std::string> system() {
        std::vector<std::string> out;
        const UINT n = midiInGetNumDevs();
        for (UINT i = 0; i < n; ++i) {
            MIDIINCAPSA caps{};
            if (midiInGetDevCapsA(i, &caps, sizeof caps) == MMSYSERR_NOERROR) out.push_back(caps.szPname);
        }
        return out;
    }
    bool openSystem(std::size_t index, std::string& error) {
        if (midiInOpen(&handle, static_cast<UINT>(index), reinterpret_cast<DWORD_PTR>(&Impl::callback), reinterpret_cast<DWORD_PTR>(this),
                       CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            handle = nullptr;
            error = "could not open that MIDI input (is another program using it?)";
            return false;
        }
        midiInStart(handle);
        return true;
    }
    void closeSystem() {
        if (!handle) return;
        midiInStop(handle);
        midiInClose(handle);
        handle = nullptr;
    }
#elif defined(__APPLE__)
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    MIDIEndpointRef source = 0;
    static void readProc(const MIDIPacketList* list, void* ref, void*) {
        auto* self = static_cast<Impl*>(ref);
        const MIDIPacket* p = &list->packet[0];
        for (UInt32 i = 0; i < list->numPackets; ++i) {
            self->feed(p->data, p->length);
            p = MIDIPacketNext(p);
        }
    }
    static std::string nameOf(MIDIEndpointRef e) {
        CFStringRef s = nullptr;
        if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &s) != noErr || !s) return "MIDI input";
        char buf[256] = {0};
        CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8);
        CFRelease(s);
        return buf;
    }
    std::vector<std::string> system() {
        std::vector<std::string> out;
        const ItemCount n = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < n; ++i) out.push_back(nameOf(MIDIGetSource(i)));
        return out;
    }
    bool openSystem(std::size_t index, std::string& error) {
        if (!client && MIDIClientCreate(CFSTR("wetwaresuite"), nullptr, nullptr, &client) != noErr) {
            error = "CoreMIDI is not available";
            return false;
        }
        if (!port && MIDIInputPortCreate(client, CFSTR("wetwaresuite in"), &Impl::readProc, this, &port) != noErr) {
            error = "could not make a MIDI input port";
            return false;
        }
        source = MIDIGetSource(static_cast<ItemCount>(index));
        if (!source || MIDIPortConnectSource(port, source, nullptr) != noErr) {
            source = 0;
            error = "could not connect to that MIDI input";
            return false;
        }
        return true;
    }
    void closeSystem() {
        if (port && source) MIDIPortDisconnectSource(port, source);
        source = 0;
    }
#else
    // Linux: ALSA's raw MIDI files (/dev/snd/midiC<card>D<device>), read on a thread
    std::thread reader;
    std::atomic<bool> stop{false};
    std::vector<std::string> paths;
    std::vector<std::string> system() {
        std::vector<std::string> out;
        paths.clear();
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator("/dev/snd", ec))
            if (e.path().filename().string().rfind("midiC", 0) == 0) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            const std::string file = f.filename().string();  // midiC1D0
            const std::string card = file.substr(5, file.find('D') - 5);
            std::string label;
            std::ifstream id("/proc/asound/card" + card + "/id");
            std::getline(id, label);
            out.push_back((label.empty() ? "MIDI" : label) + " (" + file + ")");
            paths.push_back(f.string());
        }
        return out;
    }
    bool openSystem(std::size_t index, std::string& error) {
        if (index >= paths.size()) return false;
        const int fd = ::open(paths[index].c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            error = "could not open " + paths[index] + " (is another program using it, or are you in the audio group?)";
            return false;
        }
        stop.store(false);
        reader = std::thread([this, fd] {
            std::uint8_t buf[256];
            while (!stop.load()) {
                pollfd p{fd, POLLIN, 0};
                if (::poll(&p, 1, 50) <= 0) continue;
                const ssize_t n = ::read(fd, buf, sizeof buf);
                if (n > 0) feed(buf, static_cast<std::size_t>(n));
                else if (n == 0) break;  // unplugged
            }
            ::close(fd);
        });
        return true;
    }
    void closeSystem() {
        stop.store(true);
        if (reader.joinable()) reader.join();
    }
#endif

    ~Impl() {
        closeSystem();
#if defined(__APPLE__)
        if (port) MIDIPortDispose(port);
        if (client) MIDIClientDispose(client);
#endif
    }
};

MidiInput& MidiInput::instance() {
    static MidiInput m;
    return m;
}

MidiInput::MidiInput() : impl_(std::make_unique<Impl>()) {}
MidiInput::~MidiInput() = default;

std::vector<std::string> MidiInput::devices() {
    std::vector<std::string> out = {"keyboard"};
    for (auto& d : impl_->system()) out.push_back(d);
    return out;
}

bool MidiInput::open(const std::string& name, std::string& error) {
    close();
    const auto sys = impl_->system();
    std::string pick = name;
    if (pick.empty() || pick == "default") pick = sys.empty() ? "keyboard" : sys.front();
    if (pick != "keyboard") {
        const auto it = std::find(sys.begin(), sys.end(), pick);
        if (it == sys.end()) {
            error = "no MIDI input called '" + pick + "' (see beats.midi_devices)";
            return false;
        }
        if (!impl_->openSystem(static_cast<std::size_t>(it - sys.begin()), error)) return false;
    }
    std::lock_guard lock(impl_->mutex);
    impl_->queue.clear();
    impl_->parser = {};
    impl_->opened = std::chrono::steady_clock::now();
    impl_->name = pick;
    impl_->open = true;
    return true;
}

void MidiInput::close() {
    impl_->closeSystem();
    std::lock_guard lock(impl_->mutex);
    impl_->open = false;
    impl_->name.clear();
}

bool MidiInput::isOpen() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->open;
}

std::string MidiInput::device() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->name;
}

std::vector<MidiEvent> MidiInput::read() {
    std::lock_guard lock(impl_->mutex);
    std::vector<MidiEvent> out;
    out.swap(impl_->queue);
    return out;
}

void MidiInput::press(int pitch, int velocity, bool on, double time) { impl_->push(on && velocity > 0, pitch, velocity, time); }

double MidiInput::now() const {
    std::lock_guard lock(impl_->mutex);
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - impl_->opened).count();
}

}  // namespace wetwaresuite::playback
