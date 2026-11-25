// CDP8's multichannel tools, ported in third_party/cdp (RotatingPan,
// SpreadPan, TablePan, StepSpread, CrossPan, Antiphony, MultichannelStadium,
// Flutter, FrameRotate, FrameRemap, ChannelEnvelope and ToStereo), against
// recorded CDP8 references bit for bit: each fixture played in blocks of
// 64, 1, 7, 128, 256, 333, 1000 and 16384 frames, twice with a reset
// between, its live controls set where CDP8 reads its breakpoint files (a
// block size that cannot is skipped, a few must play); the corrected modes'
// hand-computed cases; and each port's refusals, which change nothing.
// The harness checks of the porting work, merged.
#include "talas/cdp/Antiphony.h"
#include "talas/cdp/ChannelEnvelope.h"
#include "talas/cdp/CrossPan.h"
#include "talas/cdp/Flutter.h"
#include "talas/cdp/FrameRemap.h"
#include "talas/cdp/FrameRotate.h"
#include "talas/cdp/MultichannelStadium.h"
#include "talas/cdp/RotatingPan.h"
#include "talas/cdp/SpreadPan.h"
#include "talas/cdp/StepSpread.h"
#include "talas/cdp/TablePan.h"
#include "talas/cdp/ToStereo.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <vector>

// Fixture loading, breakpoint tables and bit comparison.
namespace check
{
inline int failures = 0;
inline int checks = 0;
inline std::string dir = TALAS_CDP_FIXTURE_DIR;

inline void expect(bool ok, const std::string& what)
{
    checks++;
    if (!ok)
    {
        failures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// A breakpoint table as fixture.py wrote it: times frame / rate.
struct Table
{
    std::vector<double> times;
    std::vector<double> values;

    // tklib3.c's read_value_from_brktable.
    [[nodiscard]] double at(double thistime) const
    {
        if (thistime <= times.front())
            return values.front();
        if (thistime >= times.back())
            return values.back();
        std::size_t p = 0;
        while (times[p] < thistime)
            p++;
        auto val = (thistime - times[p - 1]) / (times[p] - times[p - 1]);
        val *= (values[p] - values[p - 1]);
        val += values[p - 1];
        return val;
    }

    [[nodiscard]] double lowest() const { return *std::min_element(values.begin(), values.end()); }
    [[nodiscard]] double highest() const { return *std::max_element(values.begin(), values.end()); }
};

struct Fixture
{
    // Columns: inputs then outputs.
    std::vector<std::vector<double>> columns;
    std::map<std::string, Table> tables;
    double rate = 48000.0;
    [[nodiscard]] std::size_t frames() const { return columns.empty() ? 0 : columns[0].size(); }
};

inline Fixture load(const std::string& name)
{
    Fixture f;
    const auto path = dir + "/" + name + ".txt";
    std::ifstream file(path);
    if (!file)
    {
        std::printf("cannot open %s\n", path.c_str());
        std::exit(2);
    }
    std::string line;
    std::vector<std::pair<std::string, std::string>> pending;
    while (std::getline(file, line))
    {
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            const auto k = line.find(" kHz in");
            if (line.rfind("# CDP8", 0) == 0 && k != std::string::npos)
            {
                const auto s = line.rfind(' ', k - 1);
                f.rate = std::strtod(line.substr(s + 1, k - s - 1).c_str(), nullptr) * 1000.0;
            }
            const auto b = line.find(".brk: ");
            if (b != std::string::npos && line.find(" at frame ") != std::string::npos)
                pending.emplace_back(line.substr(2, b - 2), line.substr(b + 6));
            continue;
        }
        std::istringstream words(line);
        std::string word;
        std::size_t c = 0;
        while (words >> word)
        {
            if (f.columns.size() <= c)
                f.columns.emplace_back();
            f.columns[c++].push_back(std::strtod(word.c_str(), nullptr));
        }
    }
    for (const auto& [label, text] : pending)
    {
        Table t;
        std::istringstream items(text);
        std::string item;
        while (std::getline(items, item, '/'))
        {
            double value = 0.0, frame = 0.0;
            char at[8], fr[8];
            if (std::sscanf(item.c_str(), " %lf %7s %7s %lf", &value, at, fr, &frame) != 4)
            {
                std::printf("bad table item '%s' in %s\n", item.c_str(), path.c_str());
                std::exit(2);
            }
            t.times.push_back(frame / f.rate);
            t.values.push_back(value);
        }
        f.tables[label] = t;
    }
    return f;
}

inline std::size_t mismatches(const std::vector<double>& got, const std::vector<double>& want, std::size_t& first,
    std::size_t from = 0, std::size_t to = std::numeric_limits<std::size_t>::max())
{
    std::size_t count = 0;
    first = want.size();
    to = std::min(to, want.size());
    for (std::size_t n = from; n < to; n++)
    {
        if (n >= got.size() || std::bit_cast<std::uint64_t>(got[n]) != std::bit_cast<std::uint64_t>(want[n]))
        {
            if (count == 0)
                first = n;
            count++;
        }
    }
    return count;
}

inline bool same(const std::vector<double>& got, const std::vector<double>& want, const std::string& what,
    std::size_t from = 0, std::size_t to = std::numeric_limits<std::size_t>::max())
{
    std::size_t first = 0;
    const auto bad = mismatches(got, want, first, from, to);
    if (bad != 0)
    {
        std::printf("  %s: %zu mismatches, first at frame %zu: got %a want %a\n", what.c_str(), bad, first,
            first < got.size() ? got[first] : 0.0, want[first]);
    }
    expect(bad == 0, what + " bit-exact");
    return bad == 0;
}

inline std::vector<double> clipped(std::vector<double> v)
{
    for (auto& x : v)
        x = std::clamp(x, -1.0, 1.0);
    return v;
}

inline int finish()
{
    std::printf("%d checks, %d failures\n", checks, failures);
    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

inline const std::size_t blockSizes[] = {64, 1, 7, 128, 256, 333, 1000, 16384};
}

// Playing a port over a fixture in blocks with its controls.
namespace mc
{
using check::expect;
using check::Fixture;

// The fixture's input channels, interleaved, then `extra` frames of
// silence.
inline std::vector<double> interleaved(const Fixture& f, std::size_t inChannels, std::size_t extra)
{
    const auto frames = f.frames();
    std::vector<double> in((frames + extra) * inChannels, 0.0);
    for (std::size_t n = 0; n < frames; n++)
    {
        for (std::size_t c = 0; c < inChannels; c++)
            in[n * inChannels + c] = f.columns[c][n];
    }
    return in;
}

// Called at each block's start with the block's first frame and length;
// false means this block size cannot set the controls where CDP reads
// them, and the block size is skipped.
using Controls = std::function<bool(std::size_t start, std::size_t count)>;

// Plays the input (and latencyFrames() of silence) in blocks; returns each
// output channel from latencyFrames() on, the fixture's length.
template <class Port>
bool play(Port& port, const Fixture& f, std::size_t inChannels, std::size_t block, const Controls& controls,
    std::vector<std::vector<double>>& channels)
{
    const auto latency = port.latencyFrames();
    const auto ch = port.outputChannels();
    const auto total = f.frames() + latency;
    const auto in = interleaved(f, inChannels, latency);
    std::vector<double> out(total * ch, -7.0);
    for (std::size_t start = 0; start < total; start += block)
    {
        const auto count = std::min(block, total - start);
        if (controls && !controls(start, count))
            return false;
        const bool ok = port.process(std::span<const double>(in).subspan(start * inChannels, count * inChannels),
            std::span<double>(out).subspan(start * ch, count * ch));
        expect(ok, "process accepted");
        if (!ok)
            return false;
    }
    channels.assign(ch, {});
    for (std::size_t c = 0; c < ch; c++)
    {
        for (std::size_t n = latency; n < total; n++)
            channels[c].push_back(out[n * ch + c]);
    }
    return true;
}

// A control CDP reads every `period` frames from frame `first`: at each
// block's start the value of the next read (at or after it) is set, as
// early as possible, so that a port reading it anywhere else shows; the
// reads within one block must share one value. `value(frame)` gives what
// CDP reads at that frame; `set(value)` sets it.
inline Controls periodic(std::size_t first, std::size_t period, std::function<double(std::size_t)> value,
    std::function<bool(double)> set)
{
    return [=](std::size_t start, std::size_t count) {
        std::size_t r = start <= first ? first : first + (start - first + period - 1) / period * period;
        const auto v = value(r);
        for (r += period; r < start + count; r += period)
        {
            if (std::bit_cast<std::uint64_t>(value(r)) != std::bit_cast<std::uint64_t>(v))
                return false;
        }
        expect(set(v), "setter accepted");
        return true;
    };
}

inline Controls both(Controls a, Controls b)
{
    return [=](std::size_t start, std::size_t count) { return a(start, count) && b(start, count); };
}

// Plays every block size twice (reset between), comparing each output
// channel with the fixture's columns from `firstOut` on, bit for bit.
// `prepare` prepares a fresh port; `controls` makes the block's controls
// for that port. Returns the number of block sizes played.
template <class Port>
std::size_t checkAll(const std::string& name, const Fixture& f, std::size_t inChannels, std::size_t outChannels,
    const std::function<bool(Port&)>& prepare, const std::function<Controls(Port&)>& controls = {},
    std::size_t minimumPlayed = 8)
{
    expect(f.columns.size() == inChannels + outChannels,
        name + ": columns " + std::to_string(f.columns.size()) + " (want "
            + std::to_string(inChannels + outChannels) + ")");
    if (f.columns.size() != inChannels + outChannels)
        return 0;
    std::size_t played = 0;
    for (const auto block : check::blockSizes)
    {
        Port port;
        expect(prepare(port), name + ": prepare");
        expect(port.outputChannels() == outChannels, name + ": output channels");
        bool all = true;
        for (int pass = 0; pass < 2 && all; pass++)
        {
            std::vector<std::vector<double>> out;
            const auto c = controls ? controls(port) : Controls{};
            if (!play(port, f, inChannels, block, c, out))
            {
                all = false;
                break;
            }
            for (std::size_t ch = 0; ch < outChannels; ch++)
            {
                check::same(out[ch], f.columns[inChannels + ch],
                    name + " channel " + std::to_string(ch) + " block " + std::to_string(block)
                        + (pass ? " replay after reset" : ""));
            }
            port.reset();
        }
        played += all;
    }
    expect(played >= minimumPlayed, name + ": enough block sizes played (" + std::to_string(played) + ")");
    return played;
}
}

// Checks RotatingPan against the CDP8 mchanpan 9 fixtures bit for bit: in
// 64-frame blocks and other sizes, Speed set where CDP reads it (every 256
// frames), reset and replay, refusals.
namespace rotate
{
using namespace talas::cdp;
using namespace check;

namespace
{
void fixture(const std::string& name, int channels, double start, double speed, double focus, bool anti)
{
    const auto f = load(name);
    const Table* table = f.tables.count("speed") ? &f.tables.at("speed") : nullptr;
    const auto first = table ? table->at(0.0) : speed;
    mc::checkAll<RotatingPan>(
        name, f, 1, static_cast<std::size_t>(channels),
        [&](RotatingPan& p) { return p.prepare(f.rate, channels, start, first, focus, anti); },
        [&](RotatingPan& p) -> mc::Controls {
            if (!table)
                return {};
            return mc::periodic(
                0, RotatingPan::speedReadFrames, [&](std::size_t r) { return table->at(double(r) / f.rate); },
                [&p](double v) { return p.setSpeed(v); });
        },
        table ? 5 : 8);
}

void refusals()
{
    RotatingPan p;
    std::vector<double> in(16, 0.1), out(64, 0.0), shorter(63, 0.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    expect(!p.prepared(), "not prepared at first");
    expect(!p.process(in, out), "refuses process before prepare");
    expect(!p.setSpeed(1.0), "refuses setSpeed before prepare");
    expect(!p.prepare(nan, 4, 1, 1, 1), "refuses a NaN rate");
    expect(!p.prepare(63.9, 4, 1, 1, 1), "refuses a rate under 64");
    expect(!p.prepare(48000, 2, 1, 1, 1), "refuses 2 channels");
    expect(!p.prepare(48000, 17, 1, 1, 1), "refuses 17 channels");
    expect(!p.prepare(48000, 4, 4.01, 1, 1), "refuses a start past the channels");
    expect(!p.prepare(48000, 4, -0.01, 1, 1), "refuses a negative start");
    expect(!p.prepare(48000, 4, 1, 64.01, 1), "refuses speed over 64");
    expect(!p.prepare(48000, 4, 1, -0.01, 1), "refuses negative speed");
    expect(!p.prepare(48000, 4, 1, 1, 1.01), "refuses focus over 1");
    expect(!p.prepare(48000, 4, 1, 1, -0.01), "refuses negative focus");
    expect(!p.prepare(48000, 4, 1, nan, 1), "refuses NaN speed");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 4, 4, 64.000001, 1.000001), "takes the top ends within 0.000002");
    expect(p.outputChannels() == 4 && p.latencyFrames() == 0, "4 channels, no latency");
    const auto before = p;
    expect(!p.setSpeed(64.1), "refuses setSpeed over 64");
    expect(!p.setSpeed(nan), "refuses setSpeed NaN");
    expect(!p.process(in, shorter), "refuses an output not 4 times the input");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
}
}

void run()
{
    fixture("mchanpan-9-rotate-4-channels-speed-20", 4, 1, 20, 1, false);
    fixture("mchanpan-9-rotate-8-channels-start-3.5-speed-30-focus-0.7-anticlockwise", 8, 3.5, 30, 0.7, true);
    fixture("mchanpan-9-rotate-3-channels-speed-25-focus-0.6", 3, 2, 25, 0.6, false);
    fixture("mchanpan-9-rotate-6-channels-start-0-speed-64-focus-0-rate-44100", 6, 0, 64, 0, false);
    fixture("mchanpan-9-rotate-4-channels-live-speed", 4, 1, 0, 0.8, false);
    fixture("mchanpan-9-rotate-4-channels-speed-ramp-anticlockwise", 4, 4, 0, 0.9, true);
    refusals();
}
}

// Checks SpreadPan against the CDP8 mchanpan 4 fixtures bit for bit: in
// 64-frame blocks and other sizes, Spread, Depth and Rolloff set where CDP
// reads them (every 256 frames), reset and replay, refusals.
namespace spread
{
using namespace talas::cdp;
using namespace check;

namespace
{
void fixture(const std::string& name, int in, int channels, int centre, double spread, double depth, double rolloff)
{
    const auto f = load(name);
    const auto table = [&](const char* label) -> const Table* {
        return f.tables.count(label) ? &f.tables.at(label) : nullptr;
    };
    const auto* ts = table("spread");
    const auto* td = table("depth");
    const auto* tr = table("rolloff");
    const bool live = ts || td || tr;
    if (ts)
        spread = ts->at(0.0);
    if (td)
        depth = td->at(0.0);
    if (tr)
        rolloff = tr->at(0.0);
    mc::checkAll<SpreadPan>(
        name, f, static_cast<std::size_t>(in), static_cast<std::size_t>(channels),
        [&](SpreadPan& p) { return p.prepare(f.rate, in, channels, centre, spread, depth, rolloff); },
        [&](SpreadPan& p) -> mc::Controls {
            if (!live)
                return {};
            const auto at = [&f](const Table* t) {
                return [&f, t](std::size_t r) { return t->at(double(r) / f.rate); };
            };
            mc::Controls c = [](std::size_t, std::size_t) { return true; };
            if (ts)
                c = mc::both(c, mc::periodic(0, SpreadPan::readFrames, at(ts), [&p](double v) { return p.setSpread(v); }));
            if (td)
                c = mc::both(c, mc::periodic(0, SpreadPan::readFrames, at(td), [&p](double v) { return p.setDepth(v); }));
            if (tr)
                c = mc::both(
                    c, mc::periodic(0, SpreadPan::readFrames, at(tr), [&p](double v) { return p.setRolloff(v); }));
            return c;
        },
        live ? 5 : 8);
}

void refusals()
{
    SpreadPan p;
    std::vector<double> in(16, 0.1), out(64, 0.0), shorter(63, 0.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    expect(!p.prepared(), "not prepared at first");
    expect(!p.process(in, out), "refuses process before prepare");
    expect(!p.setSpread(1.0) && !p.setDepth(1.0) && !p.setRolloff(0.5), "refuses setters before prepare");
    expect(!p.prepare(0.0, 1, 4, 1, 2, 1, 0), "refuses a zero rate");
    expect(!p.prepare(nan, 1, 4, 1, 2, 1, 0), "refuses a NaN rate");
    expect(!p.prepare(48000, 0, 4, 1, 2, 1, 0), "refuses 0 input channels");
    expect(!p.prepare(48000, 3, 4, 1, 2, 1, 0), "refuses 3 input channels");
    expect(!p.prepare(48000, 1, 2, 1, 2, 1, 0), "refuses 2 output channels");
    expect(!p.prepare(48000, 1, 17, 1, 2, 1, 0), "refuses 17 output channels");
    expect(!p.prepare(48000, 1, 4, 5, 2, 1, 0), "refuses a centre past the channels");
    expect(!p.prepare(48000, 1, 16, 9, 2, 1, 0), "refuses a centre over 8");
    expect(!p.prepare(48000, 1, 4, -1, 2, 1, 0), "refuses a negative centre");
    expect(!p.prepare(48000, 1, 4, 1, 16.01, 1, 0), "refuses spread over 16");
    expect(!p.prepare(48000, 1, 4, 1, -0.01, 1, 0), "refuses negative spread");
    expect(!p.prepare(48000, 1, 4, 1, 2, 8.01, 0), "refuses depth over 8");
    expect(!p.prepare(48000, 1, 4, 1, 2, -0.01, 0), "refuses negative depth");
    expect(!p.prepare(48000, 1, 4, 1, 2, 1, 1.01), "refuses rolloff over 1");
    expect(!p.prepare(48000, 1, 4, 1, 2, 1, -0.01), "refuses negative rolloff");
    expect(!p.prepare(48000, 1, 4, 1, nan, 1, 0), "refuses NaN spread");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 1, 4, 4, 16.000001, 8.000001, 1.000001), "takes the top ends within 0.000002");
    expect(p.outputChannels() == 4 && p.inputChannels() == 1 && p.latencyFrames() == 0, "1 in, 4 out, no latency");
    const auto before = p;
    expect(!p.setSpread(16.1) && !p.setDepth(8.1) && !p.setRolloff(1.1), "refuses setters past the ranges");
    expect(!p.setSpread(nan), "refuses NaN spread");
    expect(!p.process(in, shorter), "refuses an output not 4 times the input");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
    expect(p.prepare(48000, 2, 8, 0, 2, 1, 0), "prepares stereo in");
    std::vector<double> odd(15, 0.1), out8(64, 0.0);
    expect(!p.process(odd, out8), "refuses a stereo input of an odd sample count");
    expect(p.process(in, out8), "processes stereo: 8 frames into 8 channels");
}
}

void run()
{
    fixture("mchanpan-4-spread-8-channels-centre-1-spread-4-depth-2-rolloff-0.5", 1, 8, 1, 4, 2, 0.5);
    fixture("mchanpan-4-spread-4-channels-centre-2-spread-3.3-depth-1-rolloff-0", 1, 4, 2, 3.3, 1, 0);
    fixture("mchanpan-4-spread-4-channels-centre-0-spread-0.6-depth-3-rolloff-1", 1, 4, 0, 0.6, 3, 1);
    fixture("mchanpan-4-spread-6-channels-centre-5-spread-6-depth-3-rolloff-0.25", 1, 6, 5, 6, 3, 0.25);
    fixture("mchanpan-4-spread-6-channels-centre-4-spread-6-depth-1-rolloff-0.3-rate-44100", 1, 6, 4, 6, 1, 0.3);
    fixture("mchanpan-4-spread-stereo-8-channels-centre-3-spread-5.5-depth-2-rolloff-0.2", 2, 8, 3, 5.5, 2, 0.2);
    fixture("mchanpan-4-spread-stereo-5-channels-centre-5-spread-2.5-depth-1-rolloff-0.7", 2, 5, 5, 2.5, 1, 0.7);
    fixture("mchanpan-4-spread-stereo-4-channels-centre-3-spread-4-depth-2-rolloff-0.5", 2, 4, 3, 4, 2, 0.5);
    fixture("mchanpan-4-spread-4-channels-live-spread-depth-rolloff", 1, 4, 2, 0, 0, 0);
    fixture("mchanpan-4-spread-stereo-5-channels-ramps", 2, 5, 4, 0, 0, 0);
    fixture("mchanpan-4-spread-6-channels-centre-3-spread-3.5-depth-0-rolloff-0.6", 1, 6, 3, 3.5, 0, 0.6);
    fixture("mchanpan-4-spread-stereo-4-channels-centre-4-spread-0-depth-1-rolloff-0", 2, 4, 4, 0, 1, 0);
    fixture("mchanpan-4-spread-stereo-4-channels-centre-3-live-spread-keeps-centre", 2, 4, 3, 0, 2, 0.5);
    refusals();
}
}

// Checks TablePan against the CDP8 mchanpan 1 fixtures bit for bit: in
// 64-frame blocks and other sizes, reset and replay, refusals.
namespace table
{
using namespace talas::cdp;
using namespace check;

namespace
{
using Table = std::vector<TablePan::Point>;

void fixture(const std::string& name, int channels, const Table& table, double focus = 1.0)
{
    const auto f = load(name);
    mc::checkAll<TablePan>(name, f, 1, static_cast<std::size_t>(channels),
        [&](TablePan& p) { return p.prepare(f.rate, channels, table, focus); });
}

void refusals()
{
    TablePan p;
    std::vector<double> in(16, 0.1), out(64, 0.0), shorter(63, 0.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const Table good{{0, 1, 1}, {0.1, 3, 0}};
    expect(!p.prepared() && !p.process(in, out), "refuses process before prepare");
    expect(!p.prepare(0, 4, good), "refuses a zero rate");
    expect(!p.prepare(48000, 2, good), "refuses 2 channels");
    expect(!p.prepare(48000, 17, good), "refuses 17 channels");
    expect(!p.prepare(48000, 4, good, 1.01), "refuses focus over 1");
    expect(!p.prepare(48000, 4, Table{}), "refuses an empty table");
    expect(!p.prepare(48000, 4, Table{{-0.1, 1, 1}, {0.1, 2, 1}}), "refuses a negative time");
    expect(!p.prepare(48000, 4, Table{{0.1, 1, 1}, {0.1, 2, 1}}), "refuses a time that does not rise");
    expect(!p.prepare(48000, 4, Table{{0, 1, 1}, {0.1, 2, 1}, {0.10001, 3, 1}}), "refuses two times in one frame");
    expect(!p.prepare(48000, 4, Table{{0, 1, 1}, {0.000001, 2, 1}}), "refuses a second point at frame 0");
    expect(!p.prepare(48000, 4, Table{{0, 1, 1}, {0.1, 16.5, 1}}), "refuses a position over 16");
    expect(!p.prepare(48000, 4, Table{{0, 1, 1}, {0.1, 4.5, 1}}), "refuses a position past the channels");
    expect(!p.prepare(48000, 4, Table{{0, 1, 2}, {0.1, 2, 1}}), "refuses type 2");
    expect(!p.prepare(48000, 4, Table{{0, 1.5, 0}, {0.1, 2, 1}}), "refuses a direct point between channels");
    expect(!p.prepare(48000, 4, Table{{0, nan, 1}}), "refuses a NaN position");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 4, Table{{0.5, 4, 0}}, 1.000001), "takes one point, focus within 0.000002 of 1");
    expect(p.prepare(48000, 4, good), "prepares");
    expect(p.outputChannels() == 4 && p.latencyFrames() == 0, "4 channels, no latency");
    const auto before = p;
    expect(!p.process(in, shorter), "refuses an output not 4 times the input");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
}
}

void run()
{
    fixture("mchanpan-1-table-8-channels-rotations-and-direct", 8,
        {{0, 1, 1}, {0.008, 5.5, 1}, {0.016, 2, -1}, {0.024, 2, 0}, {0.03, 6, 0}, {0.034, 7, 1}});
    fixture("mchanpan-1-table-4-channels-direct-focus-0.7", 4, {{0, 1, 0}, {0.01, 3, 0}, {0.02, 4, 0}, {0.03, 1, 0}},
        0.7);
    fixture("mchanpan-1-table-3-channels-first-time-0.004-focus-0.5", 3, {{0.004, 1, 1}, {0.015, 3, -1}, {0.03, 1.5, 1}},
        0.5);
    fixture("mchanpan-1-table-6-channels-rate-44100", 6, {{0, 0.5, -1}, {0.015, 4.25, 1}, {0.03, 6, 0}}, 0.9);
    refusals();
}
}

// Checks StepSpread against the CDP8 mchanpan 3 fixtures bit for bit: in
// 64-frame blocks and other sizes, controls set to what CDP reads (Spread
// at each event's time, Centre, Depth and Rolloff at its buffer-relative
// time) as each event starts, reset and replay, refusals.
namespace step
{
using namespace talas::cdp;
using namespace check;

namespace
{
struct Read
{
    std::size_t frame;
    double value;
};

// A control read at listed frames: at each block's start, the value of the
// next read at or after it; the reads within a block must agree.
mc::Controls reads(std::vector<Read> list, std::function<bool(double)> set)
{
    return [list, set](std::size_t start, std::size_t count) {
        std::size_t k = 0;
        while (k < list.size() && list[k].frame < start)
            k++;
        if (k == list.size())
            return true;
        const auto v = list[k].value;
        for (auto j = k + 1; j < list.size() && list[j].frame < start + count; j++)
        {
            if (std::bit_cast<std::uint64_t>(list[j].value) != std::bit_cast<std::uint64_t>(v))
                return false;
        }
        expect(set(v), "setter accepted");
        return true;
    };
}

void fixture(const std::string& name, int channels, double centre, double spread, double depth, double rolloff,
    double silenceMs, bool stepped = false, std::size_t buffer = 0, std::size_t minimum = 8)
{
    const auto f = load(name);
    const auto table = [&](const char* label) -> const Table* {
        return f.tables.count(label) ? &f.tables.at(label) : nullptr;
    };
    const auto* tc = table("centre");
    const auto* ts = table("spread");
    const auto* td = table("depth");
    const auto* tr = table("rolloff");
    if (buffer == 0)
        buffer = StepSpread::defaultBufferFrames(channels);
    // The events, as count_events finds them.
    const auto minsil = static_cast<std::size_t>(std::lround(silenceMs * 0.001 * f.rate));
    std::vector<std::size_t> events{0};
    std::size_t firstSound = 0;
    {
        std::size_t insil = 0;
        bool leading = true;
        for (std::size_t n = 0; n < f.frames(); n++)
        {
            if (static_cast<float>(f.columns[0][n]) == 0.0F)
            {
                insil++;
                continue;
            }
            if (leading)
            {
                leading = false;
                if (insil >= minsil)
                    firstSound = n;
            }
            else if (insil >= minsil)
                events.push_back(n);
            insil = 0;
        }
    }
    std::printf("  %s: %zu events\n", name.c_str(), events.size());
    const auto at = [&f](const Table* t, std::size_t frame) { return t->at(double(frame) / f.rate); };
    std::vector<Read> rc, rs, rd, rr;
    for (std::size_t k = 0; k < events.size(); k++)
    {
        const auto e = events[k];
#ifdef TRUE_TIMES
        const auto rel = e;
#else
        const auto rel = StepSpread::cdpReadFrame(e, buffer);
#endif
        if (tc)
            rc.push_back({e, at(tc, rel)});
        if (td)
            rd.push_back({e, at(td, rel)});
        if (tr)
            rr.push_back({e, at(tr, rel)});
        if (ts)
        {
            rs.push_back({e, at(ts, e)});
            if (k == 0 && firstSound > 0)
                rs.push_back({firstSound, at(ts, firstSound)});
        }
    }
    const bool live = tc || ts || td || tr;
    if (tc)
        centre = rc.front().value;
    if (ts)
        spread = rs.front().value;
    if (td)
        depth = rd.front().value;
    if (tr)
        rolloff = rr.front().value;
    mc::checkAll<StepSpread>(
        name, f, 1, static_cast<std::size_t>(channels),
        [&](StepSpread& p) {
            return p.prepare(f.rate, channels, centre, spread, depth, rolloff, silenceMs, stepped);
        },
        [&](StepSpread& p) -> mc::Controls {
            if (!live)
                return {};
            mc::Controls c = [](std::size_t, std::size_t) { return true; };
            if (tc)
                c = mc::both(c, reads(rc, [&p](double v) { return p.setCentre(v); }));
            if (ts)
                c = mc::both(c, reads(rs, [&p](double v) { return p.setSpread(v); }));
            if (td)
                c = mc::both(c, reads(rd, [&p](double v) { return p.setDepth(v); }));
            if (tr)
                c = mc::both(c, reads(rr, [&p](double v) { return p.setRolloff(v); }));
            return c;
        },
        minimum);
}

void refusals()
{
    StepSpread p;
    std::vector<double> in(16, 0.1), out(64, 0.0), shorter(63, 0.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    expect(!p.prepared() && !p.process(in, out), "refuses process before prepare");
    expect(!p.setCentre(1) && !p.setSpread(1) && !p.setDepth(1) && !p.setRolloff(0), "refuses setters before prepare");
    expect(!p.prepare(0, 4, 1, 2, 1, 0, 10), "refuses a zero rate");
    expect(!p.prepare(48000, 2, 1, 2, 1, 0, 10), "refuses 2 channels");
    expect(!p.prepare(48000, 17, 1, 2, 1, 0, 10), "refuses 17 channels");
    expect(!p.prepare(48000, 4, 0.4, 2, 1, 0, 10), "refuses a centre rounding to 0");
    expect(!p.prepare(48000, 4, 4.5, 2, 1, 0, 10), "refuses a centre past the channels");
    expect(!p.prepare(48000, 16, 8.5, 2, 1, 0, 10), "refuses a centre over 8");
    expect(!p.prepare(48000, 4, 1, 0.99, 1, 0, 10), "refuses a spread under 1");
    expect(!p.prepare(48000, 4, 1, 16.01, 1, 0, 10), "refuses a spread over 16");
    expect(!p.prepare(48000, 4, 1, 2, 8.01, 0, 10), "refuses a depth over 8");
    expect(!p.prepare(48000, 4, 1, 2, 1, 1.01, 10), "refuses a rolloff over 1");
    expect(!p.prepare(48000, 4, 1, 2, 1, 0, 0.0001), "refuses a silence under 2 samples");
    expect(!p.prepare(48000, 4, 1, 2, 1, 0, 700), "refuses a silence over 32767 samples");
    expect(!p.prepare(48000, 4, 1, nan, 1, 0, 10), "refuses a NaN spread");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 4, 4, 16.000001, 8.000001, 1.000001, 32767.0 / 48.0), "takes the top ends");
    expect(StepSpread::defaultBufferFrames(8) == 29127, "8 outputs: CDP's 29127-frame buffer");
    expect(StepSpread::cdpReadFrame(0, 284) == 0 && StepSpread::cdpReadFrame(1, 284) == 1
            && StepSpread::cdpReadFrame(284, 284) == 284 && StepSpread::cdpReadFrame(285, 284) == 1
            && StepSpread::cdpReadFrame(600, 284) == 32,
        "CDP's buffer-relative read frames");
    expect(p.prepare(48000, 4, 1, 3, 1, 0, 10, true), "prepares stepped");
    const auto before = p;
    expect(!p.setCentre(2) && !p.setSpread(2) && !p.setDepth(2), "stepped: refuses Centre, Spread, Depth");
    expect(!p.setRolloff(1.5), "refuses a rolloff over 1");
    expect(!p.process(in, shorter), "refuses an output not 4 times the input");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    expect(p.setRolloff(0.5), "stepped: takes Rolloff");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
}
}

void run()
{
    fixture("mchanpan-3-stepspread-6-channels-centre-3-spread-5.5-depth-2-rolloff-0.3", 6, 3, 5.5, 2, 0.3, 10);
    fixture("mchanpan-3-stepspread-6-channels-stepped-centre-4-spread-5-depth-2-rolloff-0.5", 6, 4, 5, 2, 0.5, 5, true);
    fixture("mchanpan-3-stepspread-8-channels-stepped-spread-6", 8, 6, 6, 2, 0.25, 5, true);
    fixture("mchanpan-3-stepspread-4-channels-centre-2-spread-3.6-depth-1-rolloff-0.8", 4, 2, 3.6, 1, 0.8, 1);
    fixture("mchanpan-3-stepspread-6-channels-centre-5-spread-6-depth-3-rolloff-1-rate-44100", 6, 5, 6, 3, 1, 3);
    fixture("mchanpan-3-stepspread-6-channels-live-365-frame-buffers", 6, 0, 0, 0, 0, 1, false, 365, 3);
    refusals();
}
}

// Checks CrossPan against the CDP8 mchanpan 7 fixtures bit for bit: in
// 64-frame blocks and other sizes, CDP's spill into the next frame (lost at
// its small buffers' ends), reset and replay, refusals.
namespace cross
{
using namespace talas::cdp;
using namespace check;

namespace
{
using Points = std::vector<CrossPan::Point>;

CrossPan::Point point(double time, std::vector<int> map)
{
    CrossPan::Point p;
    p.time = time;
    for (std::size_t n = 0; n < map.size(); n++)
        p.map[n] = map[n];
    return p;
}

void fixture(const std::string& name, int channels, const Points& table, double rolloff, std::size_t buffer = 0)
{
    const auto f = load(name);
    const auto ch = static_cast<std::size_t>(channels);
    mc::checkAll<CrossPan>(
        name, f, ch, ch, [&](CrossPan& p) { return p.prepare(f.rate, channels, table, rolloff, buffer); });
    if (buffer != 0)
    {
        CrossPan p;
        expect(p.prepare(f.rate, channels, table, rolloff), name + ": prepare with CDP's default buffer");
        std::vector<std::vector<double>> out;
        expect(mc::play(p, f, ch, 64, {}, out), name + ": plays");
        std::size_t differ = 0;
        for (std::size_t c = 0; c < ch; c++)
        {
            std::size_t first = 0;
            differ += mismatches(out[c], f.columns[ch + c], first);
        }
        expect(differ > 0, name + ": the buffer's ends matter");
        std::printf("  %s: %zu samples differ with CDP's default buffer\n", name.c_str(), differ);
    }
}

std::vector<double> playC(CrossPan& p, const std::vector<double>& in, std::size_t block)
{
    const auto ch = p.outputChannels();
    const auto frames = in.size() / ch;
    std::vector<double> out(in.size(), -7.0);
    for (std::size_t s = 0; s < frames; s += block)
    {
        const auto n = std::min(block, frames - s);
        expect(p.process(std::span<const double>(in).subspan(s * ch, n * ch),
                   std::span<double>(out).subspan(s * ch, n * ch)),
            "corrected: process accepted");
    }
    return out;
}

void expectSample(const std::vector<double>& got, std::size_t frame, std::size_t chan, double want,
    const std::string& what)
{
    const auto v = got[frame * 4 + chan];
    if (v != want)
        std::printf("  %s: frame %zu channel %zu got %g want %g\n", what.c_str(), frame, chan + 1, v, want);
    expect(v == want, what);
}

void corrected()
{
    const double sr = 1000.0;
    // 1: one map, 2 3 4 1, held. Corrected: input n to channel map[n], so
    // the outputs are inputs 4, 1, 2, 3; CDP8: output n takes input map[n],
    // inputs 2, 3, 4, 1.
    {
        const Points table{point(0, {2, 3, 4, 1})};
        std::vector<double> in;
        for (int f = 0; f < 8; f++)
            for (int c = 0; c < 4; c++)
                in.push_back(0.125 * (c + 1));
        for (const bool fix : {true, false})
        {
            CrossPan p;
            expect(p.prepare(sr, 4, table, 0.5, 0, fix) && p.corrected() == fix, "held map: prepare");
            const auto got = playC(p, in, 3);
            const double wantC[4] = {0.5, 0.125, 0.25, 0.375};
            const double wantF[4] = {0.25, 0.375, 0.5, 0.125};
            for (std::size_t f = 0; f < 8; f++)
                for (std::size_t c = 0; c < 4; c++)
                    expectSample(got, f, c, fix ? wantC[c] : wantF[c],
                        fix ? "corrected held map: input n on channel map[n]" : "faithful held map");
        }
    }
    // 2: from map 1 2 3 4 to the centre over 4 frames (rolloff 0), only
    // input 4 sounding (1). Frame 0: on channel 4. Frame 1 (spread over 1
    // channel, half each side): channel 4 at 1, channels 3 and 1 at 0.5.
    // Frame 2 (2 channels, 1 each side): channels 3, 4, 1 at 1. From frame
    // 4 at the centre: every channel 1 (the sum of the inputs, times 1).
    // CDP8 puts frame 1's channel-1 half in frame 2 (1.5 there), unless a
    // buffer starts at frame 2.
    {
        const Points table{point(0, {1, 2, 3, 4}), point(0.004, {0, 0, 0, 0})};
        std::vector<double> in(8 * 4, 0.0);
        for (std::size_t f = 0; f < 8; f++)
            in[f * 4 + 3] = 1.0;
        for (const std::size_t buffer : {std::size_t{0}, std::size_t{2}})
        {
            CrossPan p;
            expect(p.prepare(sr, 4, table, 0.0, buffer, true), "corrected spread: prepare");
            for (const std::size_t block : {std::size_t{1}, std::size_t{5}, std::size_t{8}})
            {
                p.reset();
                const auto got = playC(p, in, block);
                const std::string what = "corrected spread wraps within the frame (buffer " + std::to_string(buffer)
                    + ", block " + std::to_string(block) + ")";
                expectSample(got, 0, 3, 1.0, what);
                expectSample(got, 0, 0, 0.0, what);
                expectSample(got, 1, 3, 1.0, what);
                expectSample(got, 1, 2, 0.5, what);
                expectSample(got, 1, 0, 0.5, what);
                expectSample(got, 1, 1, 0.0, what);
                expectSample(got, 2, 0, 1.0, what);
                expectSample(got, 2, 2, 1.0, what);
                expectSample(got, 2, 3, 1.0, what);
                expectSample(got, 2, 1, 0.0, what);
                for (std::size_t f = 4; f < 8; f++)
                    for (std::size_t c = 0; c < 4; c++)
                        expectSample(got, f, c, 1.0, what);
            }
            CrossPan q;
            expect(q.prepare(sr, 4, table, 0.0, buffer), "faithful spread: prepare");
            const auto got = playC(q, in, 8);
            const auto what = "faithful spread spills (buffer " + std::to_string(buffer) + ")";
            expectSample(got, 1, 0, 0.0, what);
            expectSample(got, 2, 0, buffer == 2 ? 1.0 : 1.5, what);
        }
    }
    // 3: a table starting at 2 ms: corrected, the first map holds from 0,
    // exactly as with a point at 0 added; CDP8's form is refused.
    {
        const Points late{point(0.002, {2, 1, 4, 3}), point(0.006, {1, 2, 3, 4})};
        const Points early{point(0, {2, 1, 4, 3}), point(0.002, {2, 1, 4, 3}), point(0.006, {1, 2, 3, 4})};
        std::vector<double> in;
        for (int f = 0; f < 10; f++)
            for (int c = 0; c < 4; c++)
                in.push_back(static_cast<double>(static_cast<float>(0.01 * (f + 1) + 0.1 * c)));
        CrossPan a, b, c;
        expect(a.prepare(sr, 4, late, 0.3, 0, true), "corrected late table: prepare");
        expect(b.prepare(sr, 4, early, 0.3, 0, true), "corrected table from 0: prepare");
        expect(!c.prepare(sr, 4, late, 0.3), "faithful refuses the late table");
        const auto ga = playC(a, in, 3);
        const auto gb = playC(b, in, 3);
        std::size_t first = 0;
        expect(mismatches(ga, gb, first) == 0, "corrected late table holds its first map from 0");
    }
}

void refusals()
{
    CrossPan p;
    std::vector<double> in(16, 0.1), out(16, 0.0), shorter(15, 0.0), ragged(15, 0.1);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const Points good{point(0, {1, 2, 3, 4}), point(0.1, {0, 0, 0, 0})};
    expect(!p.prepared() && !p.process(in, out), "refuses process before prepare");
    expect(!p.prepare(0, 4, good, 0.5), "refuses a zero rate");
    expect(!p.prepare(48000, 1, good, 0.5), "refuses 1 channel");
    expect(!p.prepare(48000, 17, good, 0.5), "refuses 17 channels");
    expect(!p.prepare(48000, 4, Points{}, 0.5), "refuses an empty table");
    expect(!p.prepare(48000, 4, good, 1.01), "refuses rolloff over 1");
    expect(!p.prepare(48000, 4, Points{point(-1, {1, 2, 3, 4})}, 0.5), "refuses a negative time");
    expect(!p.prepare(48000, 4, Points{point(0.0001, {1, 2, 3, 4})}, 0.5), "refuses a first point after frame 0");
    expect(!p.prepare(48000, 4, Points{point(0.1, {1, 2, 3, 4}), point(0.1, {2, 1, 3, 4})}, 0.5),
        "refuses a time that does not rise");
    expect(!p.prepare(48000, 4, Points{point(0.1, {1, 2, 3, 4}), point(0.10001, {2, 1, 3, 4})}, 0.5),
        "refuses two points in one frame");
    expect(!p.prepare(48000, 4, Points{point(0, {1, 2, 3, 3})}, 0.5), "refuses a map with a repeat");
    expect(!p.prepare(48000, 4, Points{point(0, {1, 2, 3, 5})}, 0.5), "refuses a channel past N");
    expect(!p.prepare(48000, 4, Points{point(0, {0, 2, 3, 4})}, 0.5), "refuses a map part 0");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 4, good, 1.000001), "takes rolloff within 0.000002 of 1");
    expect(CrossPan::defaultBufferFrames(4) == 32768, "4 channels: CDP's 32768-frame buffer");
    const auto before = p;
    expect(!p.process(in, shorter), "refuses an output of another size");
    expect(!p.process(ragged, ragged), "refuses a part frame");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
}
}

void run()
{
    fixture("mchanpan-7-crosspan-4-channels-rolloff-0.5", 4,
        {point(0, {1, 2, 3, 4}), point(0.008, {2, 3, 4, 1}), point(0.016, {0, 0, 0, 0}), point(0.024, {0, 0, 0, 0}),
            point(0.032, {4, 3, 2, 1}), point(0.038, {4, 3, 2, 1})},
        0.5);
    fixture("mchanpan-7-crosspan-8-channels-rolloff-0", 8,
        {point(0, {1, 2, 3, 4, 5, 6, 7, 8}), point(0.005, {1, 2, 3, 4, 5, 6, 7, 8}),
            point(0.025, {8, 7, 6, 5, 4, 3, 2, 1}), point(0.04, {0, 0, 0, 0, 0, 0, 0, 0})},
        0);
    fixture("mchanpan-7-crosspan-4-channels-rolloff-0.8-320-frame-buffers", 4,
        {point(0, {3, 1, 4, 2}), point(0.012, {1, 2, 3, 4}), point(0.025, {0, 0, 0, 0}), point(0.032, {2, 4, 1, 3})},
        0.8, 320);
    fixture("mchanpan-7-crosspan-stereo-rolloff-1-rate-44100", 2,
        {point(0, {1, 2}), point(0.02, {2, 1}), point(0.04, {1, 2})}, 1);
    corrected();
    refusals();
}
}

// Checks Antiphony against the CDP8 mchanpan 6 fixtures bit for bit: in
// 64-frame blocks and other sizes, Event duration and Gap set to what CDP
// reads as each event's splice in ends, a side chain, CDP's spill into the
// next frame (lost at its small buffers' ends), reset and replay, refusals.
namespace antiphony
{
using namespace talas::cdp;
using namespace check;

namespace
{
struct Read
{
    std::size_t frame;
    double value;
};

// At each block's start, the value of the next read at or after it; the
// reads within a block must agree.
mc::Controls reads(std::vector<Read> list, std::function<bool(double)> set)
{
    return [list, set](std::size_t start, std::size_t count) {
        std::size_t k = 0;
        while (k < list.size() && list[k].frame < start)
            k++;
        if (k == list.size())
            return true;
        const auto v = list[k].value;
        for (auto j = k + 1; j < list.size() && list[j].frame < start + count; j++)
        {
            if (std::bit_cast<std::uint64_t>(list[j].value) != std::bit_cast<std::uint64_t>(v))
                return false;
        }
        expect(set(v), "setter accepted");
        return true;
    };
}

std::vector<double> columns(const Fixture& f, std::size_t from, std::size_t count, std::size_t extra)
{
    std::vector<double> v((f.frames() + extra) * count, 0.0);
    for (std::size_t n = 0; n < f.frames(); n++)
    {
        for (std::size_t c = 0; c < count; c++)
            v[n * count + c] = f.columns[from + c][n];
    }
    return v;
}

void fixture(const std::string& name, int in, int out, std::vector<int> a, std::vector<int> b, double duration,
    double gap, double splice, bool side = false, std::size_t buffer = 0, std::size_t minimum = 8)
{
    const auto f = load(name);
    const auto inCh = static_cast<std::size_t>(in);
    const auto outCh = static_cast<std::size_t>(out);
    const auto inputs = side ? 2 * inCh : inCh;
    expect(f.columns.size() == inputs + outCh, name + ": columns");
    const Table* te = f.tables.count("event") ? &f.tables.at("event") : nullptr;
    const Table* tg = f.tables.count("gap") ? &f.tables.at("gap") : nullptr;
    // CDP's schedule: event lengths read at each start time, gaps at each
    // boundary; the stream takes both where each splice in ends.
    std::vector<Read> re, rg;
    if (te || tg)
    {
        const auto S = static_cast<std::size_t>(std::lround(splice * 0.001 * f.rate));
        double time = 0.0;
        std::size_t at = 0;
        while (at < f.frames() + 1)
        {
            const auto d = te ? te->at(time) : duration;
            time += d;
            const auto boundary = static_cast<std::size_t>(std::lround(time * f.rate));
            const auto g = tg ? tg->at(double(boundary) / f.rate) : gap;
            re.push_back({at, d});
            rg.push_back({at, g});
            at = boundary + S;
        }
        duration = re.front().value;
        gap = rg.front().value;
    }
    // CDP's loop stops one boundary early: its last event before the file's
    // end plays on to the end. Compare up to where that switch would start.
    std::size_t compared = f.frames();
    {
        const auto maxdur = double(f.frames()) / f.rate;
        double time = 0.0;
        std::size_t last = 0;
        while (time < maxdur)
        {
            const auto d = te ? te->at(time) : duration;
            time += d;
            if (time < maxdur)
            {
                const auto boundary = std::lround(time * f.rate);
                const auto g = tg ? tg->at(double(boundary) / f.rate) : gap;
                last = static_cast<std::size_t>(boundary - std::lround(g * f.rate));
            }
        }
        if (last > 0)
            compared = last;
        std::printf("  %s: compared to frame %zu of %zu\n", name.c_str(), compared, f.frames());
    }
    std::size_t played = 0;
    for (const auto block : blockSizes)
    {
        Antiphony p;
        expect(p.prepare(f.rate, in, out, a, b, duration, gap, splice, side, buffer), name + ": prepare");
        expect(p.outputChannels() == outCh && p.inputChannels() == inCh && p.latencyFrames() == 0,
            name + ": channels, no latency");
        bool all = true;
        for (int pass = 0; pass < 2 && all; pass++)
        {
            mc::Controls c = [](std::size_t, std::size_t) { return true; };
            if (te)
                c = mc::both(c, reads(re, [&p](double v) { return p.setEventDuration(v); }));
            if (tg)
                c = mc::both(c, reads(rg, [&p](double v) { return p.setGap(v); }));
            const auto x = columns(f, 0, inCh, 0);
            const auto y = side ? columns(f, inCh, inCh, 0) : std::vector<double>{};
            std::vector<double> o(f.frames() * outCh, -7.0);
            for (std::size_t start = 0; start < f.frames() && all; start += block)
            {
                const auto count = std::min(block, f.frames() - start);
                if (!c(start, count))
                {
                    all = false;
                    break;
                }
                const auto xs = std::span<const double>(x).subspan(start * inCh, count * inCh);
                const auto os = std::span<double>(o).subspan(start * outCh, count * outCh);
                const bool ok = side ? p.process(xs, std::span<const double>(y).subspan(start * inCh, count * inCh), os)
                                     : p.process(xs, os);
                expect(ok, "process accepted");
            }
            if (!all)
                break;
            for (std::size_t ch = 0; ch < outCh; ch++)
            {
                std::vector<double> got;
                for (std::size_t n = 0; n < f.frames(); n++)
                    got.push_back(o[n * outCh + ch]);
                same(got, f.columns[inputs + ch],
                    name + " channel " + std::to_string(ch) + " block " + std::to_string(block)
                        + (pass ? " replay after reset" : ""),
                    0, compared);
            }
            p.reset();
        }
        played += all;
    }
    expect(played >= minimum, name + ": enough block sizes played (" + std::to_string(played) + ")");
}

// Plays `frames` frames of constant or given input in blocks of `block`.
std::vector<double> playA(Antiphony& p, const std::vector<double>& in, const std::vector<double>& side,
    std::size_t block)
{
    const auto ic = p.inputChannels();
    const auto oc = p.outputChannels();
    const auto frames = in.size() / ic;
    std::vector<double> out(frames * oc, -7.0);
    for (std::size_t s = 0; s < frames; s += block)
    {
        const auto n = std::min(block, frames - s);
        const auto x = std::span<const double>(in).subspan(s * ic, n * ic);
        const auto o = std::span<double>(out).subspan(s * oc, n * oc);
        const bool ok = side.empty() ? p.process(x, o)
                                     : p.process(x, std::span<const double>(side).subspan(s * ic, n * ic), o);
        expect(ok, "corrected: process accepted");
    }
    return out;
}

// Compares a played run (in blocks of 1 and 7, and after reset) with an
// expected frame-by-frame table.
void expectRun(Antiphony& p, const std::vector<double>& in, const std::vector<double>& side,
    const std::vector<double>& want, const std::string& what)
{
    for (const std::size_t block : {std::size_t{1}, std::size_t{7}, std::size_t{64}})
    {
        p.reset();
        const auto got = playA(p, in, side, block);
        std::size_t first = 0;
        const auto bad = mismatches(got, want, first);
        if (bad != 0)
            std::printf("  %s block %zu: %zu mismatches, first at sample %zu: got %g want %g\n", what.c_str(), block,
                bad, first, got[first], want[first]);
        expect(bad == 0, what + " block " + std::to_string(block));
    }
}

void corrected()
{
    const double sr = 1000.0;
    const std::size_t frames = 160;
    // A: sets {1,2} and {3,4}, events of 50 frames, no gap, no splice: a
    // cut every 50 frames. Corrected: channels 1, 2 then 3, 4; CDP8: only
    // the first of each set, one channel up (2, then 4).
    {
        std::vector<double> in(frames, 0.5);
        std::vector<double> want(frames * 4, 0.0), faithful(frames * 4, 0.0);
        for (std::size_t f = 0; f < frames; f++)
        {
            const bool first = (f / 50) % 2 == 0;
            want[f * 4 + (first ? 0 : 2)] = 0.5;
            want[f * 4 + (first ? 1 : 3)] = 0.5;
            faithful[f * 4 + (first ? 1 : 3)] = 0.5;
        }
        Antiphony p;
        expect(p.prepare(sr, 1, 4, std::vector<int>{1, 2}, std::vector<int>{3, 4}, 0.05, 0, 0, false, 0, true)
                && p.corrected(),
            "corrected A: prepare");
        expectRun(p, in, {}, want, "corrected A: whole sets on their own channels");
        Antiphony q;
        expect(q.prepare(sr, 1, 4, std::vector<int>{1, 2}, std::vector<int>{3, 4}, 0.05, 0, 0) && !q.corrected(),
            "faithful A: prepare");
        expectRun(q, in, {}, faithful, "faithful A: first halves, one channel up");
    }
    // B: 4 ms splices (4 frames, steps of 0.25): at each boundary the old
    // set falls 0.75, 0.5, 0.25, 0 while the new rises 0.25 ... 1.
    // C: the same with a 10 ms gap: the old set falls from frame 40, then
    // 6 frames of silence, then the new rises from frame 50.
    for (const double gap : {0.0, 0.01})
    {
        std::vector<double> in(frames, 1.0);
        std::vector<double> want(frames * 4, 0.0);
        const std::size_t g = gap > 0 ? 10 : 0;
        for (std::size_t f = 0; f < frames; f++)
        {
            // Event k runs from 50k; set (k % 2); it falls from 50(k+1) - g.
            const auto k = f / 50;
            const auto within = f % 50;
            const auto cur = k % 2;
            double level = 1.0;
            if (k > 0 && within < 4)
            {
                // Rising after the boundary at 50k.
                want[f * 4 + 2 * cur] = want[f * 4 + 2 * cur + 1] = 0.25 * double(within + 1);
                // Still falling from the previous event, if no gap.
                if (g == 0)
                    want[f * 4 + 2 * (1 - cur)] = want[f * 4 + 2 * (1 - cur) + 1] = 0.75 - 0.25 * double(within);
                continue;
            }
            const auto fallStart = 50 - g;
            if (within >= fallStart)
            {
                const auto d = within - fallStart;
                level = d < 4 ? 0.75 - 0.25 * double(d) : 0.0;
            }
            want[f * 4 + 2 * cur] = want[f * 4 + 2 * cur + 1] = level;
        }
        Antiphony p;
        expect(p.prepare(sr, 1, 4, std::vector<int>{1, 2}, std::vector<int>{3, 4}, 0.05, gap, 4, false, 0, true),
            "corrected B/C: prepare");
        expectRun(p, in, {}, want, gap > 0 ? "corrected C: gap of 10 frames" : "corrected B: 4-frame splices");
    }
    // D: stereo in, sets {1,2,3,4} (left to 1, 2; right to 3, 4) and {5,6}
    // (left to 5, right to 6). CDP8 uses only (left to 1, 2) shifted to 2,
    // 3 and (left to 5) shifted to 6.
    {
        std::vector<double> in(frames * 2);
        for (std::size_t f = 0; f < frames; f++)
        {
            in[2 * f] = 1.0;
            in[2 * f + 1] = 2.0;
        }
        std::vector<double> want(frames * 6, 0.0), faithful(frames * 6, 0.0);
        for (std::size_t f = 0; f < frames; f++)
        {
            auto* w = &want[f * 6];
            auto* c = &faithful[f * 6];
            if ((f / 50) % 2 == 0)
            {
                w[0] = 1.0, w[1] = 1.0, w[2] = 2.0, w[3] = 2.0;
                c[1] = 1.0, c[2] = 1.0;
            }
            else
            {
                w[4] = 1.0, w[5] = 2.0;
                c[5] = 1.0;
            }
        }
        Antiphony p;
        expect(p.prepare(sr, 2, 6, std::vector<int>{1, 2, 3, 4}, std::vector<int>{5, 6}, 0.05, 0, 0, false, 0, true),
            "corrected D: prepare");
        expectRun(p, in, {}, want, "corrected D: stereo groups");
        Antiphony q;
        expect(q.prepare(sr, 2, 6, std::vector<int>{1, 2, 3, 4}, std::vector<int>{5, 6}, 0.05, 0, 0),
            "faithful D: prepare");
        expectRun(q, in, {}, faithful, "faithful D: stereo groups");
    }
    // E: sets {4} and {1} of 4 outputs, a rising input. Corrected: the
    // sample on channel 4, then 1, in its own frame. CDP8: set {4} lands in
    // the next frame's channel 1 (lost where a 64-frame buffer starts), set
    // {1} on channel 2.
    {
        std::vector<double> in(frames);
        for (std::size_t f = 0; f < frames; f++)
            in[f] = static_cast<double>(static_cast<float>(0.001 * double(f + 1)));
        std::vector<double> want(frames * 4, 0.0), faithful(frames * 4, 0.0);
        for (std::size_t f = 0; f < frames; f++)
        {
            const bool first = (f / 50) % 2 == 0;
            want[f * 4 + (first ? 3 : 0)] = in[f];
            if (first)
            {
                if (f + 1 < frames && (f + 1) % 64 != 0)
                    faithful[(f + 1) * 4 + 0] = in[f];
            }
            else
                faithful[f * 4 + 1] = in[f];
        }
        Antiphony p;
        expect(p.prepare(sr, 1, 4, std::vector<int>{4}, std::vector<int>{1}, 0.05, 0, 0, false, 64, true),
            "corrected E: prepare");
        expectRun(p, in, {}, want, "corrected E: channel N in its own frame");
        Antiphony q;
        expect(q.prepare(sr, 1, 4, std::vector<int>{4}, std::vector<int>{1}, 0.05, 0, 0, false, 64),
            "faithful E: prepare");
        expectRun(q, in, {}, faithful, "faithful E: channel N spills, lost at 64-frame buffers");
    }
    // F: a side chain: the events alternate input (1, on channel 1) and side
    // chain (2, on channel 2).
    {
        std::vector<double> in(frames, 1.0), side(frames, 2.0), want(frames * 3, 0.0);
        for (std::size_t f = 0; f < frames; f++)
        {
            if ((f / 50) % 2 == 0)
                want[f * 3 + 0] = 1.0;
            else
                want[f * 3 + 1] = 2.0;
        }
        Antiphony p;
        expect(p.prepare(sr, 1, 3, std::vector<int>{1}, std::vector<int>{2}, 0.05, 0, 0, true, 0, true),
            "corrected F: prepare");
        expectRun(p, in, side, want, "corrected F: input and side chain alternate");
    }
}

void refusals()
{
    Antiphony p;
    std::vector<double> in(16, 0.1), out(64, 0.0), shorter(63, 0.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<int> a{1, 2}, b{3, 4};
    expect(!p.prepared() && !p.process(in, out), "refuses process before prepare");
    expect(!p.setEventDuration(1) && !p.setGap(0), "refuses setters before prepare");
    expect(!p.prepare(0, 1, 4, a, b, 0.1, 0, 5), "refuses a zero rate");
    expect(!p.prepare(48000, 0, 4, a, b, 0.1, 0, 5), "refuses 0 input channels");
    expect(!p.prepare(48000, 1, 2, a, b, 0.1, 0, 5), "refuses 2 outputs");
    expect(!p.prepare(48000, 1, 17, a, b, 0.1, 0, 5), "refuses 17 outputs");
    expect(!p.prepare(48000, 1, 4, std::vector<int>{}, b, 0.1, 0, 5), "refuses an empty set");
    expect(!p.prepare(48000, 1, 4, std::vector<int>{1, 1}, b, 0.1, 0, 5), "refuses a repeat in a set");
    expect(!p.prepare(48000, 1, 4, std::vector<int>{1, 5}, b, 0.1, 0, 5), "refuses a channel past the outputs");
    expect(!p.prepare(48000, 2, 4, std::vector<int>{1}, b, 0.1, 0, 5), "refuses a set not a multiple of the inputs");
    expect(!p.prepare(48000, 1, 4, a, b, 0.0499, 0, 5), "refuses an event under 0.05 s");
    expect(!p.prepare(48000, 1, 4, a, b, 60.01, 0, 5), "refuses an event over 60 s");
    expect(!p.prepare(48000, 1, 4, a, b, 0.1, 60.01, 5), "refuses a gap over 60 s");
    expect(!p.prepare(48000, 1, 4, a, b, 0.1, -0.01, 5), "refuses a negative gap");
    expect(!p.prepare(48000, 1, 4, a, b, 0.1, 0, 1000.01), "refuses a splice over 1000 ms");
    expect(!p.prepare(48000, 1, 4, a, b, nan, 0, 5), "refuses a NaN event duration");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 1, 4, a, b, 60.000001, 60.000001, 1000.000001), "takes the top ends within 0.000002");
    expect(Antiphony::defaultBufferFrames(1, 4, 1) == 52428 && Antiphony::defaultBufferFrames(2, 8, 2) == 21845,
        "CDP's buffers");
    expect(p.prepare(48000, 1, 4, a, b, 0.1, 0, 5), "prepares");
    const auto before = p;
    expect(!p.setEventDuration(0.04) && !p.setGap(61), "refuses setters past the ranges");
    expect(!p.process(in, shorter), "refuses an output not 4 times the input");
    expect(!p.process(in, in, out), "refuses a side chain when prepared without");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
    expect(p.prepare(48000, 1, 4, a, b, 0.1, 0, 5, true), "prepares with a side chain");
    std::vector<double> side(15, 0.1);
    expect(!p.process(in, out), "refuses no side chain when prepared with one");
    expect(!p.process(in, side, out), "refuses a side chain of another size");
    expect(p.process(in, in, out), "processes with a side chain");
}
}

void run()
{
    fixture("mchanpan-6-antiphony-4-channels-ab-cd-event-0.06-gap-0-splice-5-rate-8000", 1, 4, {1, 2}, {3, 4}, 0.06,
        0, 5);
    fixture("mchanpan-6-antiphony-8-channels-abcd-efgh-event-0.07-gap-0.01-splice-3-rate-8000", 1, 8, {1, 2, 3, 4},
        {5, 6, 7, 8}, 0.07, 0.01, 3);
    fixture("mchanpan-6-antiphony-4-channels-d-a-512-frame-buffers-rate-8000", 1, 4, {4}, {1}, 0.055, 0.002, 2, false,
        512);
    fixture("mchanpan-6-antiphony-stereo-8-channels-abcd-efgh-event-0.05-gap-0-splice-10-rate-8000", 2, 8,
        {1, 2, 3, 4}, {5, 6, 7, 8}, 0.05, 0, 10);
    fixture("mchanpan-6-antiphony-two-inputs-6-channels-fac-eb-event-0.05-gap-0.005-splice-10-rate-8000", 1, 6,
        {6, 1, 3}, {5, 2}, 0.05, 0.005, 10, true);
    fixture("mchanpan-6-antiphony-5-channels-live-event-gap-rate-8000", 1, 5, {1, 3}, {5, 2}, 0, 0, 4, false, 0, 3);
    fixture("mchanpan-6-antiphony-4-channels-ab-dc-event-0.05-gap-0.003-splice-0", 1, 4, {1, 2}, {4, 3}, 0.05, 0.003,
        0);
    corrected();
    refusals();
}
}

// Checks MultichannelStadium against the CDP8 mchanrev fixtures bit for
// bit: in 64-frame blocks and other sizes, reset and replay, CDP's buffer
// order (small CDP_MEMORY_BBSIZE buffers), refusals.
namespace stadium
{
using namespace talas::cdp;
using namespace check;

namespace
{
void fixture(const std::string& name, double gain, double rolloff, double size, int count, int channels,
    double centre, double spread, std::size_t buffer = 0)
{
    const auto f = load(name);
    mc::checkAll<MultichannelStadium>(name, f, 1, static_cast<std::size_t>(channels),
        [&](MultichannelStadium& p) {
            return p.prepare(f.rate, gain, rolloff, size, count, channels, centre, spread, buffer);
        });
    if (buffer != 0)
    {
        // The default buffer (52428 frames and more) does not split this
        // fixture: its output must differ.
        MultichannelStadium p;
        expect(p.prepare(f.rate, gain, rolloff, size, count, channels, centre, spread), name + ": prepare");
        std::vector<std::vector<double>> out;
        expect(mc::play(p, f, 1, 64, {}, out), name + ": default buffer plays");
        std::size_t differ = 0;
        for (std::size_t c = 0; c < static_cast<std::size_t>(channels); c++)
        {
            std::size_t first = 0;
            differ += mismatches(out[c], f.columns[1 + c], first);
        }
        expect(differ > 0, name + ": CDP's buffer order matters");
        std::printf("  %s: %zu samples differ with CDP's default buffer\n", name.c_str(), differ);
    }
}

void refusals()
{
    MultichannelStadium p;
    std::vector<double> in(16, 0.1), out(64, 0.0), shorter(63, 0.0);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto g = MultichannelStadium::minimumGain;
    expect(!p.prepared(), "not prepared at first");
    expect(!p.process(in, out), "refuses process before prepare");
    expect(!p.prepare(0.0, 0.5, 1, 1, 23, 4, 1, 2), "refuses a zero rate");
    expect(!p.prepare(nan, 0.5, 1, 1, 23, 4, 1, 2), "refuses a NaN rate");
    expect(!p.prepare(48000, g * 0.9, 1, 1, 23, 4, 1, 2), "refuses gain under 1/32767");
    expect(!p.prepare(48000, 1.01, 1, 1, 23, 4, 1, 2), "refuses gain over 1");
    expect(!p.prepare(48000, 0.5, g * 0.9, 1, 23, 4, 1, 2), "refuses rolloff under 1/32767");
    expect(!p.prepare(48000, 0.5, 1.01, 1, 23, 4, 1, 2), "refuses rolloff over 1");
    expect(!p.prepare(48000, 0.5, 1, 1e-7, 23, 4, 1, 2), "refuses size under 0.2 s over the rate");
    expect(!p.prepare(48000, 0.5, 1, 100.01, 23, 4, 1, 2), "refuses size over 100");
    expect(!p.prepare(48000, 0.5, 1, 1, 1, 4, 1, 2), "refuses 1 echo");
    expect(!p.prepare(48000, 0.5, 1, 1, 1001, 4, 1, 2), "refuses 1001 echoes");
    expect(!p.prepare(48000, 0.5, 1, 1, 23, 1, 1, 2), "refuses 1 channel");
    expect(!p.prepare(48000, 0.5, 1, 1, 23, 17, 1, 2), "refuses 17 channels");
    expect(!p.prepare(48000, 0.5, 1, 1, 23, 4, 4.01, 2), "refuses a centre past the channels");
    expect(!p.prepare(48000, 0.5, 1, 1, 23, 4, -0.01, 2), "refuses a negative centre");
    expect(!p.prepare(48000, 0.5, 1, 1, 23, 4, 1, 4.01), "refuses a spread past the channels");
    expect(!p.prepare(48000, 0.5, 1, 1, 23, 4, 1, 1.99), "refuses a spread under 2");
    expect(!p.prepare(48000, 0.5, 1, 1.1, 1000, 4, 1, 2), "refuses a last echo over 100 s (96.19 s times 1.1)");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 1.000001, 1.000001, 1, 1000, 4, 4, 4), "takes the top ends within 0.000002");
    expect(p.prepare(48000, g, g, 0.2 / 48000, 2, 2, 0, 2), "takes the bottom ends");
    expect(MultichannelStadium::defaultBufferFrames(4) == 52428, "4 outputs: CDP's 52428-frame buffer");
    expect(MultichannelStadium::defaultBufferFrames(8) == 29127, "8 outputs: CDP's 29127-frame buffer");
    expect(p.prepare(48000, 0.5, 1, 1, 23, 4, 1, 2), "prepares");
    expect(p.outputChannels() == 4 && p.latencyFrames() == 0, "4 channels, no latency");
    const auto before = p;
    expect(!p.process(in, shorter), "refuses an output not 4 times the input");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
}
}

void run()
{
    fixture("mchanrev-4-channels-count-23-size-0.1-centre-1-spread-2", 0.645654, 1, 0.1, 23, 4, 1, 2);
    fixture("mchanrev-8-channels-count-30-size-0.05-centre-3.5-spread-6-rolloff-0.5", 0.8, 0.5, 0.05, 30, 8, 3.5, 6);
    fixture("mchanrev-4-channels-count-10-size-0.05-512-frame-buffers", 0.9, 0.7, 0.05, 10, 4, 2.25, 3, 512);
    fixture("mchanrev-6-channels-count-100-size-0.02-spread-4-731-frame-buffers", 0.5, 0.8, 0.02, 100, 6, 6, 4, 731);
    fixture("mchanrev-6-channels-count-1000-size-0.003-centre-6-spread-6-rolloff-0.3-rate-44100", 0.7, 0.3, 0.003, 1000,
        6, 6, 6);
    fixture("mchanrev-2-channels-count-2-size-0.2-centre-0-spread-2", 1, 1, 0.2, 2, 2, 0, 2);
    fixture("mchanrev-3-channels-count-23-size-0.016-centre-2-spread-3", 0.6, 0.9, 0.016, 23, 3, 2, 3);
    refusals();
}
}

// Checks Flutter against the CDP8 flutter fixtures bit for bit: in 64-frame
// blocks and other sizes, Frequency and Depth set where CDP reads them
// (every frame), CDP's frozen clock after Depth 0 (small CDP buffers),
// random order, reset and replay, refusals.
namespace flutter
{
using namespace talas::cdp;
using namespace check;

namespace
{
using Sets = std::vector<std::vector<int>>;

void fixture(const std::string& name, int channels, const Sets& sets, double freq, double depth, double gain,
    bool random, std::size_t buffer = 0, std::size_t minimum = 8)
{
    const auto f = load(name);
    const Table* tf = f.tables.count("freq") ? &f.tables.at("freq") : nullptr;
    const Table* td = f.tables.count("depth") ? &f.tables.at("depth") : nullptr;
    // flutter: thistime = frame * (1 / srate).
    const auto inverse = 1.0 / f.rate;
    if (tf)
        freq = tf->at(0.0);
    if (td)
        depth = td->at(0.0);
    const auto ch = static_cast<std::size_t>(channels);
    mc::checkAll<Flutter>(
        name, f, ch, ch,
        [&](Flutter& p) { return p.prepare(f.rate, channels, sets, freq, depth, gain, random, buffer); },
        [&](Flutter& p) -> mc::Controls {
            if (!tf && !td)
                return {};
            mc::Controls c = [](std::size_t, std::size_t) { return true; };
            if (tf)
                c = mc::both(c, mc::periodic(0, 1, [=](std::size_t r) { return tf->at(double(r) * inverse); },
                                    [&p](double v) { return p.setFrequency(v); }));
            if (td)
                c = mc::both(c, mc::periodic(0, 1, [=](std::size_t r) { return td->at(double(r) * inverse); },
                                    [&p](double v) { return p.setDepth(v); }));
            return c;
        },
        minimum);
}

void refusals()
{
    Flutter p;
    const Sets sets{{1, 2}, {3}};
    std::vector<double> in(16, 0.1), out(16, 0.0), shorter(15, 0.0), ragged(15, 0.1);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    expect(!p.prepared(), "not prepared at first");
    expect(!p.process(in, out), "refuses process before prepare");
    expect(!p.setFrequency(1.0) && !p.setDepth(1.0), "refuses setters before prepare");
    expect(!p.prepare(0.0, 4, sets, 10, 1, 1), "refuses a zero rate");
    expect(!p.prepare(nan, 4, sets, 10, 1, 1), "refuses a NaN rate");
    expect(!p.prepare(48000, 0, sets, 10, 1, 1), "refuses 0 channels");
    expect(!p.prepare(48000, 17, sets, 10, 1, 1), "refuses 17 channels");
    expect(!p.prepare(48000, 2, sets, 10, 1, 1), "refuses channel 3 of 2");
    expect(!p.prepare(48000, 4, Sets{{0}}, 10, 1, 1), "refuses channel 0");
    expect(!p.prepare(48000, 4, Sets{}, 10, 1, 1), "refuses no sets");
    expect(!p.prepare(48000, 4, Sets(1025, std::vector<int>{1}), 10, 1, 1), "refuses 1025 sets");
    expect(!p.prepare(48000, 4, Sets{{1, 2}}, 10, 1, 1, true), "refuses random order with one set");
    expect(!p.prepare(48000, 4, sets, 120.01, 1, 1), "refuses frequency over 120");
    expect(!p.prepare(48000, 4, sets, -0.01, 1, 1), "refuses negative frequency");
    expect(!p.prepare(48000, 4, sets, 10, 16.01, 1), "refuses depth over 16");
    expect(!p.prepare(48000, 4, sets, 10, -0.01, 1), "refuses negative depth");
    expect(!p.prepare(48000, 4, sets, 10, 1, 10.01), "refuses gain over 10");
    expect(!p.prepare(48000, 4, sets, 10, 1, 0.0000015), "refuses a gain within 0.000002 of 0 (CDP: silent)");
    expect(!p.prepare(48000, 4, sets, nan, 1, 1), "refuses NaN frequency");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 4, sets, 120.000001, 16.000001, 10.000001), "takes the top ends within 0.000002");
    expect(p.prepare(48000, 4, Sets{{1}, {}}, 0, 0, 0.0000041), "takes the bottom ends (gain past 0.000002 from 0), an empty set");
    expect(Flutter::defaultBufferFrames(4) == 65536 && Flutter::defaultBufferFrames(3) == 87381,
        "CDP's buffers: 262144 / N frames");
    expect(p.prepare(48000, 4, sets, 10, 1, 1), "prepares");
    expect(p.outputChannels() == 4 && p.latencyFrames() == 0, "4 channels, no latency");
    const auto before = p;
    expect(!p.setFrequency(120.5) && !p.setDepth(16.5), "refuses setters past the ranges");
    expect(!p.setDepth(nan), "refuses NaN depth");
    expect(!p.process(in, shorter), "refuses an output of another size");
    expect(!p.process(ragged, ragged), "refuses a part frame");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
}
}

void run()
{
    fixture("flutter-4-channels-sets-12-3-4-freq-60-depth-1-gain-1", 4, Sets{{1, 2}, {3}, {4}}, 60, 1, 1, false);
    fixture("flutter-8-channels-pairs-freq-120-depth-0.6-gain-0.8-random", 8, Sets{{1, 5}, {2, 6}, {3, 7}, {4, 8}},
        120, 0.6, 0.8, true);
    fixture("flutter-mono-freq-30-depth-3-gain-1.5", 1, Sets{{1}}, 30, 3, 1.5, false);
    fixture("flutter-stereo-empty-and-doubled-sets-freq-90-depth-0.9-gain-0.5-rate-44100", 2,
        Sets{{1}, {}, {2, 2}, {1, 2}}, 90, 0.9, 0.5, false);
    fixture("flutter-4-channels-live-freq-depth-640-frame-buffers", 4, Sets{{1}, {2}, {3}, {4}, {1, 2, 3, 4}}, 0, 0,
        1.2, false, 640, 2);
    fixture("flutter-3-channels-ramps", 3, Sets{{1}, {2, 3}}, 0, 0, 0.9, false, 0, 1);
    fixture("flutter-3-channels-random-live-freq", 3, Sets{{1}, {2}, {3}}, 0, 0.8, 1, true, 0, 2);
    refusals();
}
}

// Checks FrameRotate against the CDP8 frame shift 1 and 2 fixtures bit for
// bit: in 64-frame blocks and other sizes, rotations set where CDP reads
// them (every round(sr / 500) frames), snakes changed where CDP changes
// them, reset and replay, refusals.
namespace frame
{
using namespace talas::cdp;
using namespace check;

namespace
{
struct Snake
{
    std::size_t frame;
    std::vector<int> route;
};

// get_first_rotation_orientation over a breakpoint table: it steps j over
// the values (brk[1], brk[3], ...) while j < the count of breakpoints, so
// it sees only the first half of them.
int firstDirection(const Table* t)
{
    if (!t)
        return 0;
    const auto count = t->values.size();
    for (std::size_t j = 1; j < count; j += 2)
    {
        const auto v = t->values[j / 2];
        if (v > 0.0)
            return 1;
        if (v < 0.0)
            return -1;
    }
    return 0;
}

void fixture(const std::string& name, int channels, bool dual, double r1, double r2, double smear,
    const std::vector<Snake>& snakes = {}, std::size_t minimum = 8)
{
    const auto f = load(name);
    const auto table = [&](const char* label) -> const Table* {
        return f.tables.count(label) ? &f.tables.at(label) : nullptr;
    };
    const auto* t1 = table(dual ? "rotation1" : "rotation");
    const auto* t2 = table("rotation2");
    const auto d1 = firstDirection(t1);
    const auto d2 = firstDirection(t2);
    if (t1)
        r1 = t1->at(0.0);
    if (t2)
        r2 = t2->at(0.0);
    const auto click = FrameRotate::readFrames(f.rate);
    const auto ch = static_cast<std::size_t>(channels);
    const std::vector<int> first = snakes.empty() ? std::vector<int>{} : snakes.front().route;
    mc::checkAll<FrameRotate>(
        name, f, ch, ch,
        [&](FrameRotate& p) {
            return dual ? p.prepareDual(f.rate, channels, r1, r2, smear, d1, d2, first)
                        : p.prepare(f.rate, channels, r1, smear, d1, first);
        },
        [&](FrameRotate& p) -> mc::Controls {
            mc::Controls c = [&p, &snakes](std::size_t start, std::size_t count) {
                for (std::size_t k = 1; k < snakes.size(); k++)
                {
                    const auto at = snakes[k].frame;
                    if (at == start)
                        expect(p.setSnake(snakes[k].route), "setSnake accepted");
                    else if (at > start && at < start + count)
                        return false;
                }
                return true;
            };
            const auto at = [&f](const Table* t) {
                return [&f, t](std::size_t r) { return t->at(double(r) / f.rate); };
            };
            if (t1)
                c = mc::both(c, mc::periodic(click, click, at(t1), [&p](double v) { return p.setRotation(v); }));
            if (t2)
                c = mc::both(c, mc::periodic(click, click, at(t2), [&p](double v) { return p.setSecondRotation(v); }));
            return c;
        },
        minimum);
}

void refusals()
{
    FrameRotate p;
    std::vector<double> in(16, 0.1), out(16, 0.0), shorter(15, 0.0), ragged(15, 0.1);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    expect(!p.prepared(), "not prepared at first");
    expect(!p.process(in, out), "refuses process before prepare");
    expect(!p.setRotation(1.0) && !p.setSecondRotation(1.0), "refuses setters before prepare");
    expect(!p.setSnake(std::vector<int>{1, 2, 3, 4}), "refuses setSnake before prepare");
    expect(!p.prepare(nan, 4, 1), "refuses a NaN rate");
    expect(!p.prepare(249, 4, 1), "refuses a rate under 250 (CDP never ends)");
    expect(!p.prepare(48000, 1, 1), "refuses 1 channel");
    expect(!p.prepare(48000, 17, 1), "refuses 17 channels");
    expect(!p.prepare(48000, 4, 500.01), "refuses rotation over 500");
    expect(!p.prepare(48000, 4, -500.01), "refuses rotation under -500");
    expect(!p.prepare(48000, 4, 1, 0.51), "refuses smear over 0.5");
    expect(!p.prepare(48000, 4, 1, -0.01), "refuses negative smear");
    expect(!p.prepare(48000, 4, 1, 0, 2), "refuses direction 2");
    expect(!p.prepare(48000, 4, 1, 0, 0, std::vector<int>{1, 2, 3}), "refuses a short snake");
    expect(!p.prepare(48000, 4, 1, 0, 0, std::vector<int>{1, 2, 3, 3}), "refuses a snake with a repeat");
    expect(!p.prepare(48000, 4, 1, 0, 0, std::vector<int>{1, 2, 3, 5}), "refuses a snake past the channels");
    expect(!p.prepareDual(48000, 5, 1, 1), "refuses mode 2 with 5 channels");
    expect(!p.prepareDual(48000, 4, 0, 0), "refuses mode 2 with no rotation");
    expect(!p.prepareDual(48000, 4, 0, 0, 0, 0, 0), "refuses mode 2 with no direction");
    expect(!p.prepare(48000, 4, nan), "refuses a NaN rotation");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 4, 500.000001, 0.500001), "takes the top ends within 0.000002");
    expect(p.prepare(48000, 4, -500.000001, 0, 1), "takes the bottom end, direction 1");
    expect(p.prepareDual(48000, 4, 0, 0, 0, 0, -1), "mode 2: one direction given");
    expect(FrameRotate::readFrames(48000) == 96 && FrameRotate::readFrames(44100) == 88
            && FrameRotate::readFrames(250) == 1 && FrameRotate::readFrames(249) == 0,
        "CDP's reads: round(sr / 500) frames");
    expect(p.prepare(48000, 4, 1), "prepares");
    expect(p.outputChannels() == 4 && p.latencyFrames() == 0, "4 channels, no latency");
    const auto before = p;
    expect(!p.setRotation(501) && !p.setSecondRotation(1), "refuses rotation over 500, a second rotation in mode 1");
    expect(!p.setSnake(std::vector<int>{4, 3, 2}), "refuses a short snake");
    expect(!p.process(in, shorter), "refuses an output of another size");
    expect(!p.process(ragged, ragged), "refuses a part frame");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");
}
}

void run()
{
    fixture("frame-1-rotate-4-channels-rotation-10", 4, false, 10, 0, 0);
    fixture("frame-1-rotate-8-channels-rotation-minus-20-smear-0.2", 8, false, -20, 0, 0.2);
    fixture("frame-1-rotate-4-channels-live-rotation", 4, false, 0, 0, 0, {}, 4);
    fixture("frame-1-rotate-4-channels-live-rotation-starts-still-set-up-clockwise", 4, false, 0, 0, 0.05, {}, 4);
    fixture("frame-1-rotate-4-channels-live-rotation-starts-still-set-up-anticlockwise", 4, false, 0, 0, 0, {}, 3);
    fixture("frame-1-rotate-6-channels-snake-rotation-30", 6, false, 30, 0, 0,
        {{0, {1, 3, 5, 2, 4, 6}}, {1536, {6, 5, 4, 3, 2, 1}}}, 4);
    fixture("frame-1-rotate-6-channels-rotation-500-smear-0.45-rate-44100", 6, false, 500, 0, 0.45);
    fixture("frame-2-rotate-6-channels-rotations-20-minus-30-smear-0.1", 6, true, 20, -30, 0.1);
    fixture("frame-2-rotate-4-channels-rotations-0-40", 4, true, 0, 40, 0);
    fixture("frame-2-rotate-6-channels-live-rotations-snake", 6, true, 0, 0, 0, {{0, {2, 4, 6, 1, 3, 5}}}, 4);
    refusals();
}
}

// Checks FrameRemap and ChannelEnvelope against the CDP8 frame shift 3-8
// fixtures bit for bit: in 64-frame blocks and other sizes, gain steps set
// at CDP's breakpoint frames, reset and replay, every map a permutation,
// refusals.
namespace remap
{
using namespace talas::cdp;
using namespace check;

namespace
{
void remap(const std::string& name, int channels, const std::function<bool(FrameRemap&, double)>& prepare)
{
    const auto f = load(name);
    const auto ch = static_cast<std::size_t>(channels);
    mc::checkAll<FrameRemap>(name, f, ch, ch, [&](FrameRemap& p) { return prepare(p, f.rate); });
}

void envelope(const std::string& name, int channels, std::vector<int> chans, double gain)
{
    const auto f = load(name);
    const Table* t = f.tables.count("gain") ? &f.tables.at("gain") : nullptr;
    // envchans' breakpoint frames: round(time * srate).
    std::vector<std::size_t> frames;
    if (t)
    {
        gain = t->values.front();
        for (const auto time : t->times)
            frames.push_back(static_cast<std::size_t>(std::lround(time * f.rate)));
        expect(frames.front() == 0, name + ": the file starts at time 0");
    }
    const auto ch = static_cast<std::size_t>(channels);
    mc::checkAll<ChannelEnvelope>(
        name, f, ch, ch, [&](ChannelEnvelope& p) { return p.prepare(f.rate, channels, chans, gain); },
        [&](ChannelEnvelope& p) -> mc::Controls {
            if (!t)
                return {};
            return [&p, frames, t](std::size_t start, std::size_t count) {
                for (std::size_t k = 0; k < frames.size(); k++)
                {
                    const auto at = frames[k];
                    if (at > start && at < start + count)
                        return false;
                    if (at != start)
                        continue;
                    if (k + 1 < frames.size())
                        expect(p.setGain(t->values[k + 1], frames[k + 1] - at), "setGain accepted");
                    else
                        expect(p.holdGain(), "holdGain accepted");
                }
                return true;
            };
        },
        t ? 4 : 8);
}

bool permutation(const FrameRemap& p, int channels)
{
    std::vector<bool> seen(static_cast<std::size_t>(channels), false);
    for (std::size_t c = 0; c < static_cast<std::size_t>(channels); c++)
    {
        const auto d = p.destination(c);
        if (d >= seen.size() || seen[d])
            return false;
        seen[d] = true;
    }
    return true;
}

void maps()
{
    std::size_t built = 0;
    bool all = true;
    for (int c = FrameRemap::minimumChannels; c <= FrameRemap::maximumChannels; c++)
    {
        FrameRemap p;
        for (int twice = 0; twice <= 2 * c + 1; twice++)
        {
            const auto plane = twice / 2.0;
            if (plane > c + 0.5)
                break;
            if (p.prepareMirror(48000, c, plane))
            {
                built++;
                all = all && permutation(p, c);
            }
        }
        for (const bool ring : {false, true})
        {
            expect(p.prepareBilateral(48000, c, ring) && permutation(p, c), "bilateral map a permutation");
            expect(p.prepareBeastBilateral(48000, c, ring) && permutation(p, c), "BEAST map a permutation");
            FrameRemap there;
            FrameRemap back;
            expect(there.prepareBilateral(48000, c, ring) && back.prepareBilateral(48000, c, !ring), "both ways");
            bool inverse = true;
            for (std::size_t k = 0; k < static_cast<std::size_t>(c); k++)
                inverse = inverse && back.destination(there.destination(k)) == k;
            expect(inverse, "bilateral to ring undoes ring to bilateral, " + std::to_string(c) + " channels");
        }
    }
    expect(all && built > 250, "every mirror map a permutation (" + std::to_string(built) + " maps)");
}

void refusals()
{
    FrameRemap p;
    std::vector<double> in(16, 0.1), out(16, 0.0), shorter(15, 0.0), ragged(15, 0.1);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    expect(!p.prepared(), "not prepared at first");
    expect(!p.process(in, out), "refuses process before prepare");
    expect(!p.prepareSwap(nan, 4, 1, 2), "refuses a NaN rate");
    expect(!p.prepareSwap(48000, 1, 1, 2), "refuses 1 channel");
    expect(!p.prepareSwap(48000, 17, 1, 2), "refuses 17 channels");
    expect(!p.prepareSwap(48000, 4, 2, 2), "refuses a swap of one channel");
    expect(!p.prepareSwap(48000, 4, 0, 2), "refuses channel 0");
    expect(!p.prepareSwap(48000, 4, 1, 5), "refuses channel 5 of 4");
    expect(!p.prepareReorient(48000, 4, std::vector<int>{1, 2, 3}), "refuses a short route");
    expect(!p.prepareReorient(48000, 4, std::vector<int>{1, 2, 3, 3}), "refuses a route with a repeat");
    expect(!p.prepareMirror(48000, 4, 4.6), "refuses a plane neither whole nor half");
    expect(!p.prepareMirror(48000, 4, 5), "refuses a plane past channel N + 0.5");
    expect(!p.prepareMirror(48000, 16, 16.6), "refuses a plane over 16.5");
    expect(!p.prepareMirror(48000, 4, -0.5), "refuses a negative plane");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepareMirror(48000, 4, 2.4999995), "takes a half within 0.000002");
    expect(p.prepareMirror(48000, 16, 16.500001), "takes 16.5 within 0.000002");
    expect(p.prepareSwap(48000, 4, 1, 2), "prepares");
    expect(p.outputChannels() == 4 && p.latencyFrames() == 0, "4 channels, no latency");
    const auto before = p;
    expect(!p.process(in, shorter), "refuses an output of another size");
    expect(!p.process(ragged, ragged), "refuses a part frame");
    in[3] = nan;
    expect(!p.process(in, out), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out), "processes");

    ChannelEnvelope e;
    expect(!e.prepared() && !e.process(in, out), "envelope: refuses process before prepare");
    expect(!e.setGain(0.5) && !e.holdGain(), "envelope: refuses setters before prepare");
    expect(!e.prepare(0, 4, std::vector<int>{1}, 0.5), "envelope: refuses a zero rate");
    expect(!e.prepare(48000, 1, std::vector<int>{1}, 0.5), "envelope: refuses 1 channel");
    expect(!e.prepare(48000, 4, std::vector<int>{}, 0.5), "envelope: refuses no channels");
    expect(!e.prepare(48000, 4, std::vector<int>{5}, 0.5), "envelope: refuses channel 5 of 4");
    expect(!e.prepare(48000, 4, std::vector<int>{2, 2}, 0.5), "envelope: refuses a channel twice");
    expect(!e.prepare(48000, 4, std::vector<int>{2}, 1.01), "envelope: refuses gain over 1");
    expect(!e.prepare(48000, 4, std::vector<int>{2}, -0.01), "envelope: refuses negative gain");
    expect(!e.prepared(), "envelope: still not prepared");
    expect(e.prepare(48000, 4, std::vector<int>{2, 4}, 1.000001), "envelope: takes 1 within 0.000002");
    const auto eBefore = e;
    expect(!e.setGain(1.1, 10) && !e.setGain(nan), "envelope: refuses gains past the range");
    expect(!e.process(in, shorter), "envelope: refuses an output of another size");
    in[3] = nan;
    expect(!e.process(in, out), "envelope: refuses a NaN input");
    expect(e == eBefore, "envelope: refusals change nothing");
}
}

void run()
{
    remap("frame-3-reorient-4-channels-4-1-2-3", 4,
        [](FrameRemap& p, double sr) { return p.prepareReorient(sr, 4, std::vector<int>{4, 1, 2, 3}); });
    remap("frame-3-reorient-6-channels-3-6-1-5-2-4", 6,
        [](FrameRemap& p, double sr) { return p.prepareReorient(sr, 6, std::vector<int>{3, 6, 1, 5, 2, 4}); });
    remap("frame-4-mirror-4-channels-plane-2", 4, [](FrameRemap& p, double sr) { return p.prepareMirror(sr, 4, 2); });
    remap("frame-4-mirror-7-channels-plane-3.5", 7,
        [](FrameRemap& p, double sr) { return p.prepareMirror(sr, 7, 3.5); });
    remap("frame-4-mirror-5-channels-plane-5.5", 5,
        [](FrameRemap& p, double sr) { return p.prepareMirror(sr, 5, 5.5); });
    remap("frame-4-mirror-6-channels-plane-0", 6, [](FrameRemap& p, double sr) { return p.prepareMirror(sr, 6, 0); });
    remap("frame-5-bilateral-7-channels", 7, [](FrameRemap& p, double sr) { return p.prepareBilateral(sr, 7); });
    remap("frame-5-bilateral-6-channels-to-ring", 6,
        [](FrameRemap& p, double sr) { return p.prepareBilateral(sr, 6, true); });
    remap("frame-6-swap-4-channels-2-4", 4, [](FrameRemap& p, double sr) { return p.prepareSwap(sr, 4, 2, 4); });
    remap("frame-8-beast-8-channels", 8, [](FrameRemap& p, double sr) { return p.prepareBeastBilateral(sr, 8); });
    remap("frame-8-beast-5-channels-to-ring", 5,
        [](FrameRemap& p, double sr) { return p.prepareBeastBilateral(sr, 5, true); });
    envelope("frame-7-envchans-4-channels-channel-2-gain-0.5", 4, {2}, 0.5);
    envelope("frame-7-envchans-6-channels-1-3-6-gain-ramps", 6, {1, 3, 6}, 0);
    maps();
    refusals();
}
}

// Checks ToStereo against the CDP8 tostereo fixtures bit for bit: in
// 64-frame blocks and other sizes, reset and replay, refusals.
namespace tostereo
{
using namespace talas::cdp;
using namespace check;

namespace
{
void fixture(const std::string& name, double start, double end, int channels = 2, int left = 0, int right = 0,
    double mix = 0.0)
{
    const auto f = load(name);
    mc::checkAll<ToStereo>(name, f, 2, static_cast<std::size_t>(channels),
        [&](ToStereo& p) { return p.prepare(f.rate, start, end, channels, left, right, mix); });
}

void refusals()
{
    ToStereo p;
    std::vector<double> in(16, 0.1), out(16, 0.0), shorter(15, 0.0), odd(15, 0.1);
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    expect(!p.prepared(), "not prepared at first");
    expect(!p.process(in, out), "refuses process before prepare");
    expect(!p.prepare(0.0, 0, 1), "refuses a zero rate");
    expect(!p.prepare(nan, 0, 1), "refuses a NaN rate");
    expect(!p.prepare(48000, -0.1, 1), "refuses a negative start");
    expect(!p.prepare(48000, 0, nan), "refuses a NaN end");
    expect(!p.prepare(48000, 0.5, 0.5000015), "refuses a start within 0.000002 s of the end");
    expect(!p.prepare(48000, 0, 1, 1), "refuses 1 output");
    expect(!p.prepare(48000, 0, 1, 17), "refuses 17 outputs");
    expect(!p.prepare(48000, 0, 1, 2, 1, 0), "refuses 2 outputs with left only");
    expect(!p.prepare(48000, 0, 1, 2, 0, 2), "refuses 2 outputs with right only");
    expect(!p.prepare(48000, 0, 1, 4), "refuses 4 outputs with neither left nor right");
    expect(!p.prepare(48000, 0, 1, 4, 5, 1), "refuses a left past the outputs");
    expect(!p.prepare(48000, 0, 1, 4, 1, 5), "refuses a right past the outputs");
    expect(!p.prepare(48000, 0, 1, 4, 2, 2), "refuses left and right the same");
    expect(!p.prepare(48000, 0, 1, 4, 1, 0, 0.4), "refuses a mix level under 0.5 (not 0)");
    expect(!p.prepare(48000, 0, 1, 4, 1, 0, 1.01), "refuses a mix level over 1");
    expect(!p.prepare(48000, 0, 1, 4, -1, 0), "refuses a negative left");
    expect(!p.prepared(), "still not prepared");
    expect(p.prepare(48000, 0, 1, 2, 2, 1, 1.000001), "takes left 2, right 1, mix within 0.000002 of 1");
    expect(p.prepare(48000, 0, 1, 2, 0, 0, 0.5), "takes mix 0.5");
    expect(p.prepare(48000, 1, 0, 3, 3, 0), "forks from channel 3 of 3");
    expect(p.outputChannels() == 3 && p.latencyFrames() == 0, "3 channels, no latency");
    std::vector<double> out3(24, 0.0), short3(23, 0.0);
    const auto before = p;
    expect(!p.process(in, short3), "refuses an output not 3/2 of the input");
    expect(!p.process(odd, out3), "refuses an odd input");
    in[3] = nan;
    expect(!p.process(in, out3), "refuses a NaN input");
    expect(p == before, "refusals change nothing");
    in[3] = 0.0;
    expect(p.process(in, out3), "processes");
}
}

void run()
{
    fixture("tostereo-start-0.005-end-0.03", 0.005, 0.03);
    fixture("tostereo-start-0.035-end-0.01-mix-0.707", 0.035, 0.01, 2, 0, 0, 0.707);
    fixture("tostereo-fork-8-channels-left-3-start-0.008-end-0.025", 0.008, 0.025, 8, 3, 0);
    fixture("tostereo-fork-4-channels-left-1-start-0.025-end-0-mix-0.9", 0.025, 0, 4, 1, 0, 0.9);
    fixture("tostereo-6-channels-left-5-right-2-start-0-end-0.04-mix-1-rate-44100", 0, 0.04, 6, 5, 2, 1);
    refusals();
}
}

int main()
{
    rotate::run();
    spread::run();
    table::run();
    step::run();
    cross::run();
    antiphony::run();
    stadium::run();
    flutter::run();
    frame::run();
    remap::run();
    tostereo::run();
    return check::finish();
}
