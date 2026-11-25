#include "talas/cdp/Antiphony.h"
#include "talas/cdp/Cascade.h"
#include "talas/cdp/ChannelEnvelope.h"
#include "talas/cdp/CrossPan.h"
#include "talas/cdp/FrameRotate.h"
#include "talas/cdp/Phasor.h"
#include "talas/cdp/Spin.h"
#include "talas/cdp/StepSpread.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// CDP's multichannel tools as nodes with N outputs (owner plan 2026-10-12):
// each plays its CDP8 references bit for bit on out1 to outN of its eight
// outputs, the others silent, offline and in 64-frame realtime callbacks
// alike, its channel inputs on in1 to inN; live fields follow Event Values
// where CDP8 reads its breakpoint files; and the nodes refuse what their
// ports and fields refuse.
namespace
{
using Parameter = talas::project::ProjectParameterExpression;
constexpr std::size_t blockFrames = 64;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

Parameter expression(std::string id, std::string text)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

Parameter choice(std::string id, std::string value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.choice = std::move(value);
    return parameter;
}

Parameter flag(std::string id, bool value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.boolean = value;
    return parameter;
}

Parameter text(std::string id, std::string value)
{
    Parameter parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

talas::runtime::EventValueChange change(std::uint64_t frame, std::string name, double value)
{
    talas::runtime::EventValueChange result;
    result.frame = frame;
    result.name = std::move(name);
    result.value = value;
    return result;
}

// A breakpoint table as a reference's header records it, times in frames
// over the rate: tklib3.c's read_value_from_brktable.
struct Table
{
    std::vector<double> times;
    std::vector<double> values;

    [[nodiscard]] double at(double time) const
    {
        if (time <= times.front())
            return values.front();
        if (time >= times.back())
            return values.back();
        std::size_t point = 0;
        while (times[point] < time)
            ++point;
        auto value = (time - times[point - 1]) / (times[point] - times[point - 1]);
        value *= values[point] - values[point - 1];
        return value + values[point - 1];
    }
};

// A CDP8 reference in tests/cdp/fixtures: its columns (each input channel,
// then each CDP output channel), its rate and its breakpoint tables.
struct Reference
{
    std::vector<std::vector<double>> columns;
    double rate = 48000.0;
    std::map<std::string, Table> tables;
};

Reference reference(const std::string& name)
{
    std::ifstream file(std::string{TALAS_CDP_FIXTURE_DIR} + "/" + name + ".txt");
    require(file.good(), "CDP reference " + name + " should be readable");
    Reference result;
    std::string line;
    while (std::getline(file, line))
    {
        if (line.empty())
            continue;
        if (line.front() == '#')
        {
            const auto kilohertz = line.find(" kHz in");
            if (line.rfind("# CDP8", 0) == 0 && kilohertz != std::string::npos)
            {
                const auto space = line.rfind(' ', kilohertz - 1);
                result.rate = std::strtod(line.substr(space + 1, kilohertz - space - 1).c_str(), nullptr) * 1000.0;
            }
            const auto label = line.find(".brk: ");
            if (label != std::string::npos && line.find(" at frame ") != std::string::npos)
            {
                Table table;
                std::istringstream items(line.substr(label + 6));
                std::string item;
                while (std::getline(items, item, '/'))
                {
                    double value = 0.0;
                    double frame = 0.0;
                    char at[8];
                    char word[8];
                    require(std::sscanf(item.c_str(), " %lf %7s %7s %lf", &value, at, word, &frame) == 4,
                        name + " should hold readable breakpoints");
                    table.times.push_back(frame);
                    table.values.push_back(value);
                }
                result.tables[line.substr(2, label - 2)] = table;
            }
            continue;
        }
        std::istringstream words(line);
        std::string word;
        std::size_t column = 0;
        while (words >> word)
        {
            if (result.columns.size() <= column)
                result.columns.emplace_back();
            result.columns[column++].push_back(std::strtod(word.c_str(), nullptr));
        }
    }
    for (auto& [label, table] : result.tables)
    {
        for (auto& time : table.times)
            time /= result.rate;
    }
    require(!result.columns.empty() && !result.columns[0].empty(), "CDP reference " + name + " should hold samples");
    return result;
}

// The node "process" fed by the given input ports, each from its own
// channel of live input (an Audio Input taking that channel alone), its
// eight outputs on an Output8.
talas::project::ProjectDocument graph(
    const std::string& type, std::vector<Parameter> fields, const std::vector<std::string>& inputPorts)
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"process", type, std::move(fields)});
    document.graph.nodes.push_back({"out", "talas.output8", {}});
    for (std::size_t index = 0; index < inputPorts.size(); ++index)
    {
        const auto id = "in" + std::to_string(index + 1);
        std::vector<Parameter> channels{choice("routing", "monoSum")};
        for (std::size_t channel = 1; channel <= 8; ++channel)
            channels.push_back(flag("channel" + std::to_string(channel), channel == index + 1));
        document.graph.nodes.push_back({id, "talas.audioInput", std::move(channels)});
        document.graph.connections.push_back({id, "audioOut", "process", inputPorts[index]});
    }
    for (std::size_t channel = 1; channel <= 8; ++channel)
    {
        const auto port = "out" + std::to_string(channel);
        document.graph.connections.push_back({"process", port, "out", port});
    }
    return document;
}

// The graph's eight output channels offline, after requiring the realtime
// session in callbacks of blockFrames to play them sample for sample.
std::vector<std::vector<double>> play(const talas::project::ProjectDocument& document,
    talas::runtime::RenderSettings settings, std::size_t frames)
{
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), "the multichannel graph should compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto offline = talas::runtime::renderOffline(*compiled.graph, frames);
    require(offline.ok(), "the multichannel graph should render offline: "
            + (offline.diagnostics.empty() ? std::string{} : offline.diagnostics.front().message));
    require(offline.channels.size() == 8, "the multichannel graph should play eight channels offline");

    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready(), "the multichannel graph should prepare for realtime: " + std::string{session.error()});
    std::vector<std::vector<double>> played(8, std::vector<double>(frames));
    std::vector<std::vector<double>> inputs(8, std::vector<double>(frames, 0.0));
    for (const auto& input : settings.audioInputChannels)
    {
        for (std::size_t frame = 0; frame < frames && frame < input.samples.size(); ++frame)
            inputs[input.channel - 1][frame] = input.samples[frame];
    }
    for (std::size_t offset = 0; offset < frames; offset += blockFrames)
    {
        const auto count = std::min(blockFrames, frames - offset);
        std::array<const double*, 8> in{};
        std::array<double*, 8> out{};
        for (std::size_t channel = 0; channel < 8; ++channel)
        {
            in[channel] = inputs[channel].data() + offset;
            out[channel] = played[channel].data() + offset;
        }
        talas::runtime::RealtimeAudioInputBlock block;
        block.channelCount = 8;
        block.doubleChannels = in.data();
        require(session.render(block, out.data(), out.size(), count),
            "the multichannel graph should render in realtime: " + std::string{session.error()});
    }
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        require(std::ranges::equal(played[channel], offline.channels[channel],
                    [](double a, double b) { return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b); }),
            "the multichannel graph should play channel " + std::to_string(channel + 1)
                + " alike offline and in realtime");
    }
    return offline.channels;
}

// A live field CDP reads every `period` frames from frame `first`: the
// Event Value `name` (scaled into the field's unit by the node's
// expression) changes as the block holding each read starts, to the value
// CDP reads there, so that every read takes CDP's value.
void periodic(talas::runtime::RenderSettings& settings, const Table& table, const std::string& name,
    std::size_t first, std::size_t period, std::size_t frames, double rate)
{
    settings.eventValues.push_back({name, table.at(0.0)});
    double current = table.at(0.0);
    std::size_t lastBlock = frames;
    for (auto read = first; read < frames; read += period)
    {
        const auto value = table.at(static_cast<double>(read) / rate);
        const auto block = read / blockFrames * blockFrames;
        if (std::bit_cast<std::uint64_t>(value) == std::bit_cast<std::uint64_t>(current))
            continue;
        require(block != lastBlock, name + ": two reads in one block want different values");
        settings.eventValueChanges.push_back(change(block, name, value));
        current = value;
        lastBlock = block;
    }
}

void sortChanges(talas::runtime::RenderSettings& settings)
{
    std::ranges::stable_sort(settings.eventValueChanges, {}, &talas::runtime::EventValueChange::frame);
}

// The node on a reference's inputs (columns 0 to the inputs), offline and
// in realtime alike: out1 to outN match CDP8's output channels bit for bit
// over the first `compared` frames (all of them by default), the other
// outputs silent throughout.
void matches(const std::string& name, const std::string& type, std::vector<Parameter> fields,
    const std::vector<std::string>& inputPorts, std::size_t outputs,
    const std::function<void(talas::runtime::RenderSettings&, const Reference&)>& live = {},
    std::size_t compared = 0)
{
    const auto cdp = reference(name);
    const auto inputs = inputPorts.size();
    require(cdp.columns.size() == inputs + outputs, name + " should hold its inputs and each output channel");
    const auto frames = cdp.columns[0].size();
    talas::runtime::RenderSettings settings{cdp.rate, blockFrames};
    for (std::size_t channel = 0; channel < inputs; ++channel)
        settings.audioInputChannels.push_back({channel + 1, cdp.columns[channel]});
    if (live)
    {
        live(settings, cdp);
        sortChanges(settings);
    }
    const auto played = play(graph(type, std::move(fields), inputPorts), settings, frames);
    const auto until = compared == 0 ? frames : compared;
    for (std::size_t channel = 0; channel < 8; ++channel)
    {
        const std::span<const double> got{played[channel].data(), until};
        if (channel < outputs)
        {
            const std::span<const double> want{cdp.columns[inputs + channel].data(), until};
            // Sample for sample: the Audio Input that feeds the node sums its
            // channels from 0, so CDP8's -0 input reaches it as 0.
            const auto differs = std::ranges::mismatch(got, want);
            require(differs.in1 == got.end(),
                type + " should match CDP8's " + name + " on out" + std::to_string(channel + 1) + ", not from frame "
                    + std::to_string(differs.in1 - got.begin()) + " ("
                    + (differs.in1 == got.end() ? std::string{} : std::to_string(*differs.in1) + " for "
                        + std::to_string(*differs.in2)) + ")");
        }
        else
        {
            require(std::ranges::all_of(played[channel], [](double sample) {
                return std::bit_cast<std::uint64_t>(sample) == 0;
            }), type + " should keep out" + std::to_string(channel + 1) + " silent on " + name);
        }
    }
}

std::vector<std::string> channelInputs(std::size_t count)
{
    std::vector<std::string> ports;
    for (std::size_t channel = 1; channel <= count; ++channel)
        ports.push_back("in" + std::to_string(channel));
    return ports;
}

const std::vector<std::string> mono{"audioIn"};
const std::vector<std::string> stereo{"audioIn", "rightIn"};

void testRotatingPanPlaysCdp()
{
    const auto fields = [](std::string channels, std::string start, std::string speed, std::string focus,
                            bool anticlockwise) {
        return std::vector<Parameter>{expression("channels", std::move(channels)),
            expression("start", std::move(start)), expression("speed", std::move(speed)),
            expression("focus", std::move(focus)), flag("anticlockwise", anticlockwise)};
    };
    matches("mchanpan-9-rotate-4-channels-speed-20", "talas.cdpRotatingPan", fields("4", "1", "20", "1", false), mono, 4);
    matches("mchanpan-9-rotate-8-channels-start-3.5-speed-30-focus-0.7-anticlockwise", "talas.cdpRotatingPan",
        fields("8", "3.5", "30", "0.7", true), mono, 8);
    matches("mchanpan-9-rotate-6-channels-start-0-speed-64-focus-0-rate-44100", "talas.cdpRotatingPan",
        fields("6", "0", "64", "0", false), mono, 6);
    matches("mchanpan-9-rotate-3-channels-speed-25-focus-0.6", "talas.cdpRotatingPan", fields("3", "2", "25", "0.6", false),
        mono, 3);
    // CDP reads its Speed file every 256 frames.
    const auto speed = [](talas::runtime::RenderSettings& settings, const Reference& cdp) {
        periodic(settings, cdp.tables.at("speed"), "Speed", 0, 256, cdp.columns[0].size(), cdp.rate);
    };
    matches("mchanpan-9-rotate-4-channels-live-speed", "talas.cdpRotatingPan", fields("4", "1", "!Speed", "0.8", false),
        mono, 4, speed);
    matches("mchanpan-9-rotate-4-channels-speed-ramp-anticlockwise", "talas.cdpRotatingPan",
        fields("4", "4", "!Speed", "0.9", true), mono, 4, speed);
}

void testSpreadPanPlaysCdp()
{
    const auto fields = [](std::string channels, std::string centre, std::string spread, std::string depth,
                            std::string rolloff) {
        return std::vector<Parameter>{expression("channels", std::move(channels)),
            expression("centre", std::move(centre)), expression("spread", std::move(spread)),
            expression("depth", std::move(depth)), expression("rolloff", std::move(rolloff))};
    };
    matches("mchanpan-4-spread-8-channels-centre-1-spread-4-depth-2-rolloff-0.5", "talas.cdpSpreadPan",
        fields("8", "1", "4", "2", "0.5"), mono, 8);
    matches("mchanpan-4-spread-6-channels-centre-4-spread-6-depth-1-rolloff-0.3-rate-44100", "talas.cdpSpreadPan",
        fields("6", "4", "6", "1", "0.3"), mono, 6);
    matches("mchanpan-4-spread-stereo-5-channels-centre-5-spread-2.5-depth-1-rolloff-0.7", "talas.cdpSpreadPan",
        fields("5", "5", "2.5", "1", "0.7"), stereo, 5);
    matches("mchanpan-4-spread-stereo-8-channels-centre-3-spread-5.5-depth-2-rolloff-0.2", "talas.cdpSpreadPan",
        fields("8", "3", "5.5", "2", "0.2"), stereo, 8);
    // CDP reads its Spread, Depth and Rolloff files every 256 frames.
    const auto live = [](talas::runtime::RenderSettings& settings, const Reference& cdp) {
        for (const auto& [label, name] : {std::pair{"spread", "Spread"}, std::pair{"depth", "Depth"},
                 std::pair{"rolloff", "Rolloff"}})
        {
            if (cdp.tables.contains(label))
                periodic(settings, cdp.tables.at(label), name, 0, 256, cdp.columns[0].size(), cdp.rate);
        }
    };
    matches("mchanpan-4-spread-4-channels-live-spread-depth-rolloff", "talas.cdpSpreadPan",
        fields("4", "2", "!Spread", "!Depth", "!Rolloff"), mono, 4, live);
    matches("mchanpan-4-spread-stereo-5-channels-ramps", "talas.cdpSpreadPan",
        fields("5", "4", "!Spread", "!Depth", "!Rolloff"), stereo, 5, live);
    matches("mchanpan-4-spread-stereo-4-channels-centre-3-live-spread-keeps-centre", "talas.cdpSpreadPan",
        fields("4", "3", "!Spread", "2", "0.5"), stereo, 4, live);
}

void testTablePanPlaysCdp()
{
    const auto fields = [](std::string channels, std::string table, std::string focus) {
        return std::vector<Parameter>{expression("channels", std::move(channels)), text("table", std::move(table)),
            expression("focus", std::move(focus))};
    };
    matches("mchanpan-1-table-8-channels-rotations-and-direct", "talas.cdpTablePan",
        fields("8", "0 1 1 / 0.008 5.5 1 / 0.016 2 -1 / 0.024 2 0 / 0.03 6 0 / 0.034 7 1", "1"), mono, 8);
    matches("mchanpan-1-table-4-channels-direct-focus-0.7", "talas.cdpTablePan",
        fields("4", "0 1 0\n0.01 3 0\n0.02 4 0\n0.03 1 0", "0.7"), mono, 4);
    matches("mchanpan-1-table-3-channels-first-time-0.004-focus-0.5", "talas.cdpTablePan",
        fields("3", "0.004 1 1 0.015 3 -1 0.03 1.5 1", "0.5"), mono, 3);
    matches("mchanpan-1-table-6-channels-rate-44100", "talas.cdpTablePan",
        fields("6", "0, 0.5, -1; 0.015, 4.25, 1; 0.03, 6, 0", "0.9"), mono, 6);
}

void testStepSpreadPlaysCdp()
{
    const auto fields = [](std::string channels, std::string centre, std::string spread, std::string depth,
                            std::string rolloff, std::string silence, bool stepped) {
        return std::vector<Parameter>{expression("channels", std::move(channels)),
            expression("centre", std::move(centre)), expression("spread", std::move(spread)),
            expression("depth", std::move(depth)), expression("rolloff", std::move(rolloff)),
            expression("minimumSilence", std::move(silence)), flag("stepped", stepped)};
    };
    matches("mchanpan-3-stepspread-6-channels-centre-3-spread-5.5-depth-2-rolloff-0.3", "talas.cdpStepSpread",
        fields("6", "3", "5.5", "2", "0.3", "10 ms", false), mono, 6);
    matches("mchanpan-3-stepspread-6-channels-stepped-centre-4-spread-5-depth-2-rolloff-0.5", "talas.cdpStepSpread",
        fields("6", "4", "5", "2", "0.5", "5 ms", true), mono, 6);
    matches("mchanpan-3-stepspread-8-channels-stepped-spread-6", "talas.cdpStepSpread",
        fields("8", "6", "6", "2", "0.25", "5 ms", true), mono, 8);
    matches("mchanpan-3-stepspread-4-channels-centre-2-spread-3.6-depth-1-rolloff-0.8", "talas.cdpStepSpread",
        fields("4", "2", "3.6", "1", "0.8", "1 ms", false), mono, 4);
    matches("mchanpan-3-stepspread-6-channels-centre-5-spread-6-depth-3-rolloff-1-rate-44100", "talas.cdpStepSpread",
        fields("6", "5", "6", "3", "1", "3 ms", false), mono, 6);
}

void testCrossPanPlaysCdp()
{
    const auto fields = [](std::string table, std::string rolloff, std::string buffer) {
        return std::vector<Parameter>{text("table", std::move(table)), expression("rolloff", std::move(rolloff)),
            expression("bufferFrames", std::move(buffer))};
    };
    matches("mchanpan-7-crosspan-4-channels-rolloff-0.5", "talas.cdpCrossPan",
        fields("0 1 2 3 4 / 0.008 2 3 4 1 / 0.016 0 0 0 0 / 0.024 0 0 0 0 / 0.032 4 3 2 1 / 0.038 4 3 2 1", "0.5", "0"),
        channelInputs(4), 4);
    matches("mchanpan-7-crosspan-8-channels-rolloff-0", "talas.cdpCrossPan",
        fields("0 1 2 3 4 5 6 7 8 / 0.005 1 2 3 4 5 6 7 8 / 0.025 8 7 6 5 4 3 2 1 / 0.04 0 0 0 0 0 0 0 0", "0", "0"),
        channelInputs(8), 8);
    matches("mchanpan-7-crosspan-4-channels-rolloff-0.8-320-frame-buffers", "talas.cdpCrossPan",
        fields("0 3 1 4 2 / 0.012 1 2 3 4 / 0.025 0 0 0 0 / 0.032 2 4 1 3", "0.8", "320"), channelInputs(4), 4);
    matches("mchanpan-7-crosspan-stereo-rolloff-1-rate-44100", "talas.cdpCrossPan",
        fields("0 1 2 / 0.02 2 1 / 0.04 1 2", "1", "0"), channelInputs(2), 2);
}

// CDP's antiphony loop stops a boundary early, its last event before the
// file's end playing on to the end: the frame that switch would start at.
std::size_t antiphonyCompared(const Reference& cdp, double duration, double gap)
{
    const auto frames = cdp.columns[0].size();
    const auto end = static_cast<double>(frames) / cdp.rate;
    double time = 0.0;
    std::size_t last = 0;
    while (time < end)
    {
        time += duration;
        if (time < end)
            last = static_cast<std::size_t>(std::lround(time * cdp.rate) - std::lround(gap * cdp.rate));
    }
    return last > 0 ? last : frames;
}

void testAntiphonyPlaysCdp()
{
    const auto fields = [](std::string channels, std::string sets, std::string duration, std::string gap,
                            std::string splice, std::string buffer) {
        return std::vector<Parameter>{expression("channels", std::move(channels)), text("sets", std::move(sets)),
            expression("eventDuration", std::move(duration)), expression("gap", std::move(gap)),
            expression("splice", std::move(splice)), expression("bufferFrames", std::move(buffer))};
    };
    const auto antiphony = [&](const std::string& name, std::string channels, std::string sets, double duration,
                               double gap, std::string splice, std::string buffer,
                               const std::vector<std::string>& ports, std::size_t outputs) {
        const auto cdp = reference(name);
        matches(name, "talas.cdpAntiphony",
            fields(std::move(channels), std::move(sets), std::to_string(duration) + " s", std::to_string(gap) + " s",
                std::move(splice), std::move(buffer)),
            ports, outputs, {}, antiphonyCompared(cdp, duration, gap));
    };
    antiphony("mchanpan-6-antiphony-4-channels-ab-cd-event-0.06-gap-0-splice-5-rate-8000", "4", "ab-cd", 0.06, 0.0,
        "5 ms", "0", mono, 4);
    antiphony("mchanpan-6-antiphony-8-channels-abcd-efgh-event-0.07-gap-0.01-splice-3-rate-8000", "8", "abcd-efgh",
        0.07, 0.01, "3 ms", "0", mono, 8);
    antiphony("mchanpan-6-antiphony-4-channels-d-a-512-frame-buffers-rate-8000", "4", "d-a", 0.055, 0.002, "2 ms",
        "512", mono, 4);
    antiphony("mchanpan-6-antiphony-stereo-8-channels-abcd-efgh-event-0.05-gap-0-splice-10-rate-8000", "8", "abcd-efgh",
        0.05, 0.0, "10 ms", "0", stereo, 8);
    antiphony("mchanpan-6-antiphony-two-inputs-6-channels-fac-eb-event-0.05-gap-0.005-splice-10-rate-8000", "6",
        "fac-eb", 0.05, 0.005, "10 ms", "0", {"audioIn", "sideChain"}, 6);
    antiphony("mchanpan-6-antiphony-4-channels-ab-dc-event-0.05-gap-0.003-splice-0", "4", "ab-dc", 0.05, 0.003, "0 ms",
        "0", mono, 4);
}

void testMultichannelStadiumPlaysCdp()
{
    const auto fields = [](std::string channels, std::string gain, std::string rolloff, std::string size,
                            std::string count, std::string centre, std::string spread, std::string buffer) {
        return std::vector<Parameter>{expression("channels", std::move(channels)), expression("gain", std::move(gain)),
            expression("rolloff", std::move(rolloff)), expression("size", std::move(size)),
            expression("count", std::move(count)), expression("centre", std::move(centre)),
            expression("spread", std::move(spread)), expression("bufferFrames", std::move(buffer))};
    };
    matches("mchanrev-4-channels-count-23-size-0.1-centre-1-spread-2", "talas.cdpMultichannelStadium",
        fields("4", "0.645654", "1", "0.1", "23", "1", "2", "0"), mono, 4);
    matches("mchanrev-8-channels-count-30-size-0.05-centre-3.5-spread-6-rolloff-0.5", "talas.cdpMultichannelStadium",
        fields("8", "0.8", "0.5", "0.05", "30", "3.5", "6", "0"), mono, 8);
    matches("mchanrev-4-channels-count-10-size-0.05-512-frame-buffers", "talas.cdpMultichannelStadium",
        fields("4", "0.9", "0.7", "0.05", "10", "2.25", "3", "512"), mono, 4);
    matches("mchanrev-6-channels-count-1000-size-0.003-centre-6-spread-6-rolloff-0.3-rate-44100",
        "talas.cdpMultichannelStadium", fields("6", "0.7", "0.3", "0.003", "1000", "6", "6", "0"), mono, 6);
    matches("mchanrev-2-channels-count-2-size-0.2-centre-0-spread-2", "talas.cdpMultichannelStadium",
        fields("2", "1", "1", "0.2", "2", "0", "2", "0"), mono, 2);
}

void testFlutterPlaysCdp()
{
    const auto fields = [](std::string sets, std::string frequency, std::string depth, std::string gain, bool random,
                            std::string buffer) {
        return std::vector<Parameter>{text("sets", std::move(sets)), expression("frequency", std::move(frequency)),
            expression("depth", std::move(depth)), expression("gain", std::move(gain)), flag("random", random),
            expression("bufferFrames", std::move(buffer))};
    };
    matches("flutter-4-channels-sets-12-3-4-freq-60-depth-1-gain-1", "talas.cdpFlutter",
        fields("1 2 / 3 / 4", "60 hz", "1", "1", false, "0"), channelInputs(4), 4);
    matches("flutter-8-channels-pairs-freq-120-depth-0.6-gain-0.8-random", "talas.cdpFlutter",
        fields("1 5 / 2 6 / 3 7 / 4 8", "120 hz", "0.6", "0.8", true, "0"), channelInputs(8), 8);
    // No sets: each channel its own.
    matches("flutter-mono-freq-30-depth-3-gain-1.5", "talas.cdpFlutter", fields("", "30 hz", "3", "1.5", false, "0"),
        channelInputs(1), 1);
    matches("flutter-stereo-empty-and-doubled-sets-freq-90-depth-0.9-gain-0.5-rate-44100", "talas.cdpFlutter",
        fields("1 / / 2 2 / 1 2", "90 hz", "0.9", "0.5", false, "0"), channelInputs(2), 2);
    // CDP reads Frequency and Depth every frame; their steps fall where
    // blocks start.
    const auto live = [](talas::runtime::RenderSettings& settings, const Reference& cdp) {
        for (const auto& [label, name] : {std::pair{"freq", "Frequency"}, std::pair{"depth", "Depth"}})
        {
            if (cdp.tables.contains(label))
                periodic(settings, cdp.tables.at(label), name, 0, 1, cdp.columns[0].size(), cdp.rate);
        }
    };
    matches("flutter-4-channels-live-freq-depth-640-frame-buffers", "talas.cdpFlutter",
        fields("1 / 2 / 3 / 4 / 1 2 3 4", "!Frequency * (1 hz)", "!Depth", "1.2", false, "640"), channelInputs(4), 4,
        live);
    matches("flutter-3-channels-random-live-freq", "talas.cdpFlutter",
        fields("", "!Frequency * (1 hz)", "0.8", "1", true, "0"), channelInputs(3), 3, live);
}

// get_first_rotation_orientation over a breakpoint table: the first
// nonzero value in the first half of it.
std::string firstDirection(const Reference& cdp, const std::string& label)
{
    const auto& values = cdp.tables.at(label).values;
    for (std::size_t j = 1; j < values.size(); j += 2)
    {
        if (values[j / 2] > 0.0)
            return "1";
        if (values[j / 2] < 0.0)
            return "-1";
    }
    return "0";
}

void testFrameRotatePlaysCdp()
{
    const auto fields = [](std::string mode, std::string rotation, std::string second, std::string smear,
                            std::string direction, std::string secondDirection, std::string snake) {
        return std::vector<Parameter>{choice("mode", std::move(mode)), expression("rotation", std::move(rotation)),
            expression("secondRotation", std::move(second)), expression("smear", std::move(smear)),
            expression("firstDirection", std::move(direction)),
            expression("secondFirstDirection", std::move(secondDirection)), text("snake", std::move(snake))};
    };
    matches("frame-1-rotate-4-channels-rotation-10", "talas.cdpFrameRotate",
        fields("oneRotation", "10", "0", "0", "0", "0", ""), channelInputs(4), 4);
    matches("frame-1-rotate-8-channels-rotation-minus-20-smear-0.2", "talas.cdpFrameRotate",
        fields("oneRotation", "-20", "0", "0.2", "0", "0", ""), channelInputs(8), 8);
    matches("frame-1-rotate-6-channels-rotation-500-smear-0.45-rate-44100", "talas.cdpFrameRotate",
        fields("oneRotation", "500", "0", "0.45", "0", "0", ""), channelInputs(6), 6);
    matches("frame-2-rotate-6-channels-rotations-20-minus-30-smear-0.1", "talas.cdpFrameRotate",
        fields("twoRotations", "20", "-30", "0.1", "0", "0", ""), channelInputs(6), 6);
    matches("frame-2-rotate-4-channels-rotations-0-40", "talas.cdpFrameRotate",
        fields("twoRotations", "0", "40", "0", "0", "0", ""), channelInputs(4), 4);
    // CDP8 changes this snake at frame 1536; the node keeps its first.
    matches("frame-1-rotate-6-channels-snake-rotation-30", "talas.cdpFrameRotate",
        fields("oneRotation", "30", "0", "0", "0", "0", "1 3 5 2 4 6"), channelInputs(6), 6, {}, 1536);
    // CDP reads its Rotation files every round(sr / 500) frames, from the
    // first such frame; its first directions look ahead in the files.
    const auto read = [](const std::vector<std::pair<std::string, std::string>>& tables) {
        return [tables](talas::runtime::RenderSettings& settings, const Reference& cdp) {
            const auto period = talas::cdp::FrameRotate::readFrames(cdp.rate);
            for (const auto& [label, name] : tables)
                periodic(settings, cdp.tables.at(label), name, period, period, cdp.columns[0].size(), cdp.rate);
        };
    };
    for (const auto& [name, smear] : {
             std::pair{std::string{"frame-1-rotate-4-channels-live-rotation"}, std::string{"0"}},
             std::pair{std::string{"frame-1-rotate-4-channels-live-rotation-starts-still-set-up-clockwise"},
                 std::string{"0.05"}},
             std::pair{std::string{"frame-1-rotate-4-channels-live-rotation-starts-still-set-up-anticlockwise"},
                 std::string{"0"}},
         })
    {
        const auto cdp = reference(name);
        matches(name, "talas.cdpFrameRotate",
            fields("oneRotation", "!Rotation", "0", smear, firstDirection(cdp, "rotation"), "0", ""),
            channelInputs(4), 4, read({{"rotation", "Rotation"}}));
    }
    {
        const auto name = std::string{"frame-2-rotate-6-channels-live-rotations-snake"};
        const auto cdp = reference(name);
        matches(name, "talas.cdpFrameRotate",
            fields("twoRotations", "!Rotation", "!Second", "0", firstDirection(cdp, "rotation1"),
                firstDirection(cdp, "rotation2"), "2 4 6 1 3 5"),
            channelInputs(6), 6, read({{"rotation1", "Rotation"}, {"rotation2", "Second"}}));
    }
}

void testFrameRemapPlaysCdp()
{
    const auto fields = [](std::string mode, std::string route, std::string plane, bool toRing, std::string first,
                            std::string second) {
        return std::vector<Parameter>{choice("mode", std::move(mode)), text("route", std::move(route)),
            expression("plane", std::move(plane)), flag("toRing", toRing), expression("first", std::move(first)),
            expression("second", std::move(second))};
    };
    matches("frame-3-reorient-4-channels-4-1-2-3", "talas.cdpFrameRemap", fields("reorient", "4 1 2 3", "1", false, "1", "2"),
        channelInputs(4), 4);
    matches("frame-3-reorient-6-channels-3-6-1-5-2-4", "talas.cdpFrameRemap",
        fields("reorient", "3 6 1 5 2 4", "1", false, "1", "2"), channelInputs(6), 6);
    matches("frame-4-mirror-4-channels-plane-2", "talas.cdpFrameRemap", fields("mirror", "", "2", false, "1", "2"),
        channelInputs(4), 4);
    matches("frame-4-mirror-7-channels-plane-3.5", "talas.cdpFrameRemap", fields("mirror", "", "3.5", false, "1", "2"),
        channelInputs(7), 7);
    matches("frame-4-mirror-5-channels-plane-5.5", "talas.cdpFrameRemap", fields("mirror", "", "5.5", false, "1", "2"),
        channelInputs(5), 5);
    matches("frame-4-mirror-6-channels-plane-0", "talas.cdpFrameRemap", fields("mirror", "", "0", false, "1", "2"),
        channelInputs(6), 6);
    matches("frame-5-bilateral-7-channels", "talas.cdpFrameRemap", fields("bilateral", "", "1", false, "1", "2"),
        channelInputs(7), 7);
    matches("frame-5-bilateral-6-channels-to-ring", "talas.cdpFrameRemap", fields("bilateral", "", "1", true, "1", "2"),
        channelInputs(6), 6);
    matches("frame-6-swap-4-channels-2-4", "talas.cdpFrameRemap", fields("swap", "", "1", false, "2", "4"),
        channelInputs(4), 4);
    matches("frame-8-beast-8-channels", "talas.cdpFrameRemap", fields("beastBilateral", "", "1", false, "1", "2"),
        channelInputs(8), 8);
    matches("frame-8-beast-5-channels-to-ring", "talas.cdpFrameRemap", fields("beastBilateral", "", "1", true, "1", "2"),
        channelInputs(5), 5);
}

void testChannelEnvelopeAndToStereoPlayCdp()
{
    matches("frame-7-envchans-4-channels-channel-2-gain-0.5", "talas.cdpChannelEnvelope",
        {text("enveloped", "2"), expression("gain", "0.5")}, channelInputs(4), 4);
    const auto fields = [](std::string channels, std::string start, std::string end, std::string left,
                            std::string right, std::string mix) {
        return std::vector<Parameter>{expression("channels", std::move(channels)), expression("start", std::move(start)),
            expression("end", std::move(end)), expression("left", std::move(left)),
            expression("right", std::move(right)), expression("mixLevel", std::move(mix))};
    };
    matches("tostereo-start-0.005-end-0.03", "talas.cdpToStereo", fields("2", "0.005 s", "0.03 s", "0", "0", "0"), stereo,
        2);
    matches("tostereo-start-0.035-end-0.01-mix-0.707", "talas.cdpToStereo",
        fields("2", "0.035 s", "0.01 s", "0", "0", "0.707"), stereo, 2);
    matches("tostereo-fork-8-channels-left-3-start-0.008-end-0.025", "talas.cdpToStereo",
        fields("8", "0.008 s", "0.025 s", "3", "0", "0"), stereo, 8);
    matches("tostereo-fork-4-channels-left-1-start-0.025-end-0-mix-0.9", "talas.cdpToStereo",
        fields("4", "0.025 s", "0 s", "1", "0", "0.9"), stereo, 4);
    matches("tostereo-6-channels-left-5-right-2-start-0-end-0.04-mix-1-rate-44100", "talas.cdpToStereo",
        fields("6", "0 s", "0.04 s", "5", "2", "1"), stereo, 6);
}

// Two tones and a pulse train of silences, as the node's channels.
std::vector<std::vector<double>> tones(std::size_t channels, std::size_t frames)
{
    std::vector<std::vector<double>> result(channels, std::vector<double>(frames));
    for (std::size_t channel = 0; channel < channels; ++channel)
    {
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto gate = (frame / 700) % 2 == 0 ? 1.0 : 0.0;
            result[channel][frame] = static_cast<double>(static_cast<float>(
                gate * 0.5 * std::sin(0.01 * static_cast<double>((channel + 1) * frame))));
        }
    }
    return result;
}

// The port on the same channels, the setter called as the block at frame
// `at` starts, its channels padded to eight with silence.
template <typename Port>
std::vector<std::vector<double>> ported(Port& port, const std::vector<std::vector<double>>& inputs,
    const std::vector<std::vector<double>>& side, std::size_t at, const std::function<bool(Port&)>& set)
{
    const auto frames = inputs[0].size();
    const auto interleave = [&](const std::vector<std::vector<double>>& channels) {
        std::vector<double> result(frames * channels.size());
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            for (std::size_t channel = 0; channel < channels.size(); ++channel)
                result[frame * channels.size() + channel] = channels[channel][frame];
        }
        return result;
    };
    const auto in = interleave(inputs);
    const auto sideIn = side.empty() ? std::vector<double>{} : interleave(side);
    const auto channels = port.outputChannels();
    std::vector<double> out(frames * channels);
    const auto run = [&](std::size_t from, std::size_t to) {
        const auto input = std::span<const double>{in}.subspan(from * inputs.size(), (to - from) * inputs.size());
        const auto output = std::span<double>{out}.subspan(from * channels, (to - from) * channels);
        if constexpr (std::is_same_v<Port, talas::cdp::Antiphony>)
        {
            return side.empty() ? port.process(input, output)
                                : port.process(input,
                                    std::span<const double>{sideIn}.subspan(from * side.size(), (to - from) * side.size()),
                                    output);
        }
        else
            return port.process(input, output);
    };
    require(run(0, at) && set(port) && run(at, frames), "the port should play");
    std::vector<std::vector<double>> result(8, std::vector<double>(frames, 0.0));
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        for (std::size_t channel = 0; channel < channels; ++channel)
            result[channel][frame] = out[frame * channels + channel];
    }
    return result;
}

// The node on the given channels, offline and in realtime alike.
std::vector<std::vector<double>> node(const std::string& type, std::vector<Parameter> fields,
    const std::vector<std::string>& ports, const std::vector<std::vector<double>>& inputs,
    talas::runtime::RenderSettings settings)
{
    for (std::size_t channel = 0; channel < inputs.size(); ++channel)
        settings.audioInputChannels.push_back({channel + 1, inputs[channel]});
    return play(graph(type, std::move(fields), ports), settings, inputs[0].size());
}

talas::runtime::RenderSettings withChange(std::string name, double from, std::uint64_t frame, double to)
{
    talas::runtime::RenderSettings settings{48000.0, blockFrames};
    settings.eventValues.push_back({name, from});
    settings.eventValueChanges.push_back(change(frame, name, to));
    return settings;
}

// Live fields CDP8's references cannot show a block at a time (stepspread
// reads at buffer-relative times, antiphony at its schedule's times,
// envchans steps its gain every frame): the node plays them as its port
// does with the setter called as the block holding the change starts, and
// the corrected modes as their ports do.
void testLiveFieldsPlayTheirPorts()
{
    constexpr std::size_t frames = 4096;
    {
        const auto inputs = tones(1, frames);
        talas::cdp::StepSpread port;
        require(port.prepare(48000.0, 6, 2.0, 3.0, 1.0, 0.25, 5.0), "the stepspread port should prepare");
        const auto expected = ported<talas::cdp::StepSpread>(port, inputs, {}, 1024, [](talas::cdp::StepSpread& played) {
            return played.setCentre(4.5) && played.setSpread(5.0) && played.setDepth(2.5) && played.setRolloff(0.75);
        });
        auto settings = withChange("X", 0.0, 1024, 1.0);
        require(node("talas.cdpStepSpread",
                    {expression("channels", "6"), expression("centre", "2 + (2.5 * !X)"),
                        expression("spread", "3 + (2 * !X)"), expression("depth", "1 + (1.5 * !X)"),
                        expression("rolloff", "0.25 + (0.5 * !X)"), expression("minimumSilence", "5 ms")},
                    mono, inputs, settings)
                == expected,
            "CDP Step Spread should take its live centre, spread, depth and rolloff as its port does");
    }
    {
        const auto inputs = tones(1, frames);
        const std::vector<std::vector<double>> side{tones(2, frames)[1]};
        talas::cdp::Antiphony port;
        require(port.prepare(48000.0, 1, 5, std::vector<int>{1, 3}, std::vector<int>{5, 2}, 0.08, 0.0, 4.0, true),
            "the antiphony port should prepare");
        const auto expected = ported<talas::cdp::Antiphony>(port, inputs, side, 1536, [](talas::cdp::Antiphony& played) {
            return played.setEventDuration(0.05) && played.setGap(0.01);
        });
        auto settings = withChange("X", 0.0, 1536, 1.0);
        settings.audioInputChannels.push_back({2, side[0]});
        require(node("talas.cdpAntiphony",
                    {expression("channels", "5"), text("sets", "ac-eb"),
                        expression("eventDuration", "0.08 s - (0.03 s * !X)"), expression("gap", "0.01 s * !X"),
                        expression("splice", "4 ms")},
                    {"audioIn", "sideChain"}, inputs, settings)
                == expected,
            "CDP Antiphony should take its live event duration and gap as its port does, alternating with its side chain");
    }
    {
        const auto inputs = tones(4, frames);
        talas::cdp::ChannelEnvelope port;
        require(port.prepare(48000.0, 4, std::vector<int>{1, 3}, 0.8), "the envchans port should prepare");
        const auto expected = ported<talas::cdp::ChannelEnvelope>(
            port, inputs, {}, 2048, [](talas::cdp::ChannelEnvelope& played) { return played.setGain(0.2); });
        require(node("talas.cdpChannelEnvelope", {text("enveloped", "1 3"), expression("gain", "0.8 - (0.6 * !X)")},
                    channelInputs(4), inputs, withChange("X", 0.0, 2048, 1.0))
                == expected,
            "CDP Channel Envelope should hold its live gain a block at a time as its port does");
    }
    for (const auto corrected : {false, true})
    {
        const auto inputs = tones(4, frames);
        talas::cdp::CrossPan port;
        std::vector<talas::cdp::CrossPan::Point> table(2);
        table[0].time = 0.01;
        table[0].map = {2, 3, 4, 1};
        table[1].time = 0.05;
        table[1].map = {4, 3, 2, 1};
        // CDP8 refuses a table starting after 0; corrected holds its first
        // map from 0.
        require(port.prepare(48000.0, 4, table, 0.5, 0, true) == true && port.corrected(),
            "the corrected crosspan port should prepare");
        if (!corrected)
        {
            require(!port.prepare(48000.0, 4, table, 0.5), "the faithful crosspan port should refuse a late table");
            table[0].time = 0.0;
            require(port.prepare(48000.0, 4, table, 0.5, 512), "the faithful crosspan port should prepare");
        }
        const auto expected = ported<talas::cdp::CrossPan>(port, inputs, {}, 0, [](talas::cdp::CrossPan&) { return true; });
        require(node("talas.cdpCrossPan",
                    {text("table", corrected ? "0.01 2 3 4 1 / 0.05 4 3 2 1" : "0 2 3 4 1 / 0.05 4 3 2 1"),
                        expression("rolloff", "0.5"), expression("bufferFrames", corrected ? "0" : "512"),
                        flag("corrected", corrected)},
                    channelInputs(4), inputs, {48000.0, blockFrames})
                == expected,
            corrected ? "CDP Cross Pan's corrected mode should play as its corrected port"
                      : "CDP Cross Pan should hold a map as CDP8 does, its buffer a field");
    }
    {
        const auto inputs = tones(2, frames);
        talas::cdp::Antiphony port;
        require(port.prepare(48000.0, 2, 6, std::vector<int>{1, 2, 3, 4}, std::vector<int>{5, 6}, 0.05, 0.0, 2.0, false,
                    0, true),
            "the corrected antiphony port should prepare");
        const auto expected = ported<talas::cdp::Antiphony>(port, inputs, {}, 0, [](talas::cdp::Antiphony&) { return true; });
        require(node("talas.cdpAntiphony",
                    {expression("channels", "6"), text("sets", "abcd-ef"), expression("eventDuration", "0.05 s"),
                        expression("splice", "2 ms"), flag("corrected", true)},
                    stereo, inputs, {48000.0, blockFrames})
                == expected,
            "CDP Antiphony's corrected mode should play every channel of each set as its corrected port");
    }
    {
        // With one rotation, a live second rotation is ignored, offline and
        // in realtime alike.
        const auto inputs = tones(4, frames);
        talas::cdp::FrameRotate port;
        require(port.prepare(48000.0, 4, 3.0), "the frame rotate port should prepare");
        const auto expected = ported<talas::cdp::FrameRotate>(port, inputs, {}, 0, [](talas::cdp::FrameRotate&) { return true; });
        require(node("talas.cdpFrameRotate",
                    {expression("rotation", "3"), expression("secondRotation", "!X * 50")}, channelInputs(4), inputs,
                    withChange("X", 0.0, 1024, 1.0))
                == expected,
            "CDP Frame Rotate should ignore a live second rotation with one rotation");
    }
}

bool fails(const talas::project::ProjectDocument& document)
{
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), {48000.0, 64});
    if (!compiled.ok())
        return true;
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    return !talas::runtime::renderOffline(*compiled.graph, 1024).ok() && !session.ready();
}

// The node's left and right outputs on an Audio Output, offline.
std::pair<std::vector<double>, std::vector<double>> leftAndRight(const std::string& type, std::vector<Parameter> fields,
    const std::vector<std::string>& inputPorts, const std::vector<std::vector<double>>& inputs)
{
    auto document = graph(type, std::move(fields), inputPorts);
    std::erase_if(document.graph.connections, [](const talas::core::Connection& connection) {
        return connection.fromNode == "process";
    });
    std::erase_if(document.graph.nodes, [](const talas::project::ProjectNode& node) { return node.instanceId == "out"; });
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.connections.push_back({"process", "leftOut", "out", "audioIn"});
    document.graph.connections.push_back({"process", "rightOut", "out", "rightIn"});
    talas::runtime::RenderSettings settings{48000.0, blockFrames};
    for (std::size_t channel = 0; channel < inputs.size(); ++channel)
        settings.audioInputChannels.push_back({channel + 1, inputs[channel]});
    const auto compiled = talas::runtime::compileProject(document, talas::core::makeBuiltinNodeRegistry(), settings);
    require(compiled.ok(), type + " should compile with its left and right outputs");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, inputs[0].size());
    require(offline.ok(), type + " should render its left and right outputs");
    return {offline.mono, offline.right.empty() ? offline.mono : offline.right};
}

// A port's interleaved channels, padded to eight with silence.
std::vector<std::vector<double>> deinterleaved(const std::vector<double>& interleaved, std::size_t channels)
{
    const auto frames = interleaved.size() / channels;
    std::vector<std::vector<double>> result(8, std::vector<double>(frames, 0.0));
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        for (std::size_t channel = 0; channel < channels; ++channel)
            result[channel][frame] = interleaved[frame * channels + channel];
    }
    return result;
}

bool silent(const std::vector<double>& samples)
{
    return std::ranges::all_of(samples, [](double sample) { return std::bit_cast<std::uint64_t>(sample) == 0; });
}

// Cascade's modes 4 and 5, phasor's channels past two and its surround,
// and spin's wide modes play CDP's channels on out1 to outN as their ports
// do (whose CDP8 references the processor tests play), the left and right
// outputs silent; with one or two channels left and right play as before,
// out1 and out2 alike.
void testWiredPortsPlayTheirChannels()
{
    constexpr std::size_t frames = 8192;
    const auto inputs = tones(2, frames);
    for (const auto& [choice_, mode] : {std::pair{std::string{"rotate"}, talas::cdp::Cascade::Mode::Rotate},
             std::pair{std::string{"rotateBack"}, talas::cdp::Cascade::Mode::RotateBack}})
    {
        talas::cdp::Cascade::Settings settings;
        settings.mode = mode;
        settings.clipSize = 0.02;
        settings.echoes = 4;
        talas::cdp::Cascade port;
        require(port.prepare(48000.0, settings) && port.outputChannels() == 8, "the cascade port should prepare");
        std::vector<double> out(frames * 8);
        require(port.process(inputs[0], out), "the cascade port should play");
        const std::vector<Parameter> fields{choice("mode", choice_), expression("clipSize", "0.02 s"),
            expression("echoes", "4")};
        require(node("talas.cdpCascade", fields, mono, {inputs[0]}, {48000.0, blockFrames}) == deinterleaved(out, 8),
            "CDP Cascade's " + choice_ + " should play its eight channels on out1 to out8 as its port does");
        const auto [left, right] = leftAndRight("talas.cdpCascade", fields, mono, {inputs[0]});
        require(silent(left) && silent(right), "CDP Cascade's " + choice_ + " should leave left and right silent");
    }
    {
        talas::cdp::Phasor port;
        require(port.prepare(48000.0, 5, 40.0, 0.5, 4, 0.0, true) && port.outputChannels() == 4,
            "the surround phasor port should prepare");
        std::vector<double> out(frames * 4);
        require(port.process(inputs[0], out), "the phasor port should play");
        const std::vector<Parameter> fields{expression("streams", "5"), expression("frequency", "40 hz"),
            expression("shift", "0.5"), expression("outputChannels", "4"), flag("surround", true)};
        const auto played = node("talas.cdpPhasor", fields, mono, {inputs[0]}, {48000.0, blockFrames});
        require(played == deinterleaved(out, 4) && !silent(played[3]),
            "CDP Phasor should play four surround channels on out1 to out4 as its port does");
        const auto [left, right] = leftAndRight("talas.cdpPhasor", fields, mono, {inputs[0]});
        require(silent(left) && silent(right), "CDP Phasor should leave left and right silent with four channels");
    }
    {
        const std::vector<Parameter> fields{expression("frequency", "40 hz")};
        const auto played = node("talas.cdpPhasor", fields, mono, {inputs[0]}, {48000.0, blockFrames});
        const auto [left, right] = leftAndRight("talas.cdpPhasor", fields, mono, {inputs[0]});
        require(left == played[0] && right == played[1] && left != right && silent(played[2]),
            "CDP Phasor's two channels should play on left and right as before and on out1 and out2");
    }
    {
        talas::cdp::Spin port;
        require(port.prepare(48000.0, talas::cdp::Spin::Mode::Wide, 2.0, 1.0, 0.3, 6, 3, 0.1, 0.7)
                && port.outputChannels() == 6 && port.latencyFrames() == 1,
            "the wide spin port should prepare, a frame late");
        std::vector<double> out(frames * 6);
        require(port.process(inputs[0], inputs[1], out), "the spin port should play");
        const std::vector<Parameter> fields{choice("mode", "wide"), expression("rate", "2"), expression("boost", "1"),
            expression("attenuation", "0.3"), expression("channels", "6"), expression("centre", "3"),
            expression("centreMinimum", "0.1"), expression("centreMaximum", "0.7")};
        require(node("talas.cdpSpin", fields, stereo, inputs, {48000.0, blockFrames}) == deinterleaved(out, 6),
            "CDP Spin's wide mode should play its six channels on out1 to out6 as its port does");
        const auto [left, right] = leftAndRight("talas.cdpSpin", fields, stereo, inputs);
        require(silent(left) && silent(right), "CDP Spin's wide mode should leave left and right silent");
    }
    {
        const auto at = std::size_t{2048};
        auto expected = std::vector<double>(frames * 8);
        {
            talas::cdp::Spin fresh;
            require(fresh.prepare(48000.0, talas::cdp::Spin::Mode::WideBareCentre, -3.0, 1.2, 0.0, 8, 8, 0.2)
                    && fresh.process(std::span{inputs[0]}.first(at), std::span{inputs[1]}.first(at),
                        std::span{expected}.first(at * 8))
                    && fresh.setRate(5.0)
                    && fresh.process(std::span{inputs[0]}.subspan(at), std::span{inputs[1]}.subspan(at),
                        std::span{expected}.subspan(at * 8)),
                "the spin port should play a rate change");
        }
        const std::vector<Parameter> fields{choice("mode", "wideBareCentre"), expression("rate", "-3 + (8 * !X)"),
            expression("boost", "1.2"), expression("channels", "8"), expression("centre", "8"),
            expression("centreMinimum", "0.2")};
        require(node("talas.cdpSpin", fields, stereo, inputs, withChange("X", 0.0, at, 1.0)) == deinterleaved(expected, 8),
            "CDP Spin's bare-centre mode should follow a live rate on out1 to out8 as its port does");
    }
    {
        const std::vector<Parameter> fields{expression("rate", "3")};
        const auto played = node("talas.cdpSpin", fields, stereo, inputs, {48000.0, blockFrames});
        const auto [left, right] = leftAndRight("talas.cdpSpin", fields, stereo, inputs);
        require(left == played[0] && right == played[1] && silent(played[2]),
            "CDP Spin's mode 1 should play on left and right as before and on out1 and out2");
    }
    require(fails(graph("talas.cdpSpin", {choice("mode", "wide"), expression("channels", "3")}, stereo))
            && fails(graph("talas.cdpSpin", {choice("mode", "wide"), expression("centre", "9")}, stereo))
            && fails(graph("talas.cdpSpin", {choice("mode", "wide"), expression("centreMinimum", "0.8")}, stereo))
            && fails(graph("talas.cdpSpin", {expression("channels", "!Channels")}, stereo))
            && fails(graph("talas.cdpPhasor", {expression("streams", "8"), expression("outputChannels", "9")}, mono))
            && fails(graph("talas.cdpPhasor", {flag("surround", true)}, mono)),
        "CDP Spin should refuse wide channels under 4, a centre past them, a minimum over the maximum and live channels, and CDP Phasor 9 channels and surround on two");
}

// CDP8's channel and edit tools (housekeep chans, chanphase, mton, pairex,
// mchstereo, modify space 2 and 4, prefix, sfedit masks and insil -o), as
// nodes on out1 to outN.
void testChannelToolsPlayCdp()
{
    matches("modify-space-2-mirror", "talas.cdpStereoSpace", {choice("mode", "mirror")}, stereo, 2);
    for (const auto& [name, narrowing] : {
             std::pair{std::string{"modify-space-4-narrow-0.3"}, std::string{"0.3"}},
             std::pair{std::string{"modify-space-4-narrow-minus-0.65"}, std::string{"-0.65"}},
             std::pair{std::string{"modify-space-4-narrow-0-mono"}, std::string{"0.0000015"}},
             std::pair{std::string{"modify-space-4-narrow-minus-1-mirror"}, std::string{"-0.9999985"}},
         })
    {
        matches(name, "talas.cdpStereoSpace", {choice("mode", "narrow"), expression("narrowing", narrowing)}, stereo, 2);
    }
    matches("housekeep-chans-3-mono-channel-1", "talas.cdpChannelZero", {expression("channel", "1")}, channelInputs(1), 2);
    matches("housekeep-chans-3-mono-channel-1-256-frame-buffers", "talas.cdpChannelZero",
        {expression("channel", "1"), expression("bufferFrames", "256")}, channelInputs(1), 2);
    matches("housekeep-chans-3-stereo-channel-2", "talas.cdpChannelZero", {expression("channel", "2")}, channelInputs(2), 2);
    matches("housekeep-chans-3-5-channels-channel-4", "talas.cdpChannelZero", {expression("channel", "4")},
        channelInputs(5), 5);
    matches("housekeep-chans-4-stereo", "talas.cdpChannelMixdown", {}, stereo, 1);
    matches("housekeep-chans-4-stereo-invert-phase", "talas.cdpChannelMixdown", {flag("invertSecond", true)}, stereo, 1);
    matches("housekeep-chans-5-mono-to-stereo", "talas.cdpMonoToStereo", {}, mono, 2);
    matches("chanphase-stereo-channel-1", "talas.cdpChannelPhase", {expression("channel", "1")}, channelInputs(2), 2);
    matches("chanphase-6-channels-channel-5", "talas.cdpChannelPhase", {expression("channel", "5")}, channelInputs(6), 6);
    matches("mton-3-channels", "talas.cdpMonoToMany", {expression("channels", "3")}, mono, 3);
    matches("mton-8-channels", "talas.cdpMonoToMany", {expression("channels", "8")}, mono, 8);
    matches("pairex-4-channels-pair-31", "talas.cdpChannelPair", {expression("first", "3"), expression("second", "1")},
        channelInputs(4), 2);
    matches("pairex-8-channels-pair-53", "talas.cdpChannelPair", {expression("first", "5"), expression("second", "3")},
        channelInputs(8), 2);
    const auto placement = [](std::string channels, std::string centres, std::string pregain, bool shifted) {
        return std::vector<Parameter>{expression("channels", std::move(channels)), text("centres", std::move(centres)),
            expression("pregain", std::move(pregain)), flag("shifted", shifted)};
    };
    matches("mchstereo-4-channels-centre-2-pregain-0.8", "talas.cdpStereoPlacement", placement("4", "2", "0.8", false),
        channelInputs(2), 4);
    // CDP8 places a centre given as a number on its command line a channel
    // up; the node's centres are numbered as CDP's data file numbers them.
    matches("mchstereo-4-channels-number-2-centres-on-3", "talas.cdpStereoPlacement", placement("4", "3", "0.8", false),
        channelInputs(2), 4);
    matches("mchstereo-3-inputs-6-channels-centres-1-3-6-pregain-0.7", "talas.cdpStereoPlacement",
        placement("6", "1 3 6", "0.7", false), channelInputs(6), 6);
    matches("mchstereo-2-inputs-2-channels-centres-1-2-pregain-1", "talas.cdpStereoPlacement",
        placement("2", "1 2", "1", false), channelInputs(4), 2);
    matches("mchstereo-2-inputs-5-channels-shifted-centres-5-2-pregain-0.5", "talas.cdpStereoPlacement",
        placement("5", "5 2", "0.5", true), channelInputs(4), 5);
    matches("prefix-silence-stereo-0.01", "talas.cdpPrefix", {expression("duration", "0.01 s")}, channelInputs(2), 2);
    matches("prefix-silence-mono-0.0123-rate-44100", "talas.cdpPrefix", {expression("duration", "0.0123 s")},
        channelInputs(1), 1);
    const auto masks = [](std::string units, std::string times, std::string splice, std::string buffer) {
        return std::vector<Parameter>{choice("units", std::move(units)), text("times", std::move(times)),
            expression("splice", std::move(splice)), expression("bufferFrames", std::move(buffer))};
    };
    matches("sfedit-masks-1-seconds-mono", "talas.cdpSilenceMasks",
        masks("seconds", "0.002 0.004\n0.0101 0.0115\n0.015 0.0193", "1.03 ms", "0"), channelInputs(1), 1);
    matches("sfedit-masks-3-frames-stereo-from-0", "talas.cdpSilenceMasks",
        masks("frames", "0 100\n400 455\n700 1023", "1.0208 ms", "0"), channelInputs(2), 2);
    matches("sfedit-masks-2-samples-3-channels", "talas.cdpSilenceMasks", masks("samples", "300 701\n1001 1300", "0.5 ms", "0"),
        channelInputs(3), 3);
    matches("sfedit-masks-1-stereo-5120-frame-buffers", "talas.cdpSilenceMasks",
        masks("seconds", "0.02 0.03\n0.10685 0.11", "2 ms", "5120"), channelInputs(2), 2);
    const auto overwrite = [](std::string units, std::string time, std::string duration, std::string splice) {
        return std::vector<Parameter>{choice("units", std::move(units)), expression("time", std::move(time)),
            expression("duration", std::move(duration)), expression("splice", std::move(splice))};
    };
    matches("sfedit-insil-1-seconds-mono-overwrite", "talas.cdpSilenceOverwrite",
        overwrite("seconds", "0.005", "0.006", "1 ms"), channelInputs(1), 1);
    matches("sfedit-insil-3-frames-stereo-overwrite", "talas.cdpSilenceOverwrite", overwrite("frames", "333", "400", "2.1 ms"),
        channelInputs(2), 2);
    matches("sfedit-insil-2-samples-3-channels-overwrite-silent-end", "talas.cdpSilenceOverwrite",
        overwrite("samples", "1700", "1200", "0.5 ms"), channelInputs(3), 3);
    require(fails(graph("talas.cdpStereoSpace", {choice("mode", "narrow"), expression("narrowing", "1")}, stereo))
            && fails(graph("talas.cdpChannelZero", {expression("channel", "3")}, channelInputs(2)))
            && fails(graph("talas.cdpChannelPair", {expression("first", "2"), expression("second", "2")}, channelInputs(4)))
            && fails(graph("talas.cdpChannelPair", {}, channelInputs(2)))
            && fails(graph("talas.cdpStereoPlacement", {text("centres", "1")}, channelInputs(3)))
            && fails(graph("talas.cdpStereoPlacement", {text("centres", "5")}, channelInputs(2)))
            && fails(graph("talas.cdpPrefix", {expression("duration", "11 s")}, channelInputs(1)))
            && fails(graph("talas.cdpSilenceMasks", {text("times", "0.5")}, channelInputs(1)))
            && fails(graph("talas.cdpSilenceOverwrite", {expression("duration", "0.01")}, channelInputs(1))),
        "the channel tools should refuse what CDP8 refuses: a narrowing of 1, a channel past the input, a pair of one "
        "channel or of a stereo input, an odd stereo count or a centre past the channels, a prefix over 10 s, an "
        "unpaired mask and an overwrite shorter than its splices");
}

void testNodesRefuseWhatTheirPortsRefuse()
{
    const auto with = [](std::string type, std::vector<Parameter> fields, std::vector<std::string> ports) {
        auto document = graph(type, std::move(fields), ports);
        return document;
    };
    require(fails(with("talas.cdpRotatingPan", {expression("channels", "9")}, mono))
            && fails(with("talas.cdpRotatingPan", {expression("channels", "2")}, mono))
            && fails(with("talas.cdpRotatingPan", {expression("channels", "4.5")}, mono))
            && fails(with("talas.cdpRotatingPan", {expression("speed", "65")}, mono))
            && fails(with("talas.cdpRotatingPan", {expression("focus", "!Focus")}, mono))
            && fails(with("talas.cdpRotatingPan", {expression("start", "5")}, mono)),
        "CDP Rotating Pan should refuse channels outside 3 to 8 or not whole, a speed over 64, a live focus and a start past the channels");
    require(fails(with("talas.cdpSpreadPan", {expression("centre", "1.5")}, mono))
            && fails(with("talas.cdpSpreadPan", {expression("centre", "!Centre")}, mono))
            && fails(with("talas.cdpSpreadPan", {expression("spread", "17")}, mono)),
        "CDP Spread Pan should refuse a centre that is not whole or is live and a spread over 16");
    require(fails(with("talas.cdpTablePan", {text("table", "0 1 1 / 1 4")}, mono))
            && fails(with("talas.cdpTablePan", {text("table", "0 1 1 / 1 5 1")}, mono))
            && fails(with("talas.cdpTablePan", {text("table", "0 1 one")}, mono))
            && fails(with("talas.cdpTablePan", {text("table", "")}, mono)),
        "CDP Table Pan should refuse a row that is not three numbers, a position past the channels and an empty table");
    require(fails(with("talas.cdpStepSpread", {flag("stepped", true), expression("centre", "!Centre")}, mono))
            && fails(with("talas.cdpStepSpread", {expression("spread", "0.5")}, mono))
            && fails(with("talas.cdpStepSpread", {expression("minimumSilence", "!Silence * (1 s)")}, mono)),
        "CDP Step Spread should refuse a live centre when stepped, a spread under 1 and a live minimum silence");
    require(fails(with("talas.cdpCrossPan", {}, {"in1", "in2", "in4"}))
            && fails(with("talas.cdpCrossPan", {text("table", "0 1 2 3 / 1 2 1 3")}, channelInputs(2)))
            && fails(with("talas.cdpCrossPan", {text("table", "0.1 1 2 / 1 2 1")}, channelInputs(2)))
            && fails(with("talas.cdpCrossPan", {expression("bufferFrames", "-1")}, channelInputs(2))),
        "CDP Cross Pan should refuse a gap in its inputs, a map that does not match them, a late table unless corrected and negative buffer frames");
    require(fails(with("talas.cdpAntiphony", {text("sets", "ab")}, mono))
            && fails(with("talas.cdpAntiphony", {text("sets", "ab-ce")}, mono))
            && fails(with("talas.cdpAntiphony", {}, {"audioIn", "sideChainRight"}))
            && fails(with("talas.cdpAntiphony", {}, {"audioIn", "rightIn", "sideChain"}))
            && fails(with("talas.cdpAntiphony", {expression("eventDuration", "0.01 s")}, mono))
            && fails(with("talas.cdpAntiphony", {expression("splice", "!Splice * (1 s)")}, mono)),
        "CDP Antiphony should refuse sets that are not two, a channel past the outputs, a side chain's right without it or of another width, a short event and a live splice");
    require(fails(with("talas.cdpMultichannelStadium", {expression("count", "1")}, mono))
            && fails(with("talas.cdpMultichannelStadium", {expression("spread", "5")}, mono))
            && fails(with("talas.cdpMultichannelStadium", {expression("gain", "!Gain")}, mono)),
        "CDP Multichannel Stadium should refuse a count under 2, a spread past the channels and a live gain");
    require(fails(with("talas.cdpFlutter", {text("sets", "1 / 3")}, channelInputs(2)))
            && fails(with("talas.cdpFlutter", {flag("random", true)}, channelInputs(1)))
            && fails(with("talas.cdpFlutter", {expression("frequency", "121 hz")}, channelInputs(1)))
            && fails(with("talas.cdpFlutter", {expression("gain", "!Gain")}, channelInputs(1))),
        "CDP Flutter should refuse a channel past its inputs, random order with one set, a frequency over 120 Hz and a live gain");
    require(fails(with("talas.cdpFrameRotate", {choice("mode", "twoRotations")}, channelInputs(3)))
            && fails(with("talas.cdpFrameRotate", {text("snake", "1 2 2")}, channelInputs(3)))
            && fails(with("talas.cdpFrameRotate", {expression("firstDirection", "2")}, channelInputs(2)))
            && fails(with("talas.cdpFrameRotate", {expression("smear", "!Smear")}, channelInputs(2)))
            && fails(with("talas.cdpFrameRotate", {}, channelInputs(1))),
        "CDP Frame Rotate should refuse two rotations on an odd count, a snake that is no route, a direction of 2, a live smear and one input");
    require(fails(with("talas.cdpFrameRemap", {choice("mode", "reorient"), text("route", "1 1")}, channelInputs(2)))
            && fails(with("talas.cdpFrameRemap", {choice("mode", "mirror"), expression("plane", "1.25")}, channelInputs(4)))
            && fails(with("talas.cdpFrameRemap", {expression("second", "1")}, channelInputs(2))),
        "CDP Frame Remap should refuse a route that is no route, a plane that is neither whole nor a half and a channel swapped with itself");
    require(fails(with("talas.cdpChannelEnvelope", {text("enveloped", "3")}, channelInputs(2)))
            && fails(with("talas.cdpChannelEnvelope", {expression("gain", "1.5")}, channelInputs(2))),
        "CDP Channel Envelope should refuse a channel past its inputs and a gain over 1");
    require(fails(with("talas.cdpToStereo", {expression("channels", "4")}, stereo))
            && fails(with("talas.cdpToStereo", {expression("end", "0 s")}, stereo))
            && fails(with("talas.cdpToStereo", {expression("mixLevel", "0.3")}, stereo)),
        "CDP To Stereo should refuse more channels without a left, a start at its end and a mix level under 0.5");
}
}

int main()
{
    try
    {
        testRotatingPanPlaysCdp();
        testSpreadPanPlaysCdp();
        testTablePanPlaysCdp();
        testStepSpreadPlaysCdp();
        testCrossPanPlaysCdp();
        testAntiphonyPlaysCdp();
        testMultichannelStadiumPlaysCdp();
        testFlutterPlaysCdp();
        testFrameRotatePlaysCdp();
        testFrameRemapPlaysCdp();
        testChannelEnvelopeAndToStereoPlayCdp();
        testChannelToolsPlayCdp();
        testLiveFieldsPlayTheirPorts();
        testNodesRefuseWhatTheirPortsRefuse();
        testWiredPortsPlayTheirChannels();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "CDP multichannel node tests passed\n";
    return 0;
}
