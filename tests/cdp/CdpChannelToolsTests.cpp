// CDP8's channel and editing tools, ported in third_party/cdp (StereoSpace,
// ChannelZero, ChannelMixdown, MonoToStereo, ChannelPhase, MonoToMany,
// ChannelPair, StereoPlacement, Prefix, SilenceMasks, SilenceOverwrite,
// MergeMany and EnvelopeWarpStretch), against recorded CDP8 references bit
// for bit: each fixture played in blocks of 64, 1, 7, 128, 256, 333, 1000
// and 16384 frames, twice with a reset between, latencyFrames() late where
// a port has latency; and each port's refusals, which change nothing. None
// of these programs takes a breakpoint file, so none has live fields.
#include "talas/cdp/ChannelTools.h"
#include "talas/cdp/EnvelopeWarpStretch.h"
#include "talas/cdp/MergeMany.h"
#include "talas/cdp/Prefix.h"
#include "talas/cdp/SilenceMasks.h"
#include "talas/cdp/StereoPlacement.h"
#include "talas/cdp/StereoSpace.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
int failures = 0;
int checks = 0;
const std::string fixtureDir = TALAS_CDP_FIXTURE_DIR;

void require(bool condition, const std::string& message)
{
    checks++;
    if (!condition)
    {
        failures++;
        std::printf("FAIL: %s\n", message.c_str());
    }
}

const auto nan = std::numeric_limits<double>::quiet_NaN();

struct Fixture
{
    // Columns: each input's channels, then the outputs.
    std::vector<std::vector<double>> columns;
    double rate = 48000.0;
    [[nodiscard]] std::size_t frames() const { return columns.empty() ? 0 : columns[0].size(); }
};

Fixture load(const std::string& name)
{
    Fixture f;
    const auto path = fixtureDir + "/" + name + ".txt";
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("cannot open " + path);
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            const auto k = line.find(" kHz");
            if (line.rfind("# CDP8", 0) == 0 && k != std::string::npos)
            {
                const auto s = line.rfind(' ', k - 1);
                f.rate = std::strtod(line.substr(s + 1, k - s - 1).c_str(), nullptr) * 1000.0;
            }
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
    return f;
}

bool same(const std::vector<double>& got, const std::vector<double>& want, const std::string& what)
{
    std::size_t bad = 0;
    std::size_t first = 0;
    for (std::size_t n = 0; n < want.size(); n++)
    {
        if (n >= got.size() || std::bit_cast<std::uint64_t>(got[n]) != std::bit_cast<std::uint64_t>(want[n]))
        {
            if (bad == 0)
                first = n;
            bad++;
        }
    }
    if (bad != 0)
    {
        std::printf("  %s: %zu mismatches, first at frame %zu: got %a want %a\n", what.c_str(), bad, first,
            first < got.size() ? got[first] : 0.0, want[first]);
    }
    require(bad == 0, what + " bit-exact");
    return bad == 0;
}

constexpr std::array<std::size_t, 8> blockSizes{64, 1, 7, 128, 256, 333, 1000, 16384};

template <class Port>
std::size_t outputsOf(const Port& port)
{
    if constexpr (requires { port.outputChannels(); })
        return port.outputChannels();
    else
        return 1;
}

// Plays the fixture's inputs (each of inputChannels[k] channels, then
// latencyFrames() of silence) through the port in blocks; returns each
// output channel from latencyFrames() on, the fixture's length.
template <class Port>
bool play(Port& port, const Fixture& f, const std::vector<std::size_t>& inputChannels, std::size_t block,
    std::vector<std::vector<double>>& channels)
{
    const auto latency = port.latencyFrames();
    const auto ch = outputsOf(port);
    const auto total = f.frames() + latency;
    std::vector<std::vector<double>> ins;
    std::size_t column = 0;
    for (const auto inCh : inputChannels)
    {
        std::vector<double> in(total * inCh, 0.0);
        for (std::size_t n = 0; n < f.frames(); n++)
        {
            for (std::size_t c = 0; c < inCh; c++)
                in[n * inCh + c] = f.columns[column + c][n];
        }
        column += inCh;
        ins.push_back(std::move(in));
    }
    std::vector<double> out(total * ch, -7.0);
    for (std::size_t start = 0; start < total; start += block)
    {
        const auto count = std::min(block, total - start);
        const auto o = std::span<double>(out).subspan(start * ch, count * ch);
        bool ok = false;
        if constexpr (requires(std::span<const std::span<const double>> s) { port.process(s, o); })
        {
            std::vector<std::span<const double>> spans;
            for (std::size_t k = 0; k < ins.size(); k++)
                spans.push_back(std::span<const double>(ins[k]).subspan(start * inputChannels[k], count * inputChannels[k]));
            ok = port.process(std::span<const std::span<const double>>(spans), o);
        }
        else
        {
            ok = port.process(std::span<const double>(ins[0]).subspan(start * inputChannels[0], count * inputChannels[0]), o);
        }
        require(ok, "process accepted");
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

// Plays every block size twice (reset between), comparing each output
// channel with the fixture's columns after the inputs', bit for bit.
template <class Port>
void checkAll(const std::string& name, const std::vector<std::size_t>& inputChannels, std::size_t outChannels,
    const std::function<bool(Port&, const Fixture&)>& prepare, std::size_t latency = 0)
{
    const auto f = load(name);
    std::size_t in = 0;
    for (const auto c : inputChannels)
        in += c;
    require(f.columns.size() == in + outChannels,
        name + ": columns " + std::to_string(f.columns.size()) + " (want " + std::to_string(in + outChannels) + ")");
    if (f.columns.size() != in + outChannels)
        return;
    for (const auto block : blockSizes)
    {
        Port port;
        require(prepare(port, f) && port.prepared(), name + ": prepare");
        require(outputsOf(port) == outChannels, name + ": output channels");
        require(port.latencyFrames() == latency, name + ": latency " + std::to_string(port.latencyFrames()));
        for (int pass = 0; pass < 2; pass++)
        {
            std::vector<std::vector<double>> out;
            if (!play(port, f, inputChannels, block, out))
                break;
            for (std::size_t ch = 0; ch < outChannels; ch++)
            {
                same(out[ch], f.columns[in + ch],
                    name + " channel " + std::to_string(ch) + " block " + std::to_string(block)
                        + (pass ? " replay after reset" : ""));
            }
            port.reset();
        }
    }
}

// ---------------------------------------------------------------------------

// Checks StereoSpace against the CDP8 references.
namespace space
{
using talas::cdp::StereoSpace;

void run()
{
    checkAll<StereoSpace>("modify-space-2-mirror", {2}, 2, [](StereoSpace& p, const Fixture&) { return p.prepareMirror(); });
    const auto narrow = [](const std::string& name, double n) {
        checkAll<StereoSpace>(name, {2}, 2, [n](StereoSpace& p, const Fixture&) { return p.prepareNarrow(n); });
    };
    narrow("modify-space-4-narrow-0.3", 0.3);
    narrow("modify-space-4-narrow-minus-0.65", -0.65);
    narrow("modify-space-4-narrow-0-mono", 0.0000015);
    narrow("modify-space-4-narrow-minus-1-mirror", -0.9999985);

    StereoSpace p;
    std::vector<double> in(8, 0.1), out(8, 0.0), odd(7, 0.1), oddOut(7, 0.0);
    require(!p.prepared() && !p.process(in, out), "space: refuses process before prepare");
    require(!p.prepareNarrow(1.0), "space: refuses narrowing 1 (no effect)");
    require(!p.prepareNarrow(0.9999985), "space: refuses narrowing within 0.000002 of 1");
    require(!p.prepareNarrow(1.1) && !p.prepareNarrow(-1.1) && !p.prepareNarrow(nan), "space: refuses out of range");
    require(!p.prepared(), "space: still not prepared");
    require(p.prepareNarrow(-1.0000015) && p.mode() == StereoSpace::Mode::Narrow, "space: takes -1.0000015 as -1");
    const auto before = p;
    require(!p.process(odd, oddOut), "space: refuses an odd size");
    require(!p.process(in, oddOut), "space: refuses sizes that differ");
    in[2] = nan;
    require(!p.process(in, out), "space: refuses a NaN");
    require(p == before, "space: refusals change nothing");
}
}

// Checks ChannelZero, ChannelMixdown, MonoToStereo, ChannelPhase, MonoToMany and ChannelPair against the CDP8 references.
namespace chans
{
using namespace talas::cdp;

void run()
{
    checkAll<ChannelZero>("housekeep-chans-3-mono-channel-1", {1}, 2,
        [](ChannelZero& p, const Fixture&) { return p.prepare(1, 1); });
    checkAll<ChannelZero>("housekeep-chans-3-mono-channel-1-256-frame-buffers", {1}, 2,
        [](ChannelZero& p, const Fixture&) { return p.prepare(1, 1, 256); });
    checkAll<ChannelZero>("housekeep-chans-3-stereo-channel-2", {2}, 2,
        [](ChannelZero& p, const Fixture&) { return p.prepare(2, 2); });
    checkAll<ChannelZero>("housekeep-chans-3-5-channels-channel-4", {5}, 5,
        [](ChannelZero& p, const Fixture&) { return p.prepare(5, 4); });
    checkAll<ChannelMixdown>("housekeep-chans-4-stereo", {2}, 1,
        [](ChannelMixdown& p, const Fixture&) { return p.prepare(); });
    checkAll<ChannelMixdown>("housekeep-chans-4-stereo-invert-phase", {2}, 1,
        [](ChannelMixdown& p, const Fixture&) { return p.prepare(true); });
    checkAll<MonoToStereo>("housekeep-chans-5-mono-to-stereo", {1}, 2,
        [](MonoToStereo& p, const Fixture&) { return p.prepare(); });
    checkAll<ChannelPhase>("chanphase-stereo-channel-1", {2}, 2,
        [](ChannelPhase& p, const Fixture&) { return p.prepare(2, 1); });
    checkAll<ChannelPhase>("chanphase-6-channels-channel-5", {6}, 6,
        [](ChannelPhase& p, const Fixture&) { return p.prepare(6, 5); });
    checkAll<MonoToMany>("mton-3-channels", {1}, 3, [](MonoToMany& p, const Fixture&) { return p.prepare(3); });
    checkAll<MonoToMany>("mton-8-channels", {1}, 8, [](MonoToMany& p, const Fixture&) { return p.prepare(8); });
    checkAll<ChannelPair>("pairex-4-channels-pair-31", {4}, 2,
        [](ChannelPair& p, const Fixture&) { return p.prepare(4, 3, 1); });
    checkAll<ChannelPair>("pairex-8-channels-pair-53", {8}, 2,
        [](ChannelPair& p, const Fixture&) { return p.prepare(8, 5, 3); });

    {
        ChannelZero p;
        std::vector<double> in(6, 0.1), out(6, 0.0), out12(12, 0.0), shorter(5, 0.0);
        require(!p.process(in, out12), "zero: refuses process before prepare");
        require(!p.prepare(0, 1) && !p.prepare(9, 1) && !p.prepare(2, 3) && !p.prepare(2, 0) && !p.prepare(1, 2),
            "zero: refuses channels outside 1 to 8 and a channel past them");
        require(!p.prepared(), "zero: still not prepared");
        require(p.prepare(1, 1) && p.outputChannels() == 2 && p.inputChannels() == 1, "zero: mono to stereo");
        require(p.process(in, out12), "zero: plays");
        const auto before = p;
        require(!p.process(in, out) && !p.process(in, shorter), "zero: refuses an output not twice the input");
        in[1] = nan;
        require(!p.process(in, out12), "zero: refuses a NaN");
        require(p == before, "zero: refusals change nothing");
        // With 3-frame buffers, the second starts at frame 3: its right is 0.
        std::vector<double> ones(6, 0.5), o(12, 9.0);
        require(p.prepare(1, 1, 3) && p.process(ones, o), "zero: plays 3-frame buffers");
        require(o[1] == 0.0 && o[3] == 0.5 && o[7] == 0.0 && o[9] == 0.5 && o[0] == 0.0 && o[6] == 0.0,
            "zero: each buffer's first frame silent on both sides");
    }
    {
        ChannelMixdown p;
        std::vector<double> in(6, 0.1), out(3, 0.0), wrong(4, 0.0), odd(5, 0.1);
        require(!p.process(in, out), "mixdown: refuses process before prepare");
        require(p.prepare(), "mixdown: prepares");
        const auto before = p;
        require(!p.process(in, wrong) && !p.process(odd, out), "mixdown: refuses sizes that do not fit");
        in[0] = nan;
        require(!p.process(in, out), "mixdown: refuses a NaN");
        require(p == before, "mixdown: refusals change nothing");
    }
    {
        MonoToStereo p;
        std::vector<double> in(3, 0.1), out(6, 0.0), wrong(5, 0.0);
        require(!p.process(in, out), "mono to stereo: refuses process before prepare");
        require(p.prepare() && !p.process(in, wrong), "mono to stereo: refuses an output not twice the input");
    }
    {
        ChannelPhase p;
        std::vector<double> in(6, 0.0), out(6, 1.0);
        require(!p.process(in, out), "phase: refuses process before prepare");
        require(!p.prepare(0, 1) && !p.prepare(9, 1) && !p.prepare(3, 4) && !p.prepare(3, 0),
            "phase: refuses channels outside 1 to 8 and a channel past them");
        require(p.prepare(3, 2) && p.process(in, out), "phase: plays");
        require(!std::signbit(out[0]) && std::signbit(out[1]) && std::signbit(out[4]) && !std::signbit(out[5]),
            "phase: 0 becomes -0 on the inverted channel");
        const auto before = p;
        std::vector<double> five(5, 0.0);
        require(!p.process(five, five), "phase: refuses part frames");
        in[2] = nan;
        require(!p.process(in, out), "phase: refuses a NaN");
        require(p == before, "phase: refusals change nothing");
    }
    {
        MonoToMany p;
        std::vector<double> in(2, 0.1), out(4, 0.0);
        require(!p.process(in, out), "mton: refuses process before prepare");
        require(!p.prepare(1) && !p.prepare(9), "mton: refuses outputs outside 2 to 8");
        require(p.prepare(2) && p.process(in, out) && !p.process(in, std::span<double>(out).first(3)),
            "mton: plays, refuses a short output");
    }
    {
        ChannelPair p;
        std::vector<double> in(8, 0.1), out(4, 0.0);
        require(!p.process(in, out), "pairex: refuses process before prepare");
        require(!p.prepare(2, 1, 2) && !p.prepare(9, 1, 2) && !p.prepare(4, 5, 1) && !p.prepare(4, 1, 5)
                    && !p.prepare(4, 2, 2) && !p.prepare(4, 0, 1),
            "pairex: refuses fewer than 3 or more than 8 channels, a channel past them, or one twice");
        require(!p.prepared(), "pairex: still not prepared");
        require(p.prepare(4, 4, 1) && p.process(in, out), "pairex: plays");
        const auto before = p;
        in[7] = nan;
        require(!p.process(in, out), "pairex: refuses a NaN");
        require(p == before, "pairex: refusals change nothing");
    }
}
}

// Checks StereoPlacement against the CDP8 references.
namespace placement
{
using talas::cdp::StereoPlacement;

void fixture(const std::string& name, int inputs, int channels, std::vector<int> centres, double pregain,
    bool shifted = false)
{
    checkAll<StereoPlacement>(name, std::vector<std::size_t>(static_cast<std::size_t>(inputs), 2),
        static_cast<std::size_t>(channels), [=](StereoPlacement& p, const Fixture&) {
            return p.prepare(inputs, channels, centres, pregain, shifted);
        });
}

void run()
{
    fixture("mchstereo-4-channels-centre-2-pregain-0.8", 1, 4, {2}, 0.8);
    // CDP's command-line centre 2 lands on channel 3.
    fixture("mchstereo-4-channels-number-2-centres-on-3", 1, 4, {3}, 0.8);
    fixture("mchstereo-3-inputs-6-channels-centres-1-3-6-pregain-0.7", 3, 6, {1, 3, 6}, 0.7);
    fixture("mchstereo-2-inputs-2-channels-centres-1-2-pregain-1", 2, 2, {1, 2}, 1.0);
    fixture("mchstereo-2-inputs-5-channels-shifted-centres-5-2-pregain-0.5", 2, 5, {5, 2}, 0.5, true);

    StereoPlacement p;
    const std::vector<int> one{1};
    const std::vector<int> two{1, 2};
    require(!p.prepare(0, 4, {}, 0.5) && !p.prepare(17, 4, std::vector<int>(17, 1), 0.5),
        "placement: refuses inputs outside 1 to 16");
    require(!p.prepare(1, 1, one, 0.5) && !p.prepare(1, 9, one, 0.5), "placement: refuses outputs outside 2 to 8");
    require(!p.prepare(2, 4, one, 0.5), "placement: refuses not one centre for each input");
    require(!p.prepare(1, 4, std::vector<int>{5}, 0.5) && !p.prepare(1, 4, std::vector<int>{0}, 0.5),
        "placement: refuses a centre outside 1 to N");
    require(!p.prepare(1, 4, one, 0.0) && !p.prepare(1, 4, one, 0.0000015) && !p.prepare(1, 4, one, 1.1)
                && !p.prepare(1, 4, one, nan),
        "placement: refuses a pregain of 0 (within 0.000002), over 1 or NaN");
    require(!p.prepared(), "placement: still not prepared");
    require(p.prepare(2, 4, two, 1.0000015) && p.inputs() == 2 && p.outputChannels() == 4,
        "placement: takes 1.0000015 as 1");
    std::vector<double> a(4, 0.1), b(4, 0.2), out(8, 0.0);
    std::vector<std::span<const double>> both{a, b};
    std::vector<std::span<const double>> single{a};
    require(p.process(both, out), "placement: plays");
    const auto before = p;
    require(!p.process(single, out), "placement: refuses the wrong number of inputs");
    require(!p.process(both, std::span<double>(out).first(4)), "placement: refuses sizes that do not match");
    b[1] = nan;
    require(!p.process(both, out), "placement: refuses a NaN");
    require(p == before, "placement: refusals change nothing");
}
}

// Checks Prefix against the CDP8 references.
namespace prefix
{
using talas::cdp::Prefix;

void run()
{
    checkAll<Prefix>("prefix-silence-stereo-0.01", {2}, 2,
        [](Prefix& p, const Fixture& f) { return p.prepare(f.rate, 2, 0.01) && p.delayFrames() == 480; });
    checkAll<Prefix>("prefix-silence-mono-0.0123-rate-44100", {1}, 1,
        [](Prefix& p, const Fixture& f) { return p.prepare(f.rate, 1, 0.0123) && p.delayFrames() == 542; });

    Prefix p;
    require(!p.prepare(0.0, 1, 0.1) && !p.prepare(nan, 1, 0.1), "prefix: refuses a bad rate");
    require(!p.prepare(48000, 0, 0.1) && !p.prepare(48000, 9, 0.1), "prefix: refuses channels outside 1 to 8");
    require(!p.prepare(48000, 1, 0.0) && !p.prepare(48000, 1, 0.0000015), "prefix: refuses a duration of 0");
    require(!p.prepare(48000, 1, -0.1) && !p.prepare(48000, 1, 10.5) && !p.prepare(48000, 1, nan),
        "prefix: refuses a negative, over 10 s or NaN duration");
    require(!p.prepare(48000, 1, 0.000005), "prefix: refuses a duration of no frames");
    require(!p.prepared(), "prefix: still not prepared");
    require(p.prepare(48000, 2, 0.0001) && p.delayFrames() == 5, "prefix: 5 frames");
    std::vector<double> in(20, 0.25), out(20, 0.0), odd(19, 0.0);
    require(p.process(in, out) && out[9] == 0.0 && out[10] == 0.25, "prefix: delays by 5 frames");
    const auto before = p;
    require(!p.process(odd, odd) && !p.process(in, std::span<double>(out).first(18)), "prefix: refuses bad sizes");
    in[0] = nan;
    require(!p.process(in, out), "prefix: refuses a NaN");
    require(p == before, "prefix: refusals change nothing");
}
}

// Checks SilenceMasks and SilenceOverwrite against the CDP8 references.
namespace masks
{
using namespace talas::cdp;

void run()
{
    const auto fixture = [](const std::string& name, int channels, EditUnits units, std::vector<double> times,
                             double splice, std::size_t buffer = 0) {
        checkAll<SilenceMasks>(name, {static_cast<std::size_t>(channels)}, static_cast<std::size_t>(channels),
            [=](SilenceMasks& p, const Fixture& f) { return p.prepare(f.rate, channels, units, times, splice, buffer); });
    };
    fixture("sfedit-masks-1-seconds-mono", 1, EditUnits::Seconds, {0.002, 0.004, 0.0101, 0.0115, 0.015, 0.0193}, 1.03);
    fixture("sfedit-masks-3-frames-stereo-from-0", 2, EditUnits::Frames, {0, 100, 400, 455, 700, 1023}, 1.0208);
    fixture("sfedit-masks-2-samples-3-channels", 3, EditUnits::Samples, {300, 701, 1001, 1300}, 0.5);
    fixture("sfedit-masks-1-stereo-5120-frame-buffers", 2, EditUnits::Seconds, {0.02, 0.03, 0.10685, 0.11}, 2.0, 5120);

    const auto overwrite = [](const std::string& name, int channels, EditUnits units, double time, double duration,
                               double splice) {
        checkAll<SilenceOverwrite>(name, {static_cast<std::size_t>(channels)}, static_cast<std::size_t>(channels),
            [=](SilenceOverwrite& p, const Fixture& f) {
                return p.prepare(f.rate, channels, units, time, duration, splice);
            });
    };
    overwrite("sfedit-insil-1-seconds-mono-overwrite", 1, EditUnits::Seconds, 0.005, 0.006, 1.0);
    overwrite("sfedit-insil-3-frames-stereo-overwrite", 2, EditUnits::Frames, 333, 400, 2.1);
    overwrite("sfedit-insil-2-samples-3-channels-overwrite-silent-end", 3, EditUnits::Samples, 1700, 1200, 0.5);

    require(SilenceMasks::defaultBufferFrames(1) == 1048576 && SilenceMasks::defaultBufferFrames(2) == 524288
                && SilenceMasks::defaultBufferFrames(3) == 349184,
        "masks: CDP's buffers");
    {
        SilenceMasks p;
        const auto ok = [&](std::vector<double> t, double splice = 1.0) {
            return p.prepare(48000, 1, EditUnits::Frames, t, splice);
        };
        require(!p.prepare(0.0, 1, EditUnits::Frames, std::vector<double>{100, 200}),
            "masks: refuses a zero rate");
        require(!p.prepare(48000, 9, EditUnits::Frames, std::vector<double>{100, 200}),
            "masks: refuses 9 channels");
        require(!ok({}) && !ok({100}) && !ok({100, nan}), "masks: refuses no pairs, an odd count or NaN");
        require(!ok({-10, 200}), "masks: refuses a negative start");
        require(!ok({200, 200}) && !ok({200, 100}), "masks: refuses an end not after its start");
        require(!ok({100, 200, 200, 300}), "masks: refuses a start not after the end before");
        require(!ok({10, 200}), "masks: refuses a start within half a splice of 0");
        require(!ok({100, 130}), "masks: refuses a mask too short for its splices");
        require(!ok({100, 200, 230, 400}), "masks: refuses masks too close for their splices");
        require(!ok({100, 200}, 5000.1) && !ok({100, 200}, -1.0), "masks: refuses a splice outside 0 to 5000 ms");
        require(!p.prepare(48000, 1, EditUnits::Frames, std::vector<double>{100, 200}, 1.0, 40),
            "masks: refuses a splice longer than the buffer");
        require(!p.prepared(), "masks: still not prepared");
        require(ok({100, 200}, 1.0208) && p.spliceFrames() == 50, "masks: 49 splice frames go up to 50");
        require(ok({0, 30}) && ok({100, 200}, 0.0) && p.spliceFrames() == 0, "masks: takes a start of 0, a zero splice");
        require(ok({24, 200, 248, 400}), "masks: takes masks just far enough apart");
        std::vector<double> in(6, 0.1), out(6, 0.0);
        require(p.process(in, out), "masks: plays");
        const auto before = p;
        require(!p.process(in, std::span<double>(out).first(5)), "masks: refuses sizes that differ");
        in[3] = nan;
        require(!p.process(in, out), "masks: refuses a NaN");
        require(p == before, "masks: refusals change nothing");
    }
    {
        SilenceOverwrite p;
        require(!p.prepare(48000, 1, EditUnits::Frames, 100, 95, 1.0), "insil: refuses a duration under two splices");
        require(!p.prepare(48000, 1, EditUnits::Frames, 47, 200, 1.0), "insil: refuses a time within a splice of 0");
        require(!p.prepare(48000, 1, EditUnits::Frames, -1, 200, 1.0) && !p.prepare(48000, 1, EditUnits::Frames, 100, nan, 1.0),
            "insil: refuses a negative time or NaN");
        require(!p.prepare(48000, 0, EditUnits::Frames, 100, 200) && !p.prepare(48000, 9, EditUnits::Frames, 100, 200),
            "insil: refuses channels outside 1 to 8");
        require(!p.prepare(48000, 1, EditUnits::Frames, 100, 200, 5001.0), "insil: refuses a splice over 5000 ms");
        require(!p.prepared(), "insil: still not prepared");
        require(p.prepare(48000, 1, EditUnits::Frames, 48, 96, 1.0) && p.spliceFrames() == 48,
            "insil: takes a time of one splice and a duration of two");
        std::vector<double> in(200, 0.5), out(200, 0.0);
        require(p.process(in, out), "insil: plays");
        require(out[0] == static_cast<double>(static_cast<float>(0.5 * (1.0 - 1.0 / 48.0))) && out[48] == 0.0 && out[96] == 0.5 && out[47] < 1e-15,
            "insil: fades out to the time, in from time + duration - 2 splices");
        const auto before = p;
        in[3] = nan;
        require(!p.process(in, out) && !p.process(in, std::span<double>(out).first(5)), "insil: refuses NaN, sizes");
        require(p == before, "insil: refusals change nothing");
    }
}
}

// Checks MergeMany against the CDP8 references.
namespace merge
{
using talas::cdp::MergeMany;

// mixmany's first pass: the largest |sum| of the inputs, in double.
double peakOf(const Fixture& f, std::size_t inputs, std::size_t channels)
{
    double peak = 0.0;
    for (std::size_t n = 0; n < f.frames(); n++)
    {
        for (std::size_t c = 0; c < channels; c++)
        {
            double sum = 0.0;
            for (std::size_t k = 0; k < inputs; k++)
                sum += f.columns[k * channels + c][n];
            peak = std::max(peak, std::fabs(sum));
        }
    }
    return peak;
}

void run()
{
    checkAll<MergeMany>("submix-mergemany-3-mono", {1, 1, 1}, 1,
        [](MergeMany& p, const Fixture& f) { return p.prepare(3, 1, peakOf(f, 3, 1)); });
    checkAll<MergeMany>("submix-mergemany-2-stereo", {2, 2}, 2,
        [](MergeMany& p, const Fixture& f) { return p.prepare(2, 2, peakOf(f, 2, 2)); });

    MergeMany p;
    require(!p.prepare(1, 1) && !p.prepare(17, 1), "merge: refuses inputs outside 2 to 16");
    require(!p.prepare(2, 0) && !p.prepare(2, 9), "merge: refuses channels outside 1 to 8");
    require(!p.prepare(2, 1, 0.0) && !p.prepare(2, 1, -1.0) && !p.prepare(2, 1, nan), "merge: refuses a bad peak");
    require(!p.prepared(), "merge: still not prepared");
    require(p.prepare(2, 2, 0.5) && p.gain() == 2.0, "merge: gain 1 / peak");
    std::vector<double> a(4, 0.1), b(4, 0.2), out(4, 0.0);
    std::vector<std::span<const double>> both{a, b};
    std::vector<std::span<const double>> single{a};
    require(p.process(both, out), "merge: plays");
    const auto before = p;
    require(!p.process(single, out) && !p.process(both, std::span<double>(out).first(2))
                && !p.process(both, std::span<double>(out).first(3)),
        "merge: refuses the wrong inputs or sizes");
    a[0] = nan;
    require(!p.process(both, out), "merge: refuses a NaN");
    require(p == before, "merge: refusals change nothing");
}
}

// Checks EnvelopeWarpStretch against the CDP8 references.
namespace stretch
{
using talas::cdp::EnvelopeWarpStretch;

void run()
{
    constexpr double window = 5.333333;
    const auto fixture = [](const std::string& name, double s) {
        checkAll<EnvelopeWarpStretch>(name, {1}, 1,
            [s](EnvelopeWarpStretch& p, const Fixture& f) { return p.prepare(f.rate, window, s); }, 511);
    };
    fixture("envel-warp-6-timestretch-1.37-ripple", 1.37);
    fixture("envel-warp-6-timestretch-3.3-ripple", 3.3);
    fixture("envel-warp-6-timestretch-1.2-shifted", 1.2);
    fixture("envel-warp-6-timestretch-1", 1.0);
    // CDP8 rescales this one: the reference is its output without the
    // rescale, clipped.
    fixture("envel-warp-6-timestretch-2-overload", 2.0);

    // With too short a memory the stream reads the oldest window it keeps:
    // exact while the lag fits, then not.
    {
        const auto f = load("envel-warp-6-timestretch-3.3-ripple");
        EnvelopeWarpStretch full;
        EnvelopeWarpStretch shortMemory;
        require(full.prepare(f.rate, window, 3.3) && shortMemory.prepare(f.rate, window, 3.3, 0.02),
            "stretch: prepares a 0.02 s memory");
        std::vector<std::vector<double>> a;
        std::vector<std::vector<double>> b;
        require(play(full, f, {1}, 64, a) && play(shortMemory, f, {1}, 64, b), "stretch: plays both");
        bool early = true;
        bool differs = false;
        // 0.02 s at 48 kHz is 4 windows (plus 4 spare, a ring of 8): window
        // m reads back to window ceil(m / 3.3) - 1, within the ring to
        // window m = 10, so the output is exact to window 9.
        for (std::size_t n = 0; n < 8 * 256; n++)
            early = early && std::bit_cast<std::uint64_t>(a[0][n]) == std::bit_cast<std::uint64_t>(b[0][n]);
        for (std::size_t n = 0; n < a[0].size(); n++)
            differs = differs || a[0][n] != b[0][n];
        require(early && differs, "stretch: a short memory is exact at first, then reads its oldest window");
    }

    EnvelopeWarpStretch p;
    require(!p.prepare(0.0, window, 2.0) && !p.prepare(48000, 4.0, 2.0), "stretch: refuses a bad rate or window");
    require(!p.prepare(48000, window, 0.999) && !p.prepare(48000, window, 1000.1) && !p.prepare(48000, window, nan),
        "stretch: refuses a timestretch below 1 or over 1000");
    require(!p.prepare(48000, window, 2.0, -1.0) && !p.prepare(48000, window, 2.0, 3601.0),
        "stretch: refuses a memory outside 0 to 3600 s");
    require(!p.prepare(48000, window, 2.0, 600.0, 0), "stretch: refuses no buffer");
    require(!p.prepared(), "stretch: still not prepared");
    require(p.prepare(48000, window, 1000.0000015) && p.latencyFrames() == 511 && p.windowFrames() == 256,
        "stretch: takes 1000.0000015 as 1000");
    std::vector<double> in(300, 0.25), out(300, 0.0);
    require(p.process(in, out), "stretch: plays");
    const auto before = p;
    in[3] = nan;
    require(!p.process(in, out) && !p.process(in, std::span<double>(out).first(5)), "stretch: refuses NaN, sizes");
    require(p == before, "stretch: refusals change nothing");
}
}
}

int main()
{
    try
    {
        space::run();
        chans::run();
        placement::run();
        prefix::run();
        masks::run();
        merge::run();
        stretch::run();
    }
    catch (const std::exception& error)
    {
        std::printf("FAIL: %s\n", error.what());
        failures++;
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
