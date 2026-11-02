// The effects rack: the effect table, settings parsing and describing, and
// the stateful processors that run a rack (with automation) over audio.
#include "wetwaresuite/audio/rack.hpp"
#include "wetwaresuite/audio/clap_host.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <sstream>

#include "wetwaresuite/audio/dsp.hpp"
#include "internal.hpp"

namespace wetwaresuite::audio {

using detail::kPi;

namespace {

const std::vector<std::string> kOffOn = {"off", "on"};

std::vector<EffectParam> eqParams() {
    std::vector<EffectParam> v;
    const double freqs[kEqBands] = {80, 200, 500, 1000, 2500, 5000, 10000, 16000};
    std::vector<std::string> types = {"off"};
    for (const auto& n : filterTypeNames()) types.push_back(n);
    for (int k = 1; k <= kEqBands; ++k) {
        const std::string b = "b" + std::to_string(k) + "_";
        v.push_back({b + "type", 0, static_cast<double>(types.size() - 1), 0, "", "band " + std::to_string(k) + " filter shape", types});
        v.push_back({b + "freq", 20, 20000, freqs[k - 1], "Hz", "band " + std::to_string(k) + " frequency", {}});
        v.push_back({b + "gain", -24, 24, 0, "dB", "band " + std::to_string(k) + " boost or cut (peaking and shelves)", {}});
        v.push_back({b + "q", 0.1, 20, 0.7071, "", "band " + std::to_string(k) + " width: higher is narrower", {}});
    }
    v.push_back({"output", -24, 24, 0, "dB", "output gain", {}});
    return v;
}

std::string bandName(double hz) {
    if (hz >= 1000) return "g" + std::to_string(static_cast<int>(hz / 1000)) + "k";
    return "g" + std::to_string(static_cast<int>(hz));
}

std::vector<EffectParam> graphicParams() {
    std::vector<EffectParam> v;
    for (double f : graphicEqFrequencies()) v.push_back({bandName(f), -24, 24, 0, "dB", "boost or cut around " + (f >= 1000 ? std::to_string(static_cast<int>(f / 1000)) + " kHz" : std::to_string(static_cast<int>(f)) + " Hz"), {}});
    v.push_back({"output", -24, 24, 0, "dB", "output gain", {}});
    return v;
}

std::vector<EffectType> buildTypes() {
    return {
        {"eq", "Parametric EQ", "Up to 8 bands, each a low/high-pass, band-pass, notch, peaking (bell) or shelf filter.", eqParams()},
        {"graphic_eq", "Graphic EQ", "Ten octave bands from 31 Hz to 16 kHz, each boosted or cut.", graphicParams()},
        {"filter", "Filter", "A resonant low-pass, high-pass, band-pass or notch filter; automate the cutoff for sweeps.",
         {{"mode", 0, 3, 0, "", "filter shape", {"lowpass", "highpass", "bandpass", "notch"}},
          {"cutoff", 20, 20000, 1000, "Hz", "cutoff frequency", {}},
          {"resonance", 0.1, 20, 0.7071, "", "resonance (Q)", {}},
          {"slope", 1, 4, 1, "", "steepness: 12 dB/octave per step", {}}}},
        {"compressor", "Compressor", "Evens out the level: everything over the threshold is turned down by the ratio.",
         {{"threshold", -60, 0, -18, "dB", "level where compression starts", {}},
          {"ratio", 1, 20, 3, "", "how hard it turns down (4 = 4 dB over becomes 1 dB over)", {}},
          {"attack", 0.1, 200, 10, "ms", "how fast it reacts", {}},
          {"release", 5, 2000, 120, "ms", "how fast it lets go", {}},
          {"knee", 0, 24, 6, "dB", "soft knee width", {}},
          {"makeup", -12, 24, 0, "dB", "gain added after compressing", {}},
          {"auto_makeup", 0, 1, 0, "", "add back about half the reduction", kOffOn}}},
        {"limiter", "Limiter", "A look-ahead brickwall limiter: nothing gets over the ceiling (5 ms look-ahead).",
         {{"ceiling", -30, 0, -1, "dB", "the most the output may reach", {}},
          {"release", 1, 1000, 60, "ms", "how fast the level comes back", {}},
          {"gain", 0, 24, 0, "dB", "drive into the limiter (louder)", {}}}},
        {"gate", "Noise gate", "Silences the sound between phrases: quieter than the threshold is turned down by range.",
         {{"threshold", -90, 0, -45, "dB", "level that opens the gate", {}},
          {"attack", 0.1, 100, 2, "ms", "how fast it opens", {}},
          {"hold", 0, 1000, 60, "ms", "how long it stays open after the sound drops", {}},
          {"release", 5, 2000, 150, "ms", "how fast it closes", {}},
          {"range", -90, 0, -80, "dB", "how far closed is", {}}}},
        {"expander", "Expander", "Downward expansion: below the threshold quiet sounds get quieter still (gentler than a gate).",
         {{"threshold", -90, 0, -40, "dB", "level below which it expands", {}},
          {"ratio", 1, 10, 2, "", "2 = every dB under becomes 2 dB under", {}},
          {"attack", 0.1, 100, 5, "ms", "how fast it reacts", {}},
          {"release", 5, 2000, 100, "ms", "how fast it lets go", {}},
          {"range", -90, 0, -40, "dB", "the most it turns down", {}}}},
        {"deesser", "De-esser", "Tames harsh s and sh sounds: turns down only the highs, only when they're loud.",
         {{"frequency", 2000, 12000, 6000, "Hz", "where the sibilance starts", {}},
          {"threshold", -60, 0, -30, "dB", "sibilance level where it starts working", {}},
          {"reduction", 0, 24, 10, "dB", "the most it turns the highs down", {}}}},
        {"reverb", "Reverb", "Algorithmic room reverb (Freeverb: 8 combs and 4 allpasses per side).",
         {{"room", 0, 1, 0.6, "", "room size", {}},
          {"damping", 0, 1, 0.5, "", "how dull the reflections are", {}},
          {"wet", 0, 1, 0.25, "", "reverb level", {}},
          {"dry", 0, 1, 1, "", "original level", {}},
          {"width", 0, 1, 1, "", "stereo spread", {}},
          {"predelay", 0, 200, 0, "ms", "gap before the reverb starts", {}}}},
        {"delay", "Delay", "Echoes: a delay line with feedback and a tone control on the repeats; ping-pong bounces them left and right.",
         {{"time", 1, 2000, 375, "ms", "time between repeats", {}},
          {"feedback", 0, 0.95, 0.35, "", "how much each repeat feeds the next", {}},
          {"mix", 0, 1, 0.3, "", "echo level (added to the original)", {}},
          {"pingpong", 0, 1, 0, "", "alternate repeats between left and right", kOffOn},
          {"tone", 200, 20000, 8000, "Hz", "repeats get darker above this", {}}}},
        {"chorus", "Chorus", "Thickens the sound with slowly wobbling delayed copies.",
         {{"rate", 0.05, 5, 0.8, "Hz", "wobble speed", {}},
          {"depth", 0, 10, 3, "ms", "wobble depth", {}},
          {"delay", 2, 40, 15, "ms", "base delay", {}},
          {"feedback", 0, 0.9, 0, "", "feedback", {}},
          {"spread", 0, 180, 90, "deg", "phase difference between left and right", {}},
          {"mix", 0, 1, 0.5, "", "wet/dry balance", {}}}},
        {"flanger", "Flanger", "The jet-plane sweep: a very short modulated delay with feedback.",
         {{"rate", 0.05, 5, 0.25, "Hz", "sweep speed", {}},
          {"depth", 0, 10, 2, "ms", "sweep depth", {}},
          {"delay", 0.1, 10, 1, "ms", "base delay", {}},
          {"feedback", -0.95, 0.95, 0.5, "", "feedback (negative for a hollow sound)", {}},
          {"mix", 0, 1, 0.5, "", "wet/dry balance", {}}}},
        {"phaser", "Phaser", "Sweeping notches from a chain of allpass filters.",
         {{"rate", 0.05, 5, 0.4, "Hz", "sweep speed", {}},
          {"depth", 0, 1, 0.8, "", "sweep range (1 = two octaves each way)", {}},
          {"stages", 2, 12, 6, "", "allpass stages (more = more notches)", {}},
          {"feedback", 0, 0.95, 0.5, "", "resonance", {}},
          {"frequency", 100, 4000, 800, "Hz", "centre of the sweep", {}},
          {"mix", 0, 1, 0.5, "", "wet/dry balance", {}}}},
        {"tremolo", "Tremolo", "Volume wobble; spread 180 turns it into an auto-pan.",
         {{"rate", 0.1, 20, 5, "Hz", "wobble speed", {}},
          {"depth", 0, 1, 0.5, "", "how deep", {}},
          {"shape", 0, 2, 0, "", "wave shape", {"sine", "triangle", "square"}},
          {"spread", 0, 180, 0, "deg", "phase difference between left and right", {}}}},
        {"distortion", "Distortion", "Saturation and clipping: soft (tape-like), hard (clipper), fuzz, foldback or a bit crusher.",
         {{"drive", 0, 48, 12, "dB", "gain into the shaper", {}},
          {"mode", 0, 4, 0, "", "kind of distortion", {"soft", "hard", "fuzz", "foldback", "bitcrush"}},
          {"bits", 2, 16, 8, "", "bit crusher resolution", {}},
          {"tone", 500, 20000, 12000, "Hz", "low-pass after the shaper", {}},
          {"mix", 0, 1, 1, "", "wet/dry balance", {}},
          {"output", -24, 12, 0, "dB", "output gain", {}}}},
        {"utility", "Utility", "Gain, balance, stereo width and phase invert.",
         {{"gain", -60, 24, 0, "dB", "gain", {}},
          {"pan", -1, 1, 0, "", "balance -1 left .. 1 right", {}},
          {"width", 0, 2, 1, "", "stereo width: 0 mono, 1 unchanged, 2 extra wide", {}},
          {"invert", 0, 1, 0, "", "flip the polarity", kOffOn}}},
    };
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string numText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4g", v);
    return buf;
}

double coefficient(double ms, int rate) { return ms <= 0 ? 0.0 : std::exp(-1.0 / (ms * 0.001 * rate)); }

// Transposed direct form II, double state.
struct BiquadState {
    double z1 = 0, z2 = 0;
    double run(const Biquad& f, double in) {
        const double out = f.b0 * in + z1;
        z1 = f.b1 * in - f.a1 * out + z2;
        z2 = f.b2 * in - f.a2 * out;
        return out;
    }
};

double lfoWave(int shape, double phase) {  // phase 0..1 -> -1..1
    phase -= std::floor(phase);
    switch (shape) {
        case 1: return phase < 0.5 ? 4 * phase - 1 : 3 - 4 * phase;
        case 2: return phase < 0.5 ? 1.0 : -1.0;
        default: return std::sin(2 * kPi * phase);
    }
}

// A circular delay line read at fractional positions.
struct DelayLine {
    std::vector<float> buf;
    std::size_t pos = 0;
    void init(std::size_t n) { buf.assign(std::max<std::size_t>(n, 4), 0.0f); pos = 0; }
    void push(float x) {
        buf[pos] = x;
        if (++pos >= buf.size()) pos = 0;
    }
    // `delay` samples ago (>= 1), linear interpolation.
    float read(double delay) const {
        delay = std::clamp(delay, 1.0, static_cast<double>(buf.size() - 2));
        const double rp = static_cast<double>(pos) - delay + static_cast<double>(buf.size());
        const auto i0 = static_cast<std::size_t>(rp);
        const double f = rp - static_cast<double>(i0);
        const float a = buf[i0 % buf.size()], b = buf[(i0 + 1) % buf.size()];
        return static_cast<float>(a + (b - a) * f);
    }
};

// ---- processors ----

class EqProc : public Processor {
public:
    EqProc(int rate, int ch, bool graphic) : rate_(rate), graphic_(graphic) {
        const std::size_t bands = graphic ? graphicEqFrequencies().size() : static_cast<std::size_t>(kEqBands);
        filters_.resize(bands);
        active_.assign(bands, 0);
        state_.assign(bands, std::vector<BiquadState>(static_cast<std::size_t>(ch)));
    }
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) update();
        for (std::size_t b = 0; b < filters_.size(); ++b) {
            if (!active_[b]) continue;
            for (int c = 0; c < ch; ++c) {
                auto& st = state_[b][static_cast<std::size_t>(c)];
                for (std::size_t i = 0; i < n; ++i) x[c][i] = static_cast<float>(st.run(filters_[b], x[c][i]));
            }
        }
        if (out_ != 1.0f) {
            for (int c = 0; c < ch; ++c)
                for (std::size_t i = 0; i < n; ++i) x[c][i] *= out_;
        }
    }

private:
    void update() {
        dirty_ = false;
        out_ = static_cast<float>(dbToGain(get("output")));
        if (graphic_) {
            const auto& f = graphicEqFrequencies();
            for (std::size_t b = 0; b < f.size(); ++b) {
                const double g = get(bandName(f[b]));
                active_[b] = std::fabs(g) > 1e-6 && f[b] < rate_ * 0.48 ? 1 : 0;
                if (active_[b]) filters_[b] = Biquad::design(FilterType::Peaking, rate_, f[b], 1.41, g);
            }
            return;
        }
        for (int k = 0; k < kEqBands; ++k) {
            const std::string p = "b" + std::to_string(k + 1) + "_";
            const int t = static_cast<int>(std::lround(get(p + "type")));
            auto& on = active_[static_cast<std::size_t>(k)];
            on = t > 0 && t <= static_cast<int>(filterTypeNames().size());
            if (on) filters_[static_cast<std::size_t>(k)] = Biquad::design(static_cast<FilterType>(t - 1), rate_, get(p + "freq"), get(p + "q"), get(p + "gain"));
        }
    }
    int rate_;
    bool graphic_;
    float out_ = 1;
    std::vector<Biquad> filters_;
    std::vector<char> active_;
    std::vector<std::vector<BiquadState>> state_;
};

class FilterProc : public Processor {
public:
    FilterProc(int rate, int ch) : rate_(rate), state_(4, std::vector<BiquadState>(static_cast<std::size_t>(ch))) {}
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            const FilterType types[] = {FilterType::LowPass, FilterType::HighPass, FilterType::BandPass, FilterType::Notch};
            f_ = Biquad::design(types[std::clamp(static_cast<int>(std::lround(get("mode"))), 0, 3)], rate_, get("cutoff"), get("resonance"));
            stages_ = std::clamp(static_cast<int>(std::lround(get("slope"))), 1, 4);
        }
        for (int s = 0; s < stages_; ++s)
            for (int c = 0; c < ch; ++c) {
                auto& st = state_[static_cast<std::size_t>(s)][static_cast<std::size_t>(c)];
                for (std::size_t i = 0; i < n; ++i) x[c][i] = static_cast<float>(st.run(f_, x[c][i]));
            }
    }

private:
    int rate_;
    Biquad f_;
    int stages_ = 1;
    std::vector<std::vector<BiquadState>> state_;
};

class CompressorProc : public Processor {
public:
    explicit CompressorProc(int rate) : rate_(rate) {}
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            T_ = get("threshold"), R_ = std::max(1.0, get("ratio")), W_ = std::max(0.0, get("knee"));
            makeup_ = get("makeup") + (get("auto_makeup") >= 0.5 ? -T_ * (1 - 1 / R_) / 2 : 0.0);
            att_ = coefficient(get("attack"), rate_), rel_ = coefficient(get("release"), rate_);
        }
        for (std::size_t i = 0; i < n; ++i) {
            float level = 0;
            for (int c = 0; c < ch; ++c) level = std::max(level, std::fabs(x[c][i]));
            const double xd = gainToDb(level);
            double y = xd;
            if (2 * (xd - T_) > W_) y = T_ + (xd - T_) / R_;
            else if (W_ > 0 && 2 * std::fabs(xd - T_) <= W_) y = xd + (1 / R_ - 1) * (xd - T_ + W_ / 2) * (xd - T_ + W_ / 2) / (2 * W_);
            const double red = xd - y;
            const double k = red > smooth_ ? att_ : rel_;
            smooth_ = k * smooth_ + (1 - k) * red;
            const auto g = static_cast<float>(dbToGain(makeup_ - smooth_));
            for (int c = 0; c < ch; ++c) x[c][i] *= g;
        }
    }

private:
    int rate_;
    double T_ = -18, R_ = 3, W_ = 6, makeup_ = 0, att_ = 0, rel_ = 0, smooth_ = 0;
};

class LimiterProc : public Processor {
public:
    LimiterProc(int rate, int ch) : rate_(rate), look_(std::max<std::size_t>(1, static_cast<std::size_t>(rate * 0.005))) {
        delay_.assign(static_cast<std::size_t>(ch), std::vector<float>(look_ + 1, 0.0f));
        ahead_.assign(look_ + 1, 1.0f);
    }
    std::size_t latency() const override { return look_; }
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            ceiling_ = static_cast<float>(dbToGain(get("ceiling")));
            drive_ = static_cast<float>(dbToGain(get("gain")));
            rel_ = coefficient(get("release"), rate_);
        }
        const std::size_t L = look_ + 1;
        for (std::size_t i = 0; i < n; ++i) {
            float level = 0;
            for (int c = 0; c < ch; ++c) {
                const float v = x[c][i] * drive_;
                delay_[static_cast<std::size_t>(c)][pos_] = v;
                level = std::max(level, std::fabs(v));
            }
            const float need = level > ceiling_ ? ceiling_ / level : 1.0f;
            // minimum of the last look+1 needs = what the frame leaving the delay must get
            while (!q_.empty() && q_.back().second >= need) q_.pop_back();
            q_.emplace_back(count_, need);
            while (q_.front().first + look_ < count_) q_.pop_front();
            const float ahead = q_.front().second;
            sum_ += ahead - ahead_[pos_];
            ahead_[pos_] = ahead;
            // the average of the last look+1 of those: a smooth attack that is never above any need it covers
            const double g = std::min(static_cast<double>(ahead), sum_ / static_cast<double>(L));
            if (g < cur_) cur_ = g;
            else cur_ = rel_ * cur_ + (1 - rel_) * g;
            const std::size_t out = (pos_ + 1) % L;  // the oldest frame in the delay
            for (int c = 0; c < ch; ++c) {
                const float v = delay_[static_cast<std::size_t>(c)][out] * static_cast<float>(cur_);
                x[c][i] = std::clamp(v, -ceiling_, ceiling_);
            }
            pos_ = out;
            ++count_;
        }
    }

private:
    int rate_;
    std::size_t look_;
    std::vector<std::vector<float>> delay_;
    std::vector<float> ahead_;
    std::deque<std::pair<std::size_t, float>> q_;
    std::size_t pos_ = 0, count_ = 0;
    double sum_ = 0;  // sum of ahead_ (look+1 ones before the start: silence needs no reduction)
    double cur_ = 1, rel_ = 0;
    float ceiling_ = 1, drive_ = 1;

public:
    void primeSum() { sum_ = static_cast<double>(look_ + 1); }
};

class GateProc : public Processor {
public:
    GateProc(int rate, bool expander) : rate_(rate), expander_(expander) {}
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            thr_ = get("threshold");
            att_ = coefficient(get("attack"), rate_), rel_ = coefficient(get("release"), rate_);
            hold_ = expander_ ? 0 : static_cast<std::size_t>(get("hold") * 0.001 * rate_);
            range_ = std::min(0.0, get("range"));
            ratio_ = expander_ ? std::max(1.0, get("ratio")) : 1.0;
            // the gate moves in dB: all the way open in `attack`, all the way closed in `release`
            const double span = std::max(1.0, -range_);
            up_ = span / std::max(1.0, get("attack") * 0.001 * rate_);
            down_ = span / std::max(1.0, get("release") * 0.001 * rate_);
        }
        const double decay = coefficient(10, rate_);
        const double openAt = dbToGain(thr_), closeAt = dbToGain(thr_ - 4);
        for (std::size_t i = 0; i < n; ++i) {
            float level = 0;
            for (int c = 0; c < ch; ++c) level = std::max(level, std::fabs(x[c][i]));
            double targetDb;
            if (expander_) {
                const double k = level > env_ ? att_ : rel_;
                env_ = k * env_ + (1 - k) * level;
                const double ed = gainToDb(env_);
                targetDb = ed < thr_ ? std::max(range_, (ed - thr_) * (ratio_ - 1)) : 0.0;
                curDb_ = targetDb;  // the envelope follower already sets the speed
            } else {
                env_ = std::max(static_cast<double>(level), env_ * decay);
                if (env_ >= openAt) {
                    open_ = true;
                    holdLeft_ = hold_;
                } else if (open_ && env_ < closeAt) {
                    if (holdLeft_ > 0) --holdLeft_;
                    else open_ = false;
                }
                targetDb = open_ ? 0.0 : range_;
                if (curDb_ < targetDb) curDb_ = std::min(targetDb, curDb_ + up_);
                else curDb_ = std::max(targetDb, curDb_ - down_);
            }
            const auto g = static_cast<float>(curDb_ <= -89.9 ? 0.0 : dbToGain(curDb_));
            for (int c = 0; c < ch; ++c) x[c][i] *= g;
        }
    }

private:
    int rate_;
    bool expander_;
    double thr_ = -45, att_ = 0, rel_ = 0, range_ = -80, ratio_ = 1, env_ = 0, curDb_ = 0, up_ = 1, down_ = 1;
    std::size_t hold_ = 0, holdLeft_ = 0;
    bool open_ = false;
};

// A dynamic high shelf: the highs above `frequency` come down only while they're loud.
class DeEsserProc : public Processor {
public:
    DeEsserProc(int rate, int ch) : rate_(rate), detect_(static_cast<std::size_t>(ch)), shelf_(static_cast<std::size_t>(ch)) {}
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            freq_ = get("frequency");
            hp_ = Biquad::design(FilterType::HighPass, rate_, freq_, 0.7071);
            thrDb_ = get("threshold");
            maxDb_ = std::fabs(get("reduction"));
            designed_ = false;
        }
        const double att = coefficient(1, rate_), rel = coefficient(60, rate_);
        for (std::size_t i = 0; i < n; ++i) {
            if (countdown_ == 0) {
                const double red = std::clamp(gainToDb(env_) - thrDb_, 0.0, maxDb_);
                if (std::fabs(red - shelfDb_) > 0.05 || !designed_) {
                    shelfDb_ = red;
                    f_ = Biquad::design(FilterType::HighShelf, rate_, freq_ * 0.8, 0.7071, -red);
                    designed_ = true;
                }
                countdown_ = 32;
            }
            --countdown_;
            double level = 0;
            for (int c = 0; c < ch; ++c) {
                level = std::max(level, std::fabs(detect_[static_cast<std::size_t>(c)].run(hp_, x[c][i])));
                x[c][i] = static_cast<float>(shelf_[static_cast<std::size_t>(c)].run(f_, x[c][i]));
            }
            const double k = level > env_ ? att : rel;
            env_ = k * env_ + (1 - k) * level;
        }
    }

private:
    int rate_;
    Biquad hp_, f_;
    std::vector<BiquadState> detect_, shelf_;
    double freq_ = 6000, thrDb_ = -30, maxDb_ = 10, env_ = 0, shelfDb_ = 0;
    int countdown_ = 0;
    bool designed_ = false;
};

// Freeverb (Jezar at Dreampoint, public domain), stateful.
struct Comb {
    std::vector<float> buf;
    std::size_t pos = 0;
    float store = 0;
    float run(float in, float fb, float damp) {
        const float out = buf[pos];
        store = out * (1 - damp) + store * damp;
        buf[pos] = in + store * fb;
        if (++pos >= buf.size()) pos = 0;
        return out;
    }
};
struct Allpass {
    std::vector<float> buf;
    std::size_t pos = 0;
    float run(float in) {
        const float b = buf[pos];
        buf[pos] = in + b * 0.5f;
        if (++pos >= buf.size()) pos = 0;
        return b - in;
    }
};
struct Bank {
    std::vector<Comb> combs;
    std::vector<Allpass> aps;
    Bank(int rate, int spread) {
        const int ct[] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        const int at[] = {556, 441, 341, 225};
        const double s = rate / 44100.0;
        for (int t : ct) combs.push_back({std::vector<float>(static_cast<std::size_t>(std::max(1.0, (t + spread) * s)), 0.0f)});
        for (int t : at) aps.push_back({std::vector<float>(static_cast<std::size_t>(std::max(1.0, (t + spread) * s)), 0.0f)});
    }
    float run(float in, float fb, float damp) {
        float out = 0;
        for (auto& c : combs) out += c.run(in, fb, damp);
        for (auto& a : aps) out = a.run(out);
        return out;
    }
};

class ReverbProc : public Processor {
public:
    ReverbProc(int rate, int ch) : rate_(rate) {
        for (int c = 0; c < std::max(1, ch); ++c) banks_.emplace_back(rate, c * 23);
        pre_.init(static_cast<std::size_t>(rate * 0.21) + 4);
    }
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            fb_ = static_cast<float>(std::clamp(get("room"), 0.0, 1.0) * 0.28 + 0.7);
            damp_ = static_cast<float>(std::clamp(get("damping"), 0.0, 1.0) * 0.4);
            const double wet = std::clamp(get("wet"), 0.0, 1.0) * 3.0, width = std::clamp(get("width"), 0.0, 1.0);
            dry_ = static_cast<float>(std::clamp(get("dry"), 0.0, 1.0));
            wet1_ = static_cast<float>(wet * (width / 2 + 0.5)), wet2_ = static_cast<float>(wet * ((1 - width) / 2));
            preFrames_ = get("predelay") * 0.001 * rate_;
        }
        const float inGain = 0.015f / static_cast<float>(std::max(1, ch));
        for (std::size_t i = 0; i < n; ++i) {
            float mono = 0;
            for (int c = 0; c < ch; ++c) mono += x[c][i] * inGain;
            pre_.push(mono);
            const float in = preFrames_ >= 1 ? pre_.read(preFrames_) : mono;
            if (ch == 2) {
                const float l = banks_[0].run(in, fb_, damp_), r = banks_[1].run(in, fb_, damp_);
                x[0][i] = x[0][i] * dry_ + l * wet1_ + r * wet2_;
                x[1][i] = x[1][i] * dry_ + r * wet1_ + l * wet2_;
            } else {
                for (int c = 0; c < ch; ++c) x[c][i] = x[c][i] * dry_ + banks_[static_cast<std::size_t>(c)].run(in, fb_, damp_) * (wet1_ + wet2_);
            }
        }
    }

private:
    int rate_;
    std::vector<Bank> banks_;
    DelayLine pre_;
    double preFrames_ = 0;
    float fb_ = 0.8f, damp_ = 0.2f, dry_ = 1, wet1_ = 0.5f, wet2_ = 0;
};

class DelayProc : public Processor {
public:
    DelayProc(int rate, int ch) : rate_(rate), lines_(static_cast<std::size_t>(ch)), tone_(static_cast<std::size_t>(ch), 0.0) {
        for (auto& l : lines_) l.init(static_cast<std::size_t>(rate * 2.05) + 4);
    }
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            target_ = std::max(1.0, get("time") * 0.001 * rate_);
            if (cur_ <= 0) cur_ = target_;
            fb_ = std::clamp(get("feedback"), 0.0, 0.95);
            mix_ = std::clamp(get("mix"), 0.0, 1.0);
            ping_ = get("pingpong") >= 0.5 && ch == 2;
            toneK_ = std::exp(-2 * kPi * std::clamp(get("tone"), 20.0, rate_ * 0.45) / rate_);
        }
        const double glide = 1.0 - std::exp(-1.0 / (0.05 * rate_));  // 50 ms to settle on a new time
        for (std::size_t i = 0; i < n; ++i) {
            cur_ += (target_ - cur_) * glide;
            if (ping_) {
                const float l = lines_[0].read(cur_), r = lines_[1].read(cur_);
                tone_[0] = (1 - toneK_) * l + toneK_ * tone_[0];
                tone_[1] = (1 - toneK_) * r + toneK_ * tone_[1];
                lines_[0].push(static_cast<float>(0.5 * (x[0][i] + x[1][i]) + fb_ * tone_[1]));
                lines_[1].push(static_cast<float>(fb_ * tone_[0]));
                x[0][i] += static_cast<float>(mix_ * l);
                x[1][i] += static_cast<float>(mix_ * r);
                continue;
            }
            for (int c = 0; c < ch; ++c) {
                auto& line = lines_[static_cast<std::size_t>(c)];
                const float d = line.read(cur_);
                double& t = tone_[static_cast<std::size_t>(c)];
                t = (1 - toneK_) * d + toneK_ * t;
                line.push(static_cast<float>(x[c][i] + fb_ * t));
                x[c][i] += static_cast<float>(mix_ * d);
            }
        }
    }

private:
    int rate_;
    std::vector<DelayLine> lines_;
    std::vector<double> tone_;
    double target_ = 0, cur_ = 0, fb_ = 0.35, mix_ = 0.3, toneK_ = 0;
    bool ping_ = false;
};

// Chorus and flanger: a modulated delay per channel.
class ModDelayProc : public Processor {
public:
    ModDelayProc(int rate, int ch, bool flanger) : rate_(rate), flanger_(flanger), lines_(static_cast<std::size_t>(ch)) {
        for (auto& l : lines_) l.init(static_cast<std::size_t>(rate * 0.06) + 8);
        last_.assign(static_cast<std::size_t>(ch), 0.0f);
    }
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            rate_hz_ = get("rate");
            depth_ = get("depth") * 0.001 * rate_;
            base_ = get("delay") * 0.001 * rate_;
            fb_ = get("feedback");
            mix_ = std::clamp(get("mix"), 0.0, 1.0);
            spread_ = flanger_ ? 0.25 : get("spread") / 360.0;
        }
        const double step = rate_hz_ / rate_;
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < ch; ++c) {
                auto& line = lines_[static_cast<std::size_t>(c)];
                const double lfo = 0.5 + 0.5 * std::sin(2 * kPi * (phase_ + spread_ * c));
                const double d = std::max(1.0, base_ + depth_ * lfo);
                const float wet = line.read(d);
                line.push(static_cast<float>(x[c][i] + fb_ * wet));
                x[c][i] = static_cast<float>((1 - mix_) * x[c][i] + mix_ * wet);
            }
            phase_ += step;
            if (phase_ >= 1) phase_ -= 1;
        }
    }

private:
    int rate_;
    bool flanger_;
    std::vector<DelayLine> lines_;
    std::vector<float> last_;
    double phase_ = 0, rate_hz_ = 1, depth_ = 0, base_ = 1, fb_ = 0, mix_ = 0.5, spread_ = 0;
};

class PhaserProc : public Processor {
public:
    PhaserProc(int rate, int ch) : rate_(rate), st_(static_cast<std::size_t>(ch), std::vector<double>(24, 0.0)), last_(static_cast<std::size_t>(ch), 0.0) {}
    void process(float* const* x, int ch, std::size_t n) override {
        if (dirty_) {
            dirty_ = false;
            hz_ = get("rate"), depth_ = std::clamp(get("depth"), 0.0, 1.0), fb_ = std::clamp(get("feedback"), 0.0, 0.95);
            centre_ = get("frequency"), mix_ = std::clamp(get("mix"), 0.0, 1.0);
            stages_ = std::clamp(static_cast<int>(std::lround(get("stages"))), 2, 12);
        }
        const double step = hz_ / rate_;
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < ch; ++c) {
                const double lfo = std::sin(2 * kPi * (phase_ + 0.25 * c));
                const double f = std::clamp(centre_ * std::pow(2.0, 2.0 * depth_ * lfo), 20.0, rate_ * 0.45);
                const double t = std::tan(kPi * f / rate_);
                const double a = (1 - t) / (1 + t);
                auto& s = st_[static_cast<std::size_t>(c)];
                double y = x[c][i] + fb_ * last_[static_cast<std::size_t>(c)];
                for (int k = 0; k < stages_; ++k) {  // first-order allpass, DF1: y = -a x + x1 + a y1
                    double& x1 = s[static_cast<std::size_t>(2 * k)];
                    double& y1 = s[static_cast<std::size_t>(2 * k + 1)];
                    const double out = -a * y + x1 + a * y1;
                    x1 = y;
                    y1 = out;
                    y = out;
                }
                last_[static_cast<std::size_t>(c)] = y;
                x[c][i] = static_cast<float>((1 - mix_) * x[c][i] + mix_ * y);
            }
            phase_ += step;
            if (phase_ >= 1) phase_ -= 1;
        }
    }

private:
    int rate_;
    std::vector<std::vector<double>> st_;
    std::vector<double> last_;
    double phase_ = 0, hz_ = 0.4, depth_ = 0.8, fb_ = 0.5, centre_ = 800, mix_ = 0.5;
    int stages_ = 6;
};

class TremoloProc : public Processor {
public:
    explicit TremoloProc(int rate) : rate_(rate) {}
    void process(float* const* x, int ch, std::size_t n) override {
        const double step = get("rate") / rate_, depth = std::clamp(get("depth"), 0.0, 1.0), spread = get("spread") / 360.0;
        const int shape = static_cast<int>(std::lround(get("shape")));
        for (std::size_t i = 0; i < n; ++i) {
            for (int c = 0; c < ch; ++c) {
                const double g = 1 - depth * (0.5 + 0.5 * lfoWave(shape, phase_ + spread * c));
                x[c][i] = static_cast<float>(x[c][i] * g);
            }
            phase_ += step;
            if (phase_ >= 1) phase_ -= 1;
        }
    }

private:
    int rate_;
    double phase_ = 0;
};

class DistortionProc : public Processor {
public:
    DistortionProc(int rate, int ch) : rate_(rate), lp_(static_cast<std::size_t>(ch), 0.0), dcx_(static_cast<std::size_t>(ch), 0.0), dcy_(static_cast<std::size_t>(ch), 0.0) {}
    void process(float* const* x, int ch, std::size_t n) override {
        const double drive = dbToGain(get("drive")), mix = std::clamp(get("mix"), 0.0, 1.0), out = dbToGain(get("output"));
        const int mode = static_cast<int>(std::lround(get("mode")));
        const double q = std::pow(2.0, std::clamp(std::round(get("bits")), 2.0, 16.0) - 1);
        const double k = std::exp(-2 * kPi * std::clamp(get("tone"), 20.0, rate_ * 0.45) / rate_);
        const double dcR = 1.0 - 2 * kPi * 10.0 / rate_;
        for (int c = 0; c < ch; ++c) {
            double& lp = lp_[static_cast<std::size_t>(c)];
            double& dx = dcx_[static_cast<std::size_t>(c)];
            double& dy = dcy_[static_cast<std::size_t>(c)];
            for (std::size_t i = 0; i < n; ++i) {
                const double in = x[c][i];
                const double u = in * drive;
                double y;
                switch (mode) {
                    case 1: y = std::clamp(u, -1.0, 1.0); break;
                    case 2: {
                        const double v = (in + 0.15) * drive;  // biased before the gain: uneven halves, even harmonics
                        y = v > 0 ? 1 - std::exp(-v) : -0.6 * (1 - std::exp(v / 0.6));
                        break;
                    }
                    case 3: {  // fold back into -1..1
                        double v = std::fmod(u + 1.0, 4.0);
                        if (v < 0) v += 4;
                        y = v < 2 ? v - 1 : 3 - v;
                        break;
                    }
                    case 4: y = std::round(u * q) / q; break;
                    default: y = std::tanh(u);
                }
                // DC blocker for fuzz's bias, then the tone low-pass
                double blocked = y;
                if (mode == 2) {
                    blocked = y - dx + dcR * dy;
                    dx = y;
                    dy = blocked;
                }
                lp = (1 - k) * blocked + k * lp;
                x[c][i] = static_cast<float>(((1 - mix) * in + mix * lp) * out);
            }
        }
    }

private:
    int rate_;
    std::vector<double> lp_, dcx_, dcy_;
};

class UtilityProc : public Processor {
public:
    void process(float* const* x, int ch, std::size_t n) override {
        const double g = dbToGain(get("gain")) * (get("invert") >= 0.5 ? -1.0 : 1.0);
        const double pan = std::clamp(get("pan"), -1.0, 1.0), width = std::clamp(get("width"), 0.0, 2.0);
        const double gl = pan > 0 ? 1 - pan : 1.0, gr = pan < 0 ? 1 + pan : 1.0;
        // ramp from the last block's gains so automation doesn't click
        const double tl = g * gl, tr = g * gr;
        if (first_) curL_ = tl, curR_ = tr, curW_ = width, first_ = false;
        const double dl = (tl - curL_) / static_cast<double>(std::max<std::size_t>(1, n));
        const double dr = (tr - curR_) / static_cast<double>(std::max<std::size_t>(1, n));
        const double dw = (width - curW_) / static_cast<double>(std::max<std::size_t>(1, n));
        for (std::size_t i = 0; i < n; ++i) {
            curL_ += dl, curR_ += dr, curW_ += dw;
            if (ch == 2) {
                const double m = 0.5 * (x[0][i] + x[1][i]), s = 0.5 * (x[0][i] - x[1][i]) * curW_;
                x[0][i] = static_cast<float>((m + s) * curL_);
                x[1][i] = static_cast<float>((m - s) * curR_);
            } else {
                for (int c = 0; c < ch; ++c) x[c][i] = static_cast<float>(x[c][i] * curL_);
            }
        }
        curL_ = tl, curR_ = tr, curW_ = width;
    }

private:
    bool first_ = true;
    double curL_ = 1, curR_ = 1, curW_ = 1;
};

}  // namespace

// ---- the table ----

const EffectParam* EffectType::param(const std::string& n) const {
    for (const auto& p : params)
        if (p.name == n) return &p;
    return nullptr;
}

const std::vector<EffectType>& effectTypes() {
    static const std::vector<EffectType> t = buildTypes();
    return t;
}

const EffectType* findEffectType(const std::string& name) {
    const std::string n = lower(name);
    for (const auto& t : effectTypes())
        if (t.name == n) return &t;
    if (n == "parametric_eq" || n == "param_eq" || n == "equalizer") return findEffectType("eq");
    if (n == "graphic") return findEffectType("graphic_eq");
    if (n == "compress") return findEffectType("compressor");
    if (n == "limit") return findEffectType("limiter");
    if (n == "echo") return findEffectType("delay");
    if (n == "de_esser" || n == "deess") return findEffectType("deesser");
    if (n == "distort" || n == "drive" || n == "saturation") return findEffectType("distortion");
    if (n == "gain") return findEffectType("utility");
    return nullptr;
}

const std::vector<std::string>& effectTypeNames() {
    static const std::vector<std::string> v = [] {
        std::vector<std::string> out;
        for (const auto& t : effectTypes()) out.push_back(t.name);
        return out;
    }();
    return v;
}

const std::vector<double>& graphicEqFrequencies() {
    static const std::vector<double> f = {31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
    return f;
}

double Effect::get(const std::string& name) const {
    if (auto it = params.find(name); it != params.end()) return it->second;
    const EffectType* t = findEffectType(type);
    const EffectParam* p = t ? t->param(name) : nullptr;
    return p ? p->def : 0.0;
}

namespace {

bool parseValue(const EffectParam& p, const std::string& text, double& out, std::string& error) {
    const std::string t = lower(text);
    if (!p.choices.empty()) {
        for (std::size_t i = 0; i < p.choices.size(); ++i) {
            if (p.choices[i] == t) return out = static_cast<double>(i), true;
        }
        if (p.choices == kOffOn) {
            if (t == "true" || t == "yes" || t == "1") return out = 1, true;
            if (t == "false" || t == "no" || t == "0") return out = 0, true;
        }
        if (p.name.size() > 5 && p.name.compare(p.name.size() - 5, 5, "_type") == 0) {
            FilterType ft;
            if (parseFilterType(t, ft)) return out = static_cast<double>(static_cast<int>(ft) + 1), true;
        }
        std::string list;
        for (const auto& c : p.choices) list += (list.empty() ? "" : ", ") + c;
        error = p.name + " is one of " + list;
        return false;
    }
    char* end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        error = p.name + " takes a number, not \"" + text + "\"";
        return false;
    }
    std::string unit = lower(std::string(end));
    if (unit == "k" || unit == "khz") v *= 1000;
    else if (unit == "s" && p.unit == "ms") v *= 1000;
    if (v < p.min - 1e-9 || v > p.max + 1e-9) {
        error = p.name + " must be " + numText(p.min) + ".." + numText(p.max) + (p.unit.empty() ? "" : " " + p.unit);
        return false;
    }
    out = v;
    return true;
}

bool parseBands(const std::string& text, std::map<std::string, double>& out, std::string& error) {
    std::vector<std::string> bands;
    std::string cur;
    for (char c : text + ",") {
        if (c == ',' || c == ';' || c == '|') {
            if (cur.find_first_not_of(" \t") != std::string::npos) bands.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (bands.size() > static_cast<std::size_t>(kEqBands)) {
        error = "the EQ has " + std::to_string(kEqBands) + " bands";
        return false;
    }
    const EffectType* eq = findEffectType("eq");
    for (int k = 0; k < kEqBands; ++k) out["b" + std::to_string(k + 1) + "_type"] = 0;
    for (std::size_t k = 0; k < bands.size(); ++k) {
        std::istringstream in(bands[k]);
        std::string type, freq, gain, q;
        in >> type >> freq >> gain >> q;
        FilterType ft;
        if (!parseFilterType(lower(type), ft)) {
            error = "band " + std::to_string(k + 1) + ": \"" + type + "\" isn't a filter (lowpass, highpass, bandpass, notch, peaking, lowshelf, highshelf)";
            return false;
        }
        const std::string b = "b" + std::to_string(k + 1) + "_";
        out[b + "type"] = static_cast<double>(static_cast<int>(ft) + 1);
        double v;
        if (freq.empty()) {
            error = "band " + std::to_string(k + 1) + " needs a frequency, e.g. \"peaking 1000 -3 2\"";
            return false;
        }
        if (!parseValue(*eq->param(b + "freq"), freq, v, error)) return false;
        out[b + "freq"] = v;
        out[b + "gain"] = 0;
        out[b + "q"] = 0.7071;
        if (!gain.empty()) {
            if (!parseValue(*eq->param(b + "gain"), gain, v, error)) return false;
            out[b + "gain"] = v;
        }
        if (!q.empty()) {
            if (!parseValue(*eq->param(b + "q"), q, v, error)) return false;
            out[b + "q"] = v;
        }
    }
    return true;
}

}  // namespace

bool parseEffectValues(const std::string& type, const std::string& text, std::map<std::string, double>& out, std::string& error) {
    const EffectType* t = findEffectType(type);
    if (!t) {
        error = "unknown effect \"" + type + "\"";
        return false;
    }
    std::string rest = text;
    if (t->name == "eq") {
        const auto at = rest.find("bands=");
        if (at != std::string::npos) {
            std::string bands = rest.substr(at + 6);
            std::string after;
            if (auto semi = bands.find(';'); semi != std::string::npos) after = bands.substr(semi + 1), bands = bands.substr(0, semi);
            if (!bands.empty() && (bands.front() == '"' || bands.front() == '\'')) bands = bands.substr(1);
            if (!bands.empty() && (bands.back() == '"' || bands.back() == '\'')) bands.pop_back();
            if (!parseBands(bands, out, error)) return false;
            rest = rest.substr(0, at) + " " + after;
        }
    }
    std::string token;
    std::vector<std::string> tokens;
    for (char c : rest + " ") {
        if (c == ' ' || c == ',' || c == ';' || c == '\t' || c == '\n') {
            if (!token.empty()) tokens.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    for (const auto& tok : tokens) {
        const auto eq = tok.find('=');
        if (eq == std::string::npos || eq == 0) {
            error = "settings are name=value, e.g. threshold=-20 (got \"" + tok + "\")";
            return false;
        }
        const std::string name = lower(tok.substr(0, eq));
        const EffectParam* p = t->param(name);
        if (!p) {
            std::string list;
            for (const auto& q : t->params) {
                if (t->name == "eq" && q.name.size() > 3 && q.name[0] == 'b' && q.name[1] != '1') continue;
                list += (list.empty() ? "" : ", ") + q.name;
            }
            if (t->name == "eq") list += ", b2_... b" + std::to_string(kEqBands) + "_..., bands=";
            error = t->name + " has no setting \"" + name + "\" (it has " + list + ")";
            return false;
        }
        double v;
        if (!parseValue(*p, tok.substr(eq + 1), v, error)) return false;
        out[name] = v;
    }
    return true;
}

bool makeEffect(const std::string& type, const std::string& text, Effect& out, std::string& error) {
    const EffectType* t = findEffectType(type);
    if (!t) {
        error = "unknown effect \"" + type + "\" (try " + [] {
            std::string s;
            for (const auto& n : effectTypeNames()) s += (s.empty() ? "" : ", ") + n;
            return s;
        }() + ")";
        return false;
    }
    Effect e;
    e.type = t->name;
    if (!parseEffectValues(t->name, text, e.params, error)) return false;
    out = std::move(e);
    return true;
}

std::string formatEffectValue(const EffectParam& p, double v) {
    if (!p.choices.empty()) {
        const auto i = static_cast<std::size_t>(std::clamp<long>(std::lround(v), 0, static_cast<long>(p.choices.size()) - 1));
        return p.choices[i];
    }
    return numText(v) + (p.unit.empty() ? "" : " " + p.unit);
}

std::string describeEffect(const Effect& e) {
    if (e.type == "clap") return describeClapEffect(e);
    const EffectType* t = findEffectType(e.type);
    if (!t) return e.type + " (unknown)";
    std::string s = t->name + ":";
    bool first = true;
    auto add = [&](const std::string& part) {
        s += (first ? " " : ", ") + part;
        first = false;
    };
    if (t->name == "eq") {
        for (int k = 1; k <= kEqBands; ++k) {
            const std::string b = "b" + std::to_string(k) + "_";
            const int type = static_cast<int>(std::lround(e.get(b + "type")));
            if (type <= 0) continue;
            std::string part = "band " + std::to_string(k) + " " + filterTypeNames()[static_cast<std::size_t>(type - 1)] + " " + numText(e.get(b + "freq")) + " Hz";
            const bool shaped = type == 5 || type == 6 || type == 7;
            if (shaped) part += " " + numText(e.get(b + "gain")) + " dB";
            part += " Q " + numText(e.get(b + "q"));
            add(part);
        }
        if (first) add("flat (no bands on)");
        if (e.get("output") != 0) add("output " + numText(e.get("output")) + " dB");
    } else {
        for (const auto& p : t->params) add(p.name + " " + formatEffectValue(p, e.get(p.name)));
    }
    if (e.bypass) s += " (bypassed)";
    return s;
}

// ---- processors ----

void Processor::set(const std::string& name, double value) {
    auto it = v_.find(name);
    if (it != v_.end() && it->second == value) return;
    v_[name] = value;
    dirty_ = true;
}

double Processor::get(const std::string& name) const {
    auto it = v_.find(name);
    return it != v_.end() ? it->second : 0.0;
}

std::unique_ptr<Processor> makeProcessor(const Effect& e, int rate, int channels) {
    if (e.type == "clap") return makeClapProcessor(e, rate, channels);
    const EffectType* t = findEffectType(e.type);
    if (!t) return nullptr;
    std::unique_ptr<Processor> p;
    const std::string& n = t->name;
    if (n == "eq") p = std::make_unique<EqProc>(rate, channels, false);
    else if (n == "graphic_eq") p = std::make_unique<EqProc>(rate, channels, true);
    else if (n == "filter") p = std::make_unique<FilterProc>(rate, channels);
    else if (n == "compressor") p = std::make_unique<CompressorProc>(rate);
    else if (n == "limiter") {
        auto l = std::make_unique<LimiterProc>(rate, channels);
        l->primeSum();
        p = std::move(l);
    } else if (n == "gate") p = std::make_unique<GateProc>(rate, false);
    else if (n == "expander") p = std::make_unique<GateProc>(rate, true);
    else if (n == "deesser") p = std::make_unique<DeEsserProc>(rate, channels);
    else if (n == "reverb") p = std::make_unique<ReverbProc>(rate, channels);
    else if (n == "delay") p = std::make_unique<DelayProc>(rate, channels);
    else if (n == "chorus") p = std::make_unique<ModDelayProc>(rate, channels, false);
    else if (n == "flanger") p = std::make_unique<ModDelayProc>(rate, channels, true);
    else if (n == "phaser") p = std::make_unique<PhaserProc>(rate, channels);
    else if (n == "tremolo") p = std::make_unique<TremoloProc>(rate);
    else if (n == "distortion") p = std::make_unique<DistortionProc>(rate, channels);
    else p = std::make_unique<UtilityProc>();
    // defaults, then the effect's own settings
    for (const auto& prm : t->params) p->set(prm.name, prm.def);
    for (const auto& [k, v] : e.params) p->set(k, v);
    return p;
}

double effectTail(const Effect& e) {
    if (e.bypass) return 0;
    if (e.type == "clap") return 3.0;  // a plugin may ring on (reverbs); silent ends are trimmed after
    if (e.type == "reverb") return 1.0 + 4.0 * std::clamp(e.get("room"), 0.0, 1.0) + e.get("predelay") * 0.001;
    if (e.type == "delay") {
        const double fb = std::clamp(e.get("feedback"), 0.0, 0.95), t = e.get("time") * 0.001;
        if (e.get("mix") <= 0) return 0;
        const double repeats = fb > 0.01 ? std::log(0.001) / std::log(fb) : 1.0;
        return std::min(10.0, t * (repeats + 1));
    }
    if (e.type == "chorus" || e.type == "flanger" || e.type == "phaser") return 0.05;
    return 0;
}

double rackTail(const Rack& r) {
    double t = 0;
    for (const auto& e : r) t += effectTail(e);
    return std::min(t, 15.0);
}

// ---- automation ----

double Automation::valueAt(double time) const {
    if (points.empty()) return 0;
    if (time <= points.front().time) return points.front().value;
    if (time >= points.back().time) return points.back().value;
    auto it = std::upper_bound(points.begin(), points.end(), time, [](double t, const AutomationPoint& p) { return t < p.time; });
    const auto& b = *it;
    const auto& a = *(it - 1);
    const double span = b.time - a.time;
    return span > 0 ? a.value + (b.value - a.value) * (time - a.time) / span : b.value;
}

void Automation::sort() {
    std::stable_sort(points.begin(), points.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.time < b.time; });
}

bool parseFxTarget(const std::string& target, int& index, std::string& param) {
    if (target.size() < 5 || target.compare(0, 2, "fx") != 0) return false;
    const auto dot = target.find('.');
    if (dot == std::string::npos || dot == 2 || dot + 1 >= target.size()) return false;
    for (std::size_t i = 2; i < dot; ++i)
        if (!std::isdigit(static_cast<unsigned char>(target[i]))) return false;
    index = std::stoi(target.substr(2, dot - 2));
    param = target.substr(dot + 1);
    return true;
}

bool validTarget(const std::string& target, const Rack& rack, std::string& error) {
    if (target == "volume" || target == "pan") return true;
    int i = 0;
    std::string p;
    if (!parseFxTarget(target, i, p)) {
        error = "automate volume, pan or an effect setting as fx<number>.<setting>, e.g. fx0.cutoff";
        return false;
    }
    if (i < 0 || i >= static_cast<int>(rack.size())) {
        error = "there's no effect " + std::to_string(i) + " in the rack (it has " + std::to_string(rack.size()) + ", numbered from 0)";
        return false;
    }
    if (rack[static_cast<std::size_t>(i)].type == "clap") {
        double lo = 0, hi = 0;
        const bool idForm = p.size() > 1 && p[0] == 'p' && std::all_of(p.begin() + 1, p.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
        if (!idForm) {
            error = "automate a plugin setting by its id, e.g. fx" + std::to_string(i) + ".p7 (audio.fx_list shows them)";
            return false;
        }
        return clapParamRange(rack[static_cast<std::size_t>(i)], p, lo, hi, error);
    }
    const EffectType* t = findEffectType(rack[static_cast<std::size_t>(i)].type);
    if (!t || !t->param(p)) {
        error = "effect " + std::to_string(i) + " (" + rack[static_cast<std::size_t>(i)].type + ") has no setting \"" + p + "\"";
        return false;
    }
    return true;
}

bool targetRange(const std::string& target, const Rack& rack, double& lo, double& hi) {
    if (target == "volume") return lo = -60, hi = 12, true;
    if (target == "pan") return lo = -1, hi = 1, true;
    int i = 0;
    std::string p;
    if (!parseFxTarget(target, i, p) || i < 0 || i >= static_cast<int>(rack.size())) return false;
    if (rack[static_cast<std::size_t>(i)].type == "clap") {
        std::string error;
        return clapParamRange(rack[static_cast<std::size_t>(i)], p, lo, hi, error);
    }
    const EffectType* t = findEffectType(rack[static_cast<std::size_t>(i)].type);
    const EffectParam* prm = t ? t->param(p) : nullptr;
    if (!prm) return false;
    lo = prm->min, hi = prm->max;
    return true;
}

const Automation* findLane(const std::vector<Automation>& lanes, const std::string& target) {
    for (const auto& l : lanes)
        if (l.target == target && !l.points.empty()) return &l;
    return nullptr;
}

void applyRack(Buffer& b, const Rack& rack, const std::vector<Automation>& lanes, double startTime, double secondsPerUnit) {
    if (rack.empty() || b.empty()) return;
    struct Live {
        std::unique_ptr<Processor> proc;
        std::vector<std::pair<std::string, const Automation*>> lanes;
    };
    std::vector<Live> live;
    std::size_t latency = 0;
    for (std::size_t i = 0; i < rack.size(); ++i) {
        if (rack[i].bypass) continue;
        Live l;
        l.proc = makeProcessor(rack[i], b.rate, b.channelCount());
        if (!l.proc) continue;
        for (const auto& lane : lanes) {
            int idx = 0;
            std::string prm;
            if (!lane.points.empty() && parseFxTarget(lane.target, idx, prm) && idx == static_cast<int>(i)) l.lanes.emplace_back(prm, &lane);
        }
        latency += l.proc->latency();
        live.push_back(std::move(l));
    }
    if (live.empty()) return;
    const std::size_t frames = b.frames();
    if (latency) b.resize(frames + latency);
    const std::size_t n = b.frames();
    std::vector<float*> ptr(b.channels.size());
    const double spu = secondsPerUnit > 0 ? secondsPerUnit : 1.0;
    constexpr std::size_t kBlock = 64;
    for (std::size_t at = 0; at < n; at += kBlock) {
        const std::size_t len = std::min(kBlock, n - at);
        const double t = startTime + static_cast<double>(at) / b.rate / spu;
        for (std::size_t c = 0; c < ptr.size(); ++c) ptr[c] = b.channels[c].data() + at;
        for (auto& l : live) {
            for (const auto& [prm, lane] : l.lanes) l.proc->set(prm, lane->valueAt(t));
            l.proc->process(ptr.data(), b.channelCount(), len);
        }
    }
    if (latency) {
        for (auto& c : b.channels) c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(latency));
    }
}

void applyEffect(Buffer& b, const Effect& e, Range r, const std::vector<int>& channels) {
    auto proc = makeProcessor(e, b.rate, static_cast<int>(resolveChannels(b, channels).size()));
    if (!proc) return;
    detail::processRegionMulti(
        b, r, channels,
        [&](std::vector<std::vector<float>>& x) {
            if (x.empty()) return;
            const std::size_t n = x[0].size(), lat = proc->latency();
            for (auto& c : x) c.resize(n + lat, 0.0f);
            std::vector<float*> ptr;
            for (auto& c : x) ptr.push_back(c.data());
            for (std::size_t at = 0; at < n + lat; at += 64) {
                const std::size_t len = std::min<std::size_t>(64, n + lat - at);
                std::vector<float*> p2;
                for (auto* q : ptr) p2.push_back(q + at);
                proc->process(p2.data(), static_cast<int>(p2.size()), len);
            }
            for (auto& c : x) c.erase(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(lat));
        },
        detail::edgeFrames(b));
}

void applyVolumeLane(Buffer& b, const Automation& lane, double startTime, double secondsPerUnit) {
    if (lane.points.empty()) return;
    const double spu = secondsPerUnit > 0 ? secondsPerUnit : 1.0;
    for (std::size_t i = 0; i < b.frames(); ++i) {
        const auto g = static_cast<float>(dbToGain(lane.valueAt(startTime + static_cast<double>(i) / b.rate / spu)));
        for (auto& c : b.channels) c[i] *= g;
    }
}

void applyPanLane(Buffer& b, const Automation& lane, double startTime, double secondsPerUnit) {
    if (lane.points.empty() || b.channelCount() < 2) return;
    const double spu = secondsPerUnit > 0 ? secondsPerUnit : 1.0;
    for (std::size_t i = 0; i < b.frames(); ++i) {
        const double p = std::clamp(lane.valueAt(startTime + static_cast<double>(i) / b.rate / spu), -1.0, 1.0);
        b.channels[0][i] *= static_cast<float>(p > 0 ? 1 - p : 1.0);
        b.channels[1][i] *= static_cast<float>(p < 0 ? 1 + p : 1.0);
    }
}

}  // namespace wetwaresuite::audio
