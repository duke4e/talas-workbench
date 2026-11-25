#pragma once

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
#include "talas/cdp/ChannelTools.h"
#include "talas/cdp/Prefix.h"
#include "talas/cdp/SilenceMasks.h"
#include "talas/cdp/StereoPlacement.h"
#include "talas/cdp/StereoSpace.h"
#include "talas/expr/Expression.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

// CDP's multichannel tools as nodes with N outputs (owner plan 2026-10-12):
// each node plays its CDP channels on out1 to outN of its eight outputs,
// the rest silent; a tool that takes N channels takes them on in1 to inN,
// the inputs connected without a gap. The fields, preparation, live setters
// and playing are shared by the offline render and the realtime session.
namespace talas::runtime::cdp_multichannel
{
// The most channels a node takes or plays: in1 to in8, out1 to out8.
inline constexpr std::size_t maximumChannels = 8;
// The frames of the ports' interleaved buffers played at a time.
inline constexpr std::size_t chunkFrames = 64;

// How a node's inputs give the port its channels.
enum class Inputs
{
    // The Input alone.
    Mono,
    // The Input, and the Right Input for a stereo input when connected.
    MonoOrStereo,
    // The Input and the Right Input.
    Stereo,
    // in1 to inN, connected without a gap.
    Channels,
    // The Input (and Right Input), and a Side Chain (and its right) of as
    // many channels when connected.
    WithSideChain,
};

enum class Kind
{
    RotatingPan,
    SpreadPan,
    TablePan,
    StepSpread,
    CrossPan,
    Antiphony,
    Stadium,
    Flutter,
    FrameRotate,
    FrameRemap,
    ChannelEnvelope,
    ToStereo,
    StereoSpace,
    ChannelZero,
    ChannelMixdown,
    MonoToStereo,
    ChannelPhase,
    MonoToMany,
    ChannelPair,
    StereoPlacement,
    Prefix,
    SilenceMasks,
    SilenceOverwrite,
};

struct Tool
{
    Kind kind;
    std::string_view typeId;
    std::string_view name;
    Inputs inputs;
    // The descriptor's fields.
    std::size_t parameters;
    // The fields CDP takes as breakpoint files that may be live, in the
    // order setControls takes them.
    std::array<std::string_view, 4> live;
    // Fields that must be whole numbers.
    std::array<std::string_view, 4> whole;
    // What the port takes, said when it refuses.
    std::string_view refusal;
};

inline constexpr std::array tools{
    Tool{Kind::RotatingPan, "talas.cdpRotatingPan", "CDP Rotating Pan", Inputs::Mono, 5, {"speed"}, {"channels"},
        "CDP Rotating Pan requires whole channels from 3 to 8, a start from 0 to the channels, a speed from 0 to 64 and a focus from 0 to 1"},
    Tool{Kind::SpreadPan, "talas.cdpSpreadPan", "CDP Spread Pan", Inputs::MonoOrStereo, 5,
        {"spread", "depth", "rolloff"}, {"channels", "centre"},
        "CDP Spread Pan requires whole channels from 3 to 8, a whole centre from 0 to 8 and the channels, a spread from 0 to 16, a depth from 0 to 8 and a rolloff from 0 to 1"},
    Tool{Kind::TablePan, "talas.cdpTablePan", "CDP Table Pan", Inputs::Mono, 3, {}, {"channels"},
        "CDP Table Pan requires whole channels from 3 to 8, a focus from 0 to 1 and a table of time, position and type rows whose times rise from 0 a frame or more apart, positions from 0 to the channels (whole for a direct move) and types -1, 0 or 1"},
    Tool{Kind::StepSpread, "talas.cdpStepSpread", "CDP Step Spread", Inputs::Mono, 7,
        {"centre", "spread", "depth", "rolloff"}, {"channels"},
        "CDP Step Spread requires whole channels from 3 to 8, a centre from 0.5 to 8 and the channels, a spread from 1 to 16, a depth from 0 to 8, a rolloff from 0 to 1, a minimum silence of 2 to 32767 frames, and when stepped a static centre, spread and depth"},
    Tool{Kind::CrossPan, "talas.cdpCrossPan", "CDP Cross Pan", Inputs::Channels, 4, {}, {"bufferFrames"},
        "CDP Cross Pan requires 2 to 8 inputs, in1 to inN, a table of rows of a time and a map of each input to a different output 1 to N (or all 0, the centre) whose times rise from 0 (or later when corrected) a frame or more apart, a rolloff from 0 to 1 and whole buffer frames from 0"},
    Tool{Kind::Antiphony, "talas.cdpAntiphony", "CDP Antiphony", Inputs::WithSideChain, 7, {"eventDuration", "gap"},
        {"channels", "bufferFrames"},
        "CDP Antiphony requires whole channels from 3 to 8, two sets of CDP's channel letters (a set's channels each once, a multiple of the input's channels), an event duration from 0.05 to 60 s, a gap from 0 to 60 s, a splice up to 1000 ms, whole buffer frames from 0, and a side chain of as many channels as the input"},
    Tool{Kind::Stadium, "talas.cdpMultichannelStadium", "CDP Multichannel Stadium", Inputs::Mono, 8, {},
        {"channels", "count", "bufferFrames"},
        "CDP Multichannel Stadium requires whole channels from 2 to 8, a gain and rolloff from 1/32767 to 1, a size up to 100, a whole count from 2 to 1000, a centre from 0 to the channels, a spread from 2 to the channels, its last echo within 100 s and whole buffer frames from 0"},
    Tool{Kind::Flutter, "talas.cdpFlutter", "CDP Flutter", Inputs::Channels, 6, {"frequency", "depth"}, {"bufferFrames"},
        "CDP Flutter requires 1 to 8 inputs, in1 to inN, channel sets of channels 1 to N (one set or more, at most 1024), a frequency from 0 to 120 Hz, a depth from 0 to 16, a gain from 0.000002 to 10, more than one set for random order, and whole buffer frames from 0"},
    Tool{Kind::FrameRotate, "talas.cdpFrameRotate", "CDP Frame Rotate", Inputs::Channels, 7,
        {"rotation", "secondRotation"}, {"firstDirection", "secondFirstDirection"},
        "CDP Frame Rotate requires 2 to 8 inputs, in1 to inN (an even count for two rotations), rotations from -500 to 500, a smear from 0 to 0.5, whole first directions from -1 to 1 (not both 0 with both rotations 0), and a snake through every channel once or none"},
    Tool{Kind::FrameRemap, "talas.cdpFrameRemap", "CDP Frame Remap", Inputs::Channels, 6, {}, {"first", "second"},
        "CDP Frame Remap requires 2 to 8 inputs, in1 to inN, and a route through every channel once, a mirror plane from 0 to N + 0.5 that is whole or a half, or two different channels to swap"},
    Tool{Kind::ChannelEnvelope, "talas.cdpChannelEnvelope", "CDP Channel Envelope", Inputs::Channels, 2, {"gain"}, {},
        "CDP Channel Envelope requires 2 to 8 inputs, in1 to inN, channels to envelope from 1 to N each once, and a gain from 0 to 1"},
    Tool{Kind::ToStereo, "talas.cdpToStereo", "CDP To Stereo", Inputs::Stereo, 6, {}, {"channels", "left", "right"},
        "CDP To Stereo requires whole channels from 2 to 8, a start and end from 0 s at least 0.000002 s apart, whole left and right channels CDP takes (with 2 channels both or neither, with more a left, never the same) and a mix level of 0 or from 0.5 to 1"},
    Tool{Kind::StereoSpace, "talas.cdpStereoSpace", "CDP Stereo Space", Inputs::Stereo, 2, {}, {},
        "CDP Stereo Space requires one of its modes and a narrowing from -1 to 1, not 1 (no effect)"},
    Tool{Kind::ChannelZero, "talas.cdpChannelZero", "CDP Channel Zero", Inputs::Channels, 2, {}, {"channel", "bufferFrames"},
        "CDP Channel Zero requires 1 to 8 inputs, in1 to inN, a whole channel from 1 to N and whole buffer frames from 0"},
    Tool{Kind::ChannelMixdown, "talas.cdpChannelMixdown", "CDP Channel Mixdown", Inputs::Stereo, 1, {}, {},
        "CDP Channel Mixdown requires a stereo input"},
    Tool{Kind::MonoToStereo, "talas.cdpMonoToStereo", "CDP Mono To Stereo", Inputs::Mono, 0, {}, {},
        "CDP Mono To Stereo requires a mono input"},
    Tool{Kind::ChannelPhase, "talas.cdpChannelPhase", "CDP Channel Phase", Inputs::Channels, 1, {}, {"channel"},
        "CDP Channel Phase requires 1 to 8 inputs, in1 to inN, and a whole channel from 1 to N"},
    Tool{Kind::MonoToMany, "talas.cdpMonoToMany", "CDP Mono To Many", Inputs::Mono, 1, {}, {"channels"},
        "CDP Mono To Many requires whole channels from 2 to 8"},
    Tool{Kind::ChannelPair, "talas.cdpChannelPair", "CDP Channel Pair", Inputs::Channels, 2, {}, {"first", "second"},
        "CDP Channel Pair requires 3 to 8 inputs, in1 to inN, and two different whole channels from 1 to N"},
    Tool{Kind::StereoPlacement, "talas.cdpStereoPlacement", "CDP Stereo Placement", Inputs::Channels, 4, {},
        {"channels"},
        "CDP Stereo Placement requires stereo inputs as pairs of in1 to inN (at most four), whole channels from 2 to 8, a centre from 1 to the channels for each input and a pregain above 0 to 1"},
    Tool{Kind::Prefix, "talas.cdpPrefix", "CDP Prefix", Inputs::Channels, 1, {}, {},
        "CDP Prefix requires 1 to 8 inputs, in1 to inN, and a duration of a frame or more up to 10 s"},
    Tool{Kind::SilenceMasks, "talas.cdpSilenceMasks", "CDP Silence Masks", Inputs::Channels, 4, {}, {"bufferFrames"},
        "CDP Silence Masks requires 1 to 8 inputs, in1 to inN, one of its units, pairs of start and end times CDP takes (in order, apart by their splices), a splice up to 5000 ms and whole buffer frames from 0 at least the splice"},
    Tool{Kind::SilenceOverwrite, "talas.cdpSilenceOverwrite", "CDP Silence Overwrite", Inputs::Channels, 4, {}, {},
        "CDP Silence Overwrite requires 1 to 8 inputs, in1 to inN, one of its units, a time a splice or more from the start, a duration of two splices or more and a splice up to 5000 ms"},
};

[[nodiscard]] inline const Tool* find(std::string_view typeId) noexcept
{
    const auto found = std::ranges::find(tools, typeId, &Tool::typeId);
    return found == tools.end() ? nullptr : &*found;
}

[[nodiscard]] inline bool isLiveField(const Tool& tool, std::string_view id) noexcept
{
    return !id.empty() && std::ranges::find(tool.live, id) != tool.live.end();
}

[[nodiscard]] inline bool isWholeField(const Tool& tool, std::string_view id) noexcept
{
    return !id.empty() && std::ranges::find(tool.whole, id) != tool.whole.end();
}

// The unit a field's expression is in.
[[nodiscard]] inline talas::expr::ValueDomain fieldDomain(const Tool& tool, std::string_view id) noexcept
{
    if (id == "eventDuration" || id == "gap" || id == "splice" || id == "minimumSilence"
        || (tool.kind == Kind::ToStereo && (id == "start" || id == "end"))
        || (tool.kind == Kind::Prefix && id == "duration"))
    {
        return talas::expr::ValueDomain::Seconds;
    }
    if (tool.kind == Kind::Flutter && id == "frequency")
        return talas::expr::ValueDomain::Hertz;
    return talas::expr::ValueDomain::Scalar;
}

// The input's channels as the node's connections give them: the Input's (or
// in1 to inN) and the Side Chain's.
struct InputShape
{
    std::size_t channels = 0;
    std::size_t sideChannels = 0;
};

// The input ports in1 to in8.
inline constexpr std::array<std::string_view, maximumChannels> channelInputs{
    "in1", "in2", "in3", "in4", "in5", "in6", "in7", "in8"};
// The output ports out1 to out8.
inline constexpr std::array<std::string_view, maximumChannels> channelOutputs{
    "out1", "out2", "out3", "out4", "out5", "out6", "out7", "out8"};

// The input ports that give a node's channels, in order, given which of the
// node's input ports are connected; nullopt when they do not form a shape
// the node takes (in1 to inN with a gap, a Right Side Chain without its
// Side Chain). The side chain's ports follow the input's.
template <typename Connected>
[[nodiscard]] std::optional<std::vector<std::string_view>> inputPorts(
    const Tool& tool, Connected&& connected, InputShape& shape)
{
    std::vector<std::string_view> ports;
    shape = {};
    switch (tool.inputs)
    {
    case Inputs::Mono:
        ports = {"audioIn"};
        shape.channels = 1;
        break;
    case Inputs::MonoOrStereo:
        ports = {"audioIn"};
        if (connected("rightIn"))
            ports.push_back("rightIn");
        shape.channels = ports.size();
        break;
    case Inputs::Stereo:
        ports = {"audioIn", "rightIn"};
        shape.channels = 2;
        break;
    case Inputs::Channels:
    {
        std::size_t count = 0;
        while (count < channelInputs.size() && connected(channelInputs[count]))
            ++count;
        for (auto later = count; later < channelInputs.size(); ++later)
        {
            if (connected(channelInputs[later]))
                return std::nullopt;
        }
        ports.assign(channelInputs.begin(), channelInputs.begin() + static_cast<std::ptrdiff_t>(count));
        shape.channels = count;
        break;
    }
    case Inputs::WithSideChain:
        ports = {"audioIn"};
        if (connected("rightIn"))
            ports.push_back("rightIn");
        shape.channels = ports.size();
        if (connected("sideChain"))
        {
            ports.push_back("sideChain");
            if (connected("sideChainRight"))
                ports.push_back("sideChainRight");
            shape.sideChannels = ports.size() - shape.channels;
        }
        else if (connected("sideChainRight"))
        {
            return std::nullopt;
        }
        break;
    }
    if (shape.channels == 0)
        return std::nullopt;
    return ports;
}

// Numbers in rows: rows split at '/', ';' or a line break, numbers at
// spaces, tabs or commas. Nullopt for anything that is not a number.
[[nodiscard]] inline std::optional<std::vector<std::vector<double>>> rows(std::string_view text)
{
    std::vector<std::vector<double>> result(1);
    std::string token;
    const auto flush = [&] {
        if (token.empty())
            return true;
        char* end = nullptr;
        const auto value = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size() || !std::isfinite(value))
            return false;
        result.back().push_back(value);
        token.clear();
        return true;
    };
    for (const auto character : text)
    {
        if (character == '/' || character == ';' || character == '\n' || character == '\r')
        {
            if (!flush())
                return std::nullopt;
            result.emplace_back();
        }
        else if (character == ' ' || character == '\t' || character == ',')
        {
            if (!flush())
                return std::nullopt;
        }
        else
        {
            token += character;
        }
    }
    if (!flush())
        return std::nullopt;
    return result;
}

// The rows' numbers in groups of `size`, a row holding one group or more;
// a row whose numbers do not divide into groups gives none of its own.
[[nodiscard]] inline std::vector<std::vector<double>> groups(
    const std::vector<std::vector<double>>& parsed, std::size_t size)
{
    std::vector<std::vector<double>> result;
    for (const auto& row : parsed)
    {
        if (size == 0 || row.size() % size != 0)
            continue;
        for (std::size_t at = 0; at < row.size(); at += size)
            result.emplace_back(row.begin() + static_cast<std::ptrdiff_t>(at),
                row.begin() + static_cast<std::ptrdiff_t>(at + size));
    }
    return result;
}

[[nodiscard]] inline std::size_t count(const std::vector<std::vector<double>>& parsed) noexcept
{
    std::size_t numbers = 0;
    for (const auto& row : parsed)
        numbers += row.size();
    return numbers;
}

[[nodiscard]] inline bool blank(std::string_view text) noexcept
{
    return std::ranges::all_of(text, [](char character) {
        return character == ' ' || character == '\t' || character == '\n' || character == '\r';
    });
}

// A whole number from low to high, as an int.
[[nodiscard]] inline std::optional<int> whole(const std::optional<double>& value, double low, double high) noexcept
{
    if (!value.has_value() || !std::isfinite(*value) || *value != std::round(*value) || *value < low
        || *value > high)
    {
        return std::nullopt;
    }
    return static_cast<int>(*value);
}

// The numbers of a text, all rows together, as whole numbers.
[[nodiscard]] inline std::optional<std::vector<int>> wholeList(std::string_view text, double low, double high)
{
    const auto parsed = rows(text);
    if (!parsed.has_value())
        return std::nullopt;
    std::vector<int> list;
    for (const auto& row : *parsed)
    {
        for (const auto value : row)
        {
            const auto number = whole(value, low, high);
            if (!number.has_value())
                return std::nullopt;
            list.push_back(*number);
        }
    }
    return list;
}

// CDP's antiphony sets: two runs of channel letters (a for channel 1 to p
// for 16) joined by a hyphen, as `ab-cd`.
[[nodiscard]] inline std::optional<std::array<std::vector<int>, 2>> antiphonySets(std::string_view text)
{
    std::array<std::vector<int>, 2> sets;
    std::size_t set = 0;
    for (const auto character : text)
    {
        if (character == ' ' || character == '\t')
            continue;
        if (character == '-')
        {
            if (++set > 1)
                return std::nullopt;
            continue;
        }
        if (character < 'a' || character > 'p')
            return std::nullopt;
        sets[set].push_back(character - 'a' + 1);
    }
    if (set != 1)
        return std::nullopt;
    return sets;
}

// A node's fields as a renderer holds them: number(id), the static value
// or a live field's starting value; live(id); choice(id), null for none;
// boolean(id); text(id), nullopt for a missing field.
template <typename Number, typename Live, typename Choice, typename Boolean, typename Text>
struct FieldReader
{
    Number number;
    Live live;
    Choice choice;
    Boolean boolean;
    Text text;
};

template <typename Number, typename Live, typename Choice, typename Boolean, typename Text>
FieldReader(Number, Live, Choice, Boolean, Text) -> FieldReader<Number, Live, Choice, Boolean, Text>;

// Whether the node as its fields set it takes its live field `index` (in
// Tool::live): Frame Rotate's second rotation only with two rotations.
template <typename Fields>
[[nodiscard]] bool takesLive(const Tool& tool, const Fields& fields, std::size_t index)
{
    if (index >= tool.live.size() || tool.live[index].empty())
        return false;
    if (tool.kind == Kind::FrameRotate && index == 1)
    {
        const auto* mode = fields.choice("mode");
        return mode != nullptr && *mode == "twoRotations";
    }
    return true;
}

// Prepares the node's port from its fields, read through a FieldReader, at
// the sample rate, for the input's shape. False, with the port left empty,
// for fields the node or its port refuses. A live field's starting value
// starts the port.
template <typename Fields>
[[nodiscard]] bool prepare(const Tool& tool, const Fields& fields, double sampleRate, const InputShape& shape,
    CdpMultichannelPort& port)
{
    port = std::monostate{};
    const auto number = [&](std::string_view id) { return fields.number(id); };
    const auto flag = [&](std::string_view id) { return fields.boolean(id); };
    const auto nodeChannels = [&](double low) { return whole(number("channels"), low, double(maximumChannels)); };
    const auto inputs = static_cast<int>(shape.channels);
    const auto onlyInputs = [&](std::size_t channels) {
        return shape.channels == channels && shape.sideChannels == 0;
    };
    const auto all = [](std::initializer_list<std::optional<double>> values) {
        return std::ranges::all_of(values, [](const std::optional<double>& value) { return value.has_value(); });
    };
    switch (tool.kind)
    {
    case Kind::RotatingPan:
    {
        const auto channels = nodeChannels(3.0);
        const auto start = number("start");
        const auto speed = number("speed");
        const auto focus = number("focus");
        const auto anticlockwise = flag("anticlockwise");
        talas::cdp::RotatingPan pan;
        if (!onlyInputs(1) || !channels.has_value() || !all({start, speed, focus}) || !anticlockwise.has_value()
            || !pan.prepare(sampleRate, *channels, *start, *speed, *focus, *anticlockwise))
        {
            return false;
        }
        port = std::move(pan);
        return true;
    }
    case Kind::SpreadPan:
    {
        const auto channels = nodeChannels(3.0);
        const auto centre = whole(number("centre"), 0.0, 8.0);
        const auto spread = number("spread");
        const auto depth = number("depth");
        const auto rolloff = number("rolloff");
        talas::cdp::SpreadPan pan;
        if (shape.sideChannels != 0 || !channels.has_value() || !centre.has_value() || !all({spread, depth, rolloff})
            || !pan.prepare(sampleRate, inputs, *channels, *centre, *spread, *depth, *rolloff))
        {
            return false;
        }
        port = std::move(pan);
        return true;
    }
    case Kind::TablePan:
    {
        const auto channels = nodeChannels(3.0);
        const auto focus = number("focus");
        const auto text = fields.text("table");
        const auto parsed = text.has_value() ? rows(*text) : std::nullopt;
        if (!onlyInputs(1) || !channels.has_value() || !focus.has_value() || !parsed.has_value())
            return false;
        std::vector<talas::cdp::TablePan::Point> table;
        for (const auto& row : groups(*parsed, 3))
        {
            const auto type = whole(row[2], -1.0, 1.0);
            if (!type.has_value())
                return false;
            table.push_back({row[0], row[1], *type});
        }
        if (table.size() * 3 != count(*parsed))
            return false;
        talas::cdp::TablePan pan;
        if (!pan.prepare(sampleRate, *channels, table, *focus))
            return false;
        port = std::move(pan);
        return true;
    }
    case Kind::StepSpread:
    {
        const auto channels = nodeChannels(3.0);
        const auto centre = number("centre");
        const auto spread = number("spread");
        const auto depth = number("depth");
        const auto rolloff = number("rolloff");
        const auto silence = number("minimumSilence");
        const auto stepped = flag("stepped");
        // CDP's stepped spread takes Centre, Spread and Depth once.
        if (!onlyInputs(1) || !channels.has_value() || !all({centre, spread, depth, rolloff, silence})
            || !stepped.has_value()
            || (*stepped && (fields.live("centre") || fields.live("spread") || fields.live("depth"))))
        {
            return false;
        }
        talas::cdp::StepSpread spreadPort;
        if (!spreadPort.prepare(sampleRate, *channels, *centre, *spread, *depth, *rolloff, *silence * 1000.0, *stepped))
            return false;
        port = std::move(spreadPort);
        return true;
    }
    case Kind::CrossPan:
    {
        const auto rolloff = number("rolloff");
        const auto buffer = whole(number("bufferFrames"), 0.0, 16777216.0);
        const auto corrected = flag("corrected");
        const auto text = fields.text("table");
        const auto parsed = text.has_value() ? rows(*text) : std::nullopt;
        if (shape.sideChannels != 0 || inputs < 2 || !rolloff.has_value() || !buffer.has_value()
            || !corrected.has_value() || !parsed.has_value())
        {
            return false;
        }
        std::vector<talas::cdp::CrossPan::Point> table;
        const auto rowSize = shape.channels + 1;
        const auto points = groups(*parsed, rowSize);
        if (points.size() * rowSize != count(*parsed))
            return false;
        for (const auto& row : points)
        {
            talas::cdp::CrossPan::Point point;
            point.time = row[0];
            for (std::size_t channel = 0; channel < shape.channels; ++channel)
            {
                const auto output = whole(row[channel + 1], 0.0, double(maximumChannels));
                if (!output.has_value())
                    return false;
                point.map[channel] = *output;
            }
            table.push_back(point);
        }
        talas::cdp::CrossPan pan;
        if (!pan.prepare(sampleRate, inputs, table, *rolloff, static_cast<std::size_t>(*buffer), *corrected))
            return false;
        port = std::move(pan);
        return true;
    }
    case Kind::Antiphony:
    {
        const auto channels = nodeChannels(3.0);
        const auto duration = number("eventDuration");
        const auto gap = number("gap");
        const auto splice = number("splice");
        const auto buffer = whole(number("bufferFrames"), 0.0, 16777216.0);
        const auto corrected = flag("corrected");
        const auto text = fields.text("sets");
        const auto sets = text.has_value() ? antiphonySets(*text) : std::nullopt;
        if ((shape.sideChannels != 0 && shape.sideChannels != shape.channels) || !channels.has_value()
            || !all({duration, gap, splice}) || !buffer.has_value() || !corrected.has_value() || !sets.has_value())
        {
            return false;
        }
        talas::cdp::Antiphony antiphony;
        if (!antiphony.prepare(sampleRate, inputs, *channels, (*sets)[0], (*sets)[1], *duration, *gap,
                *splice * 1000.0, shape.sideChannels != 0, static_cast<std::size_t>(*buffer), *corrected))
        {
            return false;
        }
        port = std::move(antiphony);
        return true;
    }
    case Kind::Stadium:
    {
        const auto channels = nodeChannels(2.0);
        const auto gain = number("gain");
        const auto rolloff = number("rolloff");
        const auto size = number("size");
        const auto count = whole(number("count"), 2.0, 1000.0);
        const auto centre = number("centre");
        const auto spread = number("spread");
        const auto buffer = whole(number("bufferFrames"), 0.0, 16777216.0);
        talas::cdp::MultichannelStadium stadium;
        if (!onlyInputs(1) || !channels.has_value() || !all({gain, rolloff, size, centre, spread})
            || !count.has_value() || !buffer.has_value()
            || !stadium.prepare(sampleRate, *gain, *rolloff, *size, *count, *channels, *centre, *spread,
                static_cast<std::size_t>(*buffer)))
        {
            return false;
        }
        port = std::move(stadium);
        return true;
    }
    case Kind::Flutter:
    {
        const auto frequency = number("frequency");
        const auto depth = number("depth");
        const auto gain = number("gain");
        const auto random = flag("random");
        const auto buffer = whole(number("bufferFrames"), 0.0, 16777216.0);
        const auto text = fields.text("sets");
        if (shape.sideChannels != 0 || !all({frequency, depth, gain}) || !random.has_value() || !buffer.has_value()
            || !text.has_value())
        {
            return false;
        }
        // No sets: each channel its own, in order.
        std::vector<std::vector<int>> sets;
        if (blank(*text))
        {
            for (std::size_t channel = 1; channel <= shape.channels; ++channel)
                sets.push_back({static_cast<int>(channel)});
        }
        else
        {
            const auto parsed = rows(*text);
            if (!parsed.has_value())
                return false;
            for (const auto& row : *parsed)
            {
                std::vector<int> set;
                for (const auto value : row)
                {
                    const auto channel = whole(value, 1.0, double(maximumChannels));
                    if (!channel.has_value())
                        return false;
                    set.push_back(*channel);
                }
                sets.push_back(std::move(set));
            }
        }
        talas::cdp::Flutter flutter;
        if (!flutter.prepare(sampleRate, inputs, sets, *frequency, *depth, *gain, *random,
                static_cast<std::size_t>(*buffer)))
        {
            return false;
        }
        port = std::move(flutter);
        return true;
    }
    case Kind::FrameRotate:
    {
        const auto* mode = fields.choice("mode");
        const auto rotation = number("rotation");
        const auto second = number("secondRotation");
        const auto smear = number("smear");
        const auto direction = whole(number("firstDirection"), -1.0, 1.0);
        const auto secondDirection = whole(number("secondFirstDirection"), -1.0, 1.0);
        const auto text = fields.text("snake");
        const auto snake = text.has_value() ? wholeList(*text, 1.0, double(maximumChannels)) : std::nullopt;
        if (shape.sideChannels != 0 || mode == nullptr || (*mode != "oneRotation" && *mode != "twoRotations")
            || !all({rotation, second, smear}) || !direction.has_value() || !secondDirection.has_value()
            || !snake.has_value())
        {
            return false;
        }
        talas::cdp::FrameRotate rotate;
        if (!(*mode == "oneRotation"
                    ? rotate.prepare(sampleRate, inputs, *rotation, *smear, *direction, *snake)
                    : rotate.prepareDual(sampleRate, inputs, *rotation, *second, *smear, *direction,
                        *secondDirection, *snake)))
        {
            return false;
        }
        port = std::move(rotate);
        return true;
    }
    case Kind::FrameRemap:
    {
        const auto* mode = fields.choice("mode");
        const auto plane = number("plane");
        const auto toRing = flag("toRing");
        const auto first = whole(number("first"), 1.0, double(maximumChannels));
        const auto second = whole(number("second"), 1.0, double(maximumChannels));
        const auto text = fields.text("route");
        const auto route = text.has_value() ? wholeList(*text, 1.0, double(maximumChannels)) : std::nullopt;
        if (shape.sideChannels != 0 || mode == nullptr || !plane.has_value() || !toRing.has_value()
            || !first.has_value() || !second.has_value() || !route.has_value())
        {
            return false;
        }
        talas::cdp::FrameRemap remap;
        const auto prepared = *mode == "reorient" ? remap.prepareReorient(sampleRate, inputs, *route)
            : *mode == "mirror"                   ? remap.prepareMirror(sampleRate, inputs, *plane)
            : *mode == "bilateral"                ? remap.prepareBilateral(sampleRate, inputs, *toRing)
            : *mode == "swap"                     ? remap.prepareSwap(sampleRate, inputs, *first, *second)
            : *mode == "beastBilateral"           ? remap.prepareBeastBilateral(sampleRate, inputs, *toRing)
                                                  : false;
        if (!prepared)
            return false;
        port = std::move(remap);
        return true;
    }
    case Kind::ChannelEnvelope:
    {
        const auto gain = number("gain");
        const auto text = fields.text("enveloped");
        const auto enveloped = text.has_value() ? wholeList(*text, 1.0, double(maximumChannels)) : std::nullopt;
        talas::cdp::ChannelEnvelope envelope;
        if (shape.sideChannels != 0 || !gain.has_value() || !enveloped.has_value()
            || !envelope.prepare(sampleRate, inputs, *enveloped, *gain))
        {
            return false;
        }
        port = std::move(envelope);
        return true;
    }
    case Kind::StereoSpace:
    {
        const auto* mode = fields.choice("mode");
        const auto narrowing = number("narrowing");
        talas::cdp::StereoSpace space;
        if (!onlyInputs(2) || mode == nullptr || !narrowing.has_value()
            || !(*mode == "mirror" ? space.prepareMirror()
                    : *mode == "narrow" && space.prepareNarrow(*narrowing)))
        {
            return false;
        }
        port = std::move(space);
        return true;
    }
    case Kind::ChannelZero:
    {
        const auto channel = whole(number("channel"), 1.0, double(maximumChannels));
        const auto buffer = whole(number("bufferFrames"), 0.0, 1048576.0);
        talas::cdp::ChannelZero zero;
        if (shape.sideChannels != 0 || !channel.has_value() || !buffer.has_value()
            || !zero.prepare(inputs, *channel, static_cast<std::size_t>(*buffer)))
        {
            return false;
        }
        port = std::move(zero);
        return true;
    }
    case Kind::ChannelMixdown:
    {
        const auto invert = flag("invertSecond");
        talas::cdp::ChannelMixdown mixdown;
        if (!onlyInputs(2) || !invert.has_value() || !mixdown.prepare(*invert))
            return false;
        port = std::move(mixdown);
        return true;
    }
    case Kind::MonoToStereo:
    {
        talas::cdp::MonoToStereo stereo;
        if (!onlyInputs(1) || !stereo.prepare())
            return false;
        port = std::move(stereo);
        return true;
    }
    case Kind::ChannelPhase:
    {
        const auto channel = whole(number("channel"), 1.0, double(maximumChannels));
        talas::cdp::ChannelPhase phase;
        if (shape.sideChannels != 0 || !channel.has_value() || !phase.prepare(inputs, *channel))
            return false;
        port = std::move(phase);
        return true;
    }
    case Kind::MonoToMany:
    {
        const auto channels = nodeChannels(2.0);
        talas::cdp::MonoToMany many;
        if (!onlyInputs(1) || !channels.has_value() || !many.prepare(*channels))
            return false;
        port = std::move(many);
        return true;
    }
    case Kind::ChannelPair:
    {
        const auto first = whole(number("first"), 1.0, double(maximumChannels));
        const auto second = whole(number("second"), 1.0, double(maximumChannels));
        talas::cdp::ChannelPair pair;
        if (shape.sideChannels != 0 || !first.has_value() || !second.has_value()
            || !pair.prepare(inputs, *first, *second))
        {
            return false;
        }
        port = std::move(pair);
        return true;
    }
    case Kind::StereoPlacement:
    {
        const auto channels = nodeChannels(2.0);
        const auto pregain = number("pregain");
        const auto shifted = flag("shifted");
        const auto text = fields.text("centres");
        const auto centres = text.has_value() ? wholeList(*text, 1.0, double(maximumChannels)) : std::nullopt;
        talas::cdp::StereoPlacement placement;
        if (shape.sideChannels != 0 || inputs % 2 != 0 || !channels.has_value() || !pregain.has_value()
            || !shifted.has_value() || !centres.has_value()
            || !placement.prepare(inputs / 2, *channels, *centres, *pregain, *shifted))
        {
            return false;
        }
        port = std::move(placement);
        return true;
    }
    case Kind::Prefix:
    {
        const auto duration = number("duration");
        talas::cdp::Prefix prefix;
        if (shape.sideChannels != 0 || !duration.has_value() || !prefix.prepare(sampleRate, inputs, *duration))
            return false;
        port = std::move(prefix);
        return true;
    }
    case Kind::SilenceMasks:
    case Kind::SilenceOverwrite:
    {
        const auto* units = fields.choice("units");
        const auto editUnits = units == nullptr ? std::optional<talas::cdp::EditUnits>{}
            : *units == "seconds"               ? std::optional{talas::cdp::EditUnits::Seconds}
            : *units == "samples"               ? std::optional{talas::cdp::EditUnits::Samples}
            : *units == "frames"                ? std::optional{talas::cdp::EditUnits::Frames}
                                                : std::optional<talas::cdp::EditUnits>{};
        const auto splice = number("splice");
        if (shape.sideChannels != 0 || !editUnits.has_value() || !splice.has_value())
            return false;
        if (tool.kind == Kind::SilenceMasks)
        {
            const auto buffer = whole(number("bufferFrames"), 0.0, 1048576.0);
            const auto text = fields.text("times");
            const auto parsed = text.has_value() ? rows(*text) : std::nullopt;
            std::vector<double> times;
            if (parsed.has_value())
            {
                for (const auto& row : *parsed)
                    times.insert(times.end(), row.begin(), row.end());
            }
            talas::cdp::SilenceMasks masks;
            if (!buffer.has_value() || !parsed.has_value()
                || !masks.prepare(sampleRate, inputs, *editUnits, times, *splice * 1000.0,
                    static_cast<std::size_t>(*buffer)))
            {
                return false;
            }
            port = std::move(masks);
            return true;
        }
        const auto time = number("time");
        const auto duration = number("duration");
        talas::cdp::SilenceOverwrite overwrite;
        if (!time.has_value() || !duration.has_value()
            || !overwrite.prepare(sampleRate, inputs, *editUnits, *time, *duration, *splice * 1000.0))
        {
            return false;
        }
        port = std::move(overwrite);
        return true;
    }
    case Kind::ToStereo:
    {
        const auto channels = nodeChannels(2.0);
        const auto start = number("start");
        const auto end = number("end");
        const auto left = whole(number("left"), 0.0, double(maximumChannels));
        const auto right = whole(number("right"), 0.0, double(maximumChannels));
        const auto mix = number("mixLevel");
        talas::cdp::ToStereo stereo;
        if (!onlyInputs(2) || !channels.has_value() || !all({start, end, mix}) || !left.has_value()
            || !right.has_value() || !stereo.prepare(sampleRate, *start, *end, *channels, *left, *right, *mix))
        {
            return false;
        }
        port = std::move(stereo);
        return true;
    }
    }
    return false;
}

// A block's live fields (in Tool::live order, nullopt for those not live
// or not taken) for the port, from the next frame it reads them on. False
// for a value the port refuses.
[[nodiscard]] inline bool setControls(
    CdpMultichannelPort& port, const std::array<std::optional<double>, 4>& controls) noexcept
{
    const auto set = [&](std::size_t index, auto&& setter) {
        return !controls[index].has_value() || setter(*controls[index]);
    };
    return std::visit(
        [&](auto& played) -> bool {
            using Port = std::decay_t<decltype(played)>;
            if constexpr (std::is_same_v<Port, talas::cdp::RotatingPan>)
                return set(0, [&](double value) { return played.setSpeed(value); });
            else if constexpr (std::is_same_v<Port, talas::cdp::SpreadPan>)
            {
                return set(0, [&](double value) { return played.setSpread(value); })
                    && set(1, [&](double value) { return played.setDepth(value); })
                    && set(2, [&](double value) { return played.setRolloff(value); });
            }
            else if constexpr (std::is_same_v<Port, talas::cdp::StepSpread>)
            {
                return set(0, [&](double value) { return played.setCentre(value); })
                    && set(1, [&](double value) { return played.setSpread(value); })
                    && set(2, [&](double value) { return played.setDepth(value); })
                    && set(3, [&](double value) { return played.setRolloff(value); });
            }
            else if constexpr (std::is_same_v<Port, talas::cdp::Antiphony>)
            {
                return set(0, [&](double value) { return played.setEventDuration(value); })
                    && set(1, [&](double value) { return played.setGap(value); });
            }
            else if constexpr (std::is_same_v<Port, talas::cdp::Flutter>)
            {
                return set(0, [&](double value) { return played.setFrequency(value); })
                    && set(1, [&](double value) { return played.setDepth(value); });
            }
            else if constexpr (std::is_same_v<Port, talas::cdp::FrameRotate>)
            {
                return set(0, [&](double value) { return played.setRotation(value); })
                    && set(1, [&](double value) { return played.setSecondRotation(value); });
            }
            else if constexpr (std::is_same_v<Port, talas::cdp::ChannelEnvelope>)
            {
                // A live gain holds the block's value.
                return set(0, [&](double value) { return played.setGain(value); });
            }
            else if constexpr (std::is_same_v<Port, std::monostate>)
                return false;
            else
                return std::ranges::none_of(controls, [](const auto& control) { return control.has_value(); });
        },
        port);
}

// Back to the port's start, as prepared.
inline void reset(CdpMultichannelPort& port) noexcept
{
    std::visit(
        [](auto& played) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(played)>, std::monostate>)
                played.reset();
        },
        port);
}

// Plays a block: the input's channels (and the side chain's) interleaved a
// stack chunk at a time into the port, and its channels written to the
// first of the outputs, the others silent, so that it allocates nothing.
// Every input must be as long as the outputs and finite, which is checked
// first, so that a refusal leaves the port as it was.
[[nodiscard]] inline bool play(CdpMultichannelPort& port, std::span<const std::span<const double>> inputs,
    std::span<const std::span<const double>> side, std::span<const std::span<double>> outputs) noexcept
{
    if (outputs.empty() || outputs.size() > maximumChannels || inputs.empty() || inputs.size() > maximumChannels
        || side.size() > maximumChannels)
    {
        return false;
    }
    const auto frames = outputs.front().size();
    const auto ready = [&](std::span<const std::span<const double>> channels) {
        return std::ranges::all_of(channels, [&](std::span<const double> channel) {
            return channel.size() == frames
                && std::ranges::all_of(channel, [](double sample) { return std::isfinite(sample); });
        });
    };
    if (!std::ranges::all_of(outputs, [&](std::span<double> output) { return output.size() == frames; })
        || !ready(inputs) || !ready(side))
    {
        return false;
    }
    const auto interleave = [&](std::span<const std::span<const double>> channels, std::size_t offset,
                                std::size_t count, std::span<double> into) {
        for (std::size_t frame = 0; frame < count; ++frame)
        {
            for (std::size_t channel = 0; channel < channels.size(); ++channel)
                into[frame * channels.size() + channel] = channels[channel][offset + frame];
        }
    };
    return std::visit(
        [&](auto& played) -> bool {
            using Port = std::decay_t<decltype(played)>;
            if constexpr (std::is_same_v<Port, std::monostate>)
                return false;
            else
            {
                const auto channels = played.outputChannels();
                if (channels == 0 || channels > outputs.size())
                    return false;
                std::array<double, maximumChannels * chunkFrames> in{};
                std::array<double, maximumChannels * chunkFrames> sideIn{};
                std::array<double, maximumChannels * chunkFrames> out{};
                for (std::size_t offset = 0; offset < frames; offset += chunkFrames)
                {
                    const auto count = std::min(chunkFrames, frames - offset);
                    const std::span<double> inChunk{in.data(), count * inputs.size()};
                    const std::span<double> outChunk{out.data(), count * channels};
                    interleave(inputs, offset, count, inChunk);
                    bool processed = false;
                    if constexpr (std::is_same_v<Port, talas::cdp::Antiphony>)
                    {
                        if (side.empty())
                            processed = played.process(inChunk, outChunk);
                        else
                        {
                            const std::span<double> sideChunk{sideIn.data(), count * side.size()};
                            interleave(side, offset, count, sideChunk);
                            processed = played.process(inChunk, sideChunk, outChunk);
                        }
                    }
                    else if constexpr (std::is_same_v<Port, talas::cdp::StereoPlacement>)
                    {
                        // Each stereo input, a pair of the inputs, on its own.
                        std::array<double, maximumChannels * chunkFrames> pairs{};
                        std::array<std::span<const double>, maximumChannels / 2> stereo{};
                        const auto count2 = inputs.size() / 2;
                        for (std::size_t pair = 0; pair < count2; ++pair)
                        {
                            const std::span<double> into{pairs.data() + pair * 2 * count, 2 * count};
                            interleave(inputs.subspan(2 * pair, 2), offset, count, into);
                            stereo[pair] = into;
                        }
                        processed = side.empty() && inputs.size() % 2 == 0
                            && played.process(std::span{stereo}.first(count2), outChunk);
                    }
                    else
                    {
                        processed = side.empty() && played.process(inChunk, outChunk);
                    }
                    if (!processed)
                        return false;
                    for (std::size_t frame = 0; frame < count; ++frame)
                    {
                        for (std::size_t channel = 0; channel < channels; ++channel)
                            outputs[channel][offset + frame] = out[frame * channels + channel];
                    }
                }
                for (auto channel = channels; channel < outputs.size(); ++channel)
                    std::ranges::fill(outputs[channel], 0.0);
                return true;
            }
        },
        port);
}
}
