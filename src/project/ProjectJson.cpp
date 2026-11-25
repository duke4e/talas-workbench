#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/ProjectEventValueSpelling.h"
#include "talas/project/SmalltalkSubset.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectExpressionAudit.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/VirtualControlMidi.h"
#include "talas/project/VirtualControlOsc.h"

#include "JsonValue.h"
#include "LegacyMixerMigration.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <array>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace talas::project
{
namespace
{
constexpr std::string_view projectFormat = "talas.project";
constexpr int versionOne = 1;
constexpr int versionTwo = 2;
constexpr int versionThree = 3;
constexpr int versionFour = 4;
constexpr int versionFive = 5;
constexpr int versionSix = 6;
constexpr int versionSeven = 7;
constexpr int versionEight = 8;
constexpr int versionNine = 9;
constexpr int versionTen = 10;
constexpr int versionEleven = 11;
constexpr int versionTwelve = 12;
constexpr int versionThirteen = 13;
constexpr int versionFourteen = 14;
constexpr int versionFifteen = 15;
constexpr int versionSixteen = 16;
constexpr int versionSeventeen = 17;
constexpr int versionEighteen = 18;
constexpr int versionNineteen = 19;
constexpr int versionTwenty = 20;
constexpr int versionTwentyOne = 21;
constexpr int versionTwentyTwo = 22;
constexpr int versionTwentyThree = 23;
constexpr int versionTwentyFour = 24;
constexpr int versionTwentyFive = 25;
constexpr int versionTwentySix = 26;
constexpr int versionTwentySeven = 27;
constexpr int versionTwentyEight = 28;
constexpr int versionTwentyNine = 29;
constexpr int versionThirty = 30;
constexpr int versionThirtyOne = 31;
constexpr int versionThirtyTwo = 32;
constexpr int versionThirtyThree = 33;
constexpr int versionThirtyFour = 34;
constexpr int versionThirtyFive = 35;
constexpr int versionThirtySix = 36;
constexpr int versionThirtySeven = 37;
constexpr int versionThirtyEight = 38;
constexpr int versionThirtyNine = 39;
constexpr int versionForty = 40;
constexpr int versionFortyOne = 41;
constexpr int versionFortyTwo = 42;
constexpr int versionFortyThree = 43;
constexpr int versionFortyFour = 44;
constexpr int versionFortyFive = 45;
constexpr int versionFortySix = 46;
constexpr int versionFortySeven = 47;
constexpr int versionFortyEight = 48;
constexpr int versionFortyNine = 49;
constexpr int versionFifty = 50;
constexpr int versionFiftyOne = 51;
constexpr int versionFiftyTwo = 52;
constexpr int versionFiftyThree = 53;
constexpr int versionFiftyFour = 54;
constexpr int versionFiftyFive = 55;
constexpr int versionFiftySix = 56;
constexpr int versionFiftySeven = 57;
constexpr int versionFiftyEight = 58;
constexpr int versionFiftyNine = 59;
constexpr int versionSixty = 60;
constexpr int versionSixtyOne = 61;
constexpr int versionSixtyTwo = 62;
constexpr int versionSixtyThree = 63;
constexpr int versionSixtyFour = 64;
constexpr int versionSixtyFive = 65;
constexpr int versionSixtySix = 66;
constexpr int versionSixtySeven = 67;
constexpr int versionSixtyEight = 68;
constexpr int versionSixtyNine = 69;
constexpr int versionSeventy = 70;
constexpr int versionSeventyOne = 71;
constexpr int versionSeventyTwo = 72;
constexpr int versionSeventyThree = 73;
constexpr int versionSeventyFour = 74;
constexpr int versionSeventyFive = 75;
constexpr int versionSeventySix = 76;
constexpr int versionSeventySeven = 77;
constexpr int versionSeventyEight = 78;
constexpr int versionSeventyNine = 79;
constexpr int versionEighty = 80;
constexpr int versionEightyOne = 81;
constexpr int versionEightyTwo = 82;
constexpr int versionEightyThree = 83;
constexpr int versionEightyFour = 84;
constexpr int versionEightyFive = 85;
constexpr int versionEightySix = 86;
constexpr int versionEightySeven = 87;
constexpr int versionEightyEight = 88;
constexpr int versionEightyNine = 89;
constexpr int versionNinety = 90;
constexpr int versionNinetyOne = 91;
constexpr int versionNinetyTwo = 92;
constexpr int versionNinetyThree = 93;
constexpr int versionNinetyFour = 94;
constexpr int versionNinetyFive = 95;
constexpr int versionNinetySix = 96;
constexpr int versionNinetySeven = 97;
constexpr int versionNinetyEight = 98;
constexpr int versionNinetyNine = 99;
constexpr int versionOneHundred = 100;
constexpr int versionOneHundredOne = 101;
constexpr int versionOneHundredTwo = 102;
constexpr int versionOneHundredThree = 103;
constexpr int versionOneHundredFour = 104;
constexpr int versionOneHundredFive = 105;
constexpr int versionOneHundredSix = 106;
constexpr int versionOneHundredSeven = 107;
constexpr int versionOneHundredEight = 108;
constexpr int versionOneHundredNine = 109;
constexpr int versionOneHundredTen = 110;
constexpr int versionOneHundredEleven = 111;
constexpr int versionOneHundredTwelve = 112;
constexpr int versionOneHundredThirteen = 113;
constexpr int versionOneHundredFourteen = 114;
constexpr int versionOneHundredFifteen = 115;
constexpr int versionOneHundredSixteen = 116;
constexpr int versionOneHundredSeventeen = 117;
constexpr int versionOneHundredEighteen = 118;
constexpr int versionOneHundredNineteen = 119;
constexpr int versionOneHundredTwenty = 120;
constexpr int versionOneHundredTwentyOne = 121;
constexpr int versionOneHundredTwentyTwo = 122;
constexpr int versionOneHundredTwentyThree = 123;
constexpr int versionOneHundredTwentyFour = 124;
constexpr int versionOneHundredTwentyFive = 125;
constexpr int versionOneHundredTwentySix = 126;
constexpr int versionOneHundredTwentySeven = 127;
constexpr int versionOneHundredTwentyEight = 128;
constexpr int versionOneHundredTwentyNine = 129;
constexpr int versionOneHundredThirty = 130;
constexpr int versionOneHundredThirtyOne = 131;
constexpr int versionOneHundredThirtyTwo = 132;
constexpr int versionOneHundredThirtyThree = 133;
constexpr int versionOneHundredThirtyFour = 134;
constexpr int versionOneHundredThirtyFive = 135;
constexpr int versionOneHundredThirtySix = 136;
constexpr int versionOneHundredThirtySeven = 137;
constexpr int versionOneHundredThirtyEight = 138;
constexpr int versionOneHundredThirtyNine = 139;
constexpr int versionOneHundredForty = 140;
constexpr int versionOneHundredFortyOne = 141;
constexpr int versionOneHundredFortyTwo = 142;
constexpr int versionOneHundredFortyThree = 143;
constexpr int versionOneHundredFortyFour = 144;
constexpr int versionOneHundredFortyFive = 145;
constexpr int versionOneHundredFortySix = 146;
constexpr int versionOneHundredFortySeven = 147;
constexpr int versionOneHundredFortyEight = 148;
constexpr int versionOneHundredFortyNine = 149;
constexpr int versionOneHundredFifty = 150;
constexpr int versionOneHundredFiftyOne = 151;
constexpr int versionOneHundredFiftyTwo = 152;
constexpr int versionOneHundredFiftyThree = 153;
constexpr int versionOneHundredFiftyFour = 154;
constexpr int versionOneHundredFiftyFive = 155;
constexpr int versionOneHundredFiftySix = 156;
constexpr int supportedVersion = currentProjectFormatVersion;

// Format 136: a lane's transformations by name.
[[nodiscard]] std::string_view scoreAutomationTransformName(ScoreAutomationTransformKind kind) noexcept
{
    switch (kind)
    {
    case ScoreAutomationTransformKind::Invert:
        return "invert";
    case ScoreAutomationTransformKind::Reverse:
        return "reverse";
    case ScoreAutomationTransformKind::Loop:
        return "loop";
    case ScoreAutomationTransformKind::SquareOff:
        return "squareOff";
    case ScoreAutomationTransformKind::Scale:
        return "scale";
    case ScoreAutomationTransformKind::Offset:
        return "offset";
    case ScoreAutomationTransformKind::Threshold:
        return "threshold";
    case ScoreAutomationTransformKind::TimeScale:
        return "timeScale";
    case ScoreAutomationTransformKind::Spray:
        return "spray";
    case ScoreAutomationTransformKind::ScaleToFit:
        return "scaleToFit";
    case ScoreAutomationTransformKind::QuantizeEndpoints:
        return "quantizeEndpoints";
    }
    return "invert";
}

[[nodiscard]] std::optional<ScoreAutomationTransformKind> parseScoreAutomationTransformName(std::string_view name) noexcept
{
    if (name == "invert")
        return ScoreAutomationTransformKind::Invert;
    if (name == "reverse")
        return ScoreAutomationTransformKind::Reverse;
    if (name == "loop")
        return ScoreAutomationTransformKind::Loop;
    if (name == "squareOff")
        return ScoreAutomationTransformKind::SquareOff;
    if (name == "scale")
        return ScoreAutomationTransformKind::Scale;
    if (name == "offset")
        return ScoreAutomationTransformKind::Offset;
    if (name == "threshold")
        return ScoreAutomationTransformKind::Threshold;
    if (name == "timeScale")
        return ScoreAutomationTransformKind::TimeScale;
    if (name == "spray")
        return ScoreAutomationTransformKind::Spray;
    if (name == "scaleToFit")
        return ScoreAutomationTransformKind::ScaleToFit;
    if (name == "quantizeEndpoints")
        return ScoreAutomationTransformKind::QuantizeEndpoints;
    return std::nullopt;
}

[[nodiscard]] std::string_view expressionLanguageName(
    talas::expr::ExpressionLanguageVersion language) noexcept
{
    switch (language)
    {
    case talas::expr::ExpressionLanguageVersion::LegacyV1:
        return "legacyV1";
    case talas::expr::ExpressionLanguageVersion::PureV2:
        return "pureV2";
    case talas::expr::ExpressionLanguageVersion::PureV3:
        return "pureV3";
    case talas::expr::ExpressionLanguageVersion::PureV4:
        return "pureV4";
    case talas::expr::ExpressionLanguageVersion::PureV5:
        return "pureV5";
    case talas::expr::ExpressionLanguageVersion::SessionV1:
        return "sessionV1";
    case talas::expr::ExpressionLanguageVersion::VariablesV1:
        return "variablesV1";
    case talas::expr::ExpressionLanguageVersion::VariablesV2:
        return "variablesV2";
    case talas::expr::ExpressionLanguageVersion::VariablesV3:
        return "variablesV3";
    case talas::expr::ExpressionLanguageVersion::VariablesV4:
        return "variablesV4";
    case talas::expr::ExpressionLanguageVersion::ValV1:
        return "valV1";
    case talas::expr::ExpressionLanguageVersion::VoiceV1:
        return "voiceV1";
    }
    return {};
}

[[nodiscard]] std::optional<talas::expr::ExpressionLanguageVersion>
parseExpressionLanguage(std::string_view value) noexcept
{
    if (value == "legacyV1")
        return talas::expr::ExpressionLanguageVersion::LegacyV1;
    if (value == "pureV2")
        return talas::expr::ExpressionLanguageVersion::PureV2;
    if (value == "pureV3")
        return talas::expr::ExpressionLanguageVersion::PureV3;
    if (value == "pureV4")
        return talas::expr::ExpressionLanguageVersion::PureV4;
    if (value == "pureV5")
        return talas::expr::ExpressionLanguageVersion::PureV5;
    if (value == "sessionV1")
        return talas::expr::ExpressionLanguageVersion::SessionV1;
    if (value == "variablesV1")
        return talas::expr::ExpressionLanguageVersion::VariablesV1;
    if (value == "variablesV2")
        return talas::expr::ExpressionLanguageVersion::VariablesV2;
    if (value == "variablesV3")
        return talas::expr::ExpressionLanguageVersion::VariablesV3;
    if (value == "variablesV4")
        return talas::expr::ExpressionLanguageVersion::VariablesV4;
    if (value == "valV1")
        return talas::expr::ExpressionLanguageVersion::ValV1;
    if (value == "voiceV1")
        return talas::expr::ExpressionLanguageVersion::VoiceV1;
    return std::nullopt;
}

[[nodiscard]] std::string_view valueDomainName(
    talas::expr::ValueDomain domain);
[[nodiscard]] std::string_view parameterKindName(
    talas::core::ParameterKind kind);
[[nodiscard]] std::optional<talas::expr::ValueDomain> parseValueDomain(
    std::string_view value);
[[nodiscard]] std::optional<talas::core::ParameterKind> parseParameterKind(
    std::string_view value);

[[nodiscard]] std::string_view portTypeName(talas::core::PortType type)
{
    switch (type)
    {
    case talas::core::PortType::Audio:
        return "audio";
    case talas::core::PortType::Control:
        return "control";
    case talas::core::PortType::Event:
        return "event";
    case talas::core::PortType::SampleBuffer:
        return "sampleBuffer";
    case talas::core::PortType::Wavetable:
        return "wavetable";
    case talas::core::PortType::Spectrum:
        return "spectrum";
    case talas::core::PortType::ComplexSpectrum:
        return "complexSpectrum";
    case talas::core::PortType::NoteStream:
        return "noteStream";
    }
    return {};
}

[[nodiscard]] std::optional<talas::core::PortType>
parsePortType(std::string_view value)
{
    if (value == "audio")
        return talas::core::PortType::Audio;
    if (value == "control")
        return talas::core::PortType::Control;
    if (value == "event")
        return talas::core::PortType::Event;
    if (value == "sampleBuffer")
        return talas::core::PortType::SampleBuffer;
    if (value == "wavetable")
        return talas::core::PortType::Wavetable;
    if (value == "spectrum")
        return talas::core::PortType::Spectrum;
    if (value == "complexSpectrum")
        return talas::core::PortType::ComplexSpectrum;
    if (value == "noteStream")
        return talas::core::PortType::NoteStream;
    return std::nullopt;
}

[[nodiscard]] std::string_view automationInterpolationName(
    talas::core::AutomationInterpolation interpolation)
{
    switch (interpolation)
    {
    case talas::core::AutomationInterpolation::Step:
        return "step";
    case talas::core::AutomationInterpolation::Linear:
        return "linear";
    }
    return {};
}

[[nodiscard]] std::optional<talas::core::AutomationInterpolation>
parseAutomationInterpolation(std::string_view value)
{
    if (value == "step")
        return talas::core::AutomationInterpolation::Step;
    if (value == "linear")
        return talas::core::AutomationInterpolation::Linear;
    return std::nullopt;
}

[[nodiscard]] std::string_view scoreTimeDomainName(ScoreTimeDomain domain)
{
    switch (domain)
    {
    case ScoreTimeDomain::Seconds:
        return "seconds";
    case ScoreTimeDomain::MusicalTicks:
        return "musicalTicks";
    }
    return {};
}

[[nodiscard]] std::optional<ScoreTimeDomain> parseScoreTimeDomain(std::string_view value)
{
    if (value == "seconds")
        return ScoreTimeDomain::Seconds;
    if (value == "musicalTicks")
        return ScoreTimeDomain::MusicalTicks;
    return std::nullopt;
}

[[nodiscard]] std::string_view scoreTrackKindName(ScoreTrackKind kind)
{
    switch (kind)
    {
    case ScoreTrackKind::Track:
        return "track";
    case ScoreTrackKind::Submix:
        return "submix";
    case ScoreTrackKind::Master:
        return "master";
    }
    return {};
}

[[nodiscard]] std::optional<ScoreTrackKind> parseScoreTrackKind(std::string_view value)
{
    if (value == "track")
        return ScoreTrackKind::Track;
    if (value == "submix")
        return ScoreTrackKind::Submix;
    if (value == "master")
        return ScoreTrackKind::Master;
    return std::nullopt;
}

[[nodiscard]] std::string_view scoreOverlapModeName(ScoreOverlapMode mode)
{
    switch (mode)
    {
    case ScoreOverlapMode::Reject:
        return "reject";
    case ScoreOverlapMode::Layer:
        return "layer";
    case ScoreOverlapMode::Replace:
        return "replace";
    }
    return {};
}

[[nodiscard]] std::optional<ScoreOverlapMode> parseScoreOverlapMode(std::string_view value)
{
    if (value == "reject")
        return ScoreOverlapMode::Reject;
    if (value == "layer")
        return ScoreOverlapMode::Layer;
    if (value == "replace")
        return ScoreOverlapMode::Replace;
    return std::nullopt;
}

[[nodiscard]] std::string_view scoreClipRenderModeName(
    ScoreClipRenderMode mode)
{
    switch (mode)
    {
    case ScoreClipRenderMode::Cached:
        return "cached";
    case ScoreClipRenderMode::Frozen:
        return "frozen";
    }
    throw std::invalid_argument("Unknown score clip render mode");
}

[[nodiscard]] std::optional<ScoreClipRenderMode>
parseScoreClipRenderMode(std::string_view value) noexcept
{
    if (value == "cached")
        return ScoreClipRenderMode::Cached;
    if (value == "frozen")
        return ScoreClipRenderMode::Frozen;
    return std::nullopt;
}

[[nodiscard]] std::string_view scoreAutomationScopeName(ScoreAutomationScope scope)
{
    switch (scope)
    {
    case ScoreAutomationScope::Clip:
        return "clip";
    case ScoreAutomationScope::Track:
        return "track";
    case ScoreAutomationScope::Master:
        return "master";
    }
    return {};
}

[[nodiscard]] std::optional<ScoreAutomationScope> parseScoreAutomationScope(std::string_view value)
{
    if (value == "clip")
        return ScoreAutomationScope::Clip;
    if (value == "track")
        return ScoreAutomationScope::Track;
    if (value == "master")
        return ScoreAutomationScope::Master;
    return std::nullopt;
}

[[nodiscard]] std::string_view scoreAutomationStateName(ScoreAutomationState state)
{
    switch (state)
    {
    case ScoreAutomationState::Automated:
        return "automated";
    case ScoreAutomationState::Live:
        return "live";
    case ScoreAutomationState::Record:
        return "record";
    }
    return {};
}

[[nodiscard]] std::optional<ScoreAutomationState> parseScoreAutomationState(std::string_view value)
{
    if (value == "automated")
        return ScoreAutomationState::Automated;
    if (value == "live")
        return ScoreAutomationState::Live;
    if (value == "record")
        return ScoreAutomationState::Record;
    return std::nullopt;
}

[[nodiscard]] std::string_view liveSetQuantizationName(
    ProjectLiveSetQuantizationKind kind)
{
    switch (kind)
    {
    case ProjectLiveSetQuantizationKind::Immediate: return "immediate";
    case ProjectLiveSetQuantizationKind::NextBeat: return "nextBeat";
    case ProjectLiveSetQuantizationKind::NextBar: return "nextBar";
    case ProjectLiveSetQuantizationKind::TickInterval: return "tickInterval";
    }
    return {};
}

[[nodiscard]] std::optional<ProjectLiveSetQuantizationKind>
parseLiveSetQuantization(std::string_view value)
{
    if (value == "immediate") return ProjectLiveSetQuantizationKind::Immediate;
    if (value == "nextBeat") return ProjectLiveSetQuantizationKind::NextBeat;
    if (value == "nextBar") return ProjectLiveSetQuantizationKind::NextBar;
    if (value == "tickInterval") return ProjectLiveSetQuantizationKind::TickInterval;
    return std::nullopt;
}

[[nodiscard]] std::string_view liveToolVariableKindName(
    ProjectLiveToolVariableKind kind)
{
    switch (kind)
    {
    case ProjectLiveToolVariableKind::Number: return "number";
    case ProjectLiveToolVariableKind::Boolean: return "boolean";
    }
    return {};
}

[[nodiscard]] std::optional<ProjectLiveToolVariableKind>
parseLiveToolVariableKind(std::string_view value)
{
    if (value == "number") return ProjectLiveToolVariableKind::Number;
    if (value == "boolean") return ProjectLiveToolVariableKind::Boolean;
    return std::nullopt;
}

[[nodiscard]] std::string_view liveToolTriggerKindName(
    ProjectLiveToolTriggerKind kind)
{
    switch (kind)
    {
    case ProjectLiveToolTriggerKind::Manual: return "manual";
    case ProjectLiveToolTriggerKind::EventValue: return "eventValue";
    case ProjectLiveToolTriggerKind::VariableChange: return "variableChange";
    case ProjectLiveToolTriggerKind::MidiProgram: return "midiProgram";
    case ProjectLiveToolTriggerKind::Osc: return "osc";
    case ProjectLiveToolTriggerKind::PeriodicTicks: return "periodicTicks";
    }
    return {};
}

[[nodiscard]] std::optional<ProjectLiveToolTriggerKind>
parseLiveToolTriggerKind(std::string_view value)
{
    if (value == "manual") return ProjectLiveToolTriggerKind::Manual;
    if (value == "eventValue") return ProjectLiveToolTriggerKind::EventValue;
    if (value == "variableChange") return ProjectLiveToolTriggerKind::VariableChange;
    if (value == "midiProgram") return ProjectLiveToolTriggerKind::MidiProgram;
    if (value == "osc") return ProjectLiveToolTriggerKind::Osc;
    if (value == "periodicTicks") return ProjectLiveToolTriggerKind::PeriodicTicks;
    return std::nullopt;
}

[[nodiscard]] std::string_view liveToolCommandKindName(
    ProjectLiveToolCommandKind kind)
{
    switch (kind)
    {
    case ProjectLiveToolCommandKind::ActivateCell: return "activateCell";
    case ProjectLiveToolCommandKind::Stop: return "stop";
    case ProjectLiveToolCommandKind::SetProjectVariable: return "setProjectVariable";
    case ProjectLiveToolCommandKind::SetWorkspaceVariable: return "setWorkspaceVariable";
    case ProjectLiveToolCommandKind::SetEventValue: return "setEventValue";
    }
    return {};
}

[[nodiscard]] std::optional<ProjectLiveToolCommandKind>
parseLiveToolCommandKind(std::string_view value)
{
    if (value == "activateCell") return ProjectLiveToolCommandKind::ActivateCell;
    if (value == "stop") return ProjectLiveToolCommandKind::Stop;
    if (value == "setProjectVariable") return ProjectLiveToolCommandKind::SetProjectVariable;
    if (value == "setWorkspaceVariable") return ProjectLiveToolCommandKind::SetWorkspaceVariable;
    if (value == "setEventValue") return ProjectLiveToolCommandKind::SetEventValue;
    return std::nullopt;
}

void appendIndent(std::string& output, int spaces)
{
    output.append(static_cast<std::size_t>(spaces), ' ');
}

void appendQuoted(std::string& output, std::string_view value)
{
    json::appendEscapedString(output, value);
}

void appendQuotedField(std::string& output, int indent, std::string_view key, std::string_view value, bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    appendQuoted(output, value);
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendIntegerField(std::string& output, int indent, std::string_view key, std::int64_t value, bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    output += std::to_string(value);
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendUnsignedIntegerField(
    std::string& output,
    int indent,
    std::string_view key,
    std::uint64_t value,
    bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    output += std::to_string(value);
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

std::string numberText(double value)
{
    char buffer[64]{};
    const auto converted = std::to_chars(
        buffer,
        buffer + sizeof(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{})
        return "0";
    return std::string(buffer, converted.ptr);
}

void appendNumberField(std::string& output, int indent, std::string_view key, double value, bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    output += numberText(value);
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendBooleanField(std::string& output, int indent, std::string_view key, bool value, bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    output += value ? "true" : "false";
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendParameters(
    std::string& output,
    const std::vector<ProjectParameterExpression>& parameters,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += R"json("parameters": )json";
    if (parameters.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto& parameter = parameters[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendQuotedField(output, indent + 4, "id", parameter.parameterId, true);
        const bool writesExpression = parameter.expression.has_value()
            || (!parameter.choice.has_value() && !parameter.boolean.has_value() && !parameter.resource.has_value() && !parameter.text.has_value());
        const bool writesChoice = parameter.choice.has_value();
        const bool writesBoolean = parameter.boolean.has_value();
        const bool writesResource = parameter.resource.has_value();
        const bool writesText = parameter.text.has_value();
        int valueFieldCount = 0;
        if (writesExpression)
            ++valueFieldCount;
        if (writesChoice)
            ++valueFieldCount;
        if (writesBoolean)
            ++valueFieldCount;
        if (writesResource)
            ++valueFieldCount;
        if (writesText)
            ++valueFieldCount;

        int writtenValueFields = 0;
        if (writesExpression)
            appendQuotedField(output, indent + 4, "expression", parameter.expression.value_or(""), ++writtenValueFields < valueFieldCount);
        if (writesChoice)
            appendQuotedField(output, indent + 4, "choice", *parameter.choice, ++writtenValueFields < valueFieldCount);
        if (writesBoolean)
            appendBooleanField(output, indent + 4, "boolean", *parameter.boolean, ++writtenValueFields < valueFieldCount);
        if (writesResource)
            appendQuotedField(output, indent + 4, "resource", *parameter.resource, ++writtenValueFields < valueFieldCount);
        if (writesText)
            appendQuotedField(output, indent + 4, "text", *parameter.text, ++writtenValueFields < valueFieldCount);
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < parameters.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendPreservedPorts(
    std::string& output,
    std::string_view key,
    const std::vector<ProjectPreservedPort>& ports,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    if (ports.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }
    output += "[\n";
    for (std::size_t index = 0; index < ports.size(); ++index)
    {
        const auto& port = ports[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendQuotedField(output, indent + 4, "id", port.portId, true);
        appendQuotedField(
            output,
            indent + 4,
            "type",
            portTypeName(port.type),
            true);
        appendBooleanField(
            output,
            indent + 4,
            "required",
            port.required,
            true);
        appendIndent(output, indent + 4);
        output += "\"signal\": {\n";
        appendQuotedField(
            output,
            indent + 6,
            "rate",
            talas::core::portSignalRateName(port.signal.rate),
            true);
        appendQuotedField(
            output,
            indent + 6,
            "audioLayout",
            talas::core::audioBusLayoutKindName(port.signal.audioLayout),
            true);
        appendQuotedField(
            output,
            indent + 6,
            "edge",
            talas::core::portEdgeSemanticsName(port.signal.edge),
            true);
        appendQuotedField(
            output,
            indent + 6,
            "spectralFormat",
            talas::core::spectralFrameFormatName(
                port.signal.spectralFormat),
            true);
        appendBooleanField(
            output,
            indent + 6,
            "timelineBearing",
            port.signal.timelineBearing,
            false);
        appendIndent(output, indent + 4);
        output += "}\n";
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < ports.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendPreservedParameters(
    std::string& output,
    const std::vector<ProjectPreservedParameter>& parameters,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += "\"parameters\": ";
    if (parameters.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }
    output += "[\n";
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto& parameter = parameters[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendQuotedField(
            output,
            indent + 4,
            "id",
            parameter.parameterId,
            true);
        appendQuotedField(
            output,
            indent + 4,
            "kind",
            parameterKindName(parameter.kind),
            true);
        appendQuotedField(
            output,
            indent + 4,
            "domain",
            valueDomainName(parameter.domain),
            true);
        appendIndent(output, indent + 4);
        output += "\"choices\": [";
        for (std::size_t choiceIndex = 0;
             choiceIndex < parameter.choices.size();
             ++choiceIndex)
        {
            if (choiceIndex != 0)
                output += ", ";
            appendQuoted(output, parameter.choices[choiceIndex]);
        }
        output += "]\n";
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < parameters.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendPreservedComponent(
    std::string& output,
    const ProjectPreservedComponent& component,
    int indent)
{
    appendIndent(output, indent);
    output += "\"preservedComponent\": {\n";
    appendIntegerField(
        output,
        indent + 2,
        "schemaVersion",
        component.schemaVersion,
        true);
    appendQuotedField(
        output,
        indent + 2,
        "typeVersion",
        component.typeVersion,
        true);
    appendIntegerField(
        output,
        indent + 2,
        "stateSchemaVersion",
        component.stateSchemaVersion,
        true);
    appendQuotedField(
        output,
        indent + 2,
        "stateEncoding",
        "base64",
        true);
    appendQuotedField(
        output,
        indent + 2,
        "state",
        encodeProjectComponentState(component.state),
        true);
    appendQuotedField(
        output,
        indent + 2,
        "displayName",
        component.displayName,
        true);
    appendQuotedField(
        output,
        indent + 2,
        "bundleId",
        component.bundleId,
        true);
    appendPreservedPorts(
        output,
        "inputs",
        component.inputs,
        indent + 2,
        true);
    appendPreservedPorts(
        output,
        "outputs",
        component.outputs,
        indent + 2,
        true);
    appendPreservedParameters(
        output,
        component.parameters,
        indent + 2,
        true);
    appendQuotedField(
        output,
        indent + 2,
        "recoveryHint",
        component.recoveryHint,
        false);
    appendIndent(output, indent);
    output += "}\n";
}

void appendNodes(std::string& output, const std::vector<ProjectNode>& nodes, int indent)
{
    appendIndent(output, indent);
    output += R"json("nodes": )json";
    if (nodes.empty())
    {
        output += "[]";
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < nodes.size(); ++index)
    {
        const auto& node = nodes[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendQuotedField(output, indent + 4, "id", node.instanceId, true);
        if (!node.typeId.empty())
            appendQuotedField(output, indent + 4, "typeId", node.typeId, true);
        if (node.soundDefinitionId.has_value())
        {
            appendQuotedField(
                output,
                indent + 4,
                "soundDefinitionId",
                *node.soundDefinitionId,
                true);
        }
        appendParameters(
            output,
            node.parameters,
            indent + 4,
            node.preservedComponent.has_value());
        if (node.preservedComponent.has_value())
        {
            appendPreservedComponent(
                output,
                *node.preservedComponent,
                indent + 4);
        }
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < nodes.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
}

void appendConnection(std::string& output, const talas::core::Connection& connection, int indent)
{
    appendIndent(output, indent);
    output += "{\n";
    appendQuotedField(output, indent + 2, "fromNode", connection.fromNode, true);
    appendQuotedField(output, indent + 2, "fromPort", connection.fromPort, true);
    appendQuotedField(output, indent + 2, "toNode", connection.toNode, true);
    appendQuotedField(output, indent + 2, "toPort", connection.toPort, false);
    appendIndent(output, indent);
    output.push_back('}');
}

void appendConnections(std::string& output, const std::vector<talas::core::Connection>& connections, int indent)
{
    appendIndent(output, indent);
    output += R"json("connections": )json";
    if (connections.empty())
    {
        output += "[]\n";
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < connections.size(); ++index)
    {
        appendConnection(output, connections[index], indent + 2);
        if (index + 1 < connections.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output += "]\n";
}

void appendGraphField(
    std::string& output,
    std::string_view key,
    const ProjectGraph& graph,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": {\n";
    appendNodes(output, graph.nodes, indent + 2);
    output += ",\n";
    appendConnections(output, graph.connections, indent + 2);
    appendIndent(output, indent);
    output.push_back('}');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendSoundPorts(
    std::string& output,
    std::string_view key,
    const std::vector<ProjectSoundPort>& ports,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    if (ports.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < ports.size(); ++index)
    {
        const auto& port = ports[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendQuotedField(output, indent + 4, "id", port.portId, true);
        appendQuotedField(output, indent + 4, "type", portTypeName(port.type), true);
        appendQuotedField(output, indent + 4, "nodeId", port.nodeId, true);
        appendQuotedField(output, indent + 4, "nodePortId", port.nodePortId, port.replacesSound);
        // Format 138: the replaceable input.
        if (port.replacesSound)
            appendBooleanField(output, indent + 4, "replaces", true, false);
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < ports.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

[[nodiscard]] std::string_view parameterKindName(
    talas::core::ParameterKind kind)
{
    switch (kind)
    {
    case talas::core::ParameterKind::Expression: return "expression";
    case talas::core::ParameterKind::Choice: return "choice";
    case talas::core::ParameterKind::Boolean: return "boolean";
    case talas::core::ParameterKind::Resource: return "resource";
    case talas::core::ParameterKind::Text: return "text";
    }
    return "expression";
}

[[nodiscard]] std::string_view soundParameterTargetName(
    ProjectSoundParameterTarget target)
{
    switch (target)
    {
    case ProjectSoundParameterTarget::NodeParameter:
        return "nodeParameter";
    case ProjectSoundParameterTarget::EventValue:
        return "eventValue";
    case ProjectSoundParameterTarget::Variable:
        return "variable";
    }
    return "unknown";
}

void appendStringArrayField(
    std::string& output,
    int indent,
    std::string_view key,
    const std::vector<std::string>& values,
    bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    if (values.empty())
    {
        output += "[]";
    }
    else
    {
        output += "[\n";
        for (std::size_t index = 0; index < values.size(); ++index)
        {
            appendIndent(output, indent + 2);
            appendQuoted(output, values[index]);
            if (index + 1 < values.size())
                output.push_back(',');
            output.push_back('\n');
        }
        appendIndent(output, indent);
        output.push_back(']');
    }
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

// Format 139: a Sound class's icon, each row of dots as hexadecimal digits.
template <typename Row>
[[nodiscard]] std::vector<std::string> soundIconRowsText(const std::vector<Row>& rows)
{
    std::vector<std::string> text;
    text.reserve(rows.size());
    for (const auto row : rows)
    {
        std::string digits(sizeof(Row) * 2, '0');
        for (std::size_t index = 0; index < digits.size(); ++index)
            digits[digits.size() - 1 - index] = "0123456789abcdef"[(row >> (4 * index)) & 0xfU];
        text.push_back(std::move(digits));
    }
    return text;
}

void appendSoundIcon(std::string& output, const ProjectSoundIcon& icon)
{
    appendIndent(output, 6);
    output += "\"icon\": {\n";
    const auto fields = static_cast<int>(!icon.sameAs.empty()) + static_cast<int>(!icon.large.empty())
        + static_cast<int>(!icon.small.empty()) + static_cast<int>(icon.png) + static_cast<int>(!icon.inputType.empty())
        + static_cast<int>(!icon.outputType.empty()) + static_cast<int>(icon.sideEffects)
        + static_cast<int>(icon.inputChannels != 0) + static_cast<int>(icon.outputChannels != 0);
    auto written = 0;
    // Format 143: the numbers of input and output channels.
    if (icon.inputChannels != 0)
        appendIntegerField(output, 8, "inputChannels", icon.inputChannels, ++written < fields);
    if (icon.outputChannels != 0)
        appendIntegerField(output, 8, "outputChannels", icon.outputChannels, ++written < fields);
    // Format 141: the input and output types and the side effects box.
    if (!icon.inputType.empty())
        appendQuotedField(output, 8, "inputType", icon.inputType, ++written < fields);
    if (!icon.outputType.empty())
        appendQuotedField(output, 8, "outputType", icon.outputType, ++written < fields);
    if (icon.sideEffects)
        appendBooleanField(output, 8, "sideEffects", true, ++written < fields);
    if (!icon.sameAs.empty())
        appendQuotedField(output, 8, "sameAs", icon.sameAs, ++written < fields);
    if (!icon.large.empty())
        appendStringArrayField(output, 8, "large", soundIconRowsText(icon.large), ++written < fields);
    if (!icon.small.empty())
        appendStringArrayField(output, 8, "small", soundIconRowsText(icon.small), ++written < fields);
    if (icon.png)
        appendBooleanField(output, 8, "png", true, ++written < fields);
    appendIndent(output, 6);
    output += "},\n";
}

void appendSoundParameterDefault(
    std::string& output,
    const ProjectParameterExpression& value,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += "\"default\": {\n";
    const bool writesExpression = value.expression.has_value()
        || (!value.choice.has_value() && !value.boolean.has_value()
            && !value.resource.has_value() && !value.text.has_value());
    const auto fieldCount = static_cast<int>(writesExpression)
        + static_cast<int>(value.choice.has_value())
        + static_cast<int>(value.boolean.has_value())
        + static_cast<int>(value.resource.has_value())
        + static_cast<int>(value.text.has_value());
    int written = 0;
    if (writesExpression)
    {
        appendQuotedField(
            output,
            indent + 2,
            "expression",
            value.expression.value_or(""),
            ++written < fieldCount);
    }
    if (value.choice.has_value())
    {
        appendQuotedField(
            output,
            indent + 2,
            "choice",
            *value.choice,
            ++written < fieldCount);
    }
    if (value.boolean.has_value())
    {
        appendBooleanField(
            output,
            indent + 2,
            "boolean",
            *value.boolean,
            ++written < fieldCount);
    }
    if (value.resource.has_value())
    {
        appendQuotedField(
            output,
            indent + 2,
            "resource",
            *value.resource,
            ++written < fieldCount);
    }
    if (value.text.has_value())
    {
        appendQuotedField(
            output,
            indent + 2,
            "text",
            *value.text,
            ++written < fieldCount);
    }
    appendIndent(output, indent);
    output.push_back('}');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendSoundParameterBindings(
    std::string& output,
    const std::vector<ProjectSoundParameterBinding>& bindings,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += "\"bindings\": ";
    if (bindings.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }
    output += "[\n";
    for (std::size_t index = 0; index < bindings.size(); ++index)
    {
        const auto& binding = bindings[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendQuotedField(
            output,
            indent + 4,
            "target",
            soundParameterTargetName(binding.target),
            true);
        if (binding.target == ProjectSoundParameterTarget::NodeParameter)
        {
            appendQuotedField(
                output,
                indent + 4,
                "nodeId",
                binding.nodeId,
                true);
            appendQuotedField(
                output,
                indent + 4,
                "parameterId",
                binding.parameterId,
                false);
        }
        else if (binding.target == ProjectSoundParameterTarget::Variable)
        {
            appendQuotedField(
                output,
                indent + 4,
                "variableName",
                binding.variableName,
                false);
        }
        else
        {
            appendQuotedField(
                output,
                indent + 4,
                "eventName",
                binding.eventName,
                true);
            appendQuotedField(
                output,
                indent + 4,
                "domain",
                valueDomainName(binding.eventDomain),
                false);
        }
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < bindings.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendSoundExposedParameters(
    std::string& output,
    const std::vector<ProjectSoundExposedParameter>& parameters,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += "\"parameters\": ";
    if (parameters.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }
    output += "[\n";
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto& parameter = parameters[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendQuotedField(
            output,
            indent + 4,
            "id",
            parameter.parameterId,
            true);
        appendQuotedField(
            output,
            indent + 4,
            "name",
            parameter.displayName,
            true);
        appendQuotedField(
            output,
            indent + 4,
            "kind",
            parameterKindName(parameter.kind),
            true);
        appendQuotedField(
            output,
            indent + 4,
            "domain",
            valueDomainName(parameter.domain),
            true);
        appendStringArrayField(
            output,
            indent + 4,
            "choices",
            parameter.choices,
            true);
        appendSoundParameterDefault(
            output,
            parameter.defaultValue,
            indent + 4,
            true);
        appendStringArrayField(
            output,
            indent + 4,
            "aliases",
            parameter.aliases,
            true);
        // Format 99: the Class Editor's range, field and comment, written
        // only when set.
        const auto classFields = parameter.minimum.has_value() || parameter.maximum.has_value()
            || parameter.field.has_value() || !parameter.comment.empty()
            || parameter.fieldKind != ProjectSoundFieldKind::Default;
        appendSoundParameterBindings(
            output,
            parameter.bindings,
            indent + 4,
            classFields);
        if (classFields)
        {
            std::vector<std::string> members;
            const auto number = [](double value) { return numberText(value); };
            if (parameter.minimum.has_value())
                members.push_back("\"minimum\": " + number(*parameter.minimum));
            if (parameter.maximum.has_value())
                members.push_back("\"maximum\": " + number(*parameter.maximum));
            if (parameter.field.has_value())
            {
                members.push_back("\"field\": {\"left\": " + number(parameter.field->left) + ", \"top\": "
                    + number(parameter.field->top) + ", \"right\": " + number(parameter.field->right)
                    + ", \"bottom\": " + number(parameter.field->bottom) + "}");
            }
            // Format 145: the kind of field.
            if (parameter.fieldKind != ProjectSoundFieldKind::Default)
                members.push_back("\"fieldKind\": \"" + std::string{projectSoundFieldKindName(parameter.fieldKind)} + "\"");
            for (std::size_t member = 0; member < members.size(); ++member)
            {
                appendIndent(output, indent + 4);
                output += members[member];
                output += member + 1 < members.size() || !parameter.comment.empty() ? ",\n" : "\n";
            }
            if (!parameter.comment.empty())
                appendQuotedField(output, indent + 4, "comment", parameter.comment, false);
        }
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < parameters.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendSoundDefinitions(
    std::string& output,
    const std::vector<ProjectSoundDefinition>& definitions)
{
    output += "  \"soundDefinitions\": ";
    if (definitions.empty())
    {
        output += "[]";
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < definitions.size(); ++index)
    {
        const auto& definition = definitions[index];
        output += "    {\n";
        appendQuotedField(output, 6, "id", definition.definitionId, true);
        appendQuotedField(output, 6, "name", definition.displayName, true);
        // Format 99: the Class Description, written only when it has one.
        if (!definition.description.empty())
            appendQuotedField(output, 6, "description", definition.description, true);
        // Format 105: Expand unchecked, written only then.
        if (!definition.expandsBeforeTransformation)
            appendBooleanField(output, 6, "expandsBeforeTransformation", false, true);
        // Format 139: the class's icon, written only when it has one.
        if (definition.icon.has_value())
            appendSoundIcon(output, *definition.icon);
        appendIntegerField(
            output,
            6,
            "interfaceVersion",
            definition.interfaceVersion,
            true);
        appendSoundPorts(output, "inputs", definition.inputs, 6, true);
        appendSoundPorts(output, "outputs", definition.outputs, 6, true);
        appendSoundExposedParameters(
            output,
            definition.exposedParameters,
            6,
            true);
        appendGraphField(output, "graph", definition.graph, 6, false);
        output += "    }";
        if (index + 1 < definitions.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ]";
}

void appendExpressionDefinitions(
    std::string& output,
    const talas::expr::ExpressionDefinitionLibrary& library)
{
    output += "  \"expressionDefinitions\": ";
    if (library.definitions.empty())
    {
        output += "[]";
        return;
    }

    std::vector<const talas::expr::ExpressionDefinition*> definitions;
    definitions.reserve(library.definitions.size());
    for (const auto& definition : library.definitions)
        definitions.push_back(&definition);
    std::ranges::sort(
        definitions,
        {},
        [](const auto* definition) -> const std::string& {
            return definition->name;
        });

    output += "[\n";
    for (std::size_t definitionIndex = 0;
         definitionIndex < definitions.size();
         ++definitionIndex)
    {
        const auto& definition = *definitions[definitionIndex];
        output += "    {\n";
        appendQuotedField(output, 6, "name", definition.name, true);
        output += "      \"parameters\": ";
        if (definition.parameters.empty())
        {
            output += "[],\n";
        }
        else
        {
            output += "[\n";
            for (std::size_t parameterIndex = 0;
                 parameterIndex < definition.parameters.size();
                 ++parameterIndex)
            {
                const auto& parameter =
                    definition.parameters[parameterIndex];
                output += "        {\n";
                appendQuotedField(
                    output, 10, "name", parameter.name, true);
                appendQuotedField(
                    output,
                    10,
                    "domain",
                    valueDomainName(parameter.domain),
                    false);
                output += "        }";
                if (parameterIndex + 1 < definition.parameters.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += "      ],\n";
        }
        appendQuotedField(
            output,
            6,
            "resultDomain",
            valueDomainName(definition.resultDomain),
            true);
        appendQuotedField(
            output, 6, "source", definition.source, false);
        output += "    }";
        if (definitionIndex + 1 < definitions.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ]";
}

void appendVariableLibrary(
    std::string& output,
    const ProjectVariableLibrary& library)
{
    output += "  \"variableLibrary\": {\n";
    appendIntegerField(output, 4, "version", library.version, true);
    output += "    \"variables\": ";
    if (library.variables.empty())
    {
        output += "[]\n";
        output += "  }";
        return;
    }

    std::vector<const ProjectVariableDeclaration*> variables;
    variables.reserve(library.variables.size());
    for (const auto& variable : library.variables)
        variables.push_back(&variable);
    std::ranges::sort(
        variables,
        [](const auto* left, const auto* right) {
            if (left->variableId != right->variableId)
                return left->variableId < right->variableId;
            if (left->owner.kind != right->owner.kind)
                return left->owner.kind < right->owner.kind;
            if (left->owner.definitionId
                != right->owner.definitionId)
            {
                return left->owner.definitionId
                    < right->owner.definitionId;
            }
            return left->owner.nodeId < right->owner.nodeId;
        });

    output += "[\n";
    for (std::size_t index = 0; index < variables.size(); ++index)
    {
        const auto& variable = *variables[index];
        output += "      {\n";
        appendQuotedField(
            output, 8, "variableId", variable.variableId, true);
        appendQuotedField(
            output, 8, "displayName", variable.displayName, true);
        appendQuotedField(
            output, 8, "domain", valueDomainName(variable.domain), true);
        appendNumberField(
            output, 8, "initialValue", variable.initialValue, true);
        output += "        \"owner\": {\n";
        const auto ownerKind = [&]() -> std::string_view {
            switch (variable.owner.kind)
            {
            case ProjectVariableOwnerKind::Project: return "project";
            case ProjectVariableOwnerKind::SoundDefinition:
                return "soundDefinition";
            case ProjectVariableOwnerKind::MidiVoice: return "midiVoice";
            case ProjectVariableOwnerKind::WorkspaceSession:
                return "workspaceSession";
            }
            return {};
        }();
        const auto hasDefinition = !variable.owner.definitionId.empty();
        const auto hasNode = !variable.owner.nodeId.empty();
        appendQuotedField(
            output,
            10,
            "kind",
            ownerKind,
            hasDefinition || hasNode);
        if (hasDefinition)
        {
            appendQuotedField(
                output,
                10,
                "definitionId",
                variable.owner.definitionId,
                hasNode);
        }
        if (hasNode)
        {
            appendQuotedField(
                output,
                10,
                "nodeId",
                variable.owner.nodeId,
                false);
        }
        output += "        },\n";
        appendBooleanField(
            output,
            8,
            "snapshotEligible",
            variable.snapshotEligible,
            false);
        output += "      }";
        if (index + 1U < variables.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "    ]\n";
    output += "  }";
}

void appendSoundVariableLibrary(
    std::string& output,
    const ProjectSoundVariableLibrary& library)
{
    output += "  \"soundVariableLibrary\": {\n";
    appendIntegerField(output, 4, "version", library.version, true);
    output += "    \"variables\": ";
    if (library.variables.empty())
    {
        output += "[]\n";
        output += "  }";
        return;
    }

    std::vector<const ProjectSoundVariableDeclaration*> variables;
    variables.reserve(library.variables.size());
    for (const auto& variable : library.variables)
        variables.push_back(&variable);
    std::ranges::sort(
        variables,
        [](const auto* left, const auto* right) {
            if (left->variableId != right->variableId)
                return left->variableId < right->variableId;
            if (left->owner.kind != right->owner.kind)
                return left->owner.kind < right->owner.kind;
            if (left->owner.definitionId != right->owner.definitionId)
                return left->owner.definitionId < right->owner.definitionId;
            return left->owner.nodeId < right->owner.nodeId;
        });

    output += "[\n";
    for (std::size_t index = 0; index < variables.size(); ++index)
    {
        const auto& variable = *variables[index];
        output += "      {\n";
        appendQuotedField(
            output, 8, "variableId", variable.variableId, true);
        appendQuotedField(
            output, 8, "displayName", variable.displayName, true);
        appendQuotedField(
            output,
            8,
            "kind",
            variable.kind == ProjectSoundVariableKind::Sound
                ? "sound"
                : "soundCollection",
            true);
        output += "        \"owner\": {\n";
        const auto ownerKind = [&]() -> std::string_view {
            switch (variable.owner.kind)
            {
            case ProjectVariableOwnerKind::Project: return "project";
            case ProjectVariableOwnerKind::SoundDefinition:
                return "soundDefinition";
            case ProjectVariableOwnerKind::MidiVoice: return "midiVoice";
            case ProjectVariableOwnerKind::WorkspaceSession:
                return "workspaceSession";
            }
            return {};
        }();
        const auto hasDefinition = !variable.owner.definitionId.empty();
        const auto hasNode = !variable.owner.nodeId.empty();
        appendQuotedField(
            output,
            10,
            "kind",
            ownerKind,
            hasDefinition || hasNode);
        if (hasDefinition)
        {
            appendQuotedField(
                output,
                10,
                "definitionId",
                variable.owner.definitionId,
                hasNode);
        }
        if (hasNode)
        {
            appendQuotedField(
                output, 10, "nodeId", variable.owner.nodeId, false);
        }
        output += "        },\n";
        appendStringArrayField(
            output,
            8,
            "candidateDefinitionIds",
            variable.candidateDefinitionIds,
            true);
        appendIntegerField(
            output,
            8,
            "initialSelectedOrdinal",
            variable.initialSelectedOrdinal,
            true);
        appendBooleanField(
            output,
            8,
            "snapshotEligible",
            variable.snapshotEligible,
            false);
        output += "      }";
        if (index + 1U < variables.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "    ]\n";
    output += "  }";
}

[[nodiscard]] std::string canonicalContextFreeGrammar(
    const talas::generative::ContextFreeGeneratorSource& source)
{
    const auto parsed = talas::generative::parseContextFreeGenerator(source);
    if (!parsed.ok())
        throw std::invalid_argument("Invalid context-free generator source");
    std::string grammar;
    grammar.reserve(source.grammar.size());
    grammar += "start:";
    for (std::size_t index = 0;
         index < parsed.program.startSymbols.size();
         ++index)
    {
        if (index != 0U)
            grammar.push_back(' ');
        grammar += parsed.program.startSymbols[index];
    }
    for (const auto& rule : parsed.program.rules)
    {
        grammar.push_back('\n');
        grammar += rule.symbol;
        grammar += " ->";
        for (std::size_t alternativeIndex = 0;
             alternativeIndex < rule.alternatives.size();
             ++alternativeIndex)
        {
            if (alternativeIndex != 0U)
                grammar.push_back('|');
            const auto& alternative = rule.alternatives[alternativeIndex];
            if (alternative.symbols.empty())
            {
                grammar += "epsilon";
                continue;
            }
            for (std::size_t symbolIndex = 0;
                 symbolIndex < alternative.symbols.size();
                 ++symbolIndex)
            {
                if (symbolIndex != 0U)
                    grammar.push_back(' ');
                grammar += alternative.symbols[symbolIndex];
            }
        }
    }
    if (grammar.size() > talas::generative::maximumContextFreeSourceBytes)
        throw std::length_error(
            "Canonical context-free grammar exceeds its byte limit");
    return grammar;
}

void appendGeneratorOutputAction(
    std::string& output,
    const ProjectGeneratorOutputAction& action,
    int indent)
{
    appendIndent(output, indent);
    output += "{\n";
    std::visit(
        [&](const auto& typed) {
            using Action = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<
                              Action,
                              ProjectGeneratorTimelineEventAction>)
            {
                appendQuotedField(
                    output, indent + 2, "kind", "timelineEventValue", true);
                appendQuotedField(
                    output, indent + 2, "eventName", typed.eventName, true);
                appendQuotedField(
                    output, indent + 2, "domain", valueDomainName(typed.domain), true);
                appendNumberField(output, indent + 2, "value", typed.value, true);
                appendQuotedField(
                    output,
                    indent + 2,
                    "interpolation",
                    automationInterpolationName(typed.interpolation),
                    false);
            }
            else if constexpr (std::is_same_v<
                                   Action,
                                   ProjectGeneratorScoreClipAction>)
            {
                appendQuotedField(output, indent + 2, "kind", "scoreClip", true);
                appendQuotedField(output, indent + 2, "trackId", typed.trackId, true);
                appendQuotedField(
                    output,
                    indent + 2,
                    "soundDefinitionId",
                    typed.soundDefinitionId,
                    true);
                appendUnsignedIntegerField(
                    output, indent + 2, "durationTicks", typed.durationTicks, true);
                appendUnsignedIntegerField(
                    output,
                    indent + 2,
                    "contentOffsetTicks",
                    typed.contentOffsetTicks,
                    true);
                appendUnsignedIntegerField(
                    output, indent + 2, "fadeInTicks", typed.fadeInTicks, true);
                appendUnsignedIntegerField(
                    output, indent + 2, "fadeOutTicks", typed.fadeOutTicks, true);
                appendUnsignedIntegerField(
                    output, indent + 2, "repeatCount", typed.repeatCount, false);
            }
            else if constexpr (std::is_same_v<
                                   Action,
                                   ProjectGeneratorScoreAutomationAction>)
            {
                appendQuotedField(
                    output, indent + 2, "kind", "scoreAutomationPoint", true);
                appendQuotedField(output, indent + 2, "laneId", typed.laneId, true);
                appendQuotedField(
                    output, indent + 2, "domain", valueDomainName(typed.domain), true);
                appendNumberField(output, indent + 2, "value", typed.value, true);
                appendQuotedField(
                    output,
                    indent + 2,
                    "interpolation",
                    automationInterpolationName(typed.interpolation),
                    false);
            }
            else if constexpr (std::is_same_v<
                                   Action,
                                   ProjectGeneratorScoreMarkerAction>)
            {
                appendQuotedField(output, indent + 2, "kind", "scoreMarker", true);
                appendQuotedField(output, indent + 2, "title", typed.title, true);
                if (typed.durationTicks.has_value())
                {
                    appendUnsignedIntegerField(
                        output,
                        indent + 2,
                        "durationTicks",
                        *typed.durationTicks,
                        false);
                }
                else
                {
                    appendIndent(output, indent + 2);
                    output += "\"durationTicks\": null\n";
                }
            }
            else
            {
                appendQuotedField(
                    output, indent + 2, "kind", "graphNode", true);
                appendQuotedField(
                    output,
                    indent + 2,
                    "nodeTypeId",
                    typed.nodeTypeId,
                    false);
            }
        },
        action);
    appendIndent(output, indent);
    output.push_back('}');
}

void appendGeneratorOutputProfile(
    std::string& output,
    const ProjectGeneratorOutputProfile& profile,
    int indent)
{
    appendIndent(output, indent);
    output += "\"output\": {\n";
    appendIntegerField(output, indent + 2, "version", profile.version, true);
    appendUnsignedIntegerField(
        output,
        indent + 2,
        "timelineStepMicroseconds",
        profile.timelineStepMicroseconds,
        true);
    appendUnsignedIntegerField(
        output,
        indent + 2,
        "scoreStepTicks",
        profile.scoreStepTicks,
        true);
    appendIndent(output, indent + 2);
    output += "\"bindings\": ";
    if (profile.bindings.empty())
    {
        output += "[]\n";
    }
    else
    {
        output += "[\n";
        for (std::size_t bindingIndex = 0;
             bindingIndex < profile.bindings.size();
             ++bindingIndex)
        {
            const auto& binding = profile.bindings[bindingIndex];
            appendIndent(output, indent + 4);
            output += "{\n";
            appendQuotedField(
                output, indent + 6, "terminal", binding.terminal, true);
            appendIndent(output, indent + 6);
            output += "\"actions\": ";
            if (binding.actions.empty())
            {
                output += "[]\n";
            }
            else
            {
                output += "[\n";
                for (std::size_t actionIndex = 0;
                     actionIndex < binding.actions.size();
                     ++actionIndex)
                {
                    appendGeneratorOutputAction(
                        output,
                        binding.actions[actionIndex],
                        indent + 8);
                    if (actionIndex + 1U < binding.actions.size())
                        output.push_back(',');
                    output.push_back('\n');
                }
                appendIndent(output, indent + 6);
                output += "]\n";
            }
            appendIndent(output, indent + 4);
            output.push_back('}');
            if (bindingIndex + 1U < profile.bindings.size())
                output.push_back(',');
            output.push_back('\n');
        }
        appendIndent(output, indent + 2);
        output += "]\n";
    }
    appendIndent(output, indent);
    output.push_back('}');
}

void appendGeneratorLibrary(
    std::string& output,
    const ProjectGeneratorLibrary& library)
{
    output += "  \"generatorLibrary\": {\n";
    appendIntegerField(output, 4, "version", library.version, true);
    output += "    \"programs\": ";
    if (library.programs.empty())
    {
        output += "[]\n";
        output += "  }";
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < library.programs.size(); ++index)
    {
        const auto& program = library.programs[index];
        const auto canonicalGrammar =
            canonicalContextFreeGrammar(program.source);
        std::string terminals;
        for (std::size_t terminalIndex = 0;
             terminalIndex < program.source.terminals.size();
             ++terminalIndex)
        {
            if (terminalIndex != 0U)
                terminals.push_back('\n');
            terminals += program.source.terminals[terminalIndex];
        }

        output += "      {\n";
        appendQuotedField(output, 8, "generatorId", program.generatorId, true);
        appendQuotedField(output, 8, "displayName", program.displayName, true);
        appendQuotedField(output, 8, "terminals", terminals, true);
        appendQuotedField(output, 8, "grammar", canonicalGrammar, true);
        appendUnsignedIntegerField(output, 8, "seed", program.source.seed, true);
        appendUnsignedIntegerField(
            output,
            8,
            "maximumRewritePasses",
            program.source.maximumRewritePasses,
            true);
        appendUnsignedIntegerField(
            output,
            8,
            "maximumOutputSymbols",
            program.source.maximumOutputSymbols,
            true);
        appendGeneratorOutputProfile(output, program.output, 8);
        output.push_back('\n');
        output += "      }";
        if (index + 1U < library.programs.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "    ]\n";
    output += "  }";
}

void appendLiveSetTarget(
    std::string& output,
    const ProjectLiveSetCellTarget& target,
    int indent)
{
    appendIndent(output, indent);
    output += "{\n";
    std::visit(
        [&](const auto& value) {
            using Target = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Target, ProjectLiveSetSoundTarget>)
            {
                appendQuotedField(output, indent + 2, "kind", "sound", true);
                appendQuotedField(
                    output,
                    indent + 2,
                    "soundDefinitionId",
                    value.soundDefinitionId,
                    true);
                appendIndent(output, indent + 2);
                output += "\"sourceInstanceId\": ";
                if (value.sourceInstanceId.has_value())
                    appendQuoted(output, *value.sourceInstanceId);
                else
                    output += "null";
                output.push_back('\n');
            }
            else if constexpr (
                std::is_same_v<Target, ProjectLiveSetSnapshotTarget>)
            {
                appendQuotedField(output, indent + 2, "kind", "snapshot", true);
                appendQuotedField(output, indent + 2, "bankId", value.bankId, true);
                appendQuotedField(output, indent + 2, "snapshotId", value.snapshotId, false);
            }
            else if constexpr (
                std::is_same_v<Target, ProjectLiveSetScoreRegionTarget>)
            {
                appendQuotedField(output, indent + 2, "kind", "scoreRegion", true);
                appendQuotedField(output, indent + 2, "markerId", value.markerId, false);
            }
            else if constexpr (
                std::is_same_v<Target, ProjectLiveSetTrackMuteTarget>)
            {
                appendQuotedField(output, indent + 2, "kind", "trackMute", true);
                appendIndent(output, indent + 2);
                output += value.muted ? "\"muted\": true\n" : "\"muted\": false\n";
            }
            else
            {
                appendQuotedField(output, indent + 2, "kind", "stop", false);
            }
        },
        target);
    appendIndent(output, indent);
    output.push_back('}');
}

void appendLiveSetMapping(
    std::string& output,
    const ProjectLiveSetTriggerMapping& mapping,
    int indent)
{
    appendIndent(output, indent);
    output += "{\n";
    std::visit(
        [&](const auto& value) {
            using Mapping = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<
                              Mapping,
                              ProjectLiveSetMidiProgramMapping>)
            {
                appendQuotedField(output, indent + 2, "kind", "midiProgram", true);
                appendQuotedField(
                    output,
                    indent + 2,
                    "sourceIdentifier",
                    value.sourceIdentifier,
                    true);
                appendIntegerField(output, indent + 2, "channel", value.channel, true);
                appendIntegerField(output, indent + 2, "program", value.program, false);
            }
            else if constexpr (std::is_same_v<
                                   Mapping,
                                   ProjectLiveSetOscMapping>)
            {
                appendQuotedField(output, indent + 2, "kind", "osc", true);
                appendQuotedField(output, indent + 2, "address", value.address, false);
            }
            else
            {
                appendQuotedField(
                    output, indent + 2, "kind", "workbench", false);
            }
        },
        mapping);
    appendIndent(output, indent);
    output.push_back('}');
}

void appendLiveToolTrigger(
    std::string& output,
    const ProjectLiveToolTrigger& trigger,
    int indent)
{
    appendIndent(output, indent);
    output += "{\n";
    appendQuotedField(output, indent + 2, "kind", liveToolTriggerKindName(trigger.kind), true);
    appendQuotedField(output, indent + 2, "eventName", trigger.eventName, true);
    appendQuotedField(output, indent + 2, "eventDomain", valueDomainName(trigger.eventDomain), true);
    appendQuotedField(output, indent + 2, "variableId", trigger.variableId, true);
    appendIndent(output, indent + 2);
    output += "\"midiProgram\": {\n";
    appendQuotedField(
        output,
        indent + 4,
        "sourceIdentifier",
        trigger.midiProgram.sourceIdentifier,
        true);
    appendIntegerField(output, indent + 4, "channel", trigger.midiProgram.channel, true);
    appendIntegerField(output, indent + 4, "program", trigger.midiProgram.program, false);
    appendIndent(output, indent + 2);
    output += "},\n";
    appendIndent(output, indent + 2);
    output += "\"osc\": {\n";
    appendQuotedField(output, indent + 4, "address", trigger.osc.address, false);
    appendIndent(output, indent + 2);
    output += "},\n";
    appendUnsignedIntegerField(output, indent + 2, "periodTicks", trigger.periodTicks, false);
    appendIndent(output, indent);
    output.push_back('}');
}

void appendLiveToolCommand(
    std::string& output,
    const ProjectLiveToolCommand& command,
    int indent)
{
    appendIndent(output, indent);
    output += "{\n";
    appendQuotedField(output, indent + 2, "kind", liveToolCommandKindName(command.kind), true);
    appendQuotedField(output, indent + 2, "liveSetId", command.liveSetId, true);
    appendQuotedField(output, indent + 2, "cellId", command.cellId, true);
    appendQuotedField(output, indent + 2, "variableId", command.variableId, true);
    appendQuotedField(output, indent + 2, "eventName", command.eventName, true);
    appendQuotedField(output, indent + 2, "eventDomain", valueDomainName(command.eventDomain), true);
    appendNumberField(output, indent + 2, "value", command.value, false);
    appendIndent(output, indent);
    output.push_back('}');
}

void appendLiveSetLibrary(
    std::string& output,
    const ProjectLiveSetLibrary& library)
{
    output += "  \"liveSetLibrary\": {\n";
    appendIntegerField(output, 4, "version", library.schemaVersion, true);
    output += "    \"liveSets\": [";
    if (!library.liveSets.empty())
        output.push_back('\n');
    for (std::size_t liveSetIndex = 0;
         liveSetIndex < library.liveSets.size();
         ++liveSetIndex)
    {
        const auto& liveSet = library.liveSets[liveSetIndex];
        output += "      {\n";
        appendQuotedField(output, 8, "liveSetId", liveSet.liveSetId, true);
        appendQuotedField(output, 8, "displayName", liveSet.displayName, true);
        output += "        \"cells\": [";
        if (!liveSet.cells.empty())
            output.push_back('\n');
        for (std::size_t cellIndex = 0;
             cellIndex < liveSet.cells.size();
             ++cellIndex)
        {
            const auto& cell = liveSet.cells[cellIndex];
            output += "          {\n";
            appendQuotedField(output, 12, "cellId", cell.cellId, true);
            appendQuotedField(output, 12, "displayName", cell.displayName, true);
            appendUnsignedIntegerField(output, 12, "row", cell.row, true);
            appendUnsignedIntegerField(output, 12, "column", cell.column, true);
            output += "            \"quantization\": {\n";
            appendQuotedField(
                output,
                14,
                "kind",
                liveSetQuantizationName(cell.quantization.kind),
                true);
            appendUnsignedIntegerField(
                output,
                14,
                "tickInterval",
                cell.quantization.tickInterval,
                false);
            output += "            },\n";
            output += "            \"target\": \n";
            appendLiveSetTarget(output, cell.target, 12);
            output += ",\n            \"mappings\": [";
            if (!cell.mappings.empty())
                output.push_back('\n');
            for (std::size_t mappingIndex = 0;
                 mappingIndex < cell.mappings.size();
                 ++mappingIndex)
            {
                appendLiveSetMapping(output, cell.mappings[mappingIndex], 14);
                if (mappingIndex + 1U < cell.mappings.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            if (!cell.mappings.empty())
                output += "            ";
            output += "]\n          }";
            if (cellIndex + 1U < liveSet.cells.size())
                output.push_back(',');
            output.push_back('\n');
        }
        if (!liveSet.cells.empty())
            output += "        ";
        if (liveSet.routes.empty())
            output += "]\n      }";
        else
        {
            output += "],\n        \"routes\": [\n";
            for (std::size_t routeIndex = 0;
                 routeIndex < liveSet.routes.size(); ++routeIndex)
            {
                const auto& route = liveSet.routes[routeIndex];
                output += "          {\n";
                appendUnsignedIntegerField(output, 12, "fromColumn",
                                           route.fromColumn, true);
                appendUnsignedIntegerField(output, 12, "toColumn",
                                           route.toColumn, false);
                output += "          }";
                if (routeIndex + 1U < liveSet.routes.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += "        ]\n      }";
        }
        if (liveSetIndex + 1U < library.liveSets.size())
            output.push_back(',');
        output.push_back('\n');
    }
    if (!library.liveSets.empty())
        output += "    ";
    output += "],\n";
    output += "    \"tools\": [";
    if (!library.tools.empty())
        output.push_back('\n');
    for (std::size_t toolIndex = 0;
         toolIndex < library.tools.size();
         ++toolIndex)
    {
        const auto& tool = library.tools[toolIndex];
        output += "      {\n";
        appendQuotedField(output, 8, "toolId", tool.toolId, true);
        appendQuotedField(output, 8, "displayName", tool.displayName, true);
        appendQuotedField(output, 8, "initialStateId", tool.initialStateId, true);
        output += "        \"states\": [";
        if (!tool.states.empty()) output.push_back('\n');
        for (std::size_t index = 0; index < tool.states.size(); ++index)
        {
            output += "          {\n";
            appendQuotedField(output, 12, "stateId", tool.states[index].stateId, true);
            appendQuotedField(output, 12, "displayName", tool.states[index].displayName, false);
            output += "          }";
            if (index + 1U < tool.states.size()) output.push_back(',');
            output.push_back('\n');
        }
        if (!tool.states.empty()) output += "        ";
        output += "],\n        \"variables\": [";
        if (!tool.variables.empty()) output.push_back('\n');
        for (std::size_t index = 0; index < tool.variables.size(); ++index)
        {
            const auto& variable = tool.variables[index];
            output += "          {\n";
            appendQuotedField(output, 12, "variableId", variable.variableId, true);
            appendQuotedField(output, 12, "displayName", variable.displayName, true);
            appendQuotedField(output, 12, "kind", liveToolVariableKindName(variable.kind), true);
            appendNumberField(output, 12, "initialNumber", variable.initialNumber, true);
            appendBooleanField(output, 12, "initialBoolean", variable.initialBoolean, true);
            appendIndent(output, 12);
            output += "\"stateId\": ";
            if (variable.stateId.has_value()) appendQuoted(output, *variable.stateId);
            else output += "null";
            output += "\n          }";
            if (index + 1U < tool.variables.size()) output.push_back(',');
            output.push_back('\n');
        }
        if (!tool.variables.empty()) output += "        ";
        output += "],\n        \"transitions\": [";
        if (!tool.transitions.empty()) output.push_back('\n');
        for (std::size_t index = 0; index < tool.transitions.size(); ++index)
        {
            const auto& transition = tool.transitions[index];
            output += "          {\n";
            appendQuotedField(output, 12, "transitionId", transition.transitionId, true);
            appendQuotedField(output, 12, "fromStateId", transition.fromStateId, true);
            appendQuotedField(output, 12, "toStateId", transition.toStateId, true);
            output += "            \"trigger\": \n";
            appendLiveToolTrigger(output, transition.trigger, 12);
            output += ",\n";
            appendQuotedField(output, 12, "guardExpression", transition.guardExpression, true);
            output += "            \"commands\": [";
            if (!transition.commands.empty()) output.push_back('\n');
            for (std::size_t commandIndex = 0;
                 commandIndex < transition.commands.size();
                 ++commandIndex)
            {
                appendLiveToolCommand(output, transition.commands[commandIndex], 14);
                if (commandIndex + 1U < transition.commands.size()) output.push_back(',');
                output.push_back('\n');
            }
            if (!transition.commands.empty()) output += "            ";
            output += "]\n          }";
            if (index + 1U < tool.transitions.size()) output.push_back(',');
            output.push_back('\n');
        }
        if (!tool.transitions.empty()) output += "        ";
        output += "]\n      }";
        if (toolIndex + 1U < library.tools.size()) output.push_back(',');
        output.push_back('\n');
    }
    if (!library.tools.empty()) output += "    ";
    output += "]\n  }";
}

std::string_view virtualControlWidgetStyleJsonName(VirtualControlWidgetStyle style)
{
    switch (style)
    {
    case VirtualControlWidgetStyle::Default: return "default";
    case VirtualControlWidgetStyle::Fader: return "fader";
    case VirtualControlWidgetStyle::HorizontalFader: return "horizontalFader";
    case VirtualControlWidgetStyle::SmallFader: return "smallFader";
    case VirtualControlWidgetStyle::Potentiometer: return "potentiometer";
    case VirtualControlWidgetStyle::ButtonFill: return "buttonFill";
    }
    return "default";
}

std::string virtualControlColourJson(std::optional<std::uint32_t> colour)
{
    if (!colour.has_value())
        return {};
    static constexpr char digits[] = "0123456789abcdef";
    std::string text = "#";
    for (int shift = 20; shift >= 0; shift -= 4)
        text.push_back(digits[(*colour >> shift) & 0xFu]);
    return text;
}

std::string_view virtualControlKindName(VirtualControlKind kind)
{
    switch (kind)
    {
    case VirtualControlKind::Fader: return "fader";
    case VirtualControlKind::Gate: return "gate";
    case VirtualControlKind::Toggle: return "toggle";
    }
    return "fader";
}

std::string_view valueDomainName(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar: return "scalar";
    case talas::expr::ValueDomain::Hertz: return "hertz";
    case talas::expr::ValueDomain::Seconds: return "seconds";
    case talas::expr::ValueDomain::NoteNumber: return "noteNumber";
    case talas::expr::ValueDomain::Samples: return "samples";
    }
    return "scalar";
}

std::string_view projectAssetKindName(ProjectAssetKind kind)
{
    switch (kind)
    {
    case ProjectAssetKind::Table: return "table";
    case ProjectAssetKind::Binary: return "binary";
    case ProjectAssetKind::Audio: return "audio";
    case ProjectAssetKind::PartialSpectrum: return "partialSpectrum";
    }
    return "table";
}

std::string_view projectAssetDispositionName(
    ProjectAssetDisposition disposition)
{
    switch (disposition)
    {
    case ProjectAssetDisposition::Referenced: return "referenced";
    case ProjectAssetDisposition::Embedded: return "embedded";
    }
    return "referenced";
}

std::string_view performanceControlKindName(
    ProjectPerformanceControlKind kind)
{
    switch (kind)
    {
    case ProjectPerformanceControlKind::Knob: return "knob";
    case ProjectPerformanceControlKind::Fader: return "fader";
    case ProjectPerformanceControlKind::Gate: return "gate";
    case ProjectPerformanceControlKind::Toggle: return "toggle";
    case ProjectPerformanceControlKind::Macro: return "macro";
    }
    return "fader";
}

void appendSnapshotTargetField(
    std::string& output,
    const SnapshotTargetIdentity& target,
    int indent,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += "\"target\": {\n";
    switch (target.kind)
    {
    case SnapshotTargetKind::EventValue:
        appendQuotedField(output, indent + 2, "kind", "eventValue", true);
        appendQuotedField(output, indent + 2, "eventName", target.eventName, true);
        appendQuotedField(
            output, indent + 2, "domain", valueDomainName(target.domain), false);
        break;
    case SnapshotTargetKind::NodeParameter:
        appendQuotedField(output, indent + 2, "kind", "nodeParameter", true);
        appendStringArrayField(
            output, indent + 2, "instancePath", target.instancePath, true);
        appendIndent(output, indent + 2);
        output += "\"containingDefinitionId\": ";
        if (target.containingDefinitionId.has_value())
            appendQuoted(output, *target.containingDefinitionId);
        else
            output += "null";
        output += ",\n";
        appendQuotedField(output, indent + 2, "nodeId", target.nodeId, true);
        appendQuotedField(
            output, indent + 2, "parameterId", target.parameterId, false);
        break;
    case SnapshotTargetKind::SoundParameter:
        appendQuotedField(output, indent + 2, "kind", "soundParameter", true);
        appendStringArrayField(
            output, indent + 2, "instancePath", target.instancePath, true);
        appendQuotedField(
            output, indent + 2, "definitionId", target.definitionId, true);
        appendQuotedField(
            output, indent + 2, "parameterId", target.parameterId, false);
        break;
    case SnapshotTargetKind::Variable:
    case SnapshotTargetKind::SoundVariable:
        appendQuotedField(
            output,
            indent + 2,
            "kind",
            target.kind == SnapshotTargetKind::SoundVariable
                ? "soundVariable"
                : "variable",
            true);
        if (target.variableOwnerKind
            == SnapshotVariableOwnerKind::WorkspaceSession)
        {
            appendQuotedField(
                output,
                indent + 2,
                "ownerKind",
                "workspaceSession",
                true);
        }
        if (!target.instancePath.empty())
        {
            appendStringArrayField(
                output, indent + 2, "instancePath", target.instancePath, true);
            appendQuotedField(
                output,
                indent + 2,
                "definitionId",
                target.definitionId,
                true);
        }
        appendQuotedField(
            output, indent + 2, "variableId", target.variableId, false);
        break;
    }
    appendIndent(output, indent);
    output.push_back('}');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

std::optional<ProjectPresentationState> canonicalPresentation(
    const ProjectDocument& document,
    const ProjectPresentationState& presentation)
{
    if (presentation.schemaVersion != 2)
        return std::nullopt;
    if (presentation.structureLayouts.size()
            != document.soundDefinitions.size() + 1
        || presentation.performanceControls.size()
            > maximumPerformanceControlCount
        || presentation.performanceMeters.size()
            > maximumPerformanceMeterCount)
    {
        return std::nullopt;
    }

    ProjectPresentationState canonical;
    const auto appendLayout = [&](const ProjectGraph& graph,
                                  std::optional<std::string> definitionId) {
        const auto matchingLayout = std::ranges::find_if(
            presentation.structureLayouts,
            [&](const auto& layout) {
                return layout.soundDefinitionId == definitionId;
            });
        if (matchingLayout == presentation.structureLayouts.end()
            || std::ranges::count_if(
                   presentation.structureLayouts,
                   [&](const auto& layout) {
                       return layout.soundDefinitionId == definitionId;
                   }) != 1
            || matchingLayout->nodes.size() != graph.nodes.size())
        {
            return false;
        }

        ProjectStructureLayout layout;
        layout.soundDefinitionId = std::move(definitionId);
        layout.nodes.reserve(graph.nodes.size());
        for (const auto& node : graph.nodes)
        {
            const auto presented = std::ranges::find_if(
                matchingLayout->nodes,
                [&](const auto& candidate) {
                    return candidate.nodeId == node.instanceId;
                });
            if (presented == matchingLayout->nodes.end()
                || std::ranges::count_if(
                       matchingLayout->nodes,
                       [&](const auto& candidate) {
                           return candidate.nodeId == node.instanceId;
                       }) != 1)
            {
                return false;
            }
            layout.nodes.push_back(*presented);
        }
        canonical.structureLayouts.push_back(std::move(layout));
        return true;
    };
    if (!appendLayout(document.graph, std::nullopt))
        return std::nullopt;
    for (const auto& definition : document.soundDefinitions)
    {
        if (!appendLayout(definition.graph, definition.definitionId))
            return std::nullopt;
    }

    std::unordered_set<std::string> controlIds;
    std::unordered_set<std::string> targetIds;
    std::size_t modulationSlotCount = 0;
    const auto validControl = [&](const auto& item) {
        const auto targetIdentity = snapshotTargetIdentityString(item.target);
        if (!isValidProjectPerformanceIdentifier(item.controlId)
            || item.displayName.empty()
            || item.displayName.size() > maximumPerformanceDisplayNameBytes
            || !isCanonicalSnapshotTargetIdentityString(targetIdentity)
            || !isProjectPerformanceTargetAvailable(document, item.target)
            || !controlIds.insert(item.controlId).second
            || !targetIds.insert(targetIdentity).second
            || item.modulationSlots.size()
                > maximumPerformanceModulationSlotsPerControl)
        {
            return false;
        }
        switch (item.kind)
        {
        case ProjectPerformanceControlKind::Knob:
        case ProjectPerformanceControlKind::Fader:
        case ProjectPerformanceControlKind::Gate:
        case ProjectPerformanceControlKind::Toggle:
        case ProjectPerformanceControlKind::Macro:
            break;
        default:
            return false;
        }
        if (item.kind != ProjectPerformanceControlKind::Macro
            && !item.modulationSlots.empty())
        {
            return false;
        }
        modulationSlotCount += item.modulationSlots.size();
        if (modulationSlotCount > maximumPerformanceModulationSlotCount)
            return false;
        std::unordered_set<std::string> destinations;
        for (const auto& slot : item.modulationSlots)
        {
            const auto destination = snapshotTargetIdentityString(slot.target);
            if (!std::isfinite(slot.depth) || slot.depth < -1.0
                || slot.depth > 1.0 || slot.target == item.target
                || !isCanonicalSnapshotTargetIdentityString(destination)
                || !isProjectPerformanceTargetAvailable(document, slot.target)
                || !destinations.insert(destination).second)
            {
                return false;
            }
        }
        return true;
    };

    canonical.performanceControls.reserve(
        presentation.performanceControls.size());
    for (const auto& control : document.virtualControls)
    {
        const auto matchesIdentity = [&](const auto& candidate) {
            return candidate.target.kind == SnapshotTargetKind::EventValue
                && candidate.target.eventName == control.eventName
                && candidate.target.domain == control.domain;
        };
        const auto presented = std::ranges::find_if(
            presentation.performanceControls,
            matchesIdentity);
        if (presented == presentation.performanceControls.end()
            || std::ranges::count_if(
                   presentation.performanceControls,
                   matchesIdentity) != 1)
        {
            return std::nullopt;
        }
        if (!validControl(*presented))
            return std::nullopt;
        canonical.performanceControls.push_back(*presented);
    }

    std::vector<ProjectPerformanceControlPresentation> optionalControls;
    for (const auto& item : presentation.performanceControls)
    {
        if (item.target.kind == SnapshotTargetKind::EventValue)
            continue;
        if (!validControl(item))
            return std::nullopt;
        optionalControls.push_back(item);
    }
    std::ranges::sort(optionalControls,
        {},
        &ProjectPerformanceControlPresentation::controlId);
    canonical.performanceControls.insert(
        canonical.performanceControls.end(),
        optionalControls.begin(),
        optionalControls.end());

    std::unordered_set<std::string> meterIds;
    canonical.performanceMeters = presentation.performanceMeters;
    std::ranges::sort(canonical.performanceMeters,
        {},
        &ProjectPerformanceMeterPresentation::meterId);
    for (const auto& meter : canonical.performanceMeters)
    {
        if (!isValidProjectPerformanceIdentifier(meter.meterId)
            || meter.outputChannel == 0
            || !meterIds.insert(meter.meterId).second)
        {
            return std::nullopt;
        }
    }
    if (projectPerformancePresentationHasCycle(document))
        return std::nullopt;
    return canonical;
}

ProjectPresentationState presentationForSerialization(
    const ProjectDocument& document)
{
    std::unordered_set<std::string> explicitControlIds;
    std::unordered_set<std::string> explicitTargetIds;
    for (const auto& control : document.presentation.performanceControls)
    {
        if ((!control.controlId.empty()
                && !isValidProjectPerformanceIdentifier(control.controlId))
            || (!control.controlId.empty()
                && !explicitControlIds.insert(control.controlId).second)
            || !explicitTargetIds.insert(
                    snapshotTargetIdentityString(control.target)).second)
        {
            throw std::invalid_argument(
                "Project Performance control identity is invalid or duplicate");
        }
    }
    std::unordered_set<std::string> explicitMeterIds;
    for (const auto& meter : document.presentation.performanceMeters)
    {
        if (!isValidProjectPerformanceIdentifier(meter.meterId)
            || !explicitMeterIds.insert(meter.meterId).second)
        {
            throw std::invalid_argument(
                "Project Performance meter identity is invalid or duplicate");
        }
    }
    if (auto canonical = canonicalPresentation(
            document,
            document.presentation))
    {
        return *std::move(canonical);
    }
    auto reconciled = document;
    reconcileProjectPresentation(reconciled);
    if (auto canonical = canonicalPresentation(
            reconciled,
            reconciled.presentation))
    {
        return *std::move(canonical);
    }
    throw std::invalid_argument(
        "Project Performance presentation is not canonicalizable");
}

void appendPresentation(
    std::string& output,
    const ProjectDocument& document)
{
    const auto presentation = presentationForSerialization(document);
    output += "  \"presentation\": {\n";
    appendUnsignedIntegerField(
        output,
        4,
        "version",
        presentation.schemaVersion,
        true);
    output += "    \"structureLayouts\": [\n";
    for (std::size_t layoutIndex = 0;
         layoutIndex < presentation.structureLayouts.size();
         ++layoutIndex)
    {
        const auto& layout = presentation.structureLayouts[layoutIndex];
        output += "      {\n        \"scope\": {\n";
        appendQuotedField(
            output,
            10,
            "kind",
            layout.soundDefinitionId.has_value()
                ? "soundDefinition"
                : "project",
            layout.soundDefinitionId.has_value());
        if (layout.soundDefinitionId.has_value())
        {
            appendQuotedField(
                output,
                10,
                "definitionId",
                *layout.soundDefinitionId,
                false);
        }
        output += "        },\n        \"nodes\": ";
        if (layout.nodes.empty())
        {
            output += "[]\n";
        }
        else
        {
            output += "[\n";
            for (std::size_t nodeIndex = 0;
                 nodeIndex < layout.nodes.size();
                 ++nodeIndex)
            {
                const auto& node = layout.nodes[nodeIndex];
                output += "          {\n";
                appendQuotedField(output, 12, "nodeId", node.nodeId, true);
                appendIntegerField(output, 12, "x", node.position.x, true);
                appendIntegerField(output, 12, "y", node.position.y, false);
                output += "          }";
                if (nodeIndex + 1 < layout.nodes.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += "        ]\n";
        }
        output += "      }";
        if (layoutIndex + 1 < presentation.structureLayouts.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "    ],\n    \"performanceControls\": ";
    if (presentation.performanceControls.empty())
    {
        output += "[],\n";
    }
    else
    {
        output += "[\n";
        for (std::size_t index = 0;
             index < presentation.performanceControls.size();
             ++index)
        {
            const auto& control = presentation.performanceControls[index];
            output += "      {\n";
            appendQuotedField(output, 8, "controlId", control.controlId, true);
            appendSnapshotTargetField(output, control.target, 8, true);
            appendQuotedField(output,
                8,
                "kind",
                performanceControlKindName(control.kind),
                true);
            appendQuotedField(
                output, 8, "displayName", control.displayName, true);
            appendIntegerField(output, 8, "x", control.position.x, true);
            appendIntegerField(output, 8, "y", control.position.y, true);
            appendBooleanField(output, 8, "visible", control.visible, true);
            output += "        \"modulationSlots\": ";
            if (control.modulationSlots.empty())
            {
                output += "[]\n";
            }
            else
            {
                output += "[\n";
                for (std::size_t slotIndex = 0;
                    slotIndex < control.modulationSlots.size();
                    ++slotIndex)
                {
                    const auto& slot = control.modulationSlots[slotIndex];
                    output += "          {\n";
                    appendSnapshotTargetField(output, slot.target, 12, true);
                    appendNumberField(output, 12, "depth", slot.depth, false);
                    output += "          }";
                    if (slotIndex + 1 < control.modulationSlots.size())
                        output.push_back(',');
                    output.push_back('\n');
                }
                output += "        ]\n";
            }
            output += "      }";
            if (index + 1 < presentation.performanceControls.size())
                output.push_back(',');
            output.push_back('\n');
        }
        output += "    ],\n";
    }
    output += "    \"performanceMeters\": ";
    if (presentation.performanceMeters.empty())
    {
        output += "[]\n";
    }
    else
    {
        output += "[\n";
        for (std::size_t index = 0;
            index < presentation.performanceMeters.size();
            ++index)
        {
            const auto& meter = presentation.performanceMeters[index];
            output += "      {\n";
            appendQuotedField(output, 8, "meterId", meter.meterId, true);
            appendUnsignedIntegerField(
                output, 8, "outputChannel", meter.outputChannel, true);
            appendIntegerField(output, 8, "x", meter.position.x, true);
            appendIntegerField(output, 8, "y", meter.position.y, true);
            appendBooleanField(output, 8, "visible", meter.visible, false);
            output += "      }";
            if (index + 1 < presentation.performanceMeters.size())
                output.push_back(',');
            output.push_back('\n');
        }
        output += "    ]\n";
    }
    output += "  }";
}

void appendSnapshotLibrary(
    std::string& output,
    const ProjectSnapshotLibraryState& snapshotLibrary)
{
    const auto appendNullableStringField = [](
                                               std::string& target,
                                               int indent,
                                               std::string_view key,
                                               const std::optional<std::string>& value,
                                               bool trailingComma) {
        appendIndent(target, indent);
        appendQuoted(target, key);
        target += ": ";
        if (value.has_value())
            appendQuoted(target, *value);
        else
            target += "null";
        if (trailingComma)
            target.push_back(',');
        target.push_back('\n');
    };
    const auto appendScope = [&](const SnapshotScopeIdentity& scope, int indent) {
        appendIndent(output, indent);
        output += "\"scope\": {\n";
        switch (scope.kind)
        {
        case SnapshotScopeKind::Project:
            appendQuotedField(output, indent + 2, "kind", "project", false);
            break;
        case SnapshotScopeKind::Definition:
            appendQuotedField(output, indent + 2, "kind", "definition", true);
            appendStringArrayField(
                output,
                indent + 2,
                "instancePath",
                scope.instancePath,
                true);
            appendQuotedField(
                output,
                indent + 2,
                "definitionId",
                scope.definitionId,
                false);
            break;
        case SnapshotScopeKind::Node:
            appendQuotedField(output, indent + 2, "kind", "node", true);
            appendStringArrayField(
                output,
                indent + 2,
                "instancePath",
                scope.instancePath,
                true);
            appendNullableStringField(
                output,
                indent + 2,
                "containingDefinitionId",
                scope.containingDefinitionId,
                true);
            appendQuotedField(
                output,
                indent + 2,
                "nodeId",
                scope.nodeId,
                false);
            break;
        }
        appendIndent(output, indent);
        output += "},\n";
    };
    const auto appendTarget = [&](const SnapshotTargetIdentity& target,
                                  int indent,
                                  bool trailingComma) {
        appendIndent(output, indent);
        output += "\"target\": {\n";
        switch (target.kind)
        {
        case SnapshotTargetKind::EventValue:
            appendQuotedField(output, indent + 2, "kind", "eventValue", true);
            appendQuotedField(
                output,
                indent + 2,
                "eventName",
                target.eventName,
                true);
            appendQuotedField(
                output,
                indent + 2,
                "domain",
                valueDomainName(target.domain),
                false);
            break;
        case SnapshotTargetKind::NodeParameter:
            appendQuotedField(output, indent + 2, "kind", "nodeParameter", true);
            appendStringArrayField(
                output,
                indent + 2,
                "instancePath",
                target.instancePath,
                true);
            appendNullableStringField(
                output,
                indent + 2,
                "containingDefinitionId",
                target.containingDefinitionId,
                true);
            appendQuotedField(output, indent + 2, "nodeId", target.nodeId, true);
            appendQuotedField(
                output,
                indent + 2,
                "parameterId",
                target.parameterId,
                false);
            break;
        case SnapshotTargetKind::SoundParameter:
            appendQuotedField(output, indent + 2, "kind", "soundParameter", true);
            appendStringArrayField(
                output,
                indent + 2,
                "instancePath",
                target.instancePath,
                true);
            appendQuotedField(
                output,
                indent + 2,
                "definitionId",
                target.definitionId,
                true);
            appendQuotedField(
                output,
                indent + 2,
                "parameterId",
                target.parameterId,
                false);
            break;
        case SnapshotTargetKind::Variable:
        case SnapshotTargetKind::SoundVariable:
            appendQuotedField(
                output,
                indent + 2,
                "kind",
                target.kind == SnapshotTargetKind::SoundVariable
                    ? "soundVariable"
                    : "variable",
                true);
            if (target.variableOwnerKind
                == SnapshotVariableOwnerKind::WorkspaceSession)
            {
                appendQuotedField(
                    output,
                    indent + 2,
                    "ownerKind",
                    "workspaceSession",
                    true);
            }
            if (!target.instancePath.empty())
            {
                appendStringArrayField(
                    output,
                    indent + 2,
                    "instancePath",
                    target.instancePath,
                    true);
                appendQuotedField(
                    output,
                    indent + 2,
                    "definitionId",
                    target.definitionId,
                    true);
            }
            appendQuotedField(
                output,
                indent + 2,
                "variableId",
                target.variableId,
                false);
            break;
        }
        appendIndent(output, indent);
        output.push_back('}');
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
    };
    const auto appendValue = [&](const SnapshotValue& value, int indent) {
        appendIndent(output, indent);
        output += "\"value\": {\n";
        switch (value.kind)
        {
        case SnapshotValueKind::Number:
            appendQuotedField(output, indent + 2, "kind", "number", true);
            appendQuotedField(
                output,
                indent + 2,
                "domain",
                valueDomainName(value.domain),
                true);
            appendNumberField(output, indent + 2, "value", value.number, false);
            break;
        case SnapshotValueKind::Choice:
            appendQuotedField(output, indent + 2, "kind", "choice", true);
            appendQuotedField(output, indent + 2, "value", value.choice, false);
            break;
        case SnapshotValueKind::Boolean:
            appendQuotedField(output, indent + 2, "kind", "boolean", true);
            appendBooleanField(output, indent + 2, "value", value.boolean, false);
            break;
        }
        appendIndent(output, indent);
        output += "}\n";
    };

    output += "  \"snapshotLibrary\": {\n";
    appendUnsignedIntegerField(
        output,
        4,
        "version",
        snapshotLibrary.schemaVersion,
        true);
    if (snapshotLibrary.banks.empty())
    {
        output += "    \"banks\": []\n  }";
        return;
    }
    output += "    \"banks\": [\n";
    for (std::size_t bankIndex = 0;
         bankIndex < snapshotLibrary.banks.size();
         ++bankIndex)
    {
        const auto& bank = snapshotLibrary.banks[bankIndex];
        output += "      {\n";
        appendQuotedField(output, 8, "id", bank.bankId, true);
        appendQuotedField(output, 8, "name", bank.displayName, true);
        appendScope(bank.scope, 8);
        appendNumberField(
            output,
            8,
            "smoothingSeconds",
            bank.smoothingSeconds,
            true);
        if (bank.parameterPolicies.empty())
        {
            output += "        \"parameterPolicies\": [],\n";
        }
        else
        {
            output += "        \"parameterPolicies\": [\n";
            for (std::size_t policyIndex = 0;
                 policyIndex < bank.parameterPolicies.size();
                 ++policyIndex)
            {
                const auto& policy = bank.parameterPolicies[policyIndex];
                output += "          {\n";
                appendTarget(policy.target, 12, true);
                appendBooleanField(output, 12, "locked", policy.locked, true);
                if (policy.range.has_value())
                {
                    output += "            \"range\": {\n";
                    appendNumberField(
                        output,
                        14,
                        "minimum",
                        policy.range->minimum,
                        true);
                    appendNumberField(
                        output,
                        14,
                        "maximum",
                        policy.range->maximum,
                        false);
                    output += "            }\n";
                }
                else
                {
                    output += "            \"range\": null\n";
                }
                output += "          }";
                if (policyIndex + 1 < bank.parameterPolicies.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += "        ],\n";
        }
        if (bank.snapshots.empty())
        {
            output += "        \"snapshots\": []\n";
        }
        else
        {
            output += "        \"snapshots\": [\n";
            for (std::size_t snapshotIndex = 0;
                 snapshotIndex < bank.snapshots.size();
                 ++snapshotIndex)
            {
                const auto& snapshot = bank.snapshots[snapshotIndex];
                output += "          {\n";
                appendQuotedField(output, 12, "id", snapshot.snapshotId, true);
                appendQuotedField(output, 12, "name", snapshot.displayName, true);
                if (snapshot.values.empty())
                {
                    output += "            \"values\": []\n";
                }
                else
                {
                    output += "            \"values\": [\n";
                    for (std::size_t valueIndex = 0;
                         valueIndex < snapshot.values.size();
                         ++valueIndex)
                    {
                        const auto& value = snapshot.values[valueIndex];
                        output += "              {\n";
                        appendTarget(value.target, 16, true);
                        appendValue(value.value, 16);
                        output += "              }";
                        if (valueIndex + 1 < snapshot.values.size())
                            output.push_back(',');
                        output.push_back('\n');
                    }
                    output += "            ]\n";
                }
                output += "          }";
                if (snapshotIndex + 1 < bank.snapshots.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += "        ]\n";
        }
        output += "      }";
        if (bankIndex + 1 < snapshotLibrary.banks.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "    ]\n  }";
}

// Format 97: the VCS Editor's widget options, the object's members at
// indent + 2 and its closing brace at indent, ending its line.
void appendVirtualControlWidget(std::string& output, const VirtualControlWidget& widget, int indent)
{
    appendIndent(output, indent);
    output += "\"widget\": {\n";
    appendQuotedField(output, indent + 2, "style", virtualControlWidgetStyleJsonName(widget.style), true);
    appendBooleanField(output, indent + 2, "showNumber", widget.showNumber, true);
    appendQuotedField(output, indent + 2, "label", widget.label, true);
    appendQuotedField(output, indent + 2, "onColour", virtualControlColourJson(widget.onColour), true);
    appendQuotedField(output, indent + 2, "offColour", virtualControlColourJson(widget.offColour), true);
    appendNumberField(output, indent + 2, "grid", widget.grid, true);
    appendQuotedField(output, indent + 2, "taper", widget.taper == VirtualControlTaper::Log ? "log" : "linear", true);
    appendQuotedField(output, indent + 2, "tab", widget.tab, widget.bounds.has_value());
    // Format 125: the widget's place once the surface is arranged.
    if (widget.bounds.has_value())
    {
        appendIndent(output, indent + 2);
        output += "\"bounds\": [" + std::to_string(widget.bounds->left) + ", " + std::to_string(widget.bounds->top)
            + ", " + std::to_string(widget.bounds->width) + ", " + std::to_string(widget.bounds->height) + "]\n";
    }
    appendIndent(output, indent);
    output += "}\n";
}

void appendVirtualControls(
    std::string& output, const std::vector<VirtualControl>& controls, std::string_view member = "virtualControls")
{
    output += "  \"";
    output += member;
    output += "\": ";
    if (controls.empty())
    {
        output += "[]";
        return;
    }
    output += "[\n";
    for (std::size_t index = 0; index < controls.size(); ++index)
    {
        const auto& control = controls[index];
        output += "    {\n";
        appendQuotedField(output, 6, "eventName", control.eventName, true);
        appendQuotedField(output, 6, "domain", valueDomainName(control.domain), true);
        appendQuotedField(output, 6, "kind", virtualControlKindName(control.kind), true);
        appendNumberField(output, 6, "value", control.value, true);
        appendNumberField(output, 6, "minimum", control.minimum, true);
        appendNumberField(
            output,
            6,
            "maximum",
            control.maximum,
            control.midiMapping.has_value()
                || control.oscMapping.has_value()
                || control.oscEgressMapping.has_value()
                || control.widget != VirtualControlWidget{});
        if (control.midiMapping.has_value())
        {
            output += "      \"midi\": {\n";
            const auto sourceMode = [&] {
                switch (control.midiMapping->sourceMode)
                {
                case MidiControlSourceMode::LegacyAnyInput:
                    return std::string_view{"legacyAnyInput"};
                case MidiControlSourceMode::ExactInput:
                    return std::string_view{"exactInput"};
                }
                return std::string_view{"invalid"};
            }();
            appendQuotedField(
                output,
                8,
                "sourceMode",
                sourceMode,
                true);
            appendQuotedField(
                output,
                8,
                "sourceIdentifier",
                control.midiMapping->sourceIdentifier,
                true);
            appendIntegerField(output, 8, "channel", control.midiMapping->channel, true);
            appendIntegerField(
                output,
                8,
                "controller",
                control.midiMapping->controller,
                false);
            output += control.oscMapping.has_value()
                    || control.oscEgressMapping.has_value()
                    || control.widget != VirtualControlWidget{}
                ? "      },\n"
                : "      }\n";
        }
        if (control.oscMapping.has_value())
        {
            output += "      \"oscMapping\": {\n";
            appendQuotedField(
                output,
                8,
                "address",
                control.oscMapping->address,
                false);
            output += control.oscEgressMapping.has_value()
                    || control.widget != VirtualControlWidget{}
                ? "      },\n"
                : "      }\n";
        }
        if (control.oscEgressMapping.has_value())
        {
            output += "      \"oscEgressMapping\": {\n";
            appendQuotedField(
                output,
                8,
                "address",
                control.oscEgressMapping->address,
                false);
            output += control.widget != VirtualControlWidget{}
                ? "      },\n"
                : "      }\n";
        }
        if (control.widget != VirtualControlWidget{})
            appendVirtualControlWidget(output, control.widget, 6);
        output += "    }";
        if (index + 1 < controls.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ]";
}

void appendSampleMapLibrary(
    std::string& output,
    const ProjectSampleMapLibraryState& library)
{
    const auto loopModeName = [](ProjectSampleLoopMode mode) {
        switch (mode)
        {
        case ProjectSampleLoopMode::Forward:
            return std::string_view{"forward"};
        case ProjectSampleLoopMode::Alternate:
            return std::string_view{"alternate"};
        }
        return std::string_view{};
    };
    const auto appendLoop = [&](
                                const std::optional<ProjectSampleLoop>& loop,
                                int indent) {
        if (!loop.has_value())
        {
            output += "null";
            return;
        }
        output += "{\n";
        appendQuotedField(
            output,
            indent + 2,
            "mode",
            loopModeName(loop->mode),
            true);
        appendUnsignedIntegerField(
            output,
            indent + 2,
            "startFrame",
            loop->startFrame,
            true);
        appendUnsignedIntegerField(
            output,
            indent + 2,
            "endFrameExclusive",
            loop->endFrameExclusive,
            false);
        appendIndent(output, indent);
        output.push_back('}');
    };

    output += "  \"sampleMapLibrary\": {\n";
    appendUnsignedIntegerField(
        output,
        4,
        "version",
        library.schemaVersion,
        true);
    if (library.maps.empty())
    {
        output += "    \"maps\": []\n  }";
        return;
    }
    output += "    \"maps\": [\n";
    for (std::size_t mapIndex = 0;
         mapIndex < library.maps.size();
         ++mapIndex)
    {
        const auto& map = library.maps[mapIndex];
        output += "      {\n";
        appendQuotedField(output, 8, "id", map.mapId, true);
        appendQuotedField(output, 8, "name", map.displayName, true);
        if (map.regions.empty())
        {
            output += "        \"regions\": []\n";
        }
        else
        {
            output += "        \"regions\": [\n";
            for (std::size_t regionIndex = 0;
                 regionIndex < map.regions.size();
                 ++regionIndex)
            {
                const auto& region = map.regions[regionIndex];
                output += "          {\n";
                appendQuotedField(output, 12, "id", region.regionId, true);
                appendQuotedField(output, 12, "assetId", region.assetId, true);
                appendUnsignedIntegerField(output, 12, "rootNote", region.rootNote, true);
                appendUnsignedIntegerField(output, 12, "lowNote", region.lowNote, true);
                appendUnsignedIntegerField(output, 12, "highNote", region.highNote, true);
                appendUnsignedIntegerField(output, 12, "lowVelocity", region.lowVelocity, true);
                appendUnsignedIntegerField(output, 12, "highVelocity", region.highVelocity, true);
                appendBooleanField(output, 12, "reverse", region.reverse, true);
                output += "            \"sustainLoop\": ";
                appendLoop(region.sustainLoop, 12);
                output += ",\n";
                output += "            \"releaseLoop\": ";
                appendLoop(region.releaseLoop, 12);
                output.push_back('\n');
                output += "          }";
                if (regionIndex + 1 < map.regions.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += "        ]\n";
        }
        output += "      }";
        if (mapIndex + 1 < library.maps.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "    ]\n  }";
}

void appendScoreTime(std::string& output,
                     int indent,
                     std::string_view key,
                     const ScoreTime& time,
                     bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": {\n";
    appendQuotedField(output, indent + 2, "domain", scoreTimeDomainName(time.domain), true);
    if (time.domain == ScoreTimeDomain::Seconds)
        appendNumberField(output, indent + 2, "seconds", time.seconds, false);
    else
        appendUnsignedIntegerField(output, indent + 2, "tick", time.tick, false);
    appendIndent(output, indent);
    output.push_back('}');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendScore(std::string& output, const ProjectScore& score)
{
    output += "    \"score\": {\n";
    output += "      \"tracks\": [\n";
    for (std::size_t index = 0; index < score.tracks.size(); ++index)
    {
        const auto& track = score.tracks[index];
        output += "        {\n";
        appendQuotedField(output, 10, "trackId", track.trackId, true);
        appendQuotedField(output, 10, "displayName", track.displayName, true);
        appendQuotedField(output, 10, "kind", scoreTrackKindName(track.kind), true);
        appendQuotedField(output, 10, "timeDomain", scoreTimeDomainName(track.timeDomain), true);
        appendQuotedField(output, 10, "outputTrackId", track.outputTrackId, true);
        // Format 127: the submix a track takes as its audio input.
        if (!track.inputTrackId.empty())
            appendQuotedField(output, 10, "inputTrackId", track.inputTrackId, true);
        // Format 133: the hardware audio input it takes instead.
        if (track.inputChannel != 0)
            appendUnsignedIntegerField(output, 10, "inputChannel", track.inputChannel, true);
        // Format 128: the submixes the track sends to besides its output.
        if (!track.sendTrackIds.empty())
            appendStringArrayField(output, 10, "sendTrackIds", track.sendTrackIds, true);
        // Format 131: the output channels the track is hard-wired to.
        if (!track.outputChannels.empty())
        {
            appendIndent(output, 10);
            output += "\"outputChannels\": [";
            for (std::size_t channel = 0; channel < track.outputChannels.size(); ++channel)
            {
                if (channel != 0)
                    output += ", ";
                output += std::to_string(track.outputChannels[channel]);
            }
            output += "],\n";
        }
        // Format 134: a track heard only through its sends.
        if (track.noOutput)
            appendBooleanField(output, 10, "noOutput", true, true);
        // Format 132: the channel the track's Sounds expect MIDI on.
        if (track.midiChannel != 0)
            appendUnsignedIntegerField(output, 10, "midiChannel", track.midiChannel, true);
        appendUnsignedIntegerField(output, 10, "channelCount", track.channelCount, true);
        appendQuotedField(output, 10, "overlapMode", scoreOverlapModeName(track.overlapMode), true);
        appendBooleanField(output, 10, "muted", track.muted, true);
        appendBooleanField(output, 10, "solo", track.solo, true);
        appendBooleanField(output, 10, "sharingMemoryWriters", track.sharingMemoryWriters, false);
        output += "        }";
        if (index + 1 < score.tracks.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "      ],\n";
    output += "      \"clips\": [\n";
    for (std::size_t index = 0; index < score.clips.size(); ++index)
    {
        const auto& clip = score.clips[index];
        output += "        {\n";
        appendQuotedField(output, 10, "clipId", clip.clipId, true);
        appendQuotedField(output, 10, "trackId", clip.trackId, true);
        appendQuotedField(output, 10, "soundDefinitionId", clip.soundDefinitionId, true);
        appendIndent(output, 10);
        output += "\"sourceInstanceId\": ";
        if (clip.sourceInstanceId.has_value())
            appendQuoted(output, *clip.sourceInstanceId);
        else
            output += "null";
        output += ",\n";
        appendScoreTime(output, 10, "start", clip.start, true);
        appendScoreTime(output, 10, "duration", clip.duration, true);
        appendScoreTime(output, 10, "contentOffset", clip.contentOffset, true);
        appendScoreTime(output, 10, "fadeIn", clip.fadeIn, true);
        appendScoreTime(output, 10, "fadeOut", clip.fadeOut, true);
        appendUnsignedIntegerField(output, 10, "repeatCount", clip.repeatCount, true);
        // Format 124: the anchor, written when it is not at the start.
        if (clip.anchorOffset.has_value())
            appendScoreTime(output, 10, "anchorOffset", *clip.anchorOffset, true);
        // Format 146: the clip's colour, written when it has one.
        if (clip.colour.has_value())
            appendUnsignedIntegerField(output, 10, "colour", *clip.colour, true);
        if (!clip.renderArtifact.has_value())
        {
            appendIndent(output, 10);
            output += "\"renderArtifact\": null\n";
        }
        else
        {
            const auto& artifact = *clip.renderArtifact;
            appendIndent(output, 10);
            output += "\"renderArtifact\": {\n";
            appendQuotedField(
                output,
                12,
                "mode",
                scoreClipRenderModeName(artifact.mode),
                true);
            appendQuotedField(
                output,
                12,
                "sourceFingerprint",
                artifact.sourceFingerprint,
                true);
            appendUnsignedIntegerField(
                output,
                12,
                "sampleRate",
                artifact.sampleRate,
                true);
            appendUnsignedIntegerField(
                output,
                12,
                "frameCount",
                artifact.frameCount,
                true);
            appendIndent(output, 12);
            output += "\"channels\": [";
            for (std::size_t channel = 0;
                 channel < artifact.channelAssetIds.size();
                 ++channel)
            {
                if (channel != 0)
                    output += ", ";
                output += "{\"assetId\": ";
                appendQuoted(output, artifact.channelAssetIds[channel]);
                output += "}";
            }
            output += "]\n";
            appendIndent(output, 10);
            output += "}\n";
        }
        output += "        }";
        if (index + 1 < score.clips.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "      ],\n";
    output += "      \"automation\": [\n";
    for (std::size_t index = 0; index < score.automation.size(); ++index)
    {
        const auto& lane = score.automation[index];
        output += "        {\n";
        appendQuotedField(output, 10, "laneId", lane.laneId, true);
        appendQuotedField(output, 10, "scope", scoreAutomationScopeName(lane.scope), true);
        appendQuotedField(output, 10, "ownerId", lane.ownerId, true);
        appendQuotedField(output, 10, "eventName", lane.eventName, true);
        appendQuotedField(output, 10, "domain", valueDomainName(lane.domain), true);
        // Format 123: a lane's state, written when it is not Automated.
        if (lane.state != ScoreAutomationState::Automated)
            appendQuotedField(output, 10, "state", scoreAutomationStateName(lane.state), true);
        const auto appendPoints = [&output](int indent, const std::vector<ScoreAutomationPoint>& points) {
            const auto pad = std::string(static_cast<std::size_t>(indent), ' ');
            output += pad + "\"points\": [\n";
            for (std::size_t pointIndex = 0; pointIndex < points.size(); ++pointIndex)
            {
                const auto& point = points[pointIndex];
                output += pad + "  {\n";
                appendScoreTime(output, indent + 4, "position", point.position, true);
                appendNumberField(output, indent + 4, "value", point.value, true);
                appendQuotedField(output,
                                  indent + 4,
                                  "interpolation",
                                  automationInterpolationName(point.interpolation),
                                  false);
                output += pad + "  }";
                if (pointIndex + 1 < points.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += pad + "]";
        };
        // Format 129: the take driving the lane when it is not take1, and
        // the lane's other takes, written when it has some.
        if (lane.takeName != "take1")
            appendQuotedField(output, 10, "takeName", lane.takeName, true);
        // Format 130: the lane it is slaved to.
        if (!lane.masterLaneId.empty())
            appendQuotedField(output, 10, "masterLaneId", lane.masterLaneId, true);
        // Format 136: the transformations its function passes through.
        if (!lane.transforms.empty())
        {
            output += "          \"transforms\": [";
            for (std::size_t transformIndex = 0; transformIndex < lane.transforms.size(); ++transformIndex)
            {
                const auto& transform = lane.transforms[transformIndex];
                if (transformIndex != 0)
                    output += ", ";
                output += "{\"kind\": \"";
                output += scoreAutomationTransformName(transform.kind);
                output += "\", \"value\": " + numberText(transform.value) + ", \"secondary\": "
                    + numberText(transform.secondary) + ", \"seed\": " + std::to_string(transform.seed) + "}";
            }
            output += "],\n";
        }
        if (!lane.takes.empty())
        {
            output += "          \"takes\": [\n";
            for (std::size_t takeIndex = 0; takeIndex < lane.takes.size(); ++takeIndex)
            {
                output += "            {\n";
                appendQuotedField(output, 14, "name", lane.takes[takeIndex].name, true);
                appendPoints(14, lane.takes[takeIndex].points);
                output += "\n            }";
                if (takeIndex + 1 < lane.takes.size())
                    output.push_back(',');
                output.push_back('\n');
            }
            output += "          ],\n";
        }
        appendPoints(10, lane.points);
        output += "\n";
        output += "        }";
        if (index + 1 < score.automation.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "      ],\n";
    output += "      \"markers\": [\n";
    for (std::size_t index = 0; index < score.markers.size(); ++index)
    {
        const auto& marker = score.markers[index];
        output += "        {\n";
        appendQuotedField(output, 10, "markerId", marker.markerId, true);
        appendQuotedField(output, 10, "title", marker.title, true);
        appendScoreTime(output, 10, "start", marker.start, true);
        // Format 124: the program change, written when it is not the
        // marker's number.
        if (marker.programNumber.has_value())
            appendUnsignedIntegerField(output, 10, "programNumber", *marker.programNumber, true);
        if (marker.end.has_value())
            appendScoreTime(output, 10, "end", *marker.end, false);
        else
        {
            appendIndent(output, 10);
            output += "\"end\": null\n";
        }
        output += "        }";
        if (index + 1 < score.markers.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "      ]\n";
    output += "    }";
}

void appendTimeline(std::string& output, const ProjectTimeline& timeline)
{
    output += "  \"timeline\": {\n";
    output += "    \"loop\": {\n";
    appendBooleanField(output, 6, "enabled", timeline.loop.enabled, true);
    appendNumberField(output, 6, "startSeconds", timeline.loop.startSeconds, true);
    appendNumberField(output, 6, "endSeconds", timeline.loop.endSeconds, false);
    output += "    },\n";
    output += "    \"musicalTime\": {\n";
    appendUnsignedIntegerField(
        output,
        6,
        "ticksPerQuarter",
        timeline.musicalTime.ticksPerQuarter,
        true);
    output += "      \"tempo\": [\n";
    for (std::size_t index = 0;
         index < timeline.musicalTime.tempo.size();
         ++index)
    {
        const auto& event = timeline.musicalTime.tempo[index];
        output += "        {\n";
        appendUnsignedIntegerField(output, 10, "tick", event.tick, true);
        appendUnsignedIntegerField(
            output,
            10,
            "microsecondsPerQuarter",
            event.microsecondsPerQuarter,
            false);
        output += "        }";
        if (index + 1 < timeline.musicalTime.tempo.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "      ],\n";
    output += "      \"meter\": [\n";
    for (std::size_t index = 0;
         index < timeline.musicalTime.meter.size();
         ++index)
    {
        const auto& event = timeline.musicalTime.meter[index];
        output += "        {\n";
        appendUnsignedIntegerField(output, 10, "tick", event.tick, true);
        appendUnsignedIntegerField(
            output, 10, "numerator", event.numerator, true);
        appendUnsignedIntegerField(
            output, 10, "denominator", event.denominator, false);
        output += "        }";
        if (index + 1 < timeline.musicalTime.meter.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "      ]\n";
    output += "    },\n";
    appendScore(output, timeline.score);
    output += ",\n";
    // Format 124: the channel of the program changes that jump to markers,
    // written when one is set.
    if (timeline.markerProgramChannel != 0)
        appendUnsignedIntegerField(output, 4, "markerProgramChannel", timeline.markerProgramChannel, true);
    // Format 135: the time code offset, written when one is set.
    if (timeline.timeCodeOffsetSeconds != 0.0)
        appendNumberField(output, 4, "timeCodeOffsetSeconds", timeline.timeCodeOffsetSeconds, true);
    // Format 146: the Timeline's own time code frames a second, when set.
    if (timeline.timeCodeFramesPerSecond != 0)
        appendUnsignedIntegerField(output, 4, "timeCodeFramesPerSecond", timeline.timeCodeFramesPerSecond, true);
    // Format 154: the caches' word size, when it is not 24 bits.
    if (timeline.cacheWordSize != 24)
        appendUnsignedIntegerField(output, 4, "cacheWordSize", timeline.cacheWordSize, true);
    // Format 156: recordings shared across the tracks, when on.
    if (timeline.sharingMemoryWritersAcrossTracks)
        appendBooleanField(output, 4, "sharingMemoryWritersAcrossTracks", true, true);
    output += "    \"eventValues\": ";
    if (timeline.eventValues.empty())
    {
        output += "[]\n";
    }
    else
    {
        output += "[\n";
        for (std::size_t index = 0; index < timeline.eventValues.size(); ++index)
        {
            const auto& event = timeline.eventValues[index];
            output += "      {\n";
            appendNumberField(output, 8, "timeSeconds", event.timeSeconds, true);
            appendQuotedField(output, 8, "eventName", event.eventName, true);
            appendQuotedField(output, 8, "domain", valueDomainName(event.domain), true);
            appendNumberField(output, 8, "value", event.value, true);
            appendQuotedField(
                output,
                8,
                "interpolation",
                automationInterpolationName(event.interpolation),
                false);
            output += "      }";
            if (index + 1 < timeline.eventValues.size())
                output.push_back(',');
            output.push_back('\n');
        }
        output += "    ]\n";
    }
    output += "  }";
}

void appendAssets(std::string& output, const std::vector<ProjectAsset>& assets)
{
    output += "  \"assets\": ";
    if (assets.empty())
    {
        output += "[]";
        return;
    }
    output += "[\n";
    for (std::size_t index = 0; index < assets.size(); ++index)
    {
        const auto& asset = assets[index];
        output += "    {\n";
        appendQuotedField(output, 6, "id", asset.id, true);
        appendQuotedField(output, 6, "kind", projectAssetKindName(asset.kind), true);
        appendQuotedField(output, 6, "path", asset.path, true);
        appendQuotedField(
            output,
            6,
            "disposition",
            projectAssetDispositionName(asset.disposition),
            true);
        appendQuotedField(output, 6, "contentHash", asset.contentHash, true);
        appendUnsignedIntegerField(
            output,
            6,
            "channelCount",
            asset.channelCount,
            true);
        appendNumberField(output, 6, "sampleRate", asset.sampleRate, true);
        appendUnsignedIntegerField(
            output,
            6,
            "frameCount",
            asset.frameCount,
            true);
        appendQuotedField(output, 6, "encoding", asset.encoding, true);
        appendUnsignedIntegerField(
            output,
            6,
            "generation",
            asset.generation,
            true);
        appendQuotedField(output, 6, "provenance", asset.provenance, false);
        output += "    }";
        if (index + 1 < assets.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ]";
}

[[nodiscard]] std::string_view partialTrackInterpolationName(
    PartialTrackPointInterpolation interpolation)
{
    switch (interpolation)
    {
    case PartialTrackPointInterpolation::Step: return "step";
    case PartialTrackPointInterpolation::Linear: return "linear";
    }
    return "linear";
}

void appendPartialTrackEdits(
    std::string& output,
    const std::vector<ProjectPartialTrackEditSet>& editSets)
{
    output += "  \"partialTrackEdits\": ";
    if (editSets.empty())
    {
        output += "[]";
        return;
    }
    output += "[\n";
    for (std::size_t setIndex = 0; setIndex < editSets.size(); ++setIndex)
    {
        const auto& edits = editSets[setIndex];
        output += "    {\n";
        appendQuotedField(output, 6, "editSetId", edits.editSetId, true);
        appendQuotedField(output, 6, "assetId", edits.assetId, true);
        appendUnsignedIntegerField(
            output, 6, "assetGeneration", edits.assetGeneration, true);
        appendQuotedField(
            output, 6, "sourceContentHash", edits.sourceContentHash, true);
        output += "      \"pointOverrides\": [";
        if (!edits.pointOverrides.empty())
            output.push_back('\n');
        for (std::size_t index = 0;
             index < edits.pointOverrides.size();
             ++index)
        {
            const auto& point = edits.pointOverrides[index];
            output += "        {\n";
            appendUnsignedIntegerField(output, 10, "sourceSegment", point.source.segmentId, true);
            appendUnsignedIntegerField(output, 10, "sourceFrame", point.source.sourceFrame, true);
            appendUnsignedIntegerField(output, 10, "sourceTrack", point.source.trackId, true);
            appendUnsignedIntegerField(output, 10, "outputFrame", point.outputFrame, true);
            appendUnsignedIntegerField(output, 10, "outputTrack", point.output.trackId, true);
            appendNumberField(output, 10, "amplitude", point.output.amplitude, true);
            appendNumberField(output, 10, "frequencyHz", point.output.frequencyHz, true);
            appendNumberField(output, 10, "bandwidthHz", point.output.bandwidthHz, true);
            appendQuotedField(output, 10, "interpolation", partialTrackInterpolationName(point.interpolation), true);
            appendBooleanField(output, 10, "excluded", point.excluded, false);
            output += "        }";
            if (index + 1 < edits.pointOverrides.size())
                output.push_back(',');
            output.push_back('\n');
        }
        output += "      ],\n";
        output += "      \"insertedPoints\": [";
        if (!edits.insertedPoints.empty())
            output.push_back('\n');
        for (std::size_t index = 0;
             index < edits.insertedPoints.size();
             ++index)
        {
            const auto& point = edits.insertedPoints[index];
            output += "        {\n";
            appendQuotedField(output, 10, "editId", point.editId, true);
            appendUnsignedIntegerField(output, 10, "segment", point.segmentId, true);
            appendUnsignedIntegerField(output, 10, "frame", point.sourceFrame, true);
            appendUnsignedIntegerField(output, 10, "track", point.point.trackId, true);
            appendNumberField(output, 10, "amplitude", point.point.amplitude, true);
            appendNumberField(output, 10, "frequencyHz", point.point.frequencyHz, true);
            appendNumberField(output, 10, "bandwidthHz", point.point.bandwidthHz, true);
            appendQuotedField(output, 10, "interpolation", partialTrackInterpolationName(point.interpolation), false);
            output += "        }";
            if (index + 1 < edits.insertedPoints.size())
                output.push_back(',');
            output.push_back('\n');
        }
        output += "      ],\n";
        output += "      \"trackStates\": [";
        if (!edits.trackStates.empty())
            output.push_back('\n');
        for (std::size_t index = 0;
             index < edits.trackStates.size();
             ++index)
        {
            const auto& state = edits.trackStates[index];
            output += "        {\n";
            appendUnsignedIntegerField(output, 10, "track", state.trackId, true);
            appendBooleanField(output, 10, "muted", state.muted, true);
            appendBooleanField(output, 10, "solo", state.solo, false);
            output += "        }";
            if (index + 1 < edits.trackStates.size())
                output.push_back(',');
            output.push_back('\n');
        }
        output += "      ]\n";
        output += "    }";
        if (setIndex + 1 < editSets.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ]";
}

template <typename Value, typename AppendValue>
void appendInlineArray(
    std::string& output,
    const std::vector<Value>& values,
    AppendValue appendValue)
{
    output.push_back('[');
    for (std::size_t index = 0; index < values.size(); ++index)
    {
        if (index != 0)
            output += ", ";
        appendValue(output, values[index]);
    }
    output.push_back(']');
}

void appendDeviceSettings(
    std::string& output,
    const ProjectDeviceSettings& settings)
{
    output += "  \"deviceSettings\": {\n";
    appendBooleanField(output, 4, "configured", settings.configured, true);
    appendQuotedField(
        output,
        4,
        "audioInputDevice",
        settings.audioInputDeviceName,
        true);
    appendQuotedField(
        output,
        4,
        "audioOutputDevice",
        settings.audioOutputDeviceName,
        true);
    appendNumberField(output, 4, "sampleRate", settings.sampleRate, true);
    appendIntegerField(output, 4, "bufferSize", settings.bufferSize, true);
    output += "    \"inputChannels\": ";
    appendInlineArray(
        output,
        settings.inputChannels,
        [](std::string& target, std::int32_t value) {
            target += std::to_string(value);
        });
    output += ",\n    \"outputChannels\": ";
    appendInlineArray(
        output,
        settings.outputChannels,
        [](std::string& target, std::int32_t value) {
            target += std::to_string(value);
        });
    output += ",\n    \"midiInputDevices\": ";
    appendInlineArray(
        output,
        settings.midiInputDeviceIdentifiers,
        [](std::string& target, const std::string& value) {
            appendQuoted(target, value);
        });
    output += ",\n    \"transportSync\": {\n";
    appendQuotedField(
        output,
        6,
        "sourceMode",
        settings.transportSync.sourceMode
                == TransportSyncSourceMode::MidiClockExactInput
            ? "midiClockExactInput"
            : settings.transportSync.sourceMode
                    == TransportSyncSourceMode::MidiTimeCodeExactInput
                ? "midiTimeCodeExactInput"
                : "internal",
        true);
    appendQuotedField(
        output,
        6,
        "sourceIdentifier",
        settings.transportSync.sourceIdentifier,
        false);
    output += "    },\n    \"oscIngress\": {\n";
    appendBooleanField(
        output,
        6,
        "enabled",
        settings.oscIngress.enabled,
        true);
    appendIntegerField(
        output,
        6,
        "port",
        settings.oscIngress.port,
        false);
    output += "    },\n    \"oscEgress\": {\n";
    appendBooleanField(
        output,
        6,
        "enabled",
        settings.oscEgress.enabled,
        true);
    appendIntegerField(
        output,
        6,
        "port",
        settings.oscEgress.port,
        false);
    output += "    }\n  }";
}

void addError(ProjectLoadResult& result, std::string path, std::string message)
{
    result.errors.push_back({
        std::move(message),
        std::move(path),
    });
}

const json::Value* objectMember(const json::Value& value, std::string_view key)
{
    return json::findObjectMember(value, key);
}

bool requiredStringMember(ProjectLoadResult& result,
                          const json::Value& object,
                          std::string_view key,
                          const std::string& path,
                          std::string& output)
{
    const auto* value = objectMember(object, key);
    if (value == nullptr)
    {
        addError(result, path, "Missing required string");
        return false;
    }
    if (!value->isString())
    {
        addError(result, path, "Expected string");
        return false;
    }
    output = value->string();
    return true;
}

bool requiredUnsignedJsonIntegerMember(
    ProjectLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::uint64_t maximum,
    std::uint64_t& output);

bool parseSigned32BitIntegerToken(std::string_view token, std::int32_t& output)
{
    std::size_t offset = 0;
    const bool isNegative = !token.empty() && token.front() == '-';
    if (isNegative)
        ++offset;

    const auto exponentMarker = token.find_first_of("eE", offset);
    const auto mantissaEnd = exponentMarker == std::string_view::npos ? token.size() : exponentMarker;
    std::int64_t exponent = 0;
    if (exponentMarker != std::string_view::npos)
    {
        offset = exponentMarker + 1;
        const bool exponentIsNegative = offset < token.size() && token[offset] == '-';
        if (exponentIsNegative || (offset < token.size() && token[offset] == '+'))
            ++offset;

        for (; offset < token.size(); ++offset)
        {
            const auto digit = static_cast<std::int64_t>(token[offset] - '0');
            if (exponent > 1000)
                exponent = 1001;
            else
                exponent = exponent * 10 + digit;
        }
        if (exponentIsNegative)
            exponent = -exponent;
    }

    std::string digits;
    digits.reserve(mantissaEnd);
    std::size_t fractionalDigits = 0;
    bool inFraction = false;
    for (std::size_t index = isNegative ? 1 : 0; index < mantissaEnd; ++index)
    {
        if (token[index] == '.')
        {
            inFraction = true;
            continue;
        }
        digits.push_back(token[index]);
        if (inFraction)
            ++fractionalDigits;
    }

    const auto firstNonZero = digits.find_first_not_of('0');
    if (firstNonZero == std::string::npos)
    {
        output = 0;
        return true;
    }

    const std::int64_t scale = exponent - static_cast<std::int64_t>(fractionalDigits);
    std::size_t integralEnd = digits.size();
    if (scale < 0)
    {
        const auto requiredTrailingZeroes = static_cast<std::size_t>(-scale);
        std::size_t trailingZeroes = 0;
        while (trailingZeroes < digits.size() && digits[digits.size() - trailingZeroes - 1] == '0')
            ++trailingZeroes;
        if (trailingZeroes < requiredTrailingZeroes)
            return false;
        integralEnd -= requiredTrailingZeroes;
    }
    else if (scale > 9)
    {
        return false;
    }

    const std::uint64_t limit = isNegative
        ? static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) + 1U
        : static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    std::uint64_t magnitude = 0;
    const auto appendDigit = [&](std::uint64_t digit) {
        if (magnitude > (limit - digit) / 10U)
            return false;
        magnitude = magnitude * 10U + digit;
        return true;
    };

    for (std::size_t index = firstNonZero; index < integralEnd; ++index)
    {
        if (!appendDigit(static_cast<std::uint64_t>(digits[index] - '0')))
            return false;
    }
    for (std::int64_t index = 0; index < scale; ++index)
    {
        if (!appendDigit(0U))
            return false;
    }

    if (isNegative && magnitude == limit)
        output = std::numeric_limits<std::int32_t>::min();
    else
        output = isNegative ? -static_cast<std::int32_t>(magnitude) : static_cast<std::int32_t>(magnitude);
    return true;
}

bool requiredExactUnsignedIntegerMember(
    ProjectLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::uint64_t maximum,
    std::uint64_t& output)
{
    const auto* value = objectMember(object, key);
    if (value == nullptr || !value->isNumber())
    {
        addError(result, path, "Expected exact unsigned integer");
        return false;
    }
    const auto token = value->numberToken();
    if (token.empty()
        || std::ranges::any_of(token, [](char character) {
               return character < '0' || character > '9';
           }))
    {
        addError(result, path, "Expected exact unsigned integer token");
        return false;
    }
    std::uint64_t parsed = 0;
    const auto [end, error] = std::from_chars(
        token.data(), token.data() + token.size(), parsed);
    if (error != std::errc{} || end != token.data() + token.size()
        || parsed > maximum)
    {
        addError(result, path, "Unsigned integer is out of range");
        return false;
    }
    output = parsed;
    return true;
}

void parseMusicalTime(
    ProjectLoadResult& result,
    const json::Value& timeline,
    int sourceVersion)
{
    const auto* source = objectMember(timeline, "musicalTime");
    if (sourceVersion < versionSixteen)
    {
        if (source != nullptr)
        {
            addError(
                result,
                "$.timeline.musicalTime",
                "Musical time requires format 16");
        }
        return;
    }
    if (source == nullptr || !source->isObject()
        || source->object().size() != 3)
    {
        addError(
            result,
            "$.timeline.musicalTime",
            "Expected closed musical-time object");
        return;
    }

    ProjectMusicalTime candidate;
    candidate.tempo.clear();
    candidate.meter.clear();
    std::uint64_t ticksPerQuarter = 0;
    auto valid = requiredExactUnsignedIntegerMember(
        result,
        *source,
        "ticksPerQuarter",
        "$.timeline.musicalTime.ticksPerQuarter",
        std::numeric_limits<std::uint32_t>::max(),
        ticksPerQuarter);
    candidate.ticksPerQuarter = static_cast<std::uint32_t>(ticksPerQuarter);

    const auto* tempo = objectMember(*source, "tempo");
    if (tempo == nullptr || !tempo->isArray()
        || tempo->array().empty()
        || tempo->array().size() > maximumMusicalMapEvents)
    {
        addError(
            result,
            "$.timeline.musicalTime.tempo",
            "Expected 1..1024 tempo events");
        valid = false;
    }
    else
    {
        for (std::size_t index = 0; index < tempo->array().size(); ++index)
        {
            const auto& value = tempo->array()[index];
            const auto path = "$.timeline.musicalTime.tempo["
                + std::to_string(index) + "]";
            if (!value.isObject() || value.object().size() != 2)
            {
                addError(result, path, "Expected closed tempo event");
                valid = false;
                continue;
            }
            std::uint64_t tick = 0;
            std::uint64_t microseconds = 0;
            const auto eventValid = requiredExactUnsignedIntegerMember(
                                        result,
                                        value,
                                        "tick",
                                        path + ".tick",
                                        std::numeric_limits<std::uint64_t>::max(),
                                        tick)
                && requiredExactUnsignedIntegerMember(
                    result,
                    value,
                    "microsecondsPerQuarter",
                    path + ".microsecondsPerQuarter",
                    std::numeric_limits<std::uint32_t>::max(),
                    microseconds);
            if (!eventValid)
            {
                valid = false;
                continue;
            }
            candidate.tempo.push_back({
                tick,
                static_cast<std::uint32_t>(microseconds),
            });
        }
    }

    const auto* meter = objectMember(*source, "meter");
    if (meter == nullptr || !meter->isArray()
        || meter->array().empty()
        || meter->array().size() > maximumMusicalMapEvents)
    {
        addError(
            result,
            "$.timeline.musicalTime.meter",
            "Expected 1..1024 meter events");
        valid = false;
    }
    else
    {
        for (std::size_t index = 0; index < meter->array().size(); ++index)
        {
            const auto& value = meter->array()[index];
            const auto path = "$.timeline.musicalTime.meter["
                + std::to_string(index) + "]";
            if (!value.isObject() || value.object().size() != 3)
            {
                addError(result, path, "Expected closed meter event");
                valid = false;
                continue;
            }
            std::uint64_t tick = 0;
            std::uint64_t numerator = 0;
            std::uint64_t denominator = 0;
            const auto eventValid = requiredExactUnsignedIntegerMember(
                                        result,
                                        value,
                                        "tick",
                                        path + ".tick",
                                        std::numeric_limits<std::uint64_t>::max(),
                                        tick)
                && requiredExactUnsignedIntegerMember(
                    result,
                    value,
                    "numerator",
                    path + ".numerator",
                    std::numeric_limits<std::uint8_t>::max(),
                    numerator)
                && requiredExactUnsignedIntegerMember(
                    result,
                    value,
                    "denominator",
                    path + ".denominator",
                    std::numeric_limits<std::uint8_t>::max(),
                    denominator);
            if (!eventValid)
            {
                valid = false;
                continue;
            }
            candidate.meter.push_back({
                tick,
                static_cast<std::uint8_t>(numerator),
                static_cast<std::uint8_t>(denominator),
            });
        }
    }
    if (!valid)
        return;
    if (const auto error = projectMusicalTimeValidationError(candidate))
    {
        addError(result, "$.timeline.musicalTime", *error);
        return;
    }
    result.document.timeline.musicalTime = std::move(candidate);
}

bool parseCanvasCoordinate(ProjectLoadResult& result,
                           const json::Value& position,
                           std::string_view coordinate,
                           const std::string& path,
                           std::int32_t& output)
{
    const auto* value = objectMember(position, coordinate);
    if (value == nullptr)
    {
        addError(result, path, "Missing required signed 32-bit integer");
        return false;
    }
    if (!value->isNumber())
    {
        addError(result, path, "Expected signed 32-bit integer");
        return false;
    }

    if (!parseSigned32BitIntegerToken(value->numberToken(), output))
    {
        addError(result, path, "Expected signed 32-bit integer");
        return false;
    }
    return true;
}

bool parseCanvasPosition(ProjectLoadResult& result,
                         const json::Value& node,
                         const std::string& nodePath,
                         CanvasPosition& output)
{
    const auto* position = objectMember(node, "position");
    if (position == nullptr)
        return true;
    if (!position->isObject())
    {
        addError(result, nodePath + ".position", "Expected object");
        return false;
    }

    const bool hasX = parseCanvasCoordinate(result, *position, "x", nodePath + ".position.x", output.x);
    const bool hasY = parseCanvasCoordinate(result, *position, "y", nodePath + ".position.y", output.y);
    return hasX && hasY;
}

void parseMetadata(ProjectLoadResult& result, const json::Value& root)
{
    const auto* metadata = objectMember(root, "metadata");
    if (metadata == nullptr)
    {
        addError(result, "$.metadata", "Missing required object");
        return;
    }
    if (!metadata->isObject())
    {
        addError(result, "$.metadata", "Expected object");
        return;
    }

    std::string title;
    if (requiredStringMember(result, *metadata, "title", "$.metadata.title", title))
        result.document.metadata.title = std::move(title);
}

std::vector<ProjectParameterExpression> parseParameters(ProjectLoadResult& result,
                                                        const json::Value& node,
                                                        const std::string& nodePath)
{
    std::vector<ProjectParameterExpression> parameters;
    const auto* parametersValue = objectMember(node, "parameters");
    if (parametersValue == nullptr)
        return parameters;
    if (!parametersValue->isArray())
    {
        addError(result, nodePath + ".parameters", "Expected array");
        return parameters;
    }

    const auto& parameterArray = parametersValue->array();
    for (std::size_t index = 0; index < parameterArray.size(); ++index)
    {
        const auto& parameterValue = parameterArray[index];
        const std::string parameterPath = nodePath + ".parameters[" + std::to_string(index) + "]";
        if (!parameterValue.isObject())
        {
            addError(result, parameterPath, "Expected object");
            continue;
        }

        ProjectParameterExpression parameter;
        const bool hasId = requiredStringMember(result, parameterValue, "id", parameterPath + ".id", parameter.parameterId);
        const auto* expressionValue = objectMember(parameterValue, "expression");
        const auto* choiceValue = objectMember(parameterValue, "choice");
        const auto* booleanValue = objectMember(parameterValue, "boolean");
        const auto* resourceValue = objectMember(parameterValue, "resource");
        const auto* textValue = objectMember(parameterValue, "text");
        bool hasParameterValue = false;

        int parameterValueCount = 0;
        if (expressionValue != nullptr)
            ++parameterValueCount;
        if (choiceValue != nullptr)
            ++parameterValueCount;
        if (booleanValue != nullptr)
            ++parameterValueCount;
        if (resourceValue != nullptr)
            ++parameterValueCount;
        if (textValue != nullptr)
            ++parameterValueCount;

        if (parameterValueCount > 1)
        {
            addError(result, parameterPath, "Expected exactly one of expression, choice, boolean, resource, or text");
        }
        else if (expressionValue != nullptr)
        {
            if (!expressionValue->isString())
            {
                addError(result, parameterPath + ".expression", "Expected string");
            }
            else
            {
                parameter.expression = expressionValue->string();
                hasParameterValue = true;
            }
        }
        else if (choiceValue != nullptr)
        {
            if (!choiceValue->isString())
            {
                addError(result, parameterPath + ".choice", "Expected string");
            }
            else
            {
                parameter.choice = choiceValue->string();
                hasParameterValue = true;
            }
        }
        else if (booleanValue != nullptr)
        {
            if (!booleanValue->isBoolean())
            {
                addError(result, parameterPath + ".boolean", "Expected boolean");
            }
            else
            {
                parameter.boolean = booleanValue->boolean();
                hasParameterValue = true;
            }
        }
        else if (resourceValue != nullptr)
        {
            if (!resourceValue->isString())
            {
                addError(result, parameterPath + ".resource", "Expected string");
            }
            else
            {
                parameter.resource = resourceValue->string();
                hasParameterValue = true;
            }
        }
        else if (textValue != nullptr)
        {
            if (!textValue->isString())
            {
                addError(result, parameterPath + ".text", "Expected string");
            }
            else
            {
                parameter.text = textValue->string();
                hasParameterValue = true;
            }
        }
        else
        {
            addError(result, parameterPath, "Missing required expression, choice, boolean, resource, or text");
        }

        if (hasId && hasParameterValue)
            parameters.push_back(std::move(parameter));
    }

    return parameters;
}

[[nodiscard]] std::optional<talas::core::PortSignalRate>
parsePortSignalRate(std::string_view value)
{
    if (value == "constant") return talas::core::PortSignalRate::Constant;
    if (value == "block") return talas::core::PortSignalRate::Block;
    if (value == "sample") return talas::core::PortSignalRate::Sample;
    if (value == "event") return talas::core::PortSignalRate::Event;
    if (value == "frame") return talas::core::PortSignalRate::Frame;
    if (value == "variableOrUnknown")
        return talas::core::PortSignalRate::VariableOrUnknown;
    return std::nullopt;
}

[[nodiscard]] std::optional<talas::core::AudioBusLayoutKind>
parseAudioBusLayoutKind(std::string_view value)
{
    if (value == "notApplicable")
        return talas::core::AudioBusLayoutKind::NotApplicable;
    if (value == "mono") return talas::core::AudioBusLayoutKind::Mono;
    if (value == "stereo") return talas::core::AudioBusLayoutKind::Stereo;
    if (value == "discrete4")
        return talas::core::AudioBusLayoutKind::Discrete4;
    if (value == "discrete6")
        return talas::core::AudioBusLayoutKind::Discrete6;
    if (value == "discrete8")
        return talas::core::AudioBusLayoutKind::Discrete8;
    if (value == "variableOrUnknown")
        return talas::core::AudioBusLayoutKind::VariableOrUnknown;
    return std::nullopt;
}

[[nodiscard]] std::optional<talas::core::PortEdgeSemantics>
parsePortEdgeSemantics(std::string_view value)
{
    if (value == "notApplicable")
        return talas::core::PortEdgeSemantics::NotApplicable;
    if (value == "continuous")
        return talas::core::PortEdgeSemantics::Continuous;
    if (value == "gate") return talas::core::PortEdgeSemantics::Gate;
    if (value == "trigger") return talas::core::PortEdgeSemantics::Trigger;
    return std::nullopt;
}

[[nodiscard]] std::optional<talas::core::SpectralFrameFormat>
parseSpectralFrameFormat(std::string_view value)
{
    if (value == "notApplicable")
        return talas::core::SpectralFrameFormat::NotApplicable;
    if (value == "legacyPartialTriples")
        return talas::core::SpectralFrameFormat::LegacyPartialTriples;
    if (value == "partialSpectrumV1")
        return talas::core::SpectralFrameFormat::PartialSpectrumV1;
    if (value == "complexSpectrumV1")
        return talas::core::SpectralFrameFormat::ComplexSpectrumV1;
    if (value == "variableOrUnknown")
        return talas::core::SpectralFrameFormat::VariableOrUnknown;
    return std::nullopt;
}

bool parsePreservedPorts(
    ProjectLoadResult& result,
    const json::Value& component,
    std::string_view key,
    const std::string& componentPath,
    std::size_t maximumCount,
    std::vector<ProjectPreservedPort>& destination)
{
    const auto path = componentPath + "." + std::string(key);
    const auto* ports = objectMember(component, key);
    if (ports == nullptr || !ports->isArray())
    {
        addError(result, path, "Expected array");
        return false;
    }
    if (ports->array().size() > maximumCount)
    {
        addError(result, path, "Frozen port count exceeds the supported limit");
        return false;
    }
    bool valid = true;
    std::unordered_set<std::string> identifiers;
    for (std::size_t index = 0; index < ports->array().size(); ++index)
    {
        const auto& source = ports->array()[index];
        const auto portPath = path + "[" + std::to_string(index) + "]";
        if (!source.isObject())
        {
            addError(result, portPath, "Expected object");
            valid = false;
            continue;
        }
        if (source.object().size() != 4)
        {
            addError(result, portPath, "Expected id, type, required, and signal only");
            valid = false;
        }

        ProjectPreservedPort port;
        std::string type;
        auto portValid = requiredStringMember(
            result, source, "id", portPath + ".id", port.portId);
        portValid = requiredStringMember(
                        result, source, "type", portPath + ".type", type)
            && portValid;
        const auto parsedType = parsePortType(type);
        if (!parsedType.has_value())
        {
            addError(result, portPath + ".type", "Expected known port type");
            portValid = false;
        }
        const auto* required = objectMember(source, "required");
        if (required == nullptr || !required->isBoolean())
        {
            addError(result, portPath + ".required", "Expected boolean");
            portValid = false;
        }
        if (port.portId.empty()
            || port.portId.size()
                > maximumProjectPreservedComponentTextBytes)
        {
            addError(result, portPath + ".id", "Expected bounded non-empty port id");
            portValid = false;
        }
        else if (!identifiers.insert(port.portId).second)
        {
            addError(result, portPath + ".id", "Duplicate frozen port id");
            portValid = false;
        }

        const auto* signal = objectMember(source, "signal");
        if (signal == nullptr || !signal->isObject())
        {
            addError(result, portPath + ".signal", "Expected object");
            portValid = false;
        }
        else
        {
            if (signal->object().size() != 5)
            {
                addError(
                    result,
                    portPath + ".signal",
                    "Expected rate, audioLayout, edge, spectralFormat, and timelineBearing only");
                portValid = false;
            }
            std::string rate;
            std::string audioLayout;
            std::string edge;
            std::string spectralFormat;
            auto signalValid = requiredStringMember(
                result,
                *signal,
                "rate",
                portPath + ".signal.rate",
                rate);
            signalValid = requiredStringMember(
                              result,
                              *signal,
                              "audioLayout",
                              portPath + ".signal.audioLayout",
                              audioLayout)
                && signalValid;
            signalValid = requiredStringMember(
                              result,
                              *signal,
                              "edge",
                              portPath + ".signal.edge",
                              edge)
                && signalValid;
            signalValid = requiredStringMember(
                              result,
                              *signal,
                              "spectralFormat",
                              portPath + ".signal.spectralFormat",
                              spectralFormat)
                && signalValid;
            const auto parsedRate = parsePortSignalRate(rate);
            const auto parsedLayout = parseAudioBusLayoutKind(audioLayout);
            const auto parsedEdge = parsePortEdgeSemantics(edge);
            const auto parsedSpectral =
                parseSpectralFrameFormat(spectralFormat);
            if (!parsedRate.has_value())
                addError(result, portPath + ".signal.rate", "Expected known signal rate");
            if (!parsedLayout.has_value())
                addError(result, portPath + ".signal.audioLayout", "Expected known audio layout");
            if (!parsedEdge.has_value())
                addError(result, portPath + ".signal.edge", "Expected known edge semantics");
            if (!parsedSpectral.has_value())
                addError(result, portPath + ".signal.spectralFormat", "Expected known spectral format");
            const auto* timelineBearing =
                objectMember(*signal, "timelineBearing");
            if (timelineBearing == nullptr
                || !timelineBearing->isBoolean())
            {
                addError(result, portPath + ".signal.timelineBearing", "Expected boolean");
                signalValid = false;
            }
            signalValid = signalValid && parsedRate.has_value()
                && parsedLayout.has_value() && parsedEdge.has_value()
                && parsedSpectral.has_value();
            if (signalValid)
            {
                port.signal = {
                    *parsedRate,
                    *parsedLayout,
                    *parsedEdge,
                    *parsedSpectral,
                    timelineBearing->boolean(),
                };
            }
            portValid = signalValid && portValid;
        }
        if (portValid)
        {
            port.type = *parsedType;
            port.required = required->boolean();
            destination.push_back(std::move(port));
        }
        else
        {
            valid = false;
        }
    }
    return valid;
}

bool parsePreservedParameters(
    ProjectLoadResult& result,
    const json::Value& component,
    const std::string& componentPath,
    std::vector<ProjectPreservedParameter>& destination)
{
    const auto path = componentPath + ".parameters";
    const auto* parameters = objectMember(component, "parameters");
    if (parameters == nullptr || !parameters->isArray())
    {
        addError(result, path, "Expected array");
        return false;
    }
    if (parameters->array().size()
        > maximumProjectPreservedComponentParameters)
    {
        addError(result, path, "Frozen parameter count exceeds the supported limit");
        return false;
    }
    bool valid = true;
    std::unordered_set<std::string> identifiers;
    for (std::size_t index = 0;
         index < parameters->array().size();
         ++index)
    {
        const auto& source = parameters->array()[index];
        const auto parameterPath =
            path + "[" + std::to_string(index) + "]";
        if (!source.isObject())
        {
            addError(result, parameterPath, "Expected object");
            valid = false;
            continue;
        }
        if (source.object().size() != 4)
        {
            addError(result, parameterPath, "Expected id, kind, domain, and choices only");
            valid = false;
        }
        ProjectPreservedParameter parameter;
        std::string kind;
        std::string domain;
        auto parameterValid = requiredStringMember(
            result,
            source,
            "id",
            parameterPath + ".id",
            parameter.parameterId);
        parameterValid = requiredStringMember(
                             result,
                             source,
                             "kind",
                             parameterPath + ".kind",
                             kind)
            && parameterValid;
        parameterValid = requiredStringMember(
                             result,
                             source,
                             "domain",
                             parameterPath + ".domain",
                             domain)
            && parameterValid;
        const auto parsedKind = parseParameterKind(kind);
        const auto parsedDomain = parseValueDomain(domain);
        if (!parsedKind.has_value())
            addError(result, parameterPath + ".kind", "Expected known parameter kind");
        if (!parsedDomain.has_value())
            addError(result, parameterPath + ".domain", "Expected known value domain");
        parameterValid = parameterValid && parsedKind.has_value()
            && parsedDomain.has_value();
        if (parameter.parameterId.empty()
            || parameter.parameterId.size()
                > maximumProjectPreservedComponentTextBytes)
        {
            addError(result, parameterPath + ".id", "Expected bounded non-empty parameter id");
            parameterValid = false;
        }
        else if (!identifiers.insert(parameter.parameterId).second)
        {
            addError(result, parameterPath + ".id", "Duplicate frozen parameter id");
            parameterValid = false;
        }

        const auto* choices = objectMember(source, "choices");
        if (choices == nullptr || !choices->isArray())
        {
            addError(result, parameterPath + ".choices", "Expected array");
            parameterValid = false;
        }
        else if (choices->array().size()
                 > maximumProjectPreservedComponentParameters)
        {
            addError(result, parameterPath + ".choices", "Choice count exceeds the supported limit");
            parameterValid = false;
        }
        else
        {
            std::unordered_set<std::string> choiceIds;
            for (std::size_t choiceIndex = 0;
                 choiceIndex < choices->array().size();
                 ++choiceIndex)
            {
                const auto& choice = choices->array()[choiceIndex];
                const auto choicePath = parameterPath + ".choices["
                    + std::to_string(choiceIndex) + "]";
                if (!choice.isString() || choice.string().empty()
                    || choice.string().size()
                        > maximumProjectPreservedComponentTextBytes)
                {
                    addError(result, choicePath, "Expected bounded non-empty choice id");
                    parameterValid = false;
                    continue;
                }
                if (!choiceIds.insert(choice.string()).second)
                {
                    addError(result, choicePath, "Duplicate frozen choice id");
                    parameterValid = false;
                    continue;
                }
                parameter.choices.push_back(choice.string());
            }
        }
        if (parameterValid)
        {
            parameter.kind = *parsedKind;
            parameter.domain = *parsedDomain;
            destination.push_back(std::move(parameter));
        }
        else
        {
            valid = false;
        }
    }
    return valid;
}

std::optional<ProjectPreservedComponent> parsePreservedComponent(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& componentPath,
    std::size_t& preservedComponentCount,
    std::size_t& preservedComponentStateBytes)
{
    if (preservedComponentCount
        >= maximumProjectPreservedComponents)
    {
        addError(
            result,
            componentPath,
            "Preserved component count exceeds the supported limit");
        return std::nullopt;
    }
    ++preservedComponentCount;
    if (!source.isObject())
    {
        addError(result, componentPath, "Expected object");
        return std::nullopt;
    }
    const auto errorCountBefore = result.errors.size();
    if (source.object().size() != 11)
    {
        addError(
            result,
            componentPath,
            "Expected schemaVersion, typeVersion, stateSchemaVersion, stateEncoding, state, displayName, bundleId, inputs, outputs, parameters, and recoveryHint only");
    }

    ProjectPreservedComponent component;
    std::uint64_t schemaVersion = 0;
    std::uint64_t stateSchemaVersion = 0;
    static_cast<void>(requiredUnsignedJsonIntegerMember(
        result,
        source,
        "schemaVersion",
        componentPath + ".schemaVersion",
        std::numeric_limits<std::uint32_t>::max(),
        schemaVersion));
    static_cast<void>(requiredUnsignedJsonIntegerMember(
        result,
        source,
        "stateSchemaVersion",
        componentPath + ".stateSchemaVersion",
        std::numeric_limits<std::uint32_t>::max(),
        stateSchemaVersion));
    component.schemaVersion = static_cast<std::uint32_t>(schemaVersion);
    component.stateSchemaVersion =
        static_cast<std::uint32_t>(stateSchemaVersion);
    static_cast<void>(requiredStringMember(
        result,
        source,
        "typeVersion",
        componentPath + ".typeVersion",
        component.typeVersion));
    std::string stateEncoding;
    static_cast<void>(requiredStringMember(
        result,
        source,
        "stateEncoding",
        componentPath + ".stateEncoding",
        stateEncoding));
    if (stateEncoding != "base64")
        addError(result, componentPath + ".stateEncoding", "Expected base64");
    std::string encodedState;
    if (requiredStringMember(
            result,
            source,
            "state",
            componentPath + ".state",
            encodedState))
    {
        const auto remainingStateBytes =
            maximumProjectPreservedComponentTotalStateBytes
            - preservedComponentStateBytes;
        auto decoded = decodeProjectComponentState(
            encodedState,
            remainingStateBytes);
        if (!decoded.ok())
        {
            addError(
                result,
                componentPath + ".state",
                remainingStateBytes
                            < maximumProjectPreservedComponentStateBytes
                        && decoded.errorCode
                            == ProjectComponentStateDecodeResult::
                                ErrorCode::SizeLimitExceeded
                    ? "Aggregate preserved component state exceeds the supported limit"
                    : decoded.error);
        }
        else
        {
            preservedComponentStateBytes += decoded.bytes.size();
            component.state = std::move(decoded.bytes);
        }
    }
    static_cast<void>(requiredStringMember(
        result,
        source,
        "displayName",
        componentPath + ".displayName",
        component.displayName));
    static_cast<void>(requiredStringMember(
        result,
        source,
        "bundleId",
        componentPath + ".bundleId",
        component.bundleId));
    static_cast<void>(requiredStringMember(
        result,
        source,
        "recoveryHint",
        componentPath + ".recoveryHint",
        component.recoveryHint));
    static_cast<void>(parsePreservedPorts(
        result,
        source,
        "inputs",
        componentPath,
        maximumProjectPreservedComponentInputs,
        component.inputs));
    static_cast<void>(parsePreservedPorts(
        result,
        source,
        "outputs",
        componentPath,
        maximumProjectPreservedComponentOutputs,
        component.outputs));
    static_cast<void>(parsePreservedParameters(
        result,
        source,
        componentPath,
        component.parameters));
    if (result.errors.size() == errorCountBefore)
    {
        if (const auto error =
                validateProjectPreservedComponent(component);
            error.has_value())
        {
            addError(
                result,
                componentPath + error->path,
                error->message);
        }
    }
    if (result.errors.size() != errorCountBefore)
        return std::nullopt;
    return component;
}

void parseNodes(
    ProjectLoadResult& result,
    const json::Value& graph,
    ProjectGraph& destination,
    ProjectStructureLayout& presentation,
    const std::string& graphPath,
    int sourceVersion,
    std::size_t& preservedComponentCount,
    std::size_t& preservedComponentStateBytes)
{
    const auto* nodes = objectMember(graph, "nodes");
    if (nodes == nullptr)
    {
        addError(result, graphPath + ".nodes", "Missing required array");
        return;
    }
    if (!nodes->isArray())
    {
        addError(result, graphPath + ".nodes", "Expected array");
        return;
    }

    const auto& nodeArray = nodes->array();
    for (std::size_t index = 0; index < nodeArray.size(); ++index)
    {
        const auto& nodeValue = nodeArray[index];
        const std::string nodePath =
            graphPath + ".nodes[" + std::to_string(index) + "]";
        if (!nodeValue.isObject())
        {
            addError(result, nodePath, "Expected object");
            continue;
        }

        ProjectNode node;
        const bool hasId = requiredStringMember(result, nodeValue, "id", nodePath + ".id", node.instanceId);
        const auto* typeId = objectMember(nodeValue, "typeId");
        const auto* soundDefinitionId =
            objectMember(nodeValue, "soundDefinitionId");
        bool hasTypeIdentity = false;
        if ((typeId == nullptr) == (soundDefinitionId == nullptr))
        {
            addError(
                result,
                nodePath,
                "Expected exactly one of typeId or soundDefinitionId");
        }
        else if (typeId != nullptr)
        {
            if (!typeId->isString())
                addError(result, nodePath + ".typeId", "Expected string");
            else
            {
                node.typeId = typeId->string();
                hasTypeIdentity = true;
            }
        }
        else if (!soundDefinitionId->isString())
        {
            addError(
                result,
                nodePath + ".soundDefinitionId",
                "Expected string");
        }
        else
        {
            node.soundDefinitionId = soundDefinitionId->string();
            if (node.soundDefinitionId->empty())
            {
                addError(
                    result,
                    nodePath + ".soundDefinitionId",
                    "Sound definition id cannot be empty");
            }
            else
            {
                hasTypeIdentity = true;
            }
        }
        CanvasPosition position;
        const bool hasPosition = parseCanvasPosition(
            result,
            nodeValue,
            nodePath,
            position);
        node.parameters = parseParameters(result, nodeValue, nodePath);
        const auto* preservedComponent =
            objectMember(nodeValue, "preservedComponent");
        if (preservedComponent != nullptr)
        {
            if (sourceVersion < versionThirteen)
            {
                addError(
                    result,
                    nodePath + ".preservedComponent",
                    "Preserved components require format 13");
            }
            else if (node.soundDefinitionId.has_value())
            {
                addError(
                    result,
                    nodePath + ".preservedComponent",
                    "Reusable Sound instances cannot carry a preserved component capsule");
            }
            else
            {
                node.preservedComponent = parsePreservedComponent(
                    result,
                    *preservedComponent,
                    nodePath + ".preservedComponent",
                    preservedComponentCount,
                    preservedComponentStateBytes);
            }
        }
        if (hasId && hasTypeIdentity && hasPosition)
        {
            presentation.nodes.push_back({node.instanceId, position});
            destination.nodes.push_back(std::move(node));
        }
    }
}

void parseConnections(
    ProjectLoadResult& result,
    const json::Value& graph,
    ProjectGraph& destination,
    const std::string& graphPath)
{
    const auto* connections = objectMember(graph, "connections");
    if (connections == nullptr)
    {
        addError(result, graphPath + ".connections", "Missing required array");
        return;
    }
    if (!connections->isArray())
    {
        addError(result, graphPath + ".connections", "Expected array");
        return;
    }

    const auto& connectionArray = connections->array();
    for (std::size_t index = 0; index < connectionArray.size(); ++index)
    {
        const auto& connectionValue = connectionArray[index];
        const std::string connectionPath =
            graphPath + ".connections[" + std::to_string(index) + "]";
        if (!connectionValue.isObject())
        {
            addError(result, connectionPath, "Expected object");
            continue;
        }

        talas::core::Connection connection;
        const bool hasFromNode = requiredStringMember(result, connectionValue, "fromNode", connectionPath + ".fromNode", connection.fromNode);
        const bool hasFromPort = requiredStringMember(result, connectionValue, "fromPort", connectionPath + ".fromPort", connection.fromPort);
        const bool hasToNode = requiredStringMember(result, connectionValue, "toNode", connectionPath + ".toNode", connection.toNode);
        const bool hasToPort = requiredStringMember(result, connectionValue, "toPort", connectionPath + ".toPort", connection.toPort);
        if (hasFromNode && hasFromPort && hasToNode && hasToPort)
            destination.connections.push_back(std::move(connection));
    }
}

void parseGraphValue(
    ProjectLoadResult& result,
    const json::Value& graph,
    ProjectGraph& destination,
    ProjectStructureLayout& presentation,
    const std::string& graphPath,
    int sourceVersion,
    std::size_t& preservedComponentCount,
    std::size_t& preservedComponentStateBytes)
{
    if (!graph.isObject())
    {
        addError(result, graphPath, "Expected object");
        return;
    }
    parseNodes(
        result,
        graph,
        destination,
        presentation,
        graphPath,
        sourceVersion,
        preservedComponentCount,
        preservedComponentStateBytes);
    parseConnections(result, graph, destination, graphPath);
}

std::optional<talas::expr::ValueDomain> parseValueDomain(std::string_view value)
{
    if (value == "scalar") return talas::expr::ValueDomain::Scalar;
    if (value == "hertz") return talas::expr::ValueDomain::Hertz;
    if (value == "seconds") return talas::expr::ValueDomain::Seconds;
    if (value == "noteNumber") return talas::expr::ValueDomain::NoteNumber;
    if (value == "samples") return talas::expr::ValueDomain::Samples;
    return std::nullopt;
}

bool requiredBooleanMember(
    ProjectLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    bool& output);
bool requiredFiniteNumberMember(ProjectLoadResult& result, const json::Value& object, std::string_view key, const std::string& path, double& output);

std::optional<std::optional<std::uint32_t>> parseVirtualControlColourJson(std::string_view text)
{
    if (text.empty())
        return std::optional<std::uint32_t>{};
    if (text.size() != 7 || text.front() != '#')
        return std::nullopt;
    std::uint32_t colour = 0;
    for (const auto character : text.substr(1))
    {
        const auto digit = character >= '0' && character <= '9' ? character - '0'
            : character >= 'a' && character <= 'f'                ? character - 'a' + 10
                                                                  : -1;
        if (digit < 0)
            return std::nullopt;
        colour = colour << 4 | static_cast<std::uint32_t>(digit);
    }
    return std::optional<std::uint32_t>{colour};
}

// Format 97: a widget's options, all eight members.
bool parseVirtualControlWidget(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    VirtualControl& control,
    int sourceVersion)
{
    const auto* bounds = source.isObject() ? objectMember(source, "bounds") : nullptr;
    if (!source.isObject() || source.object().size() != (bounds == nullptr ? 8U : 9U))
    {
        addError(result, path, "Expected style, showNumber, label, onColour, offColour, grid, taper, tab and bounds only");
        return false;
    }
    VirtualControlWidget widget;
    if (bounds != nullptr)
    {
        // Format 125: four whole numbers, left, top, width and height.
        std::array<std::int32_t, 4> values{};
        const auto read = bounds->isArray() && bounds->array().size() == 4
            && std::ranges::all_of(std::views::iota(std::size_t{0}, std::size_t{4}), [&](std::size_t index) {
                   const auto& item = bounds->array()[index];
                   return item.isNumber() && parseSigned32BitIntegerToken(item.numberToken(), values[index]);
               });
        if (sourceVersion < versionOneHundredTwentyFive)
        {
            addError(result, path + ".bounds", "A widget's place requires format 125");
            return false;
        }
        if (!read)
        {
            addError(result, path + ".bounds", "Expected left, top, width and height in whole pixels");
            return false;
        }
        widget.bounds = VirtualControlBounds{values[0], values[1], values[2], values[3]};
    }
    std::string style;
    std::string onColour;
    std::string offColour;
    std::string taper;
    auto valid = requiredStringMember(result, source, "style", path + ".style", style);
    valid = requiredBooleanMember(result, source, "showNumber", path + ".showNumber", widget.showNumber) && valid;
    valid = requiredStringMember(result, source, "label", path + ".label", widget.label) && valid;
    valid = requiredStringMember(result, source, "onColour", path + ".onColour", onColour) && valid;
    valid = requiredStringMember(result, source, "offColour", path + ".offColour", offColour) && valid;
    valid = requiredFiniteNumberMember(result, source, "grid", path + ".grid", widget.grid) && valid;
    valid = requiredStringMember(result, source, "taper", path + ".taper", taper) && valid;
    valid = requiredStringMember(result, source, "tab", path + ".tab", widget.tab) && valid;
    if (!valid)
        return false;
    if (style == "default") widget.style = VirtualControlWidgetStyle::Default;
    else if (style == "fader") widget.style = VirtualControlWidgetStyle::Fader;
    else if (style == "horizontalFader") widget.style = VirtualControlWidgetStyle::HorizontalFader;
    else if (style == "smallFader") widget.style = VirtualControlWidgetStyle::SmallFader;
    else if (style == "potentiometer") widget.style = VirtualControlWidgetStyle::Potentiometer;
    else if (style == "buttonFill" && sourceVersion >= versionOneHundredThirtySeven)
        widget.style = VirtualControlWidgetStyle::ButtonFill;
    else
    {
        addError(result, path + ".style",
            "Expected default, fader, horizontalFader, smallFader, potentiometer or, from format 137, buttonFill");
        return false;
    }
    if (taper != "linear" && taper != "log")
    {
        addError(result, path + ".taper", "Expected linear or log");
        return false;
    }
    widget.taper = taper == "log" ? VirtualControlTaper::Log : VirtualControlTaper::Linear;
    const auto on = parseVirtualControlColourJson(onColour);
    const auto off = parseVirtualControlColourJson(offColour);
    if (!on.has_value() || !off.has_value())
    {
        addError(result, path, "Expected colours as #rrggbb or empty");
        return false;
    }
    widget.onColour = *on;
    widget.offColour = *off;
    if (!isValidVirtualControlWidget(widget, control.minimum, control.maximum))
    {
        addError(result, path, "Expected a label and tab within their limits, a grid from 0 to the range, and a log taper only above 0");
        return false;
    }
    control.widget = std::move(widget);
    return true;
}

std::optional<VirtualControlKind> parseVirtualControlKind(std::string_view value)
{
    if (value == "fader") return VirtualControlKind::Fader;
    if (value == "gate") return VirtualControlKind::Gate;
    if (value == "toggle") return VirtualControlKind::Toggle;
    return std::nullopt;
}

// Format 155: what each layout keeps of its
// own, its name, its presets' bank and arrangement when it has them, and
// its widgets.
void parseVirtualControlLayoutSettings(ProjectLoadResult& result, const json::Value& layouts, int sourceVersion)
{
    const std::string member = "$.virtualControlLayoutSettings";
    if (sourceVersion < versionOneHundredFiftyFive)
    {
        addError(result, member, "virtualControlLayoutSettings requires format 155");
        return;
    }
    if (!layouts.isArray() || layouts.array().size() > maximumVirtualControlLayoutSettings)
    {
        addError(result, member, "Expected at most 256 layouts");
        return;
    }
    for (std::size_t index = 0; index < layouts.array().size(); ++index)
    {
        const auto& item = layouts.array()[index];
        const auto path = member + "[" + std::to_string(index) + "]";
        const auto* bank = item.isObject() ? objectMember(item, "presetBankId") : nullptr;
        const auto* arrangement = item.isObject() ? objectMember(item, "arrangement") : nullptr;
        const auto* widgets = item.isObject() ? objectMember(item, "widgets") : nullptr;
        VirtualControlLayoutSettings settings;
        if (!item.isObject() || item.object().size() != 2U + (bank != nullptr ? 1U : 0U) + (arrangement != nullptr ? 1U : 0U)
            || !requiredStringMember(result, item, "layout", path + ".layout", settings.layout) || widgets == nullptr
            || !widgets->isArray() || (bank != nullptr && (!bank->isString() || bank->string().empty()))
            || (arrangement != nullptr && !arrangement->isString()))
        {
            addError(result, path, "Expected a layout's name, its widgets and, when it has them, its presetBankId and arrangement");
            continue;
        }
        if (bank != nullptr)
            settings.presetBankId = bank->string();
        if (arrangement != nullptr)
        {
            const auto& text = arrangement->string();
            if (text != "automatic" && text != "fixed" && text != "fit")
            {
                addError(result, path + ".arrangement", "Expected automatic, fixed or fit");
                continue;
            }
            settings.arrangement = text == "fit" ? VirtualControlLayout::Fit
                : text == "fixed"                ? VirtualControlLayout::Fixed
                                                 : VirtualControlLayout::Automatic;
        }
        auto valid = true;
        for (std::size_t at = 0; at < widgets->array().size() && valid; ++at)
        {
            const auto& source = widgets->array()[at];
            const auto widgetPath = path + ".widgets[" + std::to_string(at) + "]";
            const auto* options = source.isObject() ? objectMember(source, "widget") : nullptr;
            VirtualControl control;
            std::string kind;
            valid = source.isObject() && source.object().size() == (options != nullptr ? 5U : 4U)
                && requiredStringMember(result, source, "eventName", widgetPath + ".eventName", control.eventName)
                && requiredStringMember(result, source, "kind", widgetPath + ".kind", kind)
                && requiredFiniteNumberMember(result, source, "minimum", widgetPath + ".minimum", control.minimum)
                && requiredFiniteNumberMember(result, source, "maximum", widgetPath + ".maximum", control.maximum)
                && parseVirtualControlKind(kind).has_value() && control.minimum < control.maximum
                && (options == nullptr || parseVirtualControlWidget(result, *options, widgetPath + ".widget", control, sourceVersion));
            if (!valid)
            {
                addError(result, widgetPath, "Expected an Event Value's name, kind (fader, gate or toggle), minimum below its maximum and widget");
                break;
            }
            settings.widgets.push_back({control.eventName, *parseVirtualControlKind(kind), control.minimum, control.maximum, control.widget});
        }
        if (!valid)
            continue;
        if (!isValidVirtualControlLayoutSettings(settings)
            || std::ranges::any_of(result.document.virtualControlLayoutSettings,
                [&](const VirtualControlLayoutSettings& earlier) { return earlier.layout == settings.layout; }))
        {
            addError(result, path, "Expected a layout named once, a preset bank's identity and each Event Value's widget once");
            continue;
        }
        result.document.virtualControlLayoutSettings.push_back(std::move(settings));
    }
}

bool isAsciiEventNameStart(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool isAsciiEventNameContinue(char value)
{
    return isAsciiEventNameStart(value) || (value >= '0' && value <= '9');
}

bool isValidEventName(std::string_view eventName)
{
    if (eventName.empty() || !isAsciiEventNameStart(eventName.front()))
        return false;
    for (const auto character : eventName.substr(1))
    {
        if (!isAsciiEventNameContinue(character))
            return false;
    }
    return true;
}

// Format 139: a Sound class's icon, its rows of dots as hexadecimal digits.
template <typename Row>
[[nodiscard]] bool parseSoundIconRows(ProjectLoadResult& result, const json::Value& icon, std::string_view key,
    const std::string& path, std::size_t height, std::vector<Row>& rows)
{
    const auto* member = objectMember(icon, key);
    if (member == nullptr)
        return true;
    if (!member->isArray() || member->array().size() != height)
    {
        addError(result, path + "." + std::string{key}, "Expected " + std::to_string(height) + " rows of the icon's dots");
        return false;
    }
    for (const auto& row : member->array())
    {
        if (!row.isString() || row.string().size() != sizeof(Row) * 2
            || !std::ranges::all_of(row.string(), [](char digit) {
                   return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
               }))
        {
            addError(result, path + "." + std::string{key}, "Expected each row as " + std::to_string(sizeof(Row) * 2)
                + " lowercase hexadecimal digits");
            return false;
        }
        Row value = 0;
        for (const auto digit : row.string())
            value = static_cast<Row>((value << 4) | static_cast<Row>(digit <= '9' ? digit - '0' : digit - 'a' + 10));
        rows.push_back(value);
    }
    return true;
}

[[nodiscard]] bool isSoundIconEdgeName(std::string_view name)
{
    return name == "none" || name == "audio" || name == "subaudio" || name == "spectrum" || name == "transparent"
        || name == "constructor" || name == "variable" || name == "environment";
}

[[nodiscard]] std::optional<ProjectSoundIcon> parseSoundIcon(
    ProjectLoadResult& result, const json::Value& icon, const std::string& path, int sourceVersion)
{
    if (!icon.isObject())
    {
        addError(result, path, "Expected the icon as an object");
        return std::nullopt;
    }
    ProjectSoundIcon parsed;
    if (const auto* sameAs = objectMember(icon, "sameAs"); sameAs != nullptr)
    {
        if (!sameAs->isString() || sameAs->string().empty())
        {
            addError(result, path + ".sameAs", "Expected the Sound whose icon this one is");
            return std::nullopt;
        }
        parsed.sameAs = sameAs->string();
    }
    if (!parseSoundIconRows(result, icon, "large", path, soundIconLargeHeight, parsed.large)
        || !parseSoundIconRows(result, icon, "small", path, soundIconSmallHeight, parsed.small))
        return std::nullopt;
    if (const auto* png = objectMember(icon, "png"); png != nullptr)
    {
        if (!png->isBoolean() || !png->boolean())
        {
            addError(result, path + ".png", "Expected true");
            return std::nullopt;
        }
        parsed.png = true;
    }
    for (const auto& [key, destination] : {std::pair{"inputType", &parsed.inputType}, std::pair{"outputType", &parsed.outputType}})
    {
        if (const auto* type = objectMember(icon, key); type != nullptr)
        {
            if (sourceVersion < versionOneHundredFortyOne || !type->isString() || !isSoundIconEdgeName(type->string()))
            {
                addError(result, path + "." + key,
                    "Expected none, audio, subaudio, spectrum, transparent, constructor, variable or environment, from format 141");
                return std::nullopt;
            }
            *destination = type->string();
        }
    }
    if (const auto* sideEffects = objectMember(icon, "sideEffects"); sideEffects != nullptr)
    {
        if (sourceVersion < versionOneHundredFortyOne || !sideEffects->isBoolean() || !sideEffects->boolean())
        {
            addError(result, path + ".sideEffects", "Expected true, from format 141");
            return std::nullopt;
        }
        parsed.sideEffects = true;
    }
    for (const auto& [key, destination] :
        {std::pair{"inputChannels", &parsed.inputChannels}, std::pair{"outputChannels", &parsed.outputChannels}})
    {
        if (const auto* channels = objectMember(icon, key); channels != nullptr)
        {
            if (sourceVersion < versionOneHundredFortyThree || !channels->isNumber()
                || (channels->number() != 1.0 && channels->number() != 2.0))
            {
                addError(result, path + "." + key, "Expected 1 or 2 channels, from format 143");
                return std::nullopt;
            }
            *destination = static_cast<std::uint8_t>(channels->number());
        }
    }
    if (parsed == ProjectSoundIcon{})
    {
        addError(result, path, "Expected the icon's Sound, dots, PNG file, types, side effects or channels");
        return std::nullopt;
    }
    return parsed;
}

bool parseSoundPorts(
    ProjectLoadResult& result,
    const json::Value& definition,
    std::string_view key,
    const std::string& definitionPath,
    int sourceVersion,
    std::vector<ProjectSoundPort>& destination)
{
    const auto path = definitionPath + "." + std::string(key);
    const auto* ports = objectMember(definition, key);
    if (ports == nullptr || !ports->isArray())
    {
        addError(result, path, "Expected required array");
        return false;
    }

    bool valid = true;
    std::unordered_set<std::string> portIds;
    for (std::size_t index = 0; index < ports->array().size(); ++index)
    {
        const auto& source = ports->array()[index];
        const auto portPath = path + "[" + std::to_string(index) + "]";
        if (!source.isObject())
        {
            addError(result, portPath, "Expected object");
            valid = false;
            continue;
        }

        ProjectSoundPort port;
        std::string type;
        const auto hasId = requiredStringMember(
            result, source, "id", portPath + ".id", port.portId);
        const auto hasType = requiredStringMember(
            result, source, "type", portPath + ".type", type);
        const auto hasNodeId = requiredStringMember(
            result, source, "nodeId", portPath + ".nodeId", port.nodeId);
        const auto hasNodePortId = requiredStringMember(
            result,
            source,
            "nodePortId",
            portPath + ".nodePortId",
            port.nodePortId);
        auto parsedType = hasType ? parsePortType(type) : std::nullopt;
        if (parsedType == talas::core::PortType::ComplexSpectrum
            && sourceVersion < versionTen)
        {
            parsedType.reset();
        }
        bool portIsValid =
            hasId && hasType && hasNodeId && hasNodePortId
            && parsedType.has_value();
        if (hasId && !isValidEventName(port.portId))
        {
            addError(result, portPath + ".id", "Expected Sound port identifier");
            portIsValid = false;
        }
        if (hasId && !portIds.insert(port.portId).second)
        {
            addError(result, portPath + ".id", "Duplicate Sound port id");
            portIsValid = false;
        }
        if (!parsedType.has_value())
        {
            if (hasType)
                addError(result, portPath + ".type", "Expected known port type");
            portIsValid = false;
        }
        if (hasNodeId && port.nodeId.empty())
        {
            addError(result, portPath + ".nodeId", "Internal node id cannot be empty");
            portIsValid = false;
        }
        if (hasNodePortId && port.nodePortId.empty())
        {
            addError(
                result,
                portPath + ".nodePortId",
                "Internal node port id cannot be empty");
            portIsValid = false;
        }
        if (const auto* replaces = objectMember(source, "replaces"); replaces != nullptr)
        {
            if (sourceVersion < versionOneHundredThirtyEight || key != "inputs")
            {
                addError(result, portPath + ".replaces", "A Sound's replaceable input requires format 138");
                portIsValid = false;
            }
            else if (!replaces->isBoolean() || !replaces->boolean())
            {
                addError(result, portPath + ".replaces", "Expected true");
                portIsValid = false;
            }
            else
            {
                port.replacesSound = true;
            }
        }
        if (portIsValid)
        {
            port.type = *parsedType;
            destination.push_back(std::move(port));
        }
        else
        {
            valid = false;
        }
    }
    return valid;
}

[[nodiscard]] std::optional<talas::core::ParameterKind>
parseParameterKind(std::string_view value)
{
    if (value == "expression")
        return talas::core::ParameterKind::Expression;
    if (value == "choice")
        return talas::core::ParameterKind::Choice;
    if (value == "boolean")
        return talas::core::ParameterKind::Boolean;
    if (value == "resource")
        return talas::core::ParameterKind::Resource;
    if (value == "text")
        return talas::core::ParameterKind::Text;
    return std::nullopt;
}

[[nodiscard]] bool soundParameterDefaultMatchesKind(
    const ProjectParameterExpression& value,
    talas::core::ParameterKind kind)
{
    switch (kind)
    {
    case talas::core::ParameterKind::Expression:
        return value.expression.has_value();
    case talas::core::ParameterKind::Choice:
        return value.choice.has_value();
    case talas::core::ParameterKind::Boolean:
        return value.boolean.has_value();
    case talas::core::ParameterKind::Resource:
        return value.resource.has_value();
    case talas::core::ParameterKind::Text:
        return value.text.has_value();
    }
    return false;
}

[[nodiscard]] std::optional<ProjectSoundParameterTarget>
parseSoundParameterTarget(std::string_view value)
{
    if (value == "nodeParameter")
        return ProjectSoundParameterTarget::NodeParameter;
    if (value == "eventValue")
        return ProjectSoundParameterTarget::EventValue;
    if (value == "variable")
        return ProjectSoundParameterTarget::Variable;
    return std::nullopt;
}

bool parseSoundParameterDefault(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    std::string_view parameterId,
    ProjectParameterExpression& destination)
{
    const auto* value = objectMember(source, "default");
    if (value == nullptr || !value->isObject())
    {
        addError(result, path + ".default", "Expected required object");
        return false;
    }

    const auto* expression = objectMember(*value, "expression");
    const auto* choice = objectMember(*value, "choice");
    const auto* boolean = objectMember(*value, "boolean");
    const auto* resource = objectMember(*value, "resource");
    const auto* text = objectMember(*value, "text");
    const auto valueCount = static_cast<int>(expression != nullptr)
        + static_cast<int>(choice != nullptr)
        + static_cast<int>(boolean != nullptr)
        + static_cast<int>(resource != nullptr)
        + static_cast<int>(text != nullptr);
    if (valueCount != 1)
    {
        addError(
            result,
            path + ".default",
            "Expected exactly one of expression, choice, boolean, resource, or text");
        return false;
    }

    destination.parameterId = std::string(parameterId);
    if (expression != nullptr)
    {
        if (!expression->isString())
        {
            addError(result, path + ".default.expression", "Expected string");
            return false;
        }
        destination.expression = expression->string();
    }
    else if (choice != nullptr)
    {
        if (!choice->isString())
        {
            addError(result, path + ".default.choice", "Expected string");
            return false;
        }
        destination.choice = choice->string();
    }
    else if (boolean != nullptr)
    {
        if (!boolean->isBoolean())
        {
            addError(result, path + ".default.boolean", "Expected boolean");
            return false;
        }
        destination.boolean = boolean->boolean();
    }
    else if (resource != nullptr)
    {
        if (!resource->isString())
        {
            addError(result, path + ".default.resource", "Expected string");
            return false;
        }
        destination.resource = resource->string();
    }
    else
    {
        if (!text->isString())
        {
            addError(result, path + ".default.text", "Expected string");
            return false;
        }
        destination.text = text->string();
    }
    return true;
}

bool parseSoundParameterAliases(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    std::vector<std::string>& destination)
{
    const auto* aliases = objectMember(source, "aliases");
    if (aliases == nullptr || !aliases->isArray())
    {
        addError(result, path + ".aliases", "Expected required array");
        return false;
    }
    bool valid = true;
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < aliases->array().size(); ++index)
    {
        const auto& alias = aliases->array()[index];
        const auto aliasPath = path + ".aliases["
            + std::to_string(index) + "]";
        if (!alias.isString() || !isValidEventName(alias.string()))
        {
            addError(result, aliasPath, "Expected Sound parameter identifier");
            valid = false;
            continue;
        }
        if (!seen.insert(alias.string()).second)
        {
            addError(result, aliasPath, "Duplicate Sound parameter alias");
            valid = false;
            continue;
        }
        destination.push_back(alias.string());
    }
    return valid;
}

bool parseSoundParameterChoices(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    std::vector<std::string>& destination)
{
    const auto* choices = objectMember(source, "choices");
    if (choices == nullptr || !choices->isArray())
    {
        addError(result, path + ".choices", "Expected required array");
        return false;
    }
    bool valid = true;
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < choices->array().size(); ++index)
    {
        const auto& choice = choices->array()[index];
        const auto choicePath = path + ".choices["
            + std::to_string(index) + "]";
        if (!choice.isString() || choice.string().empty())
        {
            addError(result, choicePath, "Expected non-empty choice string");
            valid = false;
            continue;
        }
        if (!seen.insert(choice.string()).second)
        {
            addError(result, choicePath, "Duplicate Sound parameter choice");
            valid = false;
            continue;
        }
        destination.push_back(choice.string());
    }
    return valid;
}

bool parseSoundParameterBindings(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    std::vector<ProjectSoundParameterBinding>& destination)
{
    const auto* bindings = objectMember(source, "bindings");
    if (bindings == nullptr || !bindings->isArray())
    {
        addError(result, path + ".bindings", "Expected required array");
        return false;
    }
    bool valid = true;
    for (std::size_t index = 0; index < bindings->array().size(); ++index)
    {
        const auto& sourceBinding = bindings->array()[index];
        const auto bindingPath = path + ".bindings["
            + std::to_string(index) + "]";
        if (!sourceBinding.isObject())
        {
            addError(result, bindingPath, "Expected object");
            valid = false;
            continue;
        }
        std::string target;
        const auto hasTarget = requiredStringMember(
            result,
            sourceBinding,
            "target",
            bindingPath + ".target",
            target);
        const auto parsedTarget = hasTarget
            ? parseSoundParameterTarget(target)
            : std::nullopt;
        if (!parsedTarget.has_value())
        {
            if (hasTarget)
            {
                addError(
                    result,
                    bindingPath + ".target",
                    "Expected nodeParameter, eventValue or variable");
            }
            valid = false;
            continue;
        }

        ProjectSoundParameterBinding binding;
        binding.target = *parsedTarget;
        bool bindingValid = true;
        if (binding.target == ProjectSoundParameterTarget::NodeParameter)
        {
            bindingValid = requiredStringMember(
                               result,
                               sourceBinding,
                               "nodeId",
                               bindingPath + ".nodeId",
                               binding.nodeId)
                && bindingValid;
            bindingValid = requiredStringMember(
                               result,
                               sourceBinding,
                               "parameterId",
                               bindingPath + ".parameterId",
                               binding.parameterId)
                && bindingValid;
            if (bindingValid
                && (binding.nodeId.empty()
                    || !isValidEventName(binding.parameterId)))
            {
                addError(
                    result,
                    bindingPath,
                    "Expected valid internal node and parameter identities");
                bindingValid = false;
            }
        }
        else if (binding.target == ProjectSoundParameterTarget::Variable)
        {
            bindingValid = requiredStringMember(
                               result,
                               sourceBinding,
                               "variableName",
                               bindingPath + ".variableName",
                               binding.variableName)
                && bindingValid;
            if (bindingValid && !isValidEventName(binding.variableName))
            {
                addError(
                    result,
                    bindingPath + ".variableName",
                    "Expected variable identifier");
                bindingValid = false;
            }
        }
        else
        {
            std::string domain;
            bindingValid = requiredStringMember(
                               result,
                               sourceBinding,
                               "eventName",
                               bindingPath + ".eventName",
                               binding.eventName)
                && bindingValid;
            const auto hasDomain = requiredStringMember(
                result,
                sourceBinding,
                "domain",
                bindingPath + ".domain",
                domain);
            const auto parsedDomain = hasDomain
                ? parseValueDomain(domain)
                : std::nullopt;
            if (!parsedDomain.has_value())
            {
                if (hasDomain)
                {
                    addError(
                        result,
                        bindingPath + ".domain",
                        "Expected known value domain");
                }
                bindingValid = false;
            }
            else
            {
                binding.eventDomain = *parsedDomain;
            }
            if (!isValidEventName(binding.eventName))
            {
                addError(
                    result,
                    bindingPath + ".eventName",
                    "Expected Event Value identifier");
                bindingValid = false;
            }
        }
        if (bindingValid)
            destination.push_back(std::move(binding));
        else
            valid = false;
    }
    return valid;
}

bool parseSoundExposedParameters(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    int sourceVersion,
    std::vector<ProjectSoundExposedParameter>& destination)
{
    const auto* parameters = objectMember(source, "parameters");
    if (parameters == nullptr || !parameters->isArray())
    {
        addError(result, path + ".parameters", "Expected required array");
        return false;
    }
    bool valid = true;
    std::unordered_set<std::string> identities;
    for (std::size_t index = 0; index < parameters->array().size(); ++index)
    {
        const auto& sourceParameter = parameters->array()[index];
        const auto parameterPath = path + ".parameters["
            + std::to_string(index) + "]";
        if (!sourceParameter.isObject())
        {
            addError(result, parameterPath, "Expected object");
            valid = false;
            continue;
        }

        ProjectSoundExposedParameter parameter;
        std::string kind;
        std::string domain;
        auto parameterValid = requiredStringMember(
            result,
            sourceParameter,
            "id",
            parameterPath + ".id",
            parameter.parameterId);
        parameterValid = requiredStringMember(
                             result,
                             sourceParameter,
                             "name",
                             parameterPath + ".name",
                             parameter.displayName)
            && parameterValid;
        const auto hasKind = requiredStringMember(
            result,
            sourceParameter,
            "kind",
            parameterPath + ".kind",
            kind);
        const auto parsedKind = hasKind
            ? parseParameterKind(kind)
            : std::nullopt;
        if (!parsedKind.has_value())
        {
            if (hasKind)
                addError(result, parameterPath + ".kind", "Expected known parameter kind");
            parameterValid = false;
        }
        else
        {
            parameter.kind = *parsedKind;
        }
        const auto hasDomain = requiredStringMember(
            result,
            sourceParameter,
            "domain",
            parameterPath + ".domain",
            domain);
        const auto parsedDomain = hasDomain
            ? parseValueDomain(domain)
            : std::nullopt;
        if (!parsedDomain.has_value())
        {
            if (hasDomain)
                addError(result, parameterPath + ".domain", "Expected known value domain");
            parameterValid = false;
        }
        else
        {
            parameter.domain = *parsedDomain;
        }
        if (!isValidEventName(parameter.parameterId))
        {
            addError(result, parameterPath + ".id", "Expected Sound parameter identifier");
            parameterValid = false;
        }
        else if (!identities.insert(parameter.parameterId).second)
        {
            addError(result, parameterPath + ".id", "Duplicate Sound parameter id");
            parameterValid = false;
        }
        if (parameter.displayName.empty())
        {
            addError(result, parameterPath + ".name", "Sound parameter name cannot be empty");
            parameterValid = false;
        }
        parameterValid = parseSoundParameterDefault(
                             result,
                             sourceParameter,
                             parameterPath,
                             parameter.parameterId,
                             parameter.defaultValue)
            && parameterValid;
        parameterValid = parseSoundParameterChoices(
                             result,
                             sourceParameter,
                             parameterPath,
                             parameter.choices)
            && parameterValid;
        parameterValid = parseSoundParameterAliases(
                             result,
                             sourceParameter,
                             parameterPath,
                             parameter.aliases)
            && parameterValid;
        for (const auto& alias : parameter.aliases)
        {
            if (!identities.insert(alias).second)
            {
                addError(
                    result,
                    parameterPath + ".aliases",
                    "Ambiguous Sound parameter alias");
                parameterValid = false;
            }
        }
        parameterValid = parseSoundParameterBindings(
                             result,
                             sourceParameter,
                             parameterPath,
                             parameter.bindings)
            && parameterValid;
        // Format 99: the Class Editor's range, field and comment.
        {
            const auto* minimum = objectMember(sourceParameter, "minimum");
            const auto* maximum = objectMember(sourceParameter, "maximum");
            const auto* field = objectMember(sourceParameter, "field");
            const auto* comment = objectMember(sourceParameter, "comment");
            if ((minimum != nullptr || maximum != nullptr || field != nullptr || comment != nullptr)
                && sourceVersion < versionNinetyNine)
            {
                addError(result, parameterPath, "A Sound parameter's range, field and comment require format 99");
                parameterValid = false;
            }
            else
            {
                double value = 0.0;
                if (minimum != nullptr)
                {
                    parameterValid = requiredFiniteNumberMember(result, sourceParameter, "minimum",
                                         parameterPath + ".minimum", value)
                        && parameterValid;
                    parameter.minimum = value;
                }
                if (maximum != nullptr)
                {
                    parameterValid = requiredFiniteNumberMember(result, sourceParameter, "maximum",
                                         parameterPath + ".maximum", value)
                        && parameterValid;
                    parameter.maximum = value;
                }
                if (field != nullptr)
                {
                    ProjectSoundFieldRectangle rectangle;
                    const auto fieldPath = parameterPath + ".field";
                    if (!field->isObject() || field->object().size() != 4)
                    {
                        addError(result, fieldPath, "Expected left, top, right and bottom only");
                        parameterValid = false;
                    }
                    else
                    {
                        auto fieldValid = requiredFiniteNumberMember(result, *field, "left", fieldPath + ".left", rectangle.left);
                        fieldValid = requiredFiniteNumberMember(result, *field, "top", fieldPath + ".top", rectangle.top) && fieldValid;
                        fieldValid = requiredFiniteNumberMember(result, *field, "right", fieldPath + ".right", rectangle.right) && fieldValid;
                        fieldValid = requiredFiniteNumberMember(result, *field, "bottom", fieldPath + ".bottom", rectangle.bottom) && fieldValid;
                        parameterValid = fieldValid && parameterValid;
                        parameter.field = rectangle;
                    }
                }
                if (comment != nullptr)
                {
                    if (!comment->isString())
                    {
                        addError(result, parameterPath + ".comment", "Expected string");
                        parameterValid = false;
                    }
                    else
                    {
                        parameter.comment = comment->string();
                    }
                }
                // Format 145: the kind of field.
                if (const auto* fieldKind = objectMember(sourceParameter, "fieldKind"); fieldKind != nullptr)
                {
                    const auto parsedFieldKind = fieldKind->isString()
                        ? parseProjectSoundFieldKind(fieldKind->string())
                        : std::nullopt;
                    // Format 147: the complex, polesAndZeroes and grammar fields.
                    const auto laterKind = parsedFieldKind == ProjectSoundFieldKind::Complex
                        || parsedFieldKind == ProjectSoundFieldKind::PolesAndZeroes
                        || parsedFieldKind == ProjectSoundFieldKind::Grammar;
                    if (sourceVersion < versionOneHundredFortyFive || !parsedFieldKind.has_value()
                        || (laterKind && sourceVersion < versionOneHundredFortySeven))
                    {
                        addError(result, parameterPath + ".fieldKind",
                            "Expected a field kind, from format 145 (complex, polesAndZeroes and grammar from 147)");
                        parameterValid = false;
                    }
                    else
                    {
                        parameter.fieldKind = *parsedFieldKind;
                    }
                }
                if (!isValidSoundParameterClassFields(parameter))
                {
                    addError(result, parameterPath,
                        "A Sound parameter's range lies minimum below maximum around its default, its field within 0 to 1, its comment within 4096 bytes, its field kind one of its kind's");
                    parameterValid = false;
                }
            }
        }
        if (parsedKind.has_value()
            && !soundParameterDefaultMatchesKind(
                parameter.defaultValue,
                *parsedKind))
        {
            addError(
                result,
                parameterPath + ".default",
                "Sound parameter default must match its declared kind");
            parameterValid = false;
        }
        if (parsedKind.has_value()
            && *parsedKind == talas::core::ParameterKind::Choice)
        {
            if (parameter.choices.empty())
            {
                addError(
                    result,
                    parameterPath + ".choices",
                    "Choice parameter requires at least one choice");
                parameterValid = false;
            }
            else if (parameter.defaultValue.choice.has_value()
                     && std::ranges::find(
                            parameter.choices,
                            *parameter.defaultValue.choice)
                         == parameter.choices.end())
            {
                addError(
                    result,
                    parameterPath + ".default.choice",
                    "Choice parameter default must be one of its choices");
                parameterValid = false;
            }
        }
        else if (!parameter.choices.empty())
        {
            addError(
                result,
                parameterPath + ".choices",
                "Only Choice parameters may declare choices");
            parameterValid = false;
        }
        if (parsedKind.has_value()
            && *parsedKind != talas::core::ParameterKind::Expression
            && parsedDomain.has_value()
            && *parsedDomain != talas::expr::ValueDomain::Scalar)
        {
            addError(
                result,
                parameterPath + ".domain",
                "Non-expression Sound parameters use the scalar domain");
            parameterValid = false;
        }
        if (parameter.bindings.empty())
        {
            addError(
                result,
                parameterPath + ".bindings",
                "Sound parameter requires at least one explicit binding");
            parameterValid = false;
        }
        if (parameterValid)
            destination.push_back(std::move(parameter));
        else
            valid = false;
    }
    return valid;
}

void parseSoundDefinitions(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion,
    std::size_t& preservedComponentCount,
    std::size_t& preservedComponentStateBytes)
{
    const auto* definitions = objectMember(root, "soundDefinitions");
    if (definitions == nullptr || !definitions->isArray())
    {
        addError(
            result,
            "$.soundDefinitions",
            "Expected required array");
        return;
    }

    std::unordered_set<std::string> definitionIds;
    for (std::size_t index = 0;
         index < definitions->array().size();
         ++index)
    {
        const auto& source = definitions->array()[index];
        const auto path =
            "$.soundDefinitions[" + std::to_string(index) + "]";
        if (!source.isObject())
        {
            addError(result, path, "Expected object");
            continue;
        }

        const auto errorCountBefore = result.errors.size();
        ProjectSoundDefinition definition;
        ProjectStructureLayout presentation;
        const auto hasId = requiredStringMember(
            result,
            source,
            "id",
            path + ".id",
            definition.definitionId);
        const auto hasName = requiredStringMember(
            result,
            source,
            "name",
            path + ".name",
            definition.displayName);
        bool valid = hasId && hasName;
        if (hasId)
            presentation.soundDefinitionId = definition.definitionId;
        if (sourceVersion >= versionNine)
        {
            const auto* interfaceVersion =
                objectMember(source, "interfaceVersion");
            if (interfaceVersion == nullptr
                || !interfaceVersion->isNumber()
                || !std::isfinite(interfaceVersion->number())
                || std::floor(interfaceVersion->number())
                    != interfaceVersion->number()
                || interfaceVersion->number() < 1.0
                || interfaceVersion->number()
                    > static_cast<double>(
                        std::numeric_limits<std::uint32_t>::max()))
            {
                addError(
                    result,
                    path + ".interfaceVersion",
                    "Expected positive 32-bit interface version");
                valid = false;
            }
            else
            {
                definition.interfaceVersion =
                    static_cast<std::uint32_t>(
                        interfaceVersion->number());
            }
        }
        if (hasId && !isValidEventName(definition.definitionId))
        {
            addError(
                result,
                path + ".id",
                "Expected Sound definition identifier");
            valid = false;
        }
        if (hasId
            && !definitionIds.insert(definition.definitionId).second)
        {
            addError(
                result,
                path + ".id",
                "Duplicate Sound definition id");
            valid = false;
        }
        if (hasName && definition.displayName.empty())
        {
            addError(
                result,
                path + ".name",
                "Sound definition name cannot be empty");
            valid = false;
        }

        if (const auto* icon = objectMember(source, "icon"); icon != nullptr)
        {
            if (sourceVersion < versionOneHundredThirtyNine)
            {
                addError(result, path + ".icon", "A Sound class's icon requires format 139");
                valid = false;
            }
            else if (auto parsed = parseSoundIcon(result, *icon, path + ".icon", sourceVersion))
            {
                definition.icon = std::move(*parsed);
            }
            else
            {
                valid = false;
            }
        }
        valid = parseSoundPorts(
                    result,
                    source,
                    "inputs",
                    path,
                    sourceVersion,
                    definition.inputs)
            && valid;
        if (sourceVersion >= versionNine)
        {
            valid = parseSoundExposedParameters(
                        result,
                        source,
                        path,
                        sourceVersion,
                        definition.exposedParameters)
                && valid;
        }
        if (const auto* description = objectMember(source, "description"); description != nullptr)
        {
            if (sourceVersion < versionNinetyNine)
            {
                addError(result, path + ".description", "A Sound's Class Description requires format 99");
                valid = false;
            }
            else if (!description->isString() || description->string().empty()
                || description->string().size() > maximumSoundDescriptionBytes)
            {
                addError(result, path + ".description", "Expected a description of at most 16384 bytes");
                valid = false;
            }
            else
            {
                definition.description = description->string();
            }
        }
        if (const auto* expands = objectMember(source, "expandsBeforeTransformation"); expands != nullptr)
        {
            if (sourceVersion < versionOneHundredFive)
            {
                addError(result, path + ".expandsBeforeTransformation", "A Sound's Expand box requires format 105");
                valid = false;
            }
            else if (!expands->isBoolean() || expands->boolean())
            {
                addError(result, path + ".expandsBeforeTransformation", "Expected false");
                valid = false;
            }
            else
            {
                definition.expandsBeforeTransformation = false;
            }
        }
        valid = parseSoundPorts(
                    result,
                    source,
                    "outputs",
                    path,
                    sourceVersion,
                    definition.outputs)
            && valid;

        const auto* graph = objectMember(source, "graph");
        if (graph == nullptr)
        {
            addError(
                result,
                path + ".graph",
                "Missing required object");
            valid = false;
        }
        else
        {
            parseGraphValue(
                result,
                *graph,
                definition.graph,
                presentation,
                path + ".graph",
                sourceVersion,
                preservedComponentCount,
                preservedComponentStateBytes);
        }

        if (valid && result.errors.size() == errorCountBefore)
        {
            result.document.soundDefinitions.push_back(
                std::move(definition));
            result.document.presentation.structureLayouts.push_back(
                std::move(presentation));
        }
    }
}

bool requiredFiniteNumberMember(ProjectLoadResult& result, const json::Value& object, std::string_view key, const std::string& path, double& output)
{
    const auto* value = objectMember(object, key);
    if (value == nullptr || !value->isNumber() || !std::isfinite(value->number()))
    {
        addError(result, path, "Expected finite number");
        return false;
    }
    output = value->number();
    return true;
}

bool requiredBooleanMember(
    ProjectLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    bool& output)
{
    const auto* value = objectMember(object, key);
    if (value == nullptr || !value->isBoolean())
    {
        addError(result, path, "Expected boolean");
        return false;
    }
    output = value->boolean();
    return true;
}

bool requiredUnsignedJsonIntegerMember(
    ProjectLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::uint64_t maximum,
    std::uint64_t& output)
{
    const auto* value = objectMember(object, key);
    if (value == nullptr || !value->isNumber()
        || !std::isfinite(value->number())
        || value->number() < 0.0
        || std::floor(value->number()) != value->number()
        || value->number() > static_cast<double>(maximum))
    {
        addError(result, path, "Expected non-negative safe integer");
        return false;
    }
    output = static_cast<std::uint64_t>(value->number());
    return true;
}

// The surface's controls, or with dormant the widgets kept for Event Values
// it has no control of (format 142), which have no place in the
// presentation.
void parseVirtualControls(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion,
    bool dormant = false)
{
    const std::string member = dormant ? "dormantVirtualControls" : "virtualControls";
    const auto* controls = objectMember(root, member);
    if (controls == nullptr)
        return;
    if (dormant && sourceVersion < versionOneHundredFortyTwo)
    {
        addError(result, "$." + member, "dormantVirtualControls requires format 142");
        return;
    }
    if (!controls->isArray() || (dormant && controls->array().size() > maximumDormantVirtualControls))
    {
        addError(result, "$." + member, dormant ? "Expected at most 256 dormant widgets" : "Expected array");
        return;
    }
    std::unordered_set<std::string> eventNames;
    std::vector<MidiControlMapping> midiMappings;
    std::unordered_set<std::string> oscAddresses;
    std::size_t oscMappingCount = 0;
    std::unordered_set<std::string> oscEgressAddresses;
    std::size_t oscEgressMappingCount = 0;
    for (std::size_t index = 0; index < controls->array().size(); ++index)
    {
        const auto& source = controls->array()[index];
        const auto path = "$." + member + "[" + std::to_string(index) + "]";
        if (!source.isObject())
        {
            addError(result, path, "Expected object");
            continue;
        }
        VirtualControl control;
        std::string domain;
        std::string kind;
        const bool hasEventName = requiredStringMember(result, source, "eventName", path + ".eventName", control.eventName);
        const bool hasDomain = requiredStringMember(result, source, "domain", path + ".domain", domain);
        const bool hasKind = requiredStringMember(result, source, "kind", path + ".kind", kind);
        const bool hasValue = requiredFiniteNumberMember(result, source, "value", path + ".value", control.value);
        const bool hasMinimum = requiredFiniteNumberMember(result, source, "minimum", path + ".minimum", control.minimum);
        const bool hasMaximum = requiredFiniteNumberMember(result, source, "maximum", path + ".maximum", control.maximum);
        CanvasPosition position;
        const bool hasPosition = parseCanvasPosition(
            result,
            source,
            path,
            position);
        const auto parsedDomain = hasDomain ? parseValueDomain(domain) : std::nullopt;
        const auto parsedKind = hasKind ? parseVirtualControlKind(kind) : std::nullopt;
        if (!parsedDomain.has_value()) addError(result, path + ".domain", "Expected known Event Value domain");
        if (!parsedKind.has_value()) addError(result, path + ".kind", "Expected fader, gate, or toggle");
        const bool hasValidEventName = hasEventName && isValidEventName(control.eventName);
        if (hasEventName && !hasValidEventName) addError(result, path + ".eventName", "Expected Event Value identifier");
        if (!(control.minimum < control.maximum)) addError(result, path, "Virtual control minimum must be less than maximum");
        const bool valueIsInRange = control.value >= control.minimum && control.value <= control.maximum;
        if (hasValue && hasMinimum && hasMaximum && !valueIsInRange)
            addError(result, path + ".value", "Virtual control value must be within its range");

        const auto* osc = objectMember(source, "oscMapping");
        bool hasValidOsc = true;
        if (osc != nullptr && sourceVersion < versionEighteen)
        {
            addError(
                result,
                path + ".oscMapping",
                "OSC mapping requires format 18");
            hasValidOsc = false;
        }
        else if (osc != nullptr)
        {
            const auto* address = osc->isObject()
                ? objectMember(*osc, "address")
                : nullptr;
            if (!osc->isObject() || osc->object().size() != 1
                || address == nullptr || !address->isString()
                || !isValidOscAddress(address->string()))
            {
                addError(
                    result,
                    path + ".oscMapping",
                    "Expected one exact OSC address only");
                hasValidOsc = false;
            }
            else
            {
                control.oscMapping = OscControlMapping{
                    address->string()};
            }
        }

        const auto* oscEgress = objectMember(source, "oscEgressMapping");
        bool hasValidOscEgress = true;
        if (oscEgress != nullptr && sourceVersion < versionNineteen)
        {
            addError(
                result,
                path + ".oscEgressMapping",
                "OSC feedback mapping requires format 19");
            hasValidOscEgress = false;
        }
        else if (oscEgress != nullptr)
        {
            const auto* address = oscEgress->isObject()
                ? objectMember(*oscEgress, "address")
                : nullptr;
            if (!oscEgress->isObject()
                || oscEgress->object().size() != 1
                || address == nullptr || !address->isString()
                || !isValidOscAddress(address->string()))
            {
                addError(
                    result,
                    path + ".oscEgressMapping",
                    "Expected one exact OSC feedback address only");
                hasValidOscEgress = false;
            }
            else
            {
                control.oscEgressMapping = OscEgressMapping{
                    address->string()};
            }
        }

        const auto* widget = objectMember(source, "widget");
        bool hasValidWidget = true;
        if (widget != nullptr && sourceVersion < versionNinetySeven)
        {
            addError(result, path + ".widget", "Virtual control widget options require format 97");
            hasValidWidget = false;
        }
        else if (widget != nullptr)
        {
            hasValidWidget = parseVirtualControlWidget(result, *widget, path + ".widget", control, sourceVersion);
        }

        const auto* midi = objectMember(source, "midi");
        bool hasValidMidi = true;
        if (midi != nullptr)
        {
            if (!midi->isObject())
            {
                addError(result, path + ".midi", "Expected object");
                hasValidMidi = false;
            }
            else
            {
                MidiControlMapping mapping;
                const auto* channel = objectMember(*midi, "channel");
                const auto* controller = objectMember(*midi, "controller");
                std::int32_t parsedChannel = 0;
                std::int32_t parsedController = 0;
                if (channel == nullptr || controller == nullptr
                    || !channel->isNumber() || !controller->isNumber()
                    || !parseSigned32BitIntegerToken(
                        channel->numberToken(), parsedChannel)
                    || !parseSigned32BitIntegerToken(
                        controller->numberToken(), parsedController)
                    || parsedChannel < 1 || parsedChannel > 16
                    || parsedController < 0 || parsedController > 127)
                {
                    addError(result, path + ".midi", "Expected MIDI channel 1...16 and controller 0...127");
                    hasValidMidi = false;
                }
                else
                {
                    mapping.channel = parsedChannel;
                    mapping.controller = parsedController;
                    if (sourceVersion >= versionFourteen)
                    {
                        const auto* sourceMode = objectMember(*midi, "sourceMode");
                        const auto* sourceIdentifier = objectMember(
                            *midi, "sourceIdentifier");
                        if (midi->object().size() != 4
                            || sourceMode == nullptr || !sourceMode->isString()
                            || sourceIdentifier == nullptr
                            || !sourceIdentifier->isString())
                        {
                            addError(
                                result,
                                path + ".midi",
                                "Expected sourceMode, sourceIdentifier, channel, and controller only");
                            hasValidMidi = false;
                        }
                        else
                        {
                            const std::string_view identifier =
                                sourceIdentifier->string();
                            if (sourceMode->string() == "exactInput"
                                && !identifier.empty()
                                && identifier.size()
                                    <= maximumMidiSourceIdentifierBytes
                                && json::isValidUtf8(identifier))
                            {
                                mapping.sourceMode = MidiControlSourceMode::ExactInput;
                            }
                            else if (sourceMode->string() == "legacyAnyInput"
                                && identifier.empty())
                            {
                                mapping.sourceMode = MidiControlSourceMode::LegacyAnyInput;
                            }
                            else
                                hasValidMidi = false;
                            if (hasValidMidi)
                                mapping.sourceIdentifier.assign(identifier);
                            if (!hasValidMidi
                                || !isValidMidiControlMapping(mapping))
                            {
                                addError(
                                    result,
                                    path + ".midi",
                                    "Expected a valid exactInput or legacyAnyInput MIDI source");
                                hasValidMidi = false;
                            }
                        }
                    }
                    else if (midi->object().size() != 2)
                    {
                        addError(
                            result,
                            path + ".midi",
                            "Legacy MIDI mapping expects channel and controller only");
                        hasValidMidi = false;
                    }
                    if (hasValidMidi)
                        control.midiMapping = std::move(mapping);
                }
            }
        }
        if (sourceVersion >= versionEighteen)
        {
            const std::size_t expectedMemberCount = 6
                + (midi != nullptr ? 1 : 0)
                + (osc != nullptr ? 1 : 0)
                + (oscEgress != nullptr ? 1 : 0)
                + (widget != nullptr ? 1 : 0);
            if (source.object().size() != expectedMemberCount)
            {
                addError(
                    result,
                    path,
                    sourceVersion >= versionNineteen
                        ? "Expected Event Value fields plus optional midi, oscMapping, and oscEgressMapping only"
                        : "Expected Event Value fields plus optional midi and oscMapping only");
                hasValidOsc = false;
            }
        }
        if (hasValidEventName && hasValue && hasMinimum && hasMaximum && hasPosition
            && parsedDomain.has_value() && parsedKind.has_value() && control.minimum < control.maximum && valueIsInRange && hasValidMidi && hasValidOsc && hasValidOscEgress && hasValidWidget)
        {
            if (!eventNames.insert(control.eventName).second)
            {
                addError(result, path + ".eventName", "Duplicate Event Value name");
                continue;
            }
            if (control.midiMapping.has_value())
            {
                if (std::ranges::any_of(
                        midiMappings,
                        [&](const auto& existing) {
                            return midiControlMappingsConflict(
                                existing, *control.midiMapping);
                        }))
                {
                    addError(result, path + ".midi", "Duplicate MIDI controller mapping");
                    continue;
                }
                midiMappings.push_back(*control.midiMapping);
            }
            if (control.oscMapping.has_value())
            {
                if (++oscMappingCount > maximumOscControlMappings)
                {
                    addError(
                        result,
                        path + ".oscMapping",
                        "OSC mappings exceed the fixed 128-route limit");
                    continue;
                }
                if (!oscAddresses.insert(
                        control.oscMapping->address).second)
                {
                    addError(
                        result,
                        path + ".oscMapping.address",
                        "Duplicate exact OSC address");
                    continue;
                }
            }
            if (control.oscEgressMapping.has_value())
            {
                if (++oscEgressMappingCount > maximumOscControlMappings)
                {
                    addError(
                        result,
                        path + ".oscEgressMapping",
                        "OSC feedback mappings exceed the fixed 128-route limit");
                    continue;
                }
                if (!oscEgressAddresses.insert(
                        control.oscEgressMapping->address).second)
                {
                    addError(
                        result,
                        path + ".oscEgressMapping.address",
                        "Duplicate exact OSC feedback address");
                    continue;
                }
            }
            control.domain = *parsedDomain;
            control.kind = *parsedKind;
            if (dormant)
            {
                result.document.dormantVirtualControls.push_back(std::move(control));
                continue;
            }
            ProjectPerformanceControlPresentation presentation;
            presentation.target.kind = SnapshotTargetKind::EventValue;
            presentation.target.eventName = control.eventName;
            presentation.target.domain = control.domain;
            presentation.displayName = control.eventName;
            presentation.position = position;
            switch (control.kind)
            {
            case VirtualControlKind::Fader:
                presentation.kind = ProjectPerformanceControlKind::Fader;
                break;
            case VirtualControlKind::Gate:
                presentation.kind = ProjectPerformanceControlKind::Gate;
                break;
            case VirtualControlKind::Toggle:
                presentation.kind = ProjectPerformanceControlKind::Toggle;
                break;
            }
            result.document.presentation.performanceControls.push_back(
                std::move(presentation));
            result.document.virtualControls.push_back(std::move(control));
        }
    }
}

void parseAssets(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* assets = objectMember(root, "assets");
    if (assets == nullptr)
        return;
    if (!assets->isArray())
    {
        addError(result, "$.assets", "Expected array");
        return;
    }

    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < assets->array().size(); ++index)
    {
        const auto& source = assets->array()[index];
        const auto path = "$.assets[" + std::to_string(index) + "]";
        if (!source.isObject())
        {
            addError(result, path, "Expected object");
            continue;
        }

        ProjectAsset asset;
        std::string kind;
        const auto hasId = requiredStringMember(result, source, "id", path + ".id", asset.id);
        const auto hasKind = requiredStringMember(result, source, "kind", path + ".kind", kind);
        const auto hasPath = requiredStringMember(result, source, "path", path + ".path", asset.path);
        bool valid = hasId && hasKind && hasPath;
        if (hasId && asset.id.empty())
        {
            addError(result, path + ".id", "Project asset id cannot be empty");
            valid = false;
        }
        if (hasPath && asset.path.empty())
        {
            addError(result, path + ".path", "Project asset path cannot be empty");
            valid = false;
        }
        if (kind == "table")
            asset.kind = ProjectAssetKind::Table;
        else if (kind == "binary")
            asset.kind = ProjectAssetKind::Binary;
        else if (sourceVersion >= versionTen && kind == "audio")
            asset.kind = ProjectAssetKind::Audio;
        else if (sourceVersion >= versionTwentyTwo
                 && kind == "partialSpectrum")
        {
            asset.kind = ProjectAssetKind::PartialSpectrum;
        }
        else
        {
            if (hasKind)
                addError(
                    result,
                    path + ".kind",
                    sourceVersion >= versionTwentyTwo
                        ? "Expected table, binary, audio, or partialSpectrum"
                        : sourceVersion >= versionTen
                        ? "Expected table, binary, or audio"
                        : "Expected table or binary");
            valid = false;
        }
        if (sourceVersion >= versionTen)
        {
            std::string disposition;
            valid = requiredStringMember(
                        result,
                        source,
                        "disposition",
                        path + ".disposition",
                        disposition)
                && valid;
            if (disposition == "referenced")
                asset.disposition = ProjectAssetDisposition::Referenced;
            else if (disposition == "embedded")
                asset.disposition = ProjectAssetDisposition::Embedded;
            else
            {
                addError(
                    result,
                    path + ".disposition",
                    "Expected referenced or embedded");
                valid = false;
            }
            valid = requiredStringMember(
                        result,
                        source,
                        "contentHash",
                        path + ".contentHash",
                        asset.contentHash)
                && valid;
            if (!asset.contentHash.empty()
                && !isCanonicalProjectAssetContentHash(
                    asset.contentHash))
            {
                addError(
                    result,
                    path + ".contentHash",
                    "Expected empty or lowercase SHA-256 hash");
                valid = false;
            }
            if (asset.disposition
                    == ProjectAssetDisposition::Embedded
                && asset.contentHash.empty())
            {
                addError(
                    result,
                    path + ".contentHash",
                    "Embedded assets require a verified SHA-256 hash");
                valid = false;
            }
            std::uint64_t channelCount = 0;
            valid = requiredUnsignedJsonIntegerMember(
                        result,
                        source,
                        "channelCount",
                        path + ".channelCount",
                        std::numeric_limits<std::uint32_t>::max(),
                        channelCount)
                && valid;
            asset.channelCount = static_cast<std::uint32_t>(
                channelCount);
            valid = requiredFiniteNumberMember(
                        result,
                        source,
                        "sampleRate",
                        path + ".sampleRate",
                        asset.sampleRate)
                && valid;
            if (asset.sampleRate < 0.0
                || asset.sampleRate
                    > static_cast<double>(
                        std::numeric_limits<std::uint32_t>::max()))
            {
                addError(
                    result,
                    path + ".sampleRate",
                    "Expected non-negative sample rate");
                valid = false;
            }
            valid = requiredUnsignedJsonIntegerMember(
                        result,
                        source,
                        "frameCount",
                        path + ".frameCount",
                        maximumProjectAssetJsonInteger,
                        asset.frameCount)
                && valid;
            valid = requiredStringMember(
                        result,
                        source,
                        "encoding",
                        path + ".encoding",
                        asset.encoding)
                && valid;
            valid = requiredUnsignedJsonIntegerMember(
                        result,
                        source,
                        "generation",
                        path + ".generation",
                        maximumProjectAssetJsonInteger,
                        asset.generation)
                && valid;
            if (asset.generation == 0U)
            {
                addError(
                    result,
                    path + ".generation",
                    "Project asset generation must be positive");
                valid = false;
            }
            valid = requiredStringMember(
                        result,
                        source,
                        "provenance",
                        path + ".provenance",
                        asset.provenance)
                && valid;
            if (asset.kind != ProjectAssetKind::Audio
                && asset.kind != ProjectAssetKind::PartialSpectrum
                && (asset.channelCount != 0U
                    || asset.sampleRate != 0.0
                    || asset.frameCount != 0U
                    || !asset.encoding.empty()))
            {
                addError(
                    result,
                    path,
                    "Table and binary assets require empty media metadata");
                valid = false;
            }
            if (asset.kind == ProjectAssetKind::PartialSpectrum)
            {
                if (asset.channelCount != 0U
                    || asset.sampleRate <= 0.0
                    || asset.frameCount == 0U
                    || asset.encoding != partialTrackAssetEncoding
                    || asset.contentHash.empty())
                {
                    addError(
                        result,
                        path,
                        "Partial-spectrum assets require exact typed media metadata");
                    valid = false;
                }
            }
        }
        if (hasId && !ids.insert(asset.id).second)
        {
            addError(
                result,
                path + ".id",
                "Duplicate project asset id/generation");
            valid = false;
        }
        if (valid)
            result.document.assets.push_back(std::move(asset));
    }
}

void parsePartialTrackEdits(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "partialTrackEdits");
    if (sourceVersion < versionTwentyTwo)
    {
        if (source != nullptr)
        {
            addError(
                result,
                "$.partialTrackEdits",
                "Partial-track edits require format 22");
        }
        return;
    }
    if (source == nullptr || !source->isArray())
    {
        addError(
            result,
            "$.partialTrackEdits",
            "Expected required partial-track edit array");
        return;
    }
    if (source->array().size() > maximumProjectPartialTrackEditSets)
    {
        addError(
            result,
            "$.partialTrackEdits",
            "Partial-track edit-set count exceeds the fixed limit");
        return;
    }

    std::unordered_set<std::string> editSetIds;
    std::unordered_set<std::string> assetIds;
    for (std::size_t setIndex = 0;
         setIndex < source->array().size();
         ++setIndex)
    {
        const auto& setSource = source->array()[setIndex];
        const auto path = "$.partialTrackEdits["
            + std::to_string(setIndex) + "]";
        if (!setSource.isObject() || setSource.object().size() != 7U)
        {
            addError(
                result,
                path,
                "Expected closed partial-track edit-set object");
            continue;
        }

        ProjectPartialTrackEditSet edits;
        auto valid = requiredStringMember(
            result, setSource, "editSetId", path + ".editSetId", edits.editSetId);
        valid = requiredStringMember(
                    result, setSource, "assetId", path + ".assetId", edits.assetId)
            && valid;
        valid = requiredExactUnsignedIntegerMember(
                    result,
                    setSource,
                    "assetGeneration",
                    path + ".assetGeneration",
                    maximumProjectAssetJsonInteger,
                    edits.assetGeneration)
            && valid;
        valid = requiredStringMember(
                    result,
                    setSource,
                    "sourceContentHash",
                    path + ".sourceContentHash",
                    edits.sourceContentHash)
            && valid;

        const auto* overrides = objectMember(setSource, "pointOverrides");
        if (overrides == nullptr || !overrides->isArray()
            || overrides->array().size()
                > maximumProjectPartialTrackPointOverrides)
        {
            addError(result, path + ".pointOverrides", "Expected bounded array");
            valid = false;
        }
        else
        {
            edits.pointOverrides.reserve(overrides->array().size());
            for (std::size_t index = 0;
                 index < overrides->array().size();
                 ++index)
            {
                const auto& pointSource = overrides->array()[index];
                const auto pointPath = path + ".pointOverrides["
                    + std::to_string(index) + "]";
                if (!pointSource.isObject()
                    || pointSource.object().size() != 10U)
                {
                    addError(result, pointPath, "Expected closed point override object");
                    valid = false;
                    continue;
                }
                ProjectPartialTrackPointOverride point;
                auto pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "sourceSegment", pointPath + ".sourceSegment",
                    std::numeric_limits<std::uint64_t>::max(), point.source.segmentId);
                pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "sourceFrame", pointPath + ".sourceFrame",
                    maximumPartialTrackSourceFrames, point.source.sourceFrame) && pointValid;
                pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "sourceTrack", pointPath + ".sourceTrack",
                    std::numeric_limits<std::uint64_t>::max(), point.source.trackId) && pointValid;
                pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "outputFrame", pointPath + ".outputFrame",
                    maximumPartialTrackSourceFrames, point.outputFrame) && pointValid;
                pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "outputTrack", pointPath + ".outputTrack",
                    std::numeric_limits<std::uint64_t>::max(), point.output.trackId) && pointValid;
                pointValid = requiredFiniteNumberMember(
                    result, pointSource, "amplitude", pointPath + ".amplitude", point.output.amplitude) && pointValid;
                pointValid = requiredFiniteNumberMember(
                    result, pointSource, "frequencyHz", pointPath + ".frequencyHz", point.output.frequencyHz) && pointValid;
                pointValid = requiredFiniteNumberMember(
                    result, pointSource, "bandwidthHz", pointPath + ".bandwidthHz", point.output.bandwidthHz) && pointValid;
                std::string interpolation;
                pointValid = requiredStringMember(
                    result, pointSource, "interpolation", pointPath + ".interpolation", interpolation) && pointValid;
                if (interpolation == "step")
                    point.interpolation = PartialTrackPointInterpolation::Step;
                else if (interpolation == "linear")
                    point.interpolation = PartialTrackPointInterpolation::Linear;
                else
                {
                    addError(result, pointPath + ".interpolation", "Expected step or linear");
                    pointValid = false;
                }
                pointValid = requiredBooleanMember(
                    result, pointSource, "excluded", pointPath + ".excluded", point.excluded) && pointValid;
                if (pointValid)
                    edits.pointOverrides.push_back(std::move(point));
                else
                    valid = false;
            }
        }

        const auto* inserted = objectMember(setSource, "insertedPoints");
        if (inserted == nullptr || !inserted->isArray()
            || inserted->array().size()
                > maximumProjectPartialTrackInsertedPoints)
        {
            addError(result, path + ".insertedPoints", "Expected bounded array");
            valid = false;
        }
        else
        {
            edits.insertedPoints.reserve(inserted->array().size());
            for (std::size_t index = 0;
                 index < inserted->array().size();
                 ++index)
            {
                const auto& pointSource = inserted->array()[index];
                const auto pointPath = path + ".insertedPoints["
                    + std::to_string(index) + "]";
                if (!pointSource.isObject()
                    || pointSource.object().size() != 8U)
                {
                    addError(result, pointPath, "Expected closed inserted-point object");
                    valid = false;
                    continue;
                }
                ProjectPartialTrackInsertedPoint point;
                auto pointValid = requiredStringMember(
                    result, pointSource, "editId", pointPath + ".editId", point.editId);
                pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "segment", pointPath + ".segment",
                    std::numeric_limits<std::uint64_t>::max(), point.segmentId) && pointValid;
                pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "frame", pointPath + ".frame",
                    maximumPartialTrackSourceFrames, point.sourceFrame) && pointValid;
                pointValid = requiredExactUnsignedIntegerMember(
                    result, pointSource, "track", pointPath + ".track",
                    std::numeric_limits<std::uint64_t>::max(), point.point.trackId) && pointValid;
                pointValid = requiredFiniteNumberMember(
                    result, pointSource, "amplitude", pointPath + ".amplitude", point.point.amplitude) && pointValid;
                pointValid = requiredFiniteNumberMember(
                    result, pointSource, "frequencyHz", pointPath + ".frequencyHz", point.point.frequencyHz) && pointValid;
                pointValid = requiredFiniteNumberMember(
                    result, pointSource, "bandwidthHz", pointPath + ".bandwidthHz", point.point.bandwidthHz) && pointValid;
                std::string interpolation;
                pointValid = requiredStringMember(
                    result, pointSource, "interpolation", pointPath + ".interpolation", interpolation) && pointValid;
                if (interpolation == "step")
                    point.interpolation = PartialTrackPointInterpolation::Step;
                else if (interpolation == "linear")
                    point.interpolation = PartialTrackPointInterpolation::Linear;
                else
                {
                    addError(result, pointPath + ".interpolation", "Expected step or linear");
                    pointValid = false;
                }
                if (pointValid)
                    edits.insertedPoints.push_back(std::move(point));
                else
                    valid = false;
            }
        }

        const auto* states = objectMember(setSource, "trackStates");
        if (states == nullptr || !states->isArray()
            || states->array().size() > maximumProjectPartialTrackStates)
        {
            addError(result, path + ".trackStates", "Expected bounded array");
            valid = false;
        }
        else
        {
            edits.trackStates.reserve(states->array().size());
            for (std::size_t index = 0;
                 index < states->array().size();
                 ++index)
            {
                const auto& stateSource = states->array()[index];
                const auto statePath = path + ".trackStates["
                    + std::to_string(index) + "]";
                if (!stateSource.isObject()
                    || stateSource.object().size() != 3U)
                {
                    addError(result, statePath, "Expected closed track-state object");
                    valid = false;
                    continue;
                }
                ProjectPartialTrackState state;
                auto stateValid = requiredExactUnsignedIntegerMember(
                    result, stateSource, "track", statePath + ".track",
                    std::numeric_limits<std::uint64_t>::max(), state.trackId);
                stateValid = requiredBooleanMember(
                    result, stateSource, "muted", statePath + ".muted", state.muted) && stateValid;
                stateValid = requiredBooleanMember(
                    result, stateSource, "solo", statePath + ".solo", state.solo) && stateValid;
                if (stateValid)
                    edits.trackStates.push_back(std::move(state));
                else
                    valid = false;
            }
        }

        if (valid)
        {
            if (const auto editError = validateProjectPartialTrackEditSet(edits))
            {
                addError(result, path, editError->message);
                valid = false;
            }
        }
        if (valid && !editSetIds.insert(edits.editSetId).second)
        {
            addError(result, path + ".editSetId", "Duplicate partial-track edit-set identity");
            valid = false;
        }
        if (valid && !assetIds.insert(edits.assetId).second)
        {
            addError(result, path + ".assetId", "Only one edit set may target an asset");
            valid = false;
        }
        if (valid)
        {
            const auto asset = std::ranges::find(
                result.document.assets, edits.assetId, &ProjectAsset::id);
            if (asset == result.document.assets.end()
                || !partialTrackEditSetMatchesAsset(edits, *asset))
            {
                addError(result, path, "Partial-track edit set does not match its exact source asset");
                valid = false;
            }
        }
        if (valid)
            result.document.partialTrackEdits.push_back(std::move(edits));
    }
}

void parseDeviceSettings(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "deviceSettings");
    if (source == nullptr || !source->isObject())
    {
        addError(result, "$.deviceSettings", "Expected required object");
        return;
    }
    auto& settings = result.document.deviceSettings;
    bool valid = true;
    const std::size_t expectedDeviceSettingMembers =
        sourceVersion >= versionNineteen
        ? 11
        : sourceVersion >= versionEighteen ? 10 : 9;
    if (sourceVersion >= versionFifteen
        && source->object().size() != expectedDeviceSettingMembers)
    {
        addError(
            result,
            "$.deviceSettings",
            sourceVersion >= versionNineteen
                ? "Expected device fields, one transport authority, oscIngress, and oscEgress only"
                : sourceVersion >= versionEighteen
                    ? "Expected device fields, one transport authority, and oscIngress only"
                : "Expected configured, audio input/output, sample rate, buffer size, channel lists, MIDI inputs, and one transport sync authority only");
        valid = false;
    }
    if (sourceVersion < versionFifteen
        && objectMember(*source, "midiClock") != nullptr)
    {
        addError(
            result,
            "$.deviceSettings.midiClock",
            "MIDI Clock authority requires format 15");
        valid = false;
    }
    if (sourceVersion < versionSeventeen
        && objectMember(*source, "transportSync") != nullptr)
    {
        addError(
            result,
            "$.deviceSettings.transportSync",
            "Transport sync authority requires format 17");
        valid = false;
    }
    if (sourceVersion >= versionSeventeen
        && objectMember(*source, "midiClock") != nullptr)
    {
        addError(
            result,
            "$.deviceSettings.midiClock",
            "Format 17 and later require transportSync instead of midiClock");
        valid = false;
    }
    if (sourceVersion < versionEighteen
        && objectMember(*source, "oscIngress") != nullptr)
    {
        addError(
            result,
            "$.deviceSettings.oscIngress",
            "OSC ingress requires format 18");
        valid = false;
    }
    if (sourceVersion < versionNineteen
        && objectMember(*source, "oscEgress") != nullptr)
    {
        addError(
            result,
            "$.deviceSettings.oscEgress",
            "OSC feedback requires format 19");
        valid = false;
    }
    const auto* configured = objectMember(*source, "configured");
    if (configured == nullptr || !configured->isBoolean())
    {
        addError(result, "$.deviceSettings.configured", "Expected boolean");
        valid = false;
    }
    else
    {
        settings.configured = configured->boolean();
    }
    valid = requiredStringMember(
                result,
                *source,
                "audioInputDevice",
                "$.deviceSettings.audioInputDevice",
                settings.audioInputDeviceName)
        && valid;
    valid = requiredStringMember(
                result,
                *source,
                "audioOutputDevice",
                "$.deviceSettings.audioOutputDevice",
                settings.audioOutputDeviceName)
        && valid;
    valid = requiredFiniteNumberMember(
                result,
                *source,
                "sampleRate",
                "$.deviceSettings.sampleRate",
                settings.sampleRate)
        && valid;
    const auto* bufferSize = objectMember(*source, "bufferSize");
    if (bufferSize == nullptr || !bufferSize->isNumber()
        || !parseSigned32BitIntegerToken(
            bufferSize->numberToken(),
            settings.bufferSize))
    {
        addError(
            result,
            "$.deviceSettings.bufferSize",
            "Expected signed 32-bit integer");
        valid = false;
    }

    const auto parseChannels = [&](std::string_view key,
                                   std::vector<std::int32_t>& channels) {
        const auto path = "$.deviceSettings." + std::string(key);
        const auto* values = objectMember(*source, key);
        if (values == nullptr || !values->isArray())
        {
            addError(result, path, "Expected array");
            valid = false;
            return;
        }
        std::unordered_set<std::int32_t> unique;
        for (std::size_t index = 0; index < values->array().size(); ++index)
        {
            const auto& value = values->array()[index];
            std::int32_t channel = 0;
            if (!value.isNumber()
                || !parseSigned32BitIntegerToken(
                    value.numberToken(),
                    channel)
                || channel < 1 || channel > 8
                || !unique.insert(channel).second)
            {
                addError(
                    result,
                    path + "[" + std::to_string(index) + "]",
                    "Expected unique one-based channel 1...8");
                valid = false;
                continue;
            }
            channels.push_back(channel);
        }
    };
    parseChannels("inputChannels", settings.inputChannels);
    parseChannels("outputChannels", settings.outputChannels);

    const auto* midi = objectMember(*source, "midiInputDevices");
    if (midi == nullptr || !midi->isArray())
    {
        addError(
            result,
            "$.deviceSettings.midiInputDevices",
            "Expected array");
        valid = false;
    }
    else
    {
        std::unordered_set<std::string> unique;
        for (std::size_t index = 0; index < midi->array().size(); ++index)
        {
            const auto& value = midi->array()[index];
            if (!value.isString()
                || !isValidMidiSourceIdentifier(value.string())
                || !unique.insert(value.string()).second)
            {
                addError(
                    result,
                    "$.deviceSettings.midiInputDevices["
                        + std::to_string(index) + "]",
                    "Expected unique non-empty string");
                valid = false;
                continue;
            }
            settings.midiInputDeviceIdentifiers.push_back(value.string());
        }
    }

    if (sourceVersion >= versionFifteen)
    {
        const auto currentAuthority = sourceVersion >= versionSeventeen;
        const auto authorityKey = currentAuthority
            ? std::string_view{"transportSync"}
            : std::string_view{"midiClock"};
        const auto authorityPath = currentAuthority
            ? std::string{"$.deviceSettings.transportSync"}
            : std::string{"$.deviceSettings.midiClock"};
        const auto* authority = objectMember(*source, authorityKey);
        const auto* sourceMode = authority != nullptr && authority->isObject()
            ? objectMember(*authority, "sourceMode")
            : nullptr;
        const auto* sourceIdentifier =
            authority != nullptr && authority->isObject()
            ? objectMember(*authority, "sourceIdentifier")
            : nullptr;
        if (authority == nullptr || !authority->isObject()
            || authority->object().size() != 2
            || sourceMode == nullptr || !sourceMode->isString()
            || sourceIdentifier == nullptr
            || !sourceIdentifier->isString())
        {
            addError(
                result,
                authorityPath,
                "Expected sourceMode and sourceIdentifier only");
            valid = false;
        }
        else if (sourceMode->string() == "internal")
        {
            settings.transportSync.sourceMode =
                TransportSyncSourceMode::Internal;
            if (!sourceIdentifier->string().empty())
            {
                addError(
                    result,
                    authorityPath + ".sourceIdentifier",
                    "Internal transport authority requires an empty source identifier");
                valid = false;
            }
        }
        else if ((!currentAuthority
                    && sourceMode->string() == "exactInput")
            || (currentAuthority
                && (sourceMode->string() == "midiClockExactInput"
                    || sourceMode->string()
                        == "midiTimeCodeExactInput")))
        {
            const auto identifier = std::string_view{
                sourceIdentifier->string()};
            if (!isValidMidiSourceIdentifier(identifier)
                || std::ranges::find(
                       settings.midiInputDeviceIdentifiers,
                       identifier)
                    == settings.midiInputDeviceIdentifiers.end())
            {
                addError(
                    result,
                    authorityPath + ".sourceIdentifier",
                    "Exact transport authority requires one enabled bounded MIDI input identifier");
                valid = false;
            }
            else
            {
                settings.transportSync.sourceMode =
                    currentAuthority
                        && sourceMode->string()
                            == "midiTimeCodeExactInput"
                    ? TransportSyncSourceMode::MidiTimeCodeExactInput
                    : TransportSyncSourceMode::MidiClockExactInput;
                settings.transportSync.sourceIdentifier.assign(identifier);
            }
        }
        else
        {
            addError(
                result,
                authorityPath + ".sourceMode",
                currentAuthority
                    ? "Expected internal, midiClockExactInput, or midiTimeCodeExactInput"
                    : "Expected internal or exactInput");
            valid = false;
        }
    }

    if (sourceVersion >= versionEighteen)
    {
        const auto* oscIngress = objectMember(*source, "oscIngress");
        const auto* enabled = oscIngress != nullptr
                && oscIngress->isObject()
            ? objectMember(*oscIngress, "enabled")
            : nullptr;
        const auto* port = oscIngress != nullptr
                && oscIngress->isObject()
            ? objectMember(*oscIngress, "port")
            : nullptr;
        std::int32_t parsedPort = 0;
        if (oscIngress == nullptr || !oscIngress->isObject()
            || oscIngress->object().size() != 2
            || enabled == nullptr || !enabled->isBoolean()
            || port == nullptr || !port->isNumber()
            || !parseSigned32BitIntegerToken(
                port->numberToken(), parsedPort))
        {
            addError(
                result,
                "$.deviceSettings.oscIngress",
                "Expected enabled boolean and exact integer port only");
            valid = false;
        }
        else
        {
            settings.oscIngress.enabled = enabled->boolean();
            settings.oscIngress.port = parsedPort;
            if (!isValidOscIngressSettings(settings.oscIngress))
            {
                addError(
                    result,
                    "$.deviceSettings.oscIngress",
                    "Disabled OSC requires port 9000; enabled OSC requires port 1024...65535");
                valid = false;
            }
        }
    }

    if (sourceVersion >= versionNineteen)
    {
        const auto* oscEgress = objectMember(*source, "oscEgress");
        const auto* enabled = oscEgress != nullptr
                && oscEgress->isObject()
            ? objectMember(*oscEgress, "enabled")
            : nullptr;
        const auto* port = oscEgress != nullptr
                && oscEgress->isObject()
            ? objectMember(*oscEgress, "port")
            : nullptr;
        std::int32_t parsedPort = 0;
        if (oscEgress == nullptr || !oscEgress->isObject()
            || oscEgress->object().size() != 2
            || enabled == nullptr || !enabled->isBoolean()
            || port == nullptr || !port->isNumber()
            || !parseSigned32BitIntegerToken(
                port->numberToken(), parsedPort))
        {
            addError(
                result,
                "$.deviceSettings.oscEgress",
                "Expected enabled boolean and exact integer port only");
            valid = false;
        }
        else
        {
            settings.oscEgress.enabled = enabled->boolean();
            settings.oscEgress.port = parsedPort;
            if (!isValidOscEgressSettings(settings.oscEgress))
            {
                addError(
                    result,
                    "$.deviceSettings.oscEgress",
                    "Disabled OSC feedback requires port 9001; enabled feedback requires port 1024...65535");
                valid = false;
            }
        }
        if (!isValidOscEndpointPair(
                settings.oscIngress, settings.oscEgress))
        {
            addError(
                result,
                "$.deviceSettings.oscEgress.port",
                "OSC ingress and feedback ports must differ while both are enabled");
            valid = false;
        }
    }

    if (settings.configured
        && (!std::isfinite(settings.sampleRate)
            || settings.sampleRate <= 0.0
            || settings.bufferSize < 1
            || settings.bufferSize > 1048576))
    {
        addError(
            result,
            "$.deviceSettings",
            "Configured settings require a positive sample rate and valid buffer size");
        valid = false;
    }
    if (!settings.configured
        && settings.transportSync.sourceMode
            != TransportSyncSourceMode::Internal)
    {
        addError(
            result,
            sourceVersion >= versionSeventeen
                ? "$.deviceSettings.transportSync"
                : "$.deviceSettings.midiClock",
            "Exact transport authority requires configured device settings");
        valid = false;
    }
    if (!valid)
        settings = {};
}

bool parseScoreTime(ProjectLoadResult& result,
                    const json::Value& source,
                    const std::string& path,
                    ScoreTime& output)
{
    if (!source.isObject() || source.object().size() != 2)
    {
        addError(result, path, "Expected closed score-time object");
        return false;
    }
    std::string domainName;
    if (!requiredStringMember(result, source, "domain", path + ".domain", domainName))
        return false;
    const auto domain = parseScoreTimeDomain(domainName);
    if (!domain.has_value())
    {
        addError(result, path + ".domain", "Expected seconds or musicalTicks");
        return false;
    }
    if (*domain == ScoreTimeDomain::Seconds)
    {
        double seconds = 0.0;
        if (!requiredFiniteNumberMember(result, source, "seconds", path + ".seconds", seconds))
            return false;
        output = ScoreTime{ScoreTimeDomain::Seconds, seconds, 0U};
        return true;
    }
    std::uint64_t tick = 0U;
    if (!requiredExactUnsignedIntegerMember(result,
                                            source,
                                            "tick",
                                            path + ".tick",
                                            std::numeric_limits<std::uint64_t>::max(),
                                            tick))
    {
        return false;
    }
    output = ScoreTime{ScoreTimeDomain::MusicalTicks, 0.0, tick};
    return true;
}

void parseScore(ProjectLoadResult& result, const json::Value& timeline, int sourceVersion)
{
    const auto* source = objectMember(timeline, "score");
    if (sourceVersion < versionTwentyOne)
    {
        if (source != nullptr)
            addError(result, "$.timeline.score", "Score requires format 21");
        return;
    }
    if (source == nullptr || !source->isObject() || source->object().size() != 4)
    {
        addError(
            result, "$.timeline.score", "Expected tracks, clips, automation, and markers only");
        return;
    }
    const auto* tracks = objectMember(*source, "tracks");
    const auto* clips = objectMember(*source, "clips");
    const auto* automation = objectMember(*source, "automation");
    const auto* markers = objectMember(*source, "markers");
    if (tracks == nullptr || !tracks->isArray() || clips == nullptr || !clips->isArray() ||
        automation == nullptr || !automation->isArray() || markers == nullptr ||
        !markers->isArray())
    {
        addError(result, "$.timeline.score", "Score collections must be arrays");
        return;
    }
    if (tracks->array().size() > maximumScoreTracks || clips->array().size() > maximumScoreClips ||
        automation->array().size() > maximumScoreAutomationLanes ||
        markers->array().size() > maximumScoreMarkers)
    {
        addError(result, "$.timeline.score", "Score collection exceeds its fixed limit");
        return;
    }

    ProjectScore parsedScore;
    parsedScore.tracks.reserve(tracks->array().size());
    for (std::size_t index = 0; index < tracks->array().size(); ++index)
    {
        const auto& item = tracks->array()[index];
        const auto path = "$.timeline.score.tracks[" + std::to_string(index) + "]";
        const auto* inputTrackId = item.isObject() ? objectMember(item, "inputTrackId") : nullptr;
        const auto* sendTrackIds = item.isObject() ? objectMember(item, "sendTrackIds") : nullptr;
        const auto* outputChannels = item.isObject() ? objectMember(item, "outputChannels") : nullptr;
        const auto* midiChannel = item.isObject() ? objectMember(item, "midiChannel") : nullptr;
        const auto* inputChannel = item.isObject() ? objectMember(item, "inputChannel") : nullptr;
        const auto* noOutput = item.isObject() ? objectMember(item, "noOutput") : nullptr;
        if (!item.isObject()
            || item.object().size()
                != (sourceVersion >= versionThirtySeven ? 10U : 9U) + (inputTrackId == nullptr ? 0U : 1U)
                    + (sendTrackIds == nullptr ? 0U : 1U) + (outputChannels == nullptr ? 0U : 1U)
                    + (midiChannel == nullptr ? 0U : 1U) + (inputChannel == nullptr ? 0U : 1U)
                    + (noOutput == nullptr ? 0U : 1U))
        {
            addError(result, path, "Expected closed score-track object");
            continue;
        }
        ScoreTrack track;
        std::string kindName;
        std::string timeDomainName;
        std::string overlapName;
        auto valid =
            requiredStringMember(result, item, "trackId", path + ".trackId", track.trackId);
        valid = requiredStringMember(
                    result, item, "displayName", path + ".displayName", track.displayName) &&
                valid;
        valid = requiredStringMember(result, item, "kind", path + ".kind", kindName) && valid;
        valid = requiredStringMember(
                    result, item, "timeDomain", path + ".timeDomain", timeDomainName) &&
                valid;
        valid = requiredStringMember(
                    result, item, "outputTrackId", path + ".outputTrackId", track.outputTrackId) &&
                valid;
        std::uint64_t channelCount = 0U;
        valid = requiredExactUnsignedIntegerMember(result,
                                                   item,
                                                   "channelCount",
                                                   path + ".channelCount",
                                                   maximumScoreChannels,
                                                   channelCount) &&
                valid;
        valid =
            requiredStringMember(result, item, "overlapMode", path + ".overlapMode", overlapName) &&
            valid;
        const auto* muted = objectMember(item, "muted");
        const auto* solo = objectMember(item, "solo");
        const auto* sharingMemoryWriters =
            objectMember(item, "sharingMemoryWriters");
        if (muted == nullptr || !muted->isBoolean())
        {
            addError(result, path + ".muted", "Expected boolean");
            valid = false;
        }
        if (solo == nullptr || !solo->isBoolean())
        {
            addError(result, path + ".solo", "Expected boolean");
            valid = false;
        }
        if (sourceVersion >= versionThirtySeven
            && (sharingMemoryWriters == nullptr
                || !sharingMemoryWriters->isBoolean()))
        {
            addError(result, path + ".sharingMemoryWriters", "Expected boolean");
            valid = false;
        }
        const auto kind = parseScoreTrackKind(kindName);
        const auto timeDomain = parseScoreTimeDomain(timeDomainName);
        const auto overlapMode = parseScoreOverlapMode(overlapName);
        if (!kind.has_value() || !timeDomain.has_value() || !overlapMode.has_value())
        {
            addError(result, path, "Expected known track kind, time domain, and overlap mode");
            valid = false;
        }
        if (inputTrackId != nullptr
            && (sourceVersion < versionOneHundredTwentySeven || !inputTrackId->isString()
                || inputTrackId->string().empty()))
        {
            addError(result, path + ".inputTrackId", "Expected a submix's track ID, from format 127");
            valid = false;
        }
        if (sendTrackIds != nullptr)
        {
            const auto sendsValid = sourceVersion >= versionOneHundredTwentyEight && sendTrackIds->isArray()
                && !sendTrackIds->array().empty() && sendTrackIds->array().size() <= maximumScoreTracks
                && std::ranges::all_of(sendTrackIds->array(), [](const auto& send) {
                       return send.isString() && !send.string().empty()
                           && send.string().size() <= maximumScoreIdentifierBytes;
                   });
            if (!sendsValid)
            {
                addError(result, path + ".sendTrackIds", "Expected the submixes' track IDs, from format 128");
                valid = false;
            }
        }
        if (outputChannels != nullptr)
        {
            const auto channelsValid = sourceVersion >= versionOneHundredThirtyOne && outputChannels->isArray()
                && !outputChannels->array().empty() && outputChannels->array().size() <= maximumScoreChannels
                && std::ranges::all_of(outputChannels->array(), [](const auto& channel) {
                       return channel.isNumber() && channel.number() >= 1.0 && channel.number() <= 8.0
                           && std::floor(channel.number()) == channel.number();
                   });
            if (!channelsValid)
            {
                addError(result, path + ".outputChannels", "Expected output channels from 1 to 8, from format 131");
                valid = false;
            }
        }
        if (midiChannel != nullptr
            && (sourceVersion < versionOneHundredThirtyTwo || !midiChannel->isNumber() || midiChannel->number() < 1.0
                || midiChannel->number() > 16.0 || std::floor(midiChannel->number()) != midiChannel->number()))
        {
            addError(result, path + ".midiChannel", "Expected a MIDI channel from 1 to 16, from format 132");
            valid = false;
        }
        if (inputChannel != nullptr
            && (sourceVersion < versionOneHundredThirtyThree || !inputChannel->isNumber() || inputChannel->number() < 1.0
                || inputChannel->number() > static_cast<double>(maximumScoreInputChannels)
                || std::floor(inputChannel->number()) != inputChannel->number()))
        {
            addError(result, path + ".inputChannel", "Expected an audio input channel from 1 to 8, from format 133");
            valid = false;
        }
        if (noOutput != nullptr
            && (sourceVersion < versionOneHundredThirtyFour || !noOutput->isBoolean() || !noOutput->boolean()))
        {
            addError(result, path + ".noOutput", "Expected true for a track with no output, from format 134");
            valid = false;
        }
        if (valid)
        {
            if (outputChannels != nullptr)
            {
                for (const auto& channel : outputChannels->array())
                    track.outputChannels.push_back(static_cast<std::uint32_t>(channel.number()));
            }
            if (midiChannel != nullptr)
                track.midiChannel = static_cast<std::uint8_t>(midiChannel->number());
            if (inputChannel != nullptr)
                track.inputChannel = static_cast<std::uint32_t>(inputChannel->number());
            track.noOutput = noOutput != nullptr;
            if (inputTrackId != nullptr)
                track.inputTrackId = inputTrackId->string();
            if (sendTrackIds != nullptr)
            {
                for (const auto& send : sendTrackIds->array())
                    track.sendTrackIds.push_back(send.string());
            }
            track.kind = *kind;
            track.timeDomain = *timeDomain;
            track.channelCount = static_cast<std::uint32_t>(channelCount);
            track.overlapMode = *overlapMode;
            track.muted = muted->boolean();
            track.solo = solo->boolean();
            track.sharingMemoryWriters = sourceVersion >= versionThirtySeven
                && sharingMemoryWriters->boolean();
            parsedScore.tracks.push_back(std::move(track));
        }
    }

    parsedScore.clips.reserve(clips->array().size());
    for (std::size_t index = 0; index < clips->array().size(); ++index)
    {
        const auto& item = clips->array()[index];
        const auto path = "$.timeline.score.clips[" + std::to_string(index) + "]";
        const auto* anchorOffset = item.isObject() ? objectMember(item, "anchorOffset") : nullptr;
        const auto* colour = item.isObject() ? objectMember(item, "colour") : nullptr;
        const auto expectedClipMembers = (sourceVersion >= versionThirtyThree
            ? 11U
            : 9U) + (anchorOffset == nullptr ? 0U : 1U) + (colour == nullptr ? 0U : 1U);
        if (!item.isObject()
            || item.object().size() != expectedClipMembers)
        {
            addError(
                result,
                path,
                sourceVersion >= versionThirtyThree
                    ? "Expected closed score-clip object with sourceInstanceId and renderArtifact"
                    : "Expected closed score-clip object");
            continue;
        }
        ScoreClip clip;
        auto valid = requiredStringMember(result, item, "clipId", path + ".clipId", clip.clipId);
        valid =
            requiredStringMember(result, item, "trackId", path + ".trackId", clip.trackId) && valid;
        valid = requiredStringMember(result,
                                     item,
                                     "soundDefinitionId",
                                     path + ".soundDefinitionId",
                                     clip.soundDefinitionId) &&
                valid;
        if (sourceVersion >= versionThirtyThree)
        {
            const auto* sourceInstanceId = objectMember(
                item,
                "sourceInstanceId");
            if (sourceInstanceId == nullptr
                || (!sourceInstanceId->isNull()
                    && !sourceInstanceId->isString()))
            {
                addError(
                    result,
                    path + ".sourceInstanceId",
                    "Expected null or exact source instance ID");
                valid = false;
            }
            else if (sourceInstanceId->isString())
            {
                clip.sourceInstanceId = sourceInstanceId->string();
            }
        }
        for (const auto [name, target] : {std::pair{"start", &clip.start},
                                          std::pair{"duration", &clip.duration},
                                          std::pair{"contentOffset", &clip.contentOffset},
                                          std::pair{"fadeIn", &clip.fadeIn},
                                          std::pair{"fadeOut", &clip.fadeOut}})
        {
            const auto* time = objectMember(item, name);
            if (time == nullptr)
            {
                addError(result, path + "." + name, "Expected score time");
                valid = false;
            }
            else if (!parseScoreTime(result, *time, path + "." + name, *target))
                valid = false;
        }
        if (anchorOffset != nullptr)
        {
            ScoreTime offset;
            if (sourceVersion < versionOneHundredTwentyFour)
            {
                addError(result, path + ".anchorOffset", "A clip's anchor requires format 124");
                valid = false;
            }
            else if (parseScoreTime(result, *anchorOffset, path + ".anchorOffset", offset))
                clip.anchorOffset = offset;
            else
                valid = false;
        }
        if (colour != nullptr)
        {
            std::uint64_t rgb = 0;
            if (sourceVersion < versionOneHundredFortySix)
            {
                addError(result, path + ".colour", "A clip's colour requires format 146");
                valid = false;
            }
            else if (requiredExactUnsignedIntegerMember(result, item, "colour", path + ".colour", 0xFFFFFFU, rgb))
                clip.colour = static_cast<std::uint32_t>(rgb);
            else
                valid = false;
        }
        std::uint64_t repeatCount = 0U;
        valid = requiredExactUnsignedIntegerMember(result,
                                                   item,
                                                   "repeatCount",
                                                   path + ".repeatCount",
                                                   maximumScoreClipRepeats,
                                                   repeatCount) &&
                valid;
        if (sourceVersion >= versionThirtyThree)
        {
            const auto* renderArtifact = objectMember(
                item,
                "renderArtifact");
            if (renderArtifact == nullptr)
            {
                addError(
                    result,
                    path + ".renderArtifact",
                    "Expected null or closed score clip render artifact");
                valid = false;
            }
            else if (!renderArtifact->isNull())
            {
                if (!renderArtifact->isObject()
                    || renderArtifact->object().size() != 5)
                {
                    addError(
                        result,
                        path + ".renderArtifact",
                        "Expected mode, sourceFingerprint, sampleRate, frameCount, and channels only");
                    valid = false;
                }
                else
                {
                    ScoreClipRenderArtifact artifact;
                    std::string modeName;
                    auto artifactValid = requiredStringMember(
                        result,
                        *renderArtifact,
                        "mode",
                        path + ".renderArtifact.mode",
                        modeName);
                    artifactValid = requiredStringMember(
                        result,
                        *renderArtifact,
                        "sourceFingerprint",
                        path + ".renderArtifact.sourceFingerprint",
                        artifact.sourceFingerprint)
                        && artifactValid;
                    std::uint64_t sampleRate = 0;
                    artifactValid = requiredExactUnsignedIntegerMember(
                        result,
                        *renderArtifact,
                        "sampleRate",
                        path + ".renderArtifact.sampleRate",
                        std::numeric_limits<std::uint32_t>::max(),
                        sampleRate)
                        && artifactValid;
                    artifactValid = requiredExactUnsignedIntegerMember(
                        result,
                        *renderArtifact,
                        "frameCount",
                        path + ".renderArtifact.frameCount",
                        maximumProjectAssetJsonInteger,
                        artifact.frameCount)
                        && artifactValid;
                    const auto* channels = objectMember(
                        *renderArtifact,
                        "channels");
                    if (channels == nullptr
                        || !channels->isArray()
                        || channels->array().empty()
                        || channels->array().size()
                            > maximumScoreChannels)
                    {
                        addError(
                            result,
                            path + ".renderArtifact.channels",
                            "Expected one to eight channel asset IDs");
                        artifactValid = false;
                    }
                    else
                    {
                        artifact.channelAssetIds.reserve(
                            channels->array().size());
                        for (std::size_t channel = 0;
                             channel < channels->array().size();
                             ++channel)
                        {
                            const auto& channelValue =
                                channels->array()[channel];
                            const auto channelPath = path
                                + ".renderArtifact.channels["
                                + std::to_string(channel) + "]";
                            if (!channelValue.isObject()
                                || channelValue.object().size() != 1)
                            {
                                addError(
                                    result,
                                    channelPath,
                                    "Expected closed channel asset object");
                                artifactValid = false;
                                continue;
                            }
                            std::string assetId;
                            if (!requiredStringMember(
                                    result,
                                    channelValue,
                                    "assetId",
                                    channelPath + ".assetId",
                                    assetId)
                                || assetId.empty()
                                || assetId.size()
                                    > maximumScoreClipRenderAssetIdBytes)
                            {
                                addError(
                                    result,
                                    channelPath + ".assetId",
                                    "Expected bounded UTF-8 asset ID");
                                artifactValid = false;
                                continue;
                            }
                            artifact.channelAssetIds.push_back(std::move(assetId));
                        }
                    }
                    const auto mode = parseScoreClipRenderMode(modeName);
                    if (!mode.has_value())
                    {
                        addError(
                            result,
                            path + ".renderArtifact.mode",
                            "Expected cached or frozen");
                        artifactValid = false;
                    }
                    if (artifactValid)
                    {
                        artifact.mode = *mode;
                        artifact.sampleRate = static_cast<std::uint32_t>(
                            sampleRate);
                        clip.renderArtifact = std::move(artifact);
                    }
                    valid = artifactValid && valid;
                }
            }
        }
        if (valid)
        {
            clip.repeatCount = static_cast<std::uint32_t>(repeatCount);
            parsedScore.clips.push_back(std::move(clip));
        }
    }

    parsedScore.automation.reserve(automation->array().size());
    std::size_t pointCount = 0U;
    for (std::size_t index = 0; index < automation->array().size(); ++index)
    {
        const auto& item = automation->array()[index];
        const auto path = "$.timeline.score.automation[" + std::to_string(index) + "]";
        const auto* stateMember = item.isObject() ? objectMember(item, "state") : nullptr;
        const auto* takeNameMember = item.isObject() ? objectMember(item, "takeName") : nullptr;
        const auto* takesMember = item.isObject() ? objectMember(item, "takes") : nullptr;
        const auto* masterMember = item.isObject() ? objectMember(item, "masterLaneId") : nullptr;
        const auto* transformsMember = item.isObject() ? objectMember(item, "transforms") : nullptr;
        if (!item.isObject()
            || item.object().size() != 6U + (stateMember == nullptr ? 0U : 1U) + (takeNameMember == nullptr ? 0U : 1U)
                    + (takesMember == nullptr ? 0U : 1U) + (masterMember == nullptr ? 0U : 1U)
                    + (transformsMember == nullptr ? 0U : 1U))
        {
            addError(result, path, "Expected closed score-automation object");
            continue;
        }
        ScoreAutomationLane lane;
        if (stateMember != nullptr)
        {
            const auto state = stateMember->isString()
                ? parseScoreAutomationState(stateMember->string())
                : std::nullopt;
            if (sourceVersion < versionOneHundredTwentyThree)
                addError(result, path + ".state", "A lane's state requires format 123");
            else if (!state.has_value() || *state == ScoreAutomationState::Automated)
                addError(result, path + ".state", "Expected live or record");
            else
                lane.state = *state;
        }
        std::string scopeName;
        std::string domainName;
        auto valid = requiredStringMember(result, item, "laneId", path + ".laneId", lane.laneId);
        valid = requiredStringMember(result, item, "scope", path + ".scope", scopeName) && valid;
        valid =
            requiredStringMember(result, item, "ownerId", path + ".ownerId", lane.ownerId) && valid;
        valid =
            requiredStringMember(result, item, "eventName", path + ".eventName", lane.eventName) &&
            valid;
        valid = requiredStringMember(result, item, "domain", path + ".domain", domainName) && valid;
        const auto scope = parseScoreAutomationScope(scopeName);
        const auto domain = parseValueDomain(domainName);
        if (!scope.has_value() || !domain.has_value())
        {
            addError(result, path, "Expected known automation scope and value domain");
            valid = false;
        }
        const auto parsePoints = [&](const json::Value* points, const std::string& pointsPath,
                                     std::vector<ScoreAutomationPoint>& parsed) {
        if (points == nullptr || !points->isArray() ||
            points->array().size() > maximumScoreAutomationPoints - pointCount)
        {
            addError(result, pointsPath, "Expected bounded automation point array");
            return false;
        }
        auto valid = true;
        {
            pointCount += points->array().size();
            parsed.reserve(points->array().size());
            for (std::size_t pointIndex = 0; pointIndex < points->array().size(); ++pointIndex)
            {
                const auto& pointValue = points->array()[pointIndex];
                const auto pointPath = pointsPath + "[" + std::to_string(pointIndex) + "]";
                if (!pointValue.isObject() || pointValue.object().size() != 3)
                {
                    addError(result, pointPath, "Expected closed automation-point object");
                    valid = false;
                    continue;
                }
                ScoreAutomationPoint point;
                const auto* position = objectMember(pointValue, "position");
                auto pointValid = position != nullptr;
                if (position == nullptr)
                    addError(result, pointPath + ".position", "Expected score time");
                else
                    pointValid =
                        parseScoreTime(result, *position, pointPath + ".position", point.position);
                pointValid = requiredFiniteNumberMember(
                                 result, pointValue, "value", pointPath + ".value", point.value) &&
                             pointValid;
                std::string interpolationName;
                pointValid = requiredStringMember(result,
                                                  pointValue,
                                                  "interpolation",
                                                  pointPath + ".interpolation",
                                                  interpolationName) &&
                             pointValid;
                const auto interpolation = parseAutomationInterpolation(interpolationName);
                if (!interpolation.has_value())
                {
                    addError(result, pointPath + ".interpolation", "Expected step or linear");
                    pointValid = false;
                }
                if (pointValid)
                {
                    point.interpolation = *interpolation;
                    parsed.push_back(std::move(point));
                }
                else
                {
                    valid = false;
                }
            }
        }
        return valid;
        };
        valid = parsePoints(objectMember(item, "points"), path + ".points", lane.points) && valid;
        // Format 129: the lane's takes.
        if ((takeNameMember != nullptr || takesMember != nullptr) && sourceVersion < versionOneHundredTwentyNine)
        {
            addError(result, path, "A lane's takes require format 129");
            valid = false;
        }
        if (masterMember != nullptr)
        {
            if (sourceVersion < versionOneHundredThirty || !masterMember->isString() || masterMember->string().empty()
                || masterMember->string().size() > maximumScoreIdentifierBytes)
            {
                addError(result, path + ".masterLaneId", "Expected the master lane's ID, from format 130");
                valid = false;
            }
            else
                lane.masterLaneId = masterMember->string();
        }
        if (transformsMember != nullptr)
        {
            auto transformsValid = sourceVersion >= versionOneHundredThirtySix && transformsMember->isArray()
                && !transformsMember->array().empty()
                && transformsMember->array().size() <= maximumScoreAutomationTransforms;
            for (std::size_t transformIndex = 0; transformsValid && transformIndex < transformsMember->array().size();
                 ++transformIndex)
            {
                const auto& entry = transformsMember->array()[transformIndex];
                const auto* kind = entry.isObject() ? objectMember(entry, "kind") : nullptr;
                const auto* value = entry.isObject() ? objectMember(entry, "value") : nullptr;
                const auto* secondary = entry.isObject() ? objectMember(entry, "secondary") : nullptr;
                const auto* seed = entry.isObject() ? objectMember(entry, "seed") : nullptr;
                const auto parsedKind = kind != nullptr && kind->isString()
                    ? parseScoreAutomationTransformName(kind->string())
                    : std::nullopt;
                transformsValid = entry.isObject() && entry.object().size() == 4U && parsedKind.has_value()
                    && value != nullptr && value->isNumber() && std::isfinite(value->number())
                    && secondary != nullptr && secondary->isNumber() && std::isfinite(secondary->number())
                    && seed != nullptr && seed->isNumber() && seed->number() >= 0.0
                    && seed->number() <= 9007199254740992.0 && std::floor(seed->number()) == seed->number();
                if (transformsValid)
                    lane.transforms.push_back({*parsedKind, value->number(), secondary->number(),
                        static_cast<std::uint64_t>(seed->number())});
            }
            if (!transformsValid)
            {
                addError(result, path + ".transforms", "Expected the lane's transformations, from format 136");
                valid = false;
            }
        }
        if (takeNameMember != nullptr)
        {
            if (!takeNameMember->isString() || takeNameMember->string().empty())
            {
                addError(result, path + ".takeName", "Expected a take's name");
                valid = false;
            }
            else
                lane.takeName = takeNameMember->string();
        }
        if (takesMember != nullptr)
        {
            if (!takesMember->isArray() || takesMember->array().empty()
                || takesMember->array().size() > maximumScoreAutomationTakes)
            {
                addError(result, path + ".takes", "Expected a bounded array of takes");
                valid = false;
            }
            else
            {
                for (std::size_t takeIndex = 0; takeIndex < takesMember->array().size(); ++takeIndex)
                {
                    const auto& takeValue = takesMember->array()[takeIndex];
                    const auto takePath = path + ".takes[" + std::to_string(takeIndex) + "]";
                    const auto* name = takeValue.isObject() ? objectMember(takeValue, "name") : nullptr;
                    ScoreAutomationTake take;
                    if (!takeValue.isObject() || takeValue.object().size() != 2U || name == nullptr || !name->isString())
                    {
                        addError(result, takePath, "Expected a take's name and points");
                        valid = false;
                        continue;
                    }
                    take.name = name->string();
                    valid = parsePoints(objectMember(takeValue, "points"), takePath + ".points", take.points) && valid;
                    lane.takes.push_back(std::move(take));
                }
            }
        }
        if (valid)
        {
            lane.scope = *scope;
            lane.domain = *domain;
            parsedScore.automation.push_back(std::move(lane));
        }
    }

    parsedScore.markers.reserve(markers->array().size());
    for (std::size_t index = 0; index < markers->array().size(); ++index)
    {
        const auto& item = markers->array()[index];
        const auto path = "$.timeline.score.markers[" + std::to_string(index) + "]";
        const auto* programNumber = item.isObject() ? objectMember(item, "programNumber") : nullptr;
        if (!item.isObject() || item.object().size() != (programNumber == nullptr ? 4U : 5U))
        {
            addError(result, path, "Expected closed score-marker object");
            continue;
        }
        ScoreMarker marker;
        auto valid =
            requiredStringMember(result, item, "markerId", path + ".markerId", marker.markerId);
        if (programNumber != nullptr)
        {
            std::uint64_t number = 0;
            if (sourceVersion < versionOneHundredTwentyFour)
            {
                addError(result, path + ".programNumber", "A marker's program change requires format 124");
                valid = false;
            }
            else if (requiredExactUnsignedIntegerMember(
                         result, item, "programNumber", path + ".programNumber", 128, number)
                && number >= 1)
                marker.programNumber = static_cast<std::uint8_t>(number);
            else
            {
                addError(result, path + ".programNumber", "Expected a program change from 1 to 128");
                valid = false;
            }
        }
        valid = requiredStringMember(result, item, "title", path + ".title", marker.title) && valid;
        const auto* start = objectMember(item, "start");
        if (start == nullptr)
        {
            addError(result, path + ".start", "Expected score time");
            valid = false;
        }
        else
        {
            valid = parseScoreTime(result, *start, path + ".start", marker.start) && valid;
        }
        const auto* end = objectMember(item, "end");
        if (end == nullptr)
        {
            addError(result, path + ".end", "Expected null or score time");
            valid = false;
        }
        else if (!end->isNull())
        {
            ScoreTime parsedEnd;
            if (parseScoreTime(result, *end, path + ".end", parsedEnd))
                marker.end = parsedEnd;
            else
                valid = false;
        }
        if (valid)
            parsedScore.markers.push_back(std::move(marker));
    }

    result.document.timeline.score = std::move(parsedScore);
}

void parseTimeline(ProjectLoadResult& result, const json::Value& root, int sourceVersion)
{
    const auto* timeline = objectMember(root, "timeline");
    if (timeline == nullptr)
    {
        if (sourceVersion >= versionSix)
            addError(result, "$.timeline", "Expected required object");
        return;
    }
    if (!timeline->isObject())
    {
        addError(result, "$.timeline", "Expected object");
        return;
    }
    const auto* markerProgramChannel = objectMember(*timeline, "markerProgramChannel");
    const auto* timeCodeOffset = objectMember(*timeline, "timeCodeOffsetSeconds");
    const auto* framesPerSecond = objectMember(*timeline, "timeCodeFramesPerSecond");
    const auto* cacheWordSize = objectMember(*timeline, "cacheWordSize");
    const auto* sharingAcrossTracks = objectMember(*timeline, "sharingMemoryWritersAcrossTracks");
    const auto expectedTimelineFields = (sourceVersion >= versionTwentyOne ? 4U : 3U)
        + (markerProgramChannel == nullptr ? 0U : 1U) + (timeCodeOffset == nullptr ? 0U : 1U)
        + (framesPerSecond == nullptr ? 0U : 1U) + (cacheWordSize == nullptr ? 0U : 1U)
        + (sharingAcrossTracks == nullptr ? 0U : 1U);
    // Format 156: recordings shared across the Timeline's tracks, written
    // only when on.
    if (sharingAcrossTracks != nullptr)
    {
        if (sourceVersion < versionOneHundredFiftySix)
            addError(result, "$.timeline.sharingMemoryWritersAcrossTracks",
                "Recordings shared across the Timeline's tracks require format 156");
        else if (!sharingAcrossTracks->isBoolean() || !sharingAcrossTracks->boolean())
            addError(result, "$.timeline.sharingMemoryWritersAcrossTracks", "Expected true, written only when on");
        else
            result.document.timeline.sharingMemoryWritersAcrossTracks = true;
    }
    if (cacheWordSize != nullptr)
    {
        std::uint64_t bits = 0;
        if (sourceVersion < versionOneHundredFiftyFour)
            addError(result, "$.timeline.cacheWordSize", "The caches' word size requires format 154");
        else if (requiredExactUnsignedIntegerMember(
                     result, *timeline, "cacheWordSize", "$.timeline.cacheWordSize", 24, bits)
            && (bits == 16 || bits == 24))
            result.document.timeline.cacheWordSize = static_cast<std::uint8_t>(bits);
        else
            addError(result, "$.timeline.cacheWordSize", "Expected caches of 16 or 24 bits");
    }
    if (framesPerSecond != nullptr)
    {
        std::uint64_t frames = 0;
        if (sourceVersion < versionOneHundredFortySix)
            addError(result, "$.timeline.timeCodeFramesPerSecond", "The Timeline's own frames a second require format 146");
        else if (requiredExactUnsignedIntegerMember(
                     result, *timeline, "timeCodeFramesPerSecond", "$.timeline.timeCodeFramesPerSecond", 30, frames)
            && (frames == 24 || frames == 25 || frames == 30))
            result.document.timeline.timeCodeFramesPerSecond = static_cast<std::uint8_t>(frames);
        else
            addError(result, "$.timeline.timeCodeFramesPerSecond", "Expected 24, 25 or 30 time code frames a second");
    }
    if (timeCodeOffset != nullptr)
    {
        if (sourceVersion < versionOneHundredThirtyFive || !timeCodeOffset->isNumber()
            || !std::isfinite(timeCodeOffset->number()) || timeCodeOffset->number() <= 0.0
            || timeCodeOffset->number() >= 86400.0)
            addError(result, "$.timeline.timeCodeOffsetSeconds",
                "Expected a time code offset of up to a day in seconds, from format 135");
        else
            result.document.timeline.timeCodeOffsetSeconds = timeCodeOffset->number();
    }
    if (markerProgramChannel != nullptr)
    {
        std::uint64_t channel = 0;
        if (sourceVersion < versionOneHundredTwentyFour)
            addError(result, "$.timeline.markerProgramChannel", "The Timeline's program change channel requires format 124");
        else if (requiredExactUnsignedIntegerMember(
                     result, *timeline, "markerProgramChannel", "$.timeline.markerProgramChannel", 16, channel)
            && channel >= 1)
            result.document.timeline.markerProgramChannel = static_cast<std::uint8_t>(channel);
        else
            addError(result, "$.timeline.markerProgramChannel", "Expected a MIDI channel from 1 to 16");
    }
    if (sourceVersion >= versionSixteen
        && timeline->object().size() != expectedTimelineFields)
    {
        addError(
            result,
            "$.timeline",
            sourceVersion >= versionTwentyOne
                ? "Expected loop, musicalTime, score, and eventValues only"
                : "Expected loop, musicalTime, and eventValues only");
    }

    parseMusicalTime(result, *timeline, sourceVersion);
    parseScore(result, *timeline, sourceVersion);

    if (sourceVersion >= versionSix)
    {
        const auto* loop = objectMember(*timeline, "loop");
        if (loop == nullptr || !loop->isObject())
        {
            addError(result, "$.timeline.loop", "Expected required object");
        }
        else
        {
            auto& region = result.document.timeline.loop;
            bool valid = true;
            const auto* enabled = objectMember(*loop, "enabled");
            if (enabled == nullptr || !enabled->isBoolean())
            {
                addError(result, "$.timeline.loop.enabled", "Expected boolean");
                valid = false;
            }
            else
            {
                region.enabled = enabled->boolean();
            }
            valid = requiredFiniteNumberMember(
                        result,
                        *loop,
                        "startSeconds",
                        "$.timeline.loop.startSeconds",
                        region.startSeconds)
                && valid;
            valid = requiredFiniteNumberMember(
                        result,
                        *loop,
                        "endSeconds",
                        "$.timeline.loop.endSeconds",
                        region.endSeconds)
                && valid;
            if (valid
                && (region.startSeconds < 0.0
                    || region.endSeconds <= region.startSeconds))
            {
                addError(
                    result,
                    "$.timeline.loop",
                    "Loop requires non-negative bounds with end later than start");
            }
        }
    }

    const auto* events = objectMember(*timeline, "eventValues");
    if (events == nullptr || !events->isArray())
    {
        addError(result, "$.timeline.eventValues", "Expected array");
        return;
    }

    double previousTime = 0.0;
    bool hasPreviousTime = false;
    std::unordered_set<std::string> namesAtCurrentTime;
    for (std::size_t index = 0; index < events->array().size(); ++index)
    {
        const auto& source = events->array()[index];
        const auto path = "$.timeline.eventValues[" + std::to_string(index) + "]";
        if (!source.isObject())
        {
            addError(result, path, "Expected object");
            continue;
        }

        TimelineEventValue event;
        std::string domain;
        const auto hasTime = requiredFiniteNumberMember(
            result, source, "timeSeconds", path + ".timeSeconds", event.timeSeconds);
        const auto hasName = requiredStringMember(
            result, source, "eventName", path + ".eventName", event.eventName);
        const auto hasDomain = requiredStringMember(
            result, source, "domain", path + ".domain", domain);
        const auto hasValue = requiredFiniteNumberMember(
            result, source, "value", path + ".value", event.value);
        const auto parsedDomain = hasDomain ? parseValueDomain(domain) : std::nullopt;
        std::optional<talas::core::AutomationInterpolation> parsedInterpolation =
            talas::core::AutomationInterpolation::Step;
        if (sourceVersion >= versionSeven)
        {
            std::string interpolation;
            const auto hasInterpolation = requiredStringMember(
                result,
                source,
                "interpolation",
                path + ".interpolation",
                interpolation);
            parsedInterpolation = hasInterpolation
                ? parseAutomationInterpolation(interpolation)
                : std::nullopt;
            if (!parsedInterpolation.has_value())
            {
                addError(
                    result,
                    path + ".interpolation",
                    "Expected step or linear Timeline interpolation");
            }
        }

        bool valid = hasTime && hasName && hasDomain && hasValue
            && parsedInterpolation.has_value();
        if (hasTime && event.timeSeconds < 0.0)
        {
            addError(result, path + ".timeSeconds", "Timeline time must be non-negative");
            valid = false;
        }
        if (hasName && !isValidEventName(event.eventName))
        {
            addError(result, path + ".eventName", "Expected Event Value identifier");
            valid = false;
        }
        if (!parsedDomain.has_value())
        {
            addError(result, path + ".domain", "Expected known Event Value domain");
            valid = false;
        }
        if (hasTime && hasPreviousTime && event.timeSeconds < previousTime)
        {
            addError(result, path + ".timeSeconds", "Timeline events must be in chronological order");
            valid = false;
        }
        if (hasTime)
        {
            if (!hasPreviousTime || event.timeSeconds != previousTime)
                namesAtCurrentTime.clear();
            if (hasName && !namesAtCurrentTime.insert(event.eventName).second)
            {
                addError(result, path + ".eventName", "Duplicate Event Value at the same Timeline time");
                valid = false;
            }
            previousTime = event.timeSeconds;
            hasPreviousTime = true;
        }

        if (valid)
        {
            event.domain = *parsedDomain;
            event.interpolation = *parsedInterpolation;
            result.document.timeline.eventValues.push_back(std::move(event));
        }
    }
}

void parseGraph(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion,
    std::size_t& preservedComponentCount,
    std::size_t& preservedComponentStateBytes)
{
    const auto* graph = objectMember(root, "graph");
    if (graph == nullptr)
    {
        addError(result, "$.graph", "Missing required object");
        return;
    }
    auto* presentation = findProjectStructureLayout(result.document);
    if (presentation == nullptr)
    {
        result.document.presentation.structureLayouts.push_back({});
        presentation = &result.document.presentation.structureLayouts.back();
    }
    parseGraphValue(
        result,
        *graph,
        result.document.graph,
        *presentation,
        "$.graph",
        sourceVersion,
        preservedComponentCount,
        preservedComponentStateBytes);
}

void migrateVersionOne(ProjectDocument& document)
{
    for (auto& node : document.graph.nodes)
    {
        if (node.typeId == "talas.ramp")
            node.typeId = "talas.linearRamp";
    }
}

[[nodiscard]] const ProjectSoundDefinition* findSoundDefinition(
    const ProjectDocument& document,
    std::string_view definitionId)
{
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == definitionId;
        });
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

void migrateSoundParameterAliasesInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (!node.soundDefinitionId.has_value())
            continue;
        const auto* definition = findSoundDefinition(
            result.document,
            *node.soundDefinitionId);
        if (definition == nullptr)
            continue;

        std::unordered_set<std::string> canonicalIds;
        for (std::size_t parameterIndex = 0;
             parameterIndex < node.parameters.size();
             ++parameterIndex)
        {
            auto& value = node.parameters[parameterIndex];
            const auto exposed = std::ranges::find_if(
                definition->exposedParameters,
                [&](const auto& parameter) {
                    return parameter.parameterId
                            == value.parameterId
                        || std::ranges::find(
                               parameter.aliases,
                               value.parameterId)
                            != parameter.aliases.end();
                });
            if (exposed == definition->exposedParameters.end())
                continue;
            value.parameterId = exposed->parameterId;
            if (!canonicalIds.insert(value.parameterId).second)
            {
                addError(
                    result,
                    std::string(graphPath) + ".nodes["
                        + std::to_string(nodeIndex)
                        + "].parameters["
                        + std::to_string(parameterIndex) + "]",
                    "Ambiguous Sound override after alias migration");
            }
        }
    }
}

void migrateSoundParameterAliases(ProjectLoadResult& result)
{
    migrateSoundParameterAliasesInGraph(
        result,
        result.document.graph,
        "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateSoundParameterAliasesInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index)
                + "].graph");
    }
}

void rejectBindingsToNewerNodeParameter(
    ProjectLoadResult& result,
    std::string_view typeId,
    std::string_view parameterId,
    const std::string& message)
{
    for (std::size_t definitionIndex = 0;
         definitionIndex < result.document.soundDefinitions.size();
         ++definitionIndex)
    {
        const auto& definition =
            result.document.soundDefinitions[definitionIndex];
        for (std::size_t parameterIndex = 0;
             parameterIndex < definition.exposedParameters.size();
             ++parameterIndex)
        {
            const auto& parameter =
                definition.exposedParameters[parameterIndex];
            for (std::size_t bindingIndex = 0;
                 bindingIndex < parameter.bindings.size();
                 ++bindingIndex)
            {
                const auto& binding = parameter.bindings[bindingIndex];
                if (binding.target
                        != ProjectSoundParameterTarget::NodeParameter
                    || binding.parameterId != parameterId)
                {
                    continue;
                }
                const auto target = std::ranges::find(
                    definition.graph.nodes,
                    binding.nodeId,
                    &ProjectNode::instanceId);
                if (target == definition.graph.nodes.end()
                    || target->typeId != typeId)
                {
                    continue;
                }
                addError(
                    result,
                    "$.soundDefinitions[" + std::to_string(definitionIndex)
                        + "].parameters[" + std::to_string(parameterIndex)
                        + "].bindings[" + std::to_string(bindingIndex) + "]",
                    message);
            }
        }
    }
}

void migrateLegacyMidiMapperVoiceModelInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size(); ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.midiMapper")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "voiceModel";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "MIDIMapper voiceModel requires project format 38");
            continue;
        }
        ProjectParameterExpression model;
        model.parameterId = "voiceModel";
        model.choice = "legacyPassthrough";
        node.parameters.push_back(std::move(model));
    }
}

void migrateLegacyMidiMapperVoiceModels(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.midiMapper", "voiceModel",
        "MIDIMapper voiceModel requires project format 38");
    migrateLegacyMidiMapperVoiceModelInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size(); ++index)
    {
        migrateLegacyMidiMapperVoiceModelInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

void migrateLegacyAnalogSequencerIndicesInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size(); ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.analogSequencer")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "indexOrigin";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "AnalogSequencer indexOrigin requires project format 39");
            continue;
        }
        ProjectParameterExpression origin;
        origin.parameterId = "indexOrigin";
        origin.choice = "legacyOneBased";
        node.parameters.push_back(std::move(origin));
    }
}

void migrateLegacyAnalogSequencerIndices(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.analogSequencer", "indexOrigin",
        "AnalogSequencer indexOrigin requires project format 39");
    migrateLegacyAnalogSequencerIndicesInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size(); ++index)
    {
        migrateLegacyAnalogSequencerIndicesInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index)
                + "].graph");
    }
}

void migrateLegacyAnalogSequencerStepZeroModesInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.analogSequencer")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "stepZeroMode";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "AnalogSequencer stepZeroMode requires project format 41");
            continue;
        }
        ProjectParameterExpression mode;
        mode.parameterId = "stepZeroMode";
        mode.choice = "legacyAutomatic";
        node.parameters.push_back(std::move(mode));
    }
}

void migrateLegacyAnalogSequencerStepZeroModes(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.analogSequencer", "stepZeroMode",
        "AnalogSequencer stepZeroMode requires project format 41");
    migrateLegacyAnalogSequencerStepZeroModesInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyAnalogSequencerStepZeroModesInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

void rejectNodeTypeInGraph(
    ProjectLoadResult& result,
    const ProjectGraph& graph,
    std::string_view graphPath,
    std::string_view typeId,
    const std::string& message)
{
    for (std::size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex)
    {
        if (graph.nodes[nodeIndex].typeId == typeId)
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].typeId",
                message);
        }
    }
}

void migrateLegacyMixers(ProjectLoadResult& result)
{
    const auto message = std::string{"Mixer requires project format 43"};
    rejectNodeTypeInGraph(
        result, result.document.graph, "$.graph", "talas.mixerV2", message);
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        rejectNodeTypeInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph",
            "talas.mixerV2",
            message);
    }
    if (result.ok())
        detail::migrateLegacyMixers(result.document);
}

void migrateLegacyTunableVocoderSpacingInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.tunableVocoder")
            continue;
        const auto authored = [&](std::string_view parameterId) {
            return std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                return parameter.parameterId == parameterId;
            });
        };
        if (authored("spacingUnits"))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "TunableVocoder spacingUnits requires project format 44");
            continue;
        }
        // The spacing default was 1 before it took hz or nn.
        for (const std::string_view parameterId :
            {"analysisSpacing", "synthesisSpacing"})
        {
            if (authored(parameterId))
                continue;
            ProjectParameterExpression spacing;
            spacing.parameterId = std::string{parameterId};
            spacing.expression = "1";
            node.parameters.push_back(std::move(spacing));
        }
        ProjectParameterExpression units;
        units.parameterId = "spacingUnits";
        units.choice = "legacyRelative";
        node.parameters.push_back(std::move(units));
    }
}

// Before format 44 a TunableVocoder's spacing was a multiple of its base
// frequency, or octaves; it is now given in hz, or nn.
void migrateLegacyTunableVocoderSpacing(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.tunableVocoder", "spacingUnits",
        "TunableVocoder spacingUnits requires project format 44");
    migrateLegacyTunableVocoderSpacingInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyTunableVocoderSpacingInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 45 a TimeControl replayed its Input at Rate and a
// TimeStopper held its Input's sample; both now move only the Input's
// events.
void migrateLegacyTimePacingInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        const auto isTimeControl = node.typeId == "talas.timeControl";
        if (!isTimeControl && node.typeId != "talas.timeStopper")
            continue;
        const auto parameterId = isTimeControl ? "rateChanges" : "whileStopped";
        if (std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                return parameter.parameterId == parameterId;
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                std::string(isTimeControl ? "TimeControl" : "TimeStopper") + " "
                    + parameterId + " requires project format 45");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = parameterId;
        legacy.choice = isTimeControl ? "legacyPlaybackSpeed" : "legacyHold";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyTimePacing(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.timeControl", "rateChanges",
        "TimeControl rateChanges requires project format 45");
    rejectBindingsToNewerNodeParameter(
        result, "talas.timeStopper", "whileStopped",
        "TimeStopper whileStopped requires project format 45");
    migrateLegacyTimePacingInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyTimePacingInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 46 an OverlappingMixer mixed its Inputs at once; it now
// plays them one after another.
void migrateLegacyOverlappingMixersInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.overlappingMixer")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "sequence";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "OverlappingMixer sequence requires project format 46");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "sequence";
        legacy.choice = "legacyAllAtOnce";
        node.parameters.push_back(std::move(legacy));
    }
}

// Before format 120 a MIDIVoice without a Polyphony played four voices;
// the prototype plays two. One without keeps its four.
void migrateLegacyMidiVoicePolyphonyInGraph(ProjectGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != "talas.midiVoice"
            || std::ranges::any_of(node.parameters, [](const auto& parameter) {
                   return parameter.parameterId == "polyphony";
               }))
        {
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "polyphony";
        legacy.expression = "4";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyMidiVoicePolyphony(ProjectLoadResult& result)
{
    migrateLegacyMidiVoicePolyphonyInGraph(result.document.graph);
    for (auto& definition : result.document.soundDefinitions)
        migrateLegacyMidiVoicePolyphonyInGraph(definition.graph);
}

// Whether a project holds Smalltalk: a Script or a file interpreter, a
// ParameterTransformer, a MIDIVoice's or MIDIMapper's script, or a field
// written as a Smalltalk program.
[[nodiscard]] bool holdsSmalltalkPrograms(const ProjectDocument& document)
{
    const auto inGraph = [](const ProjectGraph& graph) {
        return std::ranges::any_of(graph.nodes, [](const ProjectNode& node) {
            if (node.typeId == "talas.script" || node.typeId == "talas.textFileInterpreter"
                || node.typeId == "talas.midiFileInterpreter" || node.typeId == "talas.parameterTransformer")
                return true;
            return std::ranges::any_of(node.parameters, [&](const ProjectParameterExpression& parameter) {
                if (parameter.parameterId == "script" && parameter.text.has_value() && !parameter.text->empty())
                    return true;
                return (parameter.expression.has_value() && smalltalk::fieldProgramText(*parameter.expression).has_value())
                    || (parameter.text.has_value()
                        && (smalltalk::listFieldNumbers(*parameter.text).has_value()
                            || smalltalk::fieldProgramText(*parameter.text).has_value()));
            });
        });
    };
    return inGraph(document.graph)
        || std::ranges::any_of(document.soundDefinitions, [&](const ProjectSoundDefinition& definition) {
               return inGraph(definition.graph);
           });
}

void migrateLegacyOverlappingMixers(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.overlappingMixer", "sequence",
        "OverlappingMixer sequence requires project format 46");
    migrateLegacyOverlappingMixersInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyOverlappingMixersInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 47 a FrequencyTracker gave its estimate in hz and a
// FrequencyScale read its FreqTracker in hz; both are now a fraction of
// half the sample rate.
void migrateLegacyTrackerUnitsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        const auto isTracker = node.typeId == "talas.frequencyTracker";
        if (!isTracker && node.typeId != "talas.frequencyScale")
            continue;
        const auto parameterId = isTracker ? "outputUnits" : "trackerUnits";
        if (std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                return parameter.parameterId == parameterId;
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                std::string(isTracker ? "FrequencyTracker" : "FrequencyScale")
                    + " " + parameterId + " requires project format 47");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = parameterId;
        legacy.choice = "legacyHertz";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyTrackerUnits(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.frequencyTracker", "outputUnits",
        "FrequencyTracker outputUnits requires project format 47");
    rejectBindingsToNewerNodeParameter(
        result, "talas.frequencyScale", "trackerUnits",
        "FrequencyScale trackerUnits requires project format 47");
    migrateLegacyTrackerUnitsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyTrackerUnitsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 48 an ArcTan took the arctangent of its one input, in
// radians; it is now of its Input's right channel over its left, as a
// fraction of a half turn.
void migrateLegacyArcTansInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.arcTan")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "angle";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "ArcTan angle requires project format 48");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "angle";
        legacy.choice = "legacyRadians";
        node.parameters.push_back(std::move(legacy));
    }
    for (std::size_t connectionIndex = 0;
         connectionIndex < graph.connections.size();
         ++connectionIndex)
    {
        const auto& connection = graph.connections[connectionIndex];
        if (connection.toPort != "rightValue")
            continue;
        const auto target = std::ranges::find(
            graph.nodes, connection.toNode, &ProjectNode::instanceId);
        if (target != graph.nodes.end() && target->typeId == "talas.arcTan")
        {
            addError(result,
                std::string(graphPath) + ".connections["
                    + std::to_string(connectionIndex) + "]",
                "ArcTan rightValue requires project format 48");
        }
    }
}

void migrateLegacyArcTans(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.arcTan", "angle",
        "ArcTan angle requires project format 48");
    migrateLegacyArcTansInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyArcTansInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 49 a FrequencyTracker's Scale multiplied its estimate;
// it now attenuates its Input.
void migrateLegacyTrackerScalesInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.frequencyTracker")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "scaleAttenuates";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "FrequencyTracker scaleAttenuates requires project format 49");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "scaleAttenuates";
        legacy.choice = "legacyEstimate";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyTrackerScales(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.frequencyTracker", "scaleAttenuates",
        "FrequencyTracker scaleAttenuates requires project format 49");
    migrateLegacyTrackerScalesInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyTrackerScalesInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 50 a GrainCloud's grain lasted GrainDur, or its cycles when
// GrainDur was 0; it now lasts both.
void migrateLegacyGrainDurationsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.grainCloud")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "grainDuration";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "GrainCloud grainDuration requires project format 50");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "grainDuration";
        legacy.choice = "legacyDurOrCycles";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyGrainDurations(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.grainCloud", "grainDuration",
        "GrainCloud grainDuration requires project format 50");
    migrateLegacyGrainDurationsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyGrainDurationsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 51 a TimeOffset with Reverse reversed its Input in windows
// of the offset; it now puts the silence after the Input when Reverse or
// Retrograde, but not both, is set.
void migrateLegacyTimeOffsetsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.timeOffset")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "reverseMeans";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "TimeOffset reverseMeans requires project format 51");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "reverseMeans";
        legacy.choice = "legacyReverseWindows";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyTimeOffsets(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.timeOffset", "reverseMeans",
        "TimeOffset reverseMeans requires project format 51");
    migrateLegacyTimeOffsetsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyTimeOffsetsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 52 a FrequencyScale moved the formants with the pitch;
// its grains now keep them where they are.
void migrateLegacyFrequencyScaleFormantsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.frequencyScale")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "formants";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "FrequencyScale formants requires project format 52");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "formants";
        legacy.choice = "legacyMove";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyFrequencyScaleFormants(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.frequencyScale", "formants",
        "FrequencyScale formants requires project format 52");
    migrateLegacyFrequencyScaleFormantsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyFrequencyScaleFormantsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 53 a SpectrumTrackSelector gave its track's frequency in
// hz; a Spectrum carries it as a fraction of half the sample rate.
void migrateLegacyTrackSelectorUnitsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.spectrumTrackSelector")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "freqUnits";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "SpectrumTrackSelector freqUnits requires project format 53");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "freqUnits";
        legacy.choice = "legacyHertz";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyTrackSelectorUnits(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.spectrumTrackSelector", "freqUnits",
        "SpectrumTrackSelector freqUnits requires project format 53");
    migrateLegacyTrackSelectorUnitsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyTrackSelectorUnitsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 54 an AnalysisFilter was a bandpass and an allpass; it now
// averages one period of the Fundamental, which isolates each harmonic.
void migrateLegacyAnalysisFiltersInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.analysisFilter")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "design";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "AnalysisFilter design requires project format 54");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "design";
        legacy.choice = "legacyBandpass";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyAnalysisFilters(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.analysisFilter", "design",
        "AnalysisFilter design requires project format 54");
    migrateLegacyAnalysisFiltersInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyAnalysisFiltersInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 55 a SpectralShape scaled each partial by one Wavetable
// sample; its Wavetable is now the shape of the spectrum from 0 hz to half
// the sample rate.
void migrateLegacySpectralShapesInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.spectralShape")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "envelope";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "SpectralShape envelope requires project format 55");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "envelope";
        legacy.choice = "legacyPartial";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacySpectralShapes(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.spectralShape", "envelope",
        "SpectralShape envelope requires project format 55");
    migrateLegacySpectralShapesInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacySpectralShapesInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 56 a DiskPlayer's Duration bounded each triggered playback;
// it is now the Sound's whole life, within which a Trigger restarts the
// file.
void migrateLegacyDiskPlayerDurationsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.diskPlayer")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "durationBounds";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "DiskPlayer durationBounds requires project format 56");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "durationBounds";
        legacy.choice = "legacyPlayback";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyDiskPlayerDurations(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.diskPlayer", "durationBounds",
        "DiskPlayer durationBounds requires project format 56");
    migrateLegacyDiskPlayerDurationsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyDiskPlayerDurationsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 57 a LiveSpectralAnalysis kept the strongest peaks between
// LowFreq and HighFreq; it now gives one track per band up to half the
// sample rate.
void migrateLegacyLiveSpectralTracksInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.liveSpectralAnalysis")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "tracks";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "LiveSpectralAnalysis tracks requires project format 57");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "tracks";
        legacy.choice = "legacyPeaks";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyLiveSpectralTracks(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.liveSpectralAnalysis", "tracks",
        "LiveSpectralAnalysis tracks requires project format 57");
    migrateLegacyLiveSpectralTracksInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyLiveSpectralTracksInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 58 a FilterBank's Bandwidth was in hertz, 0 taking the
// Spectrum's own bandwidths; it is now a 0 to 1 control on each filter's
// spacing from its neighbours.
void migrateLegacyFilterBankBandwidthsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.filterBank"
            && node.typeId != "talas.filterBankElement")
        {
            continue;
        }
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "bandwidthUnits";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "FilterBank bandwidthUnits requires project format 58");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "bandwidthUnits";
        legacy.choice = "legacyHertz";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyFilterBankBandwidths(ProjectLoadResult& result)
{
    for (const auto typeId : {"talas.filterBank", "talas.filterBankElement"})
    {
        rejectBindingsToNewerNodeParameter(
            result, typeId, "bandwidthUnits",
            "FilterBank bandwidthUnits requires project format 58");
    }
    migrateLegacyFilterBankBandwidthsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyFilterBankBandwidthsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Before format 59 a CloudBank laid its grains out regularly by
// GrainsPerTrack; they now start on its Trigger.
void migrateLegacyCloudBankGrainStartsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.cloudBank"
            && node.typeId != "talas.cloudBankElement")
        {
            continue;
        }
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "grainStarts";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "CloudBank grainStarts requires project format 59");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "grainStarts";
        legacy.choice = "legacyRegular";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyCloudBankGrainStarts(ProjectLoadResult& result)
{
    for (const auto typeId : {"talas.cloudBank", "talas.cloudBankElement"})
    {
        rejectBindingsToNewerNodeParameter(
            result, typeId, "grainStarts",
            "CloudBank grainStarts requires project format 59");
    }
    migrateLegacyCloudBankGrainStartsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyCloudBankGrainStartsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// A TwoFormantVoiceElement sings a glottal pulse with vibrato; projects
// before format 60 heard a sine.
void migrateTwoFormantVoiceExcitationInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.twoFormantVoiceElement")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "excitation";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "TwoFormantVoiceElement excitation requires project format 60");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "excitation";
        legacy.choice = "legacySine";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateTwoFormantVoiceExcitation(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.twoFormantVoiceElement", "excitation",
        "TwoFormantVoiceElement excitation requires project format 60");
    migrateTwoFormantVoiceExcitationInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateTwoFormantVoiceExcitationInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// A SpectrumModifier adds a FreqOffset without units as a fraction of
// half the sample rate; projects before format
// 61 added hertz.
void migrateSpectrumModifierFreqOffsetUnitsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.spectrumModifier")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "freqOffsetUnits";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "SpectrumModifier freqOffsetUnits requires project format 61");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "freqOffsetUnits";
        legacy.choice = "legacyHertz";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateSpectrumModifierFreqOffsetUnits(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.spectrumModifier", "freqOffsetUnits",
        "SpectrumModifier freqOffsetUnits requires project format 61");
    migrateSpectrumModifierFreqOffsetUnitsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateSpectrumModifierFreqOffsetUnitsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 62 keep the FFT's packed layout on one channel;
// from format 62 the FFT takes the stereo layout by default.
void migrateFftPackingInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.fft")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "packing";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "FFT packing requires project format 62");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "packing";
        legacy.choice = "legacyPacked";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateFftPacking(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.fft", "packing",
        "FFT packing requires project format 62");
    migrateFftPackingInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateFftPackingInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 63 read an REResonator's Wavetable as one static
// set of FIR coefficients; from format 63 it is the time-varying all-pole
// lattice of an RE table (ADR-0022).
void migrateReResonatorFilterFormInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.reResonator")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "filterForm";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "REResonator filterForm requires project format 63");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "filterForm";
        legacy.choice = "legacyFir";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateReResonatorFilterForm(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.reResonator", "filterForm",
        "REResonator filterForm requires project format 63");
    migrateReResonatorFilterFormInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateReResonatorFilterFormInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 64 delayed a TimeOffset's Input by its SilentTime;
// from format 64 a fixed SilentTime starts the Input late (ADR-0023).
void migrateTimeOffsetOffsetMeansInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.timeOffset")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "offsetMeans";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "TimeOffset offsetMeans requires project format 64");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "offsetMeans";
        legacy.choice = "legacyDelay";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateTimeOffsetOffsetMeans(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.timeOffset", "offsetMeans",
        "TimeOffset offsetMeans requires project format 64");
    migrateTimeOffsetOffsetMeansInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateTimeOffsetOffsetMeansInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 65 kept a ContextFreeGrammar's Inputs as text; from
// format 65 they are Sounds connected to its inputs port (ADR-0015
// amendment), so the text field goes.
void migrateContextFreeGrammarInputsInGraph(ProjectGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != "talas.contextFreeGrammar")
            continue;
        std::erase_if(node.parameters, [](const ProjectParameterExpression& parameter) {
            return parameter.parameterId == "inputs";
        });
    }
}

void migrateContextFreeGrammarInputs(ProjectLoadResult& result)
{
    migrateContextFreeGrammarInputsInGraph(result.document.graph);
    for (auto& definition : result.document.soundDefinitions)
        migrateContextFreeGrammarInputsInGraph(definition.graph);
}

// Projects before format 66 delayed a SetDuration's Input by its StartTime;
// from format 66 a fixed StartTime starts the Input late (ADR-0023).
void migrateSetDurationStartTimesInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.setDuration")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "startTimeMeans";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "SetDuration startTimeMeans requires project format 66");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "startTimeMeans";
        legacy.choice = "legacyDelay";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateSetDurationStartTimes(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.setDuration", "startTimeMeans",
        "SetDuration startTimeMeans requires project format 66");
    migrateSetDurationStartTimesInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateSetDurationStartTimesInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 67 sent a MIDIOutputController's Value as the
// controller value itself and a MIDIOutputEvent's Frequency as the nearest key
// alone; from format 67 Value is an amplitude and Frequencies between keys
// bend.
void migrateMidiOutputsInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        const auto controller = node.typeId == "talas.midiOutputController";
        if (!controller && node.typeId != "talas.midiOutputEvent")
            continue;
        const std::string_view parameterId = controller ? "valueMeans" : "offKeyPitch";
        if (std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                return parameter.parameterId == parameterId;
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                controller
                    ? "MidiOutputController valueMeans requires project format 67"
                    : "MidiOutputEvent offKeyPitch requires project format 67");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = std::string{parameterId};
        legacy.choice = controller ? "legacyControllerValue" : "legacyNearestNote";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateMidiOutputs(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.midiOutputController", "valueMeans",
        "MidiOutputController valueMeans requires project format 67");
    rejectBindingsToNewerNodeParameter(
        result, "talas.midiOutputEvent", "offKeyPitch",
        "MidiOutputEvent offKeyPitch requires project format 67");
    migrateMidiOutputsInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateMidiOutputsInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 148 boosted a Gain past 1 unclipped; from 148 a
// Gain clips at full scale, so older
// projects keep theirs unclipped.
void migrateGainOverflowInGraph(ProjectLoadResult& result, ProjectGraph& graph, std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.gain")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) { return parameter.parameterId == "overflow"; }))
        {
            addError(result, std::string(graphPath) + ".nodes[" + std::to_string(nodeIndex) + "].parameters",
                "Gain overflow requires project format 148");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "overflow";
        legacy.choice = "unclipped";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateGainOverflow(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(result, "talas.gain", "overflow", "Gain overflow requires project format 148");
    migrateGainOverflowInGraph(result, result.document.graph, "$.graph");
    for (std::size_t index = 0; index < result.document.soundDefinitions.size(); ++index)
    {
        migrateGainOverflowInGraph(result, result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 149 kept a LimeInterpreter's Inputs as text the
// runtime only accepted blank; from 149 they are Sounds connected to its
// inputs port, scheduled by name from its Script, so the text field goes.
void migrateLimeInterpreterInputsInGraph(ProjectGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != "talas.limeInterpreter")
            continue;
        std::erase_if(node.parameters, [](const ProjectParameterExpression& parameter) {
            return parameter.parameterId == "inputs";
        });
    }
}

void migrateLimeInterpreterInputs(ProjectLoadResult& result)
{
    migrateLimeInterpreterInputsInGraph(result.document.graph);
    for (auto& definition : result.document.soundDefinitions)
        migrateLimeInterpreterInputsInGraph(definition.graph);
}

// Projects before format 150 played a PhaseShiftBy90 unclipped; from 150 its
// filters saturate at full scale, very sensitive to Input amplitude
//, so older projects keep theirs unclipped.
void migratePhaseShiftOverflowInGraph(ProjectLoadResult& result, ProjectGraph& graph, std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.phaseShiftBy90")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) { return parameter.parameterId == "overflow"; }))
        {
            addError(result, std::string(graphPath) + ".nodes[" + std::to_string(nodeIndex) + "].parameters",
                "PhaseShiftBy90 overflow requires project format 150");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "overflow";
        legacy.choice = "unclipped";
        node.parameters.push_back(std::move(legacy));
    }
}

void migratePhaseShiftOverflow(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.phaseShiftBy90", "overflow", "PhaseShiftBy90 overflow requires project format 150");
    migratePhaseShiftOverflowInGraph(result, result.document.graph, "$.graph");
    for (std::size_t index = 0; index < result.document.soundDefinitions.size(); ++index)
    {
        migratePhaseShiftOverflowInGraph(result, result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Projects before format 69 ignored a ReverbSection's SmoothDelayChanges;
// from format 69 it glides the delay (ADR-0025), so older projects keep it
// off.
void migrateReverbSectionSmoothingInGraph(ProjectGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != "talas.reverbSection")
            continue;
        const auto smoothing = std::ranges::find(
            node.parameters, std::string_view{"smoothDelayChanges"},
            &ProjectParameterExpression::parameterId);
        if (smoothing != node.parameters.end())
        {
            smoothing->boolean = false;
            continue;
        }
        ProjectParameterExpression off;
        off.parameterId = "smoothDelayChanges";
        off.boolean = false;
        node.parameters.push_back(std::move(off));
    }
}

void migrateReverbSectionSmoothing(ProjectLoadResult& result)
{
    migrateReverbSectionSmoothingInGraph(result.document.graph);
    for (auto& definition : result.document.soundDefinitions)
        migrateReverbSectionSmoothingInGraph(definition.graph);
}

// Projects before format 70 cleared a HarmonicResonator over a restart
// whatever its Prezero; from format 70 one without Prezero remembers its
// state, so older projects turn it on.
void migrateHarmonicResonatorPrezeroInGraph(ProjectGraph& graph)
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != "talas.harmonicResonator")
            continue;
        const auto prezero = std::ranges::find(
            node.parameters, std::string_view{"prezero"},
            &ProjectParameterExpression::parameterId);
        if (prezero != node.parameters.end())
        {
            prezero->boolean = true;
            continue;
        }
        ProjectParameterExpression on;
        on.parameterId = "prezero";
        on.boolean = true;
        node.parameters.push_back(std::move(on));
    }
}

void migrateHarmonicResonatorPrezero(ProjectLoadResult& result)
{
    migrateHarmonicResonatorPrezeroInGraph(result.document.graph);
    for (auto& definition : result.document.soundDefinitions)
        migrateHarmonicResonatorPrezeroInGraph(definition.graph);
}

// Format 71 adds choices whose new defaults change how older projects
// play, and gives those projects their legacy choice: a Sample loops at the
// loop in its sample file's header while Gate is positive, a
// TriggeredTableRead loops at its table's header loop
// unless IgnoreLoops is checked and otherwise plays once, and a
// GraphicEQ's lowest band reaches down to DC and its highest up to half the
// sample rate. Before, header loops were ignored, a
// TriggeredTableRead looped its whole table and a GraphicEQ's outer bands
// were octave bandpasses.
void migrateLegacyChoiceInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath,
    std::string_view typeId,
    std::string_view parameterId,
    std::string_view legacyChoice,
    std::string_view message)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != typeId)
            continue;
        if (std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                return parameter.parameterId == parameterId;
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                std::string(message));
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = std::string(parameterId);
        legacy.choice = std::string(legacyChoice);
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateLegacyChoice(
    ProjectLoadResult& result,
    std::string_view typeId,
    std::string_view parameterId,
    std::string_view legacyChoice,
    std::string_view message)
{
    rejectBindingsToNewerNodeParameter(
        result, typeId, parameterId, std::string(message));
    migrateLegacyChoiceInGraph(
        result, result.document.graph, "$.graph",
        typeId, parameterId, legacyChoice, message);
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateLegacyChoiceInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph",
            typeId, parameterId, legacyChoice, message);
    }
}

// Since format 155 a Script's event wants a space
// between each keyword's colon and its value. A Script of an older project
// whose events put a value against a colon, as freq:500 hz, keeps taking
// them (keywordSpacing legacyTouching); one that names the choice is
// refused.
void migrateScriptKeywordSpacing(ProjectLoadResult& result)
{
    const std::string message = "Script keywordSpacing requires project format 155";
    rejectBindingsToNewerNodeParameter(result, "talas.script", "keywordSpacing", message);
    const auto migrate = [&](ProjectGraph& graph, const std::string& graphPath) {
        for (std::size_t nodeIndex = 0; nodeIndex < graph.nodes.size(); ++nodeIndex)
        {
            auto& node = graph.nodes[nodeIndex];
            if (node.typeId != "talas.script")
                continue;
            if (std::ranges::any_of(node.parameters, [](const ProjectParameterExpression& parameter) {
                    return parameter.parameterId == "keywordSpacing";
                }))
            {
                addError(result, graphPath + ".nodes[" + std::to_string(nodeIndex) + "].parameters", message);
                continue;
            }
            const auto script = std::ranges::find(node.parameters, std::string_view{"script"}, &ProjectParameterExpression::parameterId);
            if (script == node.parameters.end() || !script->text.has_value())
                continue;
            const auto parsed = smalltalk::parse(*script->text);
            if (!parsed.ok() || !smalltalk::eventValueTouchingItsColon(*parsed.program).has_value())
                continue;
            ProjectParameterExpression legacy;
            legacy.parameterId = "keywordSpacing";
            legacy.choice = "legacyTouching";
            node.parameters.push_back(std::move(legacy));
        }
    };
    migrate(result.document.graph, "$.graph");
    for (std::size_t index = 0; index < result.document.soundDefinitions.size(); ++index)
        migrate(result.document.soundDefinitions[index].graph, "$.soundDefinitions[" + std::to_string(index) + "].graph");
}

void migrateFormatSeventyOneChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.sample", "headerLoop", "legacyIgnore",
        "Sample headerLoop requires project format 71");
    migrateLegacyChoice(result, "talas.triggeredTableRead", "headerLoop", "legacyWholeTable",
        "Triggered Table Read headerLoop requires project format 71");
    migrateLegacyChoice(result, "talas.graphicEq", "outerBands", "legacyBandPass",
        "Graphic EQ outerBands requires project format 71");
}

// A HarmonicResonator resonates at its Frequency and harmonics;
// projects before format 72 rounded its period to whole samples.
void migrateHarmonicResonatorTuning(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.harmonicResonator", "tuning", "legacyRounded",
        "HarmonicResonator tuning requires project format 72");
}

// A parameter whose default changed: older projects that leave it out
// keep the old default.
void migrateLegacyDefaultInGraph(
    ProjectGraph& graph,
    std::string_view typeId,
    std::string_view parameterId,
    std::string_view legacyExpression)
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != typeId
            || std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                   return parameter.parameterId == parameterId;
               }))
        {
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = std::string(parameterId);
        legacy.expression = std::string(legacyExpression);
        node.parameters.push_back(std::move(legacy));
    }
}

// A text parameter whose default changed: older projects that leave it
// out keep the old text.
void migrateLegacyTextDefault(
    ProjectLoadResult& result, std::string_view typeId, std::string_view parameterId, std::string_view legacyText)
{
    const auto inGraph = [&](ProjectGraph& graph) {
        for (auto& node : graph.nodes)
        {
            if (node.typeId != typeId
                || std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                       return parameter.parameterId == parameterId;
                   }))
            {
                continue;
            }
            ProjectParameterExpression legacy;
            legacy.parameterId = std::string(parameterId);
            legacy.text = std::string(legacyText);
            node.parameters.push_back(std::move(legacy));
        }
    };
    inGraph(result.document.graph);
    for (auto& definition : result.document.soundDefinitions)
        inGraph(definition.graph);
}

void migrateLegacyDefault(
    ProjectLoadResult& result,
    std::string_view typeId,
    std::string_view parameterId,
    std::string_view legacyExpression)
{
    migrateLegacyDefaultInGraph(
        result.document.graph, typeId, parameterId, legacyExpression);
    for (auto& definition : result.document.soundDefinitions)
        migrateLegacyDefaultInGraph(
            definition.graph, typeId, parameterId, legacyExpression);
}

// Format 73: a MultisegmentEnvelope repeats its loop only while Gate is
// on, then plays on through its last segments; a Multisample and a KeyMappedMultisample loop at the loop in the
// file's header; a DelayWithFeedback's Mix defaults to 1, so
// a Comb is heard only after the first delay. Older projects keep
// the loop until the next trigger, LoopStart and LoopEnd or the whole file,
// and a Mix of 0.5.
void migrateFormatSeventyThreeChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.multisegmentEnvelope", "loopRelease", "legacyNever",
        "MultisegmentEnvelope loopRelease requires project format 73");
    migrateLegacyChoice(result, "talas.multisample", "headerLoop", "legacyIgnore",
        "Multisample headerLoop requires project format 73");
    migrateLegacyChoice(result, "talas.keyMappedMultisample", "headerLoop", "legacyIgnore",
        "KeyMappedMultisample headerLoop requires project format 73");
    if (result.ok())
        migrateLegacyDefault(result, "talas.delayWithFeedback", "mix", "0.5");
}

// Format 74: a GrainCloud's and a SampleCloud's grains keep the frequency,
// start point, duration and pan they start with. Older projects follow
// them every frame.
void migrateFormatSeventyFourChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.grainCloud", "grainShapes", "legacyEveryFrame",
        "GrainCloud grainShapes requires project format 74");
    migrateLegacyChoice(result, "talas.sampleCloud", "grainShapes", "legacyEveryFrame",
        "SampleCloud grainShapes requires project format 74");
}

// Format 75: an AnalogSequencer without Pitches or KeyDowns steps through
// its stages each time Step becomes positive;
// a Waveshaper without interpolation reads the entry at the integer part of
// its index; a LiveSpectralAnalysis takes the power of two nearest
// the sample rate over LowestAnalyzedFreq, so 1 F through 5 F give 512 to 32
// partials. Older projects hold the stage Step numbers, read the
// nearest entry and take the next larger power of two.
void migrateFormatSeventyFiveChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.analogSequencer", "stepMode", "legacyStageNumber",
        "AnalogSequencer stepMode requires project format 75");
    migrateLegacyChoice(result, "talas.waveshaper", "integerIndex", "legacyRound",
        "Waveshaper integerIndex requires project format 75");
    migrateLegacyChoice(result, "talas.liveSpectralAnalysis", "bandWidth", "legacyNextPower",
        "LiveSpectralAnalysis bandWidth requires project format 75");
}

// Format 76: an AnalogSequencer stage with a DutyCycle of 1 keeps its key
// down into the next stage, whose note takes over its voice without a new
// attack. Older projects release and retrigger
// it.
void migrateFormatSeventySixChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.analogSequencer", "dutyCycleOne", "legacyRetriggers",
        "AnalogSequencer dutyCycleOne requires project format 76");
}

// Format 77: a Multisample whose Event Value Gate falls is released to play
// on through its file, as one with a connected
// Gate is. Older projects stop it.
void migrateFormatSeventySevenChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.multisample", "gateRelease", "legacyStops",
        "Multisample gateRelease requires project format 77");
}

// Format 78: an OverlappingMixer plays any number of Inputs on its inputs
// port. Its list of in1 to in4, the text field
// that held that name, becomes legacyInputs; in1 to in4 keep their Inputs.
void migrateFormatSeventyEightOverlappingMixersInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.overlappingMixer")
            continue;
        const auto path = std::string(graphPath) + ".nodes["
            + std::to_string(nodeIndex) + "]";
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "legacyInputs";
            }))
        {
            addError(result, path + ".parameters",
                "OverlappingMixer legacyInputs requires project format 78");
            continue;
        }
        if (std::ranges::any_of(graph.connections, [&](const auto& connection) {
                return connection.toNode == node.instanceId
                    && connection.toPort == "inputs";
            }))
        {
            addError(result, std::string(graphPath) + ".connections",
                "OverlappingMixer inputs requires project format 78");
            continue;
        }
        for (auto& parameter : node.parameters)
            if (parameter.parameterId == "inputs")
                parameter.parameterId = "legacyInputs";
    }
}

void migrateFormatSeventyEightChoices(ProjectLoadResult& result)
{
    migrateFormatSeventyEightOverlappingMixersInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateFormatSeventyEightOverlappingMixersInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Format 79: a Vocoder's LoCutoff, HiCutoff and Rolloff shape its Input as a
// weak tone control, and a TunableVocoder's
// Rolloff sets how far its Tone tilts the filters, 0 weighting them alike
//. Older projects tilt both by Rolloff.
void migrateFormatSeventyNineChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.vocoder", "inputShaping", "legacyTilt",
        "Vocoder inputShaping requires project format 79");
    migrateLegacyChoice(result, "talas.tunableVocoder", "rolloffShape", "legacyTilt",
        "TunableVocoder rolloffShape requires project format 79");
}

// Format 80: a Filter's Order is its number of poles: one-pole sections, an AllPass's paired with Q, Scale on the
// Input and Feedback around the cascade, subtracted for a LowPass. Older
// projects keep a two-pole section with Q for each Order.
void migrateFormatEightyChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.filter", "poles", "legacyTwoPerOrder",
        "Filter poles requires project format 80");
}

// Format 81: a MIDIVoice's or MIDIMapper's notes are its alone unless
// Shared is checked, the first unshared Sound on a channel and pitch range
// taking a note. Older projects give every
// matching Sound each note.
void migrateFormatEightyOneChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.midiVoice", "unsharedNotes", "legacyToEach",
        "MIDIVoice unsharedNotes requires project format 81");
    migrateLegacyChoice(result, "talas.midiMapper", "unsharedNotes", "legacyToEach",
        "MIDIMapper unsharedNotes requires project format 81");
}

// Format 82: a KeyMappedMultisample maps every sample file in FirstSample's
// folder. Older projects play FirstSample
// alone.
void migrateFormatEightyTwoChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.keyMappedMultisample", "samples", "legacyFirstSampleOnly",
        "KeyMappedMultisample samples requires project format 82");
}

// Format 83: a FrequencyScale granulates its Input by the period its
// FreqTracker gives. Older projects shift it
// with a phase vocoder.
void migrateFormatEightyThreeChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.frequencyScale", "scaling", "legacyVocoder",
        "FrequencyScale scaling requires project format 83");
}

// Format 84: an Oscillator reads only the first 4096 entries of a longer
// wavetable. Older projects play the whole
// table as one cycle.
// Format 144: a Threshold's Hysteresis on either side of it in full; an
// older project keeps half of it each way.
void migrateFormatOneHundredFortyFourChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.threshold", "hysteresisBand", "legacyHalf",
        "Threshold hysteresisBand requires project format 144");
}

void migrateFormatEightyFourChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.oscillator", "tableLength", "legacyWholeTable",
        "Oscillator tableLength requires project format 84");
}

// A table field a node of an older project leaves out keeps the empty table
// it had then.
void migrateLegacyEmptyTableInGraph(
    ProjectGraph& graph,
    std::string_view typeId,
    std::string_view parameterId)
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != typeId
            || std::ranges::any_of(node.parameters, [&](const auto& parameter) {
                   return parameter.parameterId == parameterId;
               }))
        {
            continue;
        }
        ProjectParameterExpression empty;
        empty.parameterId = std::string(parameterId);
        empty.resource = std::string{};
        node.parameters.push_back(std::move(empty));
    }
}

void migrateLegacyEmptyTable(
    ProjectLoadResult& result,
    std::string_view typeId,
    std::string_view parameterId)
{
    migrateLegacyEmptyTableInGraph(result.document.graph, typeId, parameterId);
    for (auto& definition : result.document.soundDefinitions)
        migrateLegacyEmptyTableInGraph(definition.graph, typeId, parameterId);
}

// Format 85: a FrequencyScale reads its Delay once, at the start. Older
// projects follow a Delay that changes offline.
void migrateFormatEightyFiveChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.frequencyScale", "delayRead", "legacyFollows",
        "FrequencyScale delayRead requires project format 85");
}

// Projects from format 64 (TimeOffset) and 66 (SetDuration) to 86 start an
// Input late only with a fixed field outside MIDI voices, and those of
// formats 87 and 88 only in Sounds without nested MIDIVoices; they keep that.
void migrateFixedLateStartsInGraph(
    ProjectGraph& graph,
    std::string_view typeId,
    std::string_view parameterId,
    std::string_view legacyChoice = "legacyFixedStartsLate")
{
    for (auto& node : graph.nodes)
    {
        if (node.typeId != typeId)
            continue;
        const auto means = std::ranges::find(
            node.parameters, parameterId, &ProjectParameterExpression::parameterId);
        if (means == node.parameters.end())
        {
            ProjectParameterExpression fixed;
            fixed.parameterId = std::string(parameterId);
            fixed.choice = std::string(legacyChoice);
            node.parameters.push_back(std::move(fixed));
        }
        else if (means->choice == std::optional<std::string>{"startsInputLate"})
            means->choice = std::string(legacyChoice);
    }
}

void migrateFixedLateStarts(ProjectLoadResult& result,
    std::string_view legacyChoice = "legacyFixedStartsLate")
{
    for (const auto& [typeId, parameterId] :
        {std::pair{std::string_view{"talas.timeOffset"}, std::string_view{"offsetMeans"}},
            std::pair{std::string_view{"talas.setDuration"}, std::string_view{"startTimeMeans"}}})
    {
        migrateFixedLateStartsInGraph(result.document.graph, typeId, parameterId, legacyChoice);
        for (auto& definition : result.document.soundDefinitions)
            migrateFixedLateStartsInGraph(definition.graph, typeId, parameterId, legacyChoice);
    }
}

// Format 86: table fields default to the system wavetables by kind: Sine
// for a waveform, Hann for an envelope or a window,
// LinearEnvelope for a FormantBank's impulse
// and FullRamp for a shaping function. Older projects that leave such a
// field out keep the empty table they had.
void migrateFormatEightySixChoices(ProjectLoadResult& result)
{
    migrateLegacyEmptyTable(result, "talas.oscillatorBank", "wavetable");
    migrateLegacyEmptyTable(result, "talas.sosOscillators", "wavetable");
    migrateLegacyEmptyTable(result, "talas.grainCloud", "waveform");
    migrateLegacyEmptyTable(result, "talas.grainCloud", "grainEnv");
    migrateLegacyEmptyTable(result, "talas.cloudBank", "grainWave");
    migrateLegacyEmptyTable(result, "talas.cloudBank", "grainEnvelope");
    migrateLegacyEmptyTable(result, "talas.cloudBankElement", "grainWave");
    migrateLegacyEmptyTable(result, "talas.cloudBankElement", "grainEnvelope");
    migrateLegacyEmptyTable(result, "talas.sampleCloud", "grainEnv");
    migrateLegacyEmptyTable(result, "talas.chopper", "wavetable");
    migrateLegacyEmptyTable(result, "talas.functionGenerator", "wavetable");
    migrateLegacyEmptyTable(result, "talas.spectralShape", "wavetable");
    migrateLegacyEmptyTable(result, "talas.triggeredTableRead", "wavetable");
    migrateLegacyEmptyTable(result, "talas.formantBank", "impulseResponse");
    migrateLegacyEmptyTable(result, "talas.formantBankOscillator", "formantImpulse");
    migrateLegacyEmptyTable(result, "talas.waveshaper", "shapingFunction");
    migrateLegacyEmptyTable(result, "talas.iteratedWaveshaper", "wavetable");
    migrateLegacyEmptyTable(result, "talas.multiplyingWaveshaper", "wavetable");
}

// Format 87: a TimeOffset's SilentTime and a SetDuration's StartTime start
// their Input late in a flat MIDIVoice too, counted from the MIDIVoice's
// start, and one fed by a Sound or that moves is
// read as the step starts (ADR-0023). Older projects start an Input late
// only with a fixed field outside MIDI voices.
void migrateFormatEightySevenChoices(ProjectLoadResult& result)
{
    migrateFixedLateStarts(result);
}

// Format 88: a Gate of -1 makes an ADSR, AR, MultisegmentEnvelope or
// GraphicalEnvelope decay rapidly to zero. Older
// projects release over ReleaseTime, or play on.
void migrateFormatEightyEightChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.adsr", "negativeGate", "legacyReleases",
        "ADSR negativeGate requires project format 88");
    migrateLegacyChoice(result, "talas.ar", "negativeGate", "legacyReleases",
        "AR negativeGate requires project format 88");
    migrateLegacyChoice(result, "talas.multisegmentEnvelope", "negativeGate", "legacyReleases",
        "MultisegmentEnvelope negativeGate requires project format 88");
    migrateLegacyChoice(result, "talas.graphicalEnvelope", "negativeGate", "legacyReleases",
        "GraphicalEnvelope negativeGate requires project format 88");
}

// Format 89: a TimeOffset's SilentTime and a SetDuration's StartTime start
// their Input late in Sounds with nested MIDIVoices too, each counted from
// its own MIDIVoice's start, and a value read from a Sound is paced by the
// fixed Rates around it (ADR-0023, ADR-0019). Projects of formats 87 and 88
// keep starting an Input late only without nested MIDIVoices and reading a
// Sound in real time; older ones already chose the fixed-only rule.
void migrateFormatEightyNineChoices(ProjectLoadResult& result)
{
    migrateFixedLateStarts(result, "legacyFlatVoicesOnly");
}

// Format 90: a SoundCollectionVariable in a many-Sound field
// stands for all its Sounds; older projects keep the one Sound it selected.
void migrateSoundCollectionMembersInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.soundCollectionVariable")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "members";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "SoundCollectionVariable members requires project format 90");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "members";
        legacy.choice = "legacySelected";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateSoundCollectionMembers(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.soundCollectionVariable", "members",
        "SoundCollectionVariable members requires project format 90");
    migrateSoundCollectionMembersInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateSoundCollectionMembersInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Format 91: a constant field inside a MIDIVoice, MIDIMapper or
// AnalogSequencer is read as each voice starts, with that voice's Event
// Values (owner decision 2026-10-12). Older projects read it once, as the
// Sound compiles.
void migrateFormatNinetyOneChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.midiVoice", "constantFieldsRead", "legacyWhenCompiled",
        "MIDIVoice constantFieldsRead requires project format 91");
    migrateLegacyChoice(result, "talas.midiMapper", "constantFieldsRead", "legacyWhenCompiled",
        "MIDIMapper constantFieldsRead requires project format 91");
    migrateLegacyChoice(result, "talas.analogSequencer", "constantFieldsRead", "legacyWhenCompiled",
        "AnalogSequencer constantFieldsRead requires project format 91");
}

// Format 92: documented behaviours (owner decision 2026-10-18, as
// recommended): HotPink noise's spectrum peaks at Frequency, a Filter's
// negative Feedback is the positive one out of phase, a
// SpectrumFrequencyScale leaves the formants, a
// TimeFrequencyScale's Rate is at most 1, a VocoderChannelBank
// takes its share of the spectra's NbrPartials and a
// SyntheticSpectrumFromArray's live elements update once every NbrPartials
// samples. Older projects keep what they played.
void migrateFormatNinetyTwoChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.noise", "hotPink", "legacySmoothedSteps",
        "Noise hotPink requires project format 92");
    migrateLegacyChoice(result, "talas.filter", "negativeFeedback", "legacyTurnsSignOver",
        "Filter negativeFeedback requires project format 92");
    migrateLegacyChoice(result, "talas.spectrumFrequencyScale", "formants", "legacyMove",
        "SpectrumFrequencyScale formants requires project format 92");
    migrateLegacyChoice(result, "talas.timeFrequencyScale", "rateAboveOne", "legacyCompresses",
        "TimeFrequencyScale rateAboveOne requires project format 92");
    migrateLegacyChoice(result, "talas.vocoderChannelBank", "gain", "legacyCount",
        "VocoderChannelBank gain requires project format 92");
    migrateLegacyChoice(result, "talas.syntheticSpectrumFromArray", "updates", "legacyEveryBlock",
        "SyntheticSpectrumFromArray updates requires project format 92");
}

// Format 93: an Audio pair's frequencies are fractions of half the sample
// rate, as audio values are (a Ramp's cycle gives low to high partials):
// the spectral sources' frequencyOut, a
// SyntheticSpectrumFromSounds's FrequenciesAndBandwidths and the paired
// inputs of the banks and the FrameSynchronizer. Older projects keep hertz.
void migrateFormatNinetyThreeChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.spectrumInRam", "pairFrequencies", "legacyHertz",
        "SpectrumInRam pairFrequencies requires project format 93");
    migrateLegacyChoice(result, "talas.syntheticSpectrumFromArray", "pairFrequencies", "legacyHertz",
        "SyntheticSpectrumFromArray pairFrequencies requires project format 93");
    migrateLegacyChoice(result, "talas.syntheticSpectrumFromSounds", "pairFrequencies", "legacyHertz",
        "SyntheticSpectrumFromSounds pairFrequencies requires project format 93");
    migrateLegacyChoice(result, "talas.frameSynchronizer", "pairFrequencies", "legacyHertz",
        "FrameSynchronizer pairFrequencies requires project format 93");
    migrateLegacyChoice(result, "talas.formantBank", "pairFrequencies", "legacyHertz",
        "FormantBank pairFrequencies requires project format 93");
    migrateLegacyChoice(result, "talas.formantBankOscillator", "pairFrequencies", "legacyHertz",
        "FormantBankOscillator pairFrequencies requires project format 93");
    migrateLegacyChoice(result, "talas.oscillatorBank", "pairFrequencies", "legacyHertz",
        "OscillatorBank pairFrequencies requires project format 93");
}

// Format 94: a ScaleVocoder's Rolloff sets the width of its Tone control
//, 0 weighting the bands alike, as a
// TunableVocoder's does since format 79. Older projects keep tilting the
// bands by Tone and again by Rolloff.
void migrateFormatNinetyFourChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.scaleVocoder", "rolloffShape", "legacyTilt",
        "ScaleVocoder rolloffShape requires project format 94");
}

// Format 95: a stereo sample plays its left channel on audioOut and its
// right channel on rightOut in a Sample,
// Multisample, KeyMappedMultisample, DiskPlayer and MultifileDiskPlayer.
// Older projects keep both channels averaged, or a stereo table refused.
void migrateFormatNinetyFiveChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.sample", "channels", "legacyMono",
        "Sample channels requires project format 95");
    migrateLegacyChoice(result, "talas.multisample", "channels", "legacyMono",
        "Multisample channels requires project format 95");
    migrateLegacyChoice(result, "talas.keyMappedMultisample", "channels", "legacyMono",
        "KeyMappedMultisample channels requires project format 95");
    migrateLegacyChoice(result, "talas.diskPlayer", "channels", "legacyMono",
        "DiskPlayer channels requires project format 95");
    migrateLegacyChoice(result, "talas.multifileDiskPlayer", "channels", "legacyMono",
        "MultifileDiskPlayer channels requires project format 95");
}

// Format 96: a SetDuration's Duration fed by a Sound is paced by the fixed
// Rates around it, as its StartTime has been since 89 (ADR-0019). Projects
// with the startsInputLate rule keep an unpaced Duration; earlier choices
// already read a Sound in real time.
void migrateFormatNinetySixChoices(ProjectLoadResult& result)
{
    migrateFixedLateStartsInGraph(result.document.graph, "talas.setDuration", "startTimeMeans", "legacyDurationUnpaced");
    for (auto& definition : result.document.soundDefinitions)
        migrateFixedLateStartsInGraph(definition.graph, "talas.setDuration", "startTimeMeans", "legacyDurationUnpaced");
}

// Projects before format 100 divided a DynamicRangeController's level past its
// Threshold by the Ratio in amplitude; the Ratio is now in decibels (4/1
// means 4 dB in for 1 dB out), so they keep the linear law.
void migrateDynamicRangeControllerRatioLawInGraph(
    ProjectLoadResult& result,
    ProjectGraph& graph,
    std::string_view graphPath)
{
    for (std::size_t nodeIndex = 0;
         nodeIndex < graph.nodes.size();
         ++nodeIndex)
    {
        auto& node = graph.nodes[nodeIndex];
        if (node.typeId != "talas.dynamicRangeController")
            continue;
        if (std::ranges::any_of(node.parameters, [](const auto& parameter) {
                return parameter.parameterId == "ratioLaw";
            }))
        {
            addError(result,
                std::string(graphPath) + ".nodes["
                    + std::to_string(nodeIndex) + "].parameters",
                "DynamicRangeController ratioLaw requires project format 100");
            continue;
        }
        ProjectParameterExpression legacy;
        legacy.parameterId = "ratioLaw";
        legacy.choice = "legacyLinear";
        node.parameters.push_back(std::move(legacy));
    }
}

void migrateDynamicRangeControllerRatioLaw(ProjectLoadResult& result)
{
    rejectBindingsToNewerNodeParameter(
        result, "talas.dynamicRangeController", "ratioLaw",
        "DynamicRangeController ratioLaw requires project format 100");
    migrateDynamicRangeControllerRatioLawInGraph(
        result, result.document.graph, "$.graph");
    for (std::size_t index = 0;
         index < result.document.soundDefinitions.size();
         ++index)
    {
        migrateDynamicRangeControllerRatioLawInGraph(
            result,
            result.document.soundDefinitions[index].graph,
            "$.soundDefinitions[" + std::to_string(index) + "].graph");
    }
}

// Format 101 makes two changes (owner decision 2026-10-21, as
// recommended). A MIDIVoice's or MIDIMapper's script note without a
// frequency plays its LowPitch; older projects play note 0. A
// Mixer lasts its longest Input for the Sounds that read their
// Inputs' durations; an older project with a Mixer and such a Sound keeps
// the Mixer's duration unknown.
void migrateFormatOneHundredOneChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.midiVoice", "scriptNotePitch", "legacyNoteZero",
        "MIDIVoice scriptNotePitch requires project format 101");
    migrateLegacyChoice(result, "talas.midiMapper", "scriptNotePitch", "legacyNoteZero",
        "MIDIMapper scriptNotePitch requires project format 101");
    const auto holds = [&](std::initializer_list<std::string_view> typeIds) {
        const auto inGraph = [&](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [&](const ProjectNode& node) {
                return std::ranges::find(typeIds, std::string_view{node.typeId}) != typeIds.end();
            });
        };
        return inGraph(result.document.graph)
            || std::ranges::any_of(result.document.soundDefinitions,
                [&](const ProjectSoundDefinition& definition) { return inGraph(definition.graph); });
    };
    result.document.mixerDurationsUnknown = holds({"talas.mixer"})
        && holds({"talas.overlappingMixer", "talas.cellularAutomaton", "talas.rhythmicCellularAutomaton",
            "talas.randomSelection", "talas.preset", "talas.contextFreeGrammar"});
}

// Format 102 lets the Mixer, as the Two-Input Mixer
// at format 101, last its longest Input for the Sounds that
// read their Inputs' durations (owner decision 2026-10-21, as recommended);
// an older project with a Mixer and such a Sound keeps the Mixer's
// duration unknown.
void migrateFormatOneHundredTwoChoices(ProjectLoadResult& result)
{
    const auto holds = [&](std::initializer_list<std::string_view> typeIds) {
        const auto inGraph = [&](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [&](const ProjectNode& node) {
                return std::ranges::find(typeIds, std::string_view{node.typeId}) != typeIds.end();
            });
        };
        return inGraph(result.document.graph)
            || std::ranges::any_of(result.document.soundDefinitions,
                [&](const ProjectSoundDefinition& definition) { return inGraph(definition.graph); });
    };
    result.document.manyInputMixerDurationsUnknown = holds({"talas.mixerV2"})
        && holds({"talas.overlappingMixer", "talas.cellularAutomaton", "talas.rhythmicCellularAutomaton",
            "talas.randomSelection", "talas.preset", "talas.contextFreeGrammar"});
}

// Format 107: with LogScale, a SyntheticSpectrumFromSounds reads its
// FrequenciesAndBandwidths as pitches, 1 half the sample rate and each 1/15
// below it an octave lower. Older projects keep
// reading frequencies and taking their pitch.
void migrateFormatOneHundredSevenChoices(ProjectLoadResult& result)
{
    migrateLegacyChoice(result, "talas.syntheticSpectrumFromSounds", "logFrequencies", "legacyLinear",
        "SyntheticSpectrumFromSounds logFrequencies requires project format 107");
}

ProjectGraph* graphForPresentationScope(
    ProjectDocument& document,
    const std::optional<std::string>& definitionId)
{
    if (!definitionId.has_value())
        return &document.graph;
    const auto found = std::ranges::find_if(
        document.soundDefinitions,
        [&](const auto& definition) {
            return definition.definitionId == *definitionId;
        });
    return found == document.soundDefinitions.end()
        ? nullptr
        : &found->graph;
}

void migrateLegacyPresentation(ProjectDocument& document)
{
    reconcileProjectPresentation(document);
}

void rejectInlinePosition(
    ProjectLoadResult& result,
    const json::Value& value,
    const std::string& path)
{
    if (value.isObject() && objectMember(value, "position") != nullptr)
    {
        addError(
            result,
            path + ".position",
            "Format 11 stores positions in presentation");
    }
}

void rejectFormatElevenInlinePositions(
    ProjectLoadResult& result,
    const json::Value& root)
{
    if (const auto* controls = objectMember(root, "virtualControls");
        controls != nullptr && controls->isArray())
    {
        for (std::size_t index = 0; index < controls->array().size(); ++index)
        {
            rejectInlinePosition(
                result,
                controls->array()[index],
                "$.virtualControls[" + std::to_string(index) + "]");
        }
    }

    const auto rejectGraph = [&](const json::Value* graph,
                                 const std::string& path) {
        if (graph == nullptr || !graph->isObject())
            return;
        const auto* nodes = objectMember(*graph, "nodes");
        if (nodes == nullptr || !nodes->isArray())
            return;
        for (std::size_t index = 0; index < nodes->array().size(); ++index)
        {
            rejectInlinePosition(
                result,
                nodes->array()[index],
                path + ".nodes[" + std::to_string(index) + "]");
        }
    };
    rejectGraph(objectMember(root, "graph"), "$.graph");
    if (const auto* definitions = objectMember(root, "soundDefinitions");
        definitions != nullptr && definitions->isArray())
    {
        for (std::size_t index = 0; index < definitions->array().size(); ++index)
        {
            const auto& definition = definitions->array()[index];
            rejectGraph(
                definition.isObject()
                    ? objectMember(definition, "graph")
                    : nullptr,
                "$.soundDefinitions[" + std::to_string(index) + "].graph");
        }
    }
}

std::optional<SnapshotTargetIdentity> parseSnapshotTarget(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    int sourceVersion);

void parsePresentation(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "presentation");
    if (source == nullptr || !source->isObject())
    {
        addError(result, "$.presentation", "Expected required object");
        return;
    }
    const auto currentPresentation = sourceVersion >= versionTwenty;
    const auto expectedMemberCount = currentPresentation ? 4u : 3u;
    if (source->object().size() != expectedMemberCount)
    {
        addError(result,
            "$.presentation",
            currentPresentation
                ? "Expected version, structureLayouts, performanceControls, and performanceMeters only"
                : "Expected version, structureLayouts, and performanceControls only");
    }

    const auto* version = objectMember(*source, "version");
    std::int32_t parsedPresentationVersion = 0;
    const auto expectedPresentationVersion = currentPresentation ? 2 : 1;
    if (version == nullptr || !version->isNumber()
        || !parseSigned32BitIntegerToken(
            version->numberToken(), parsedPresentationVersion)
        || parsedPresentationVersion != expectedPresentationVersion)
    {
        addError(result,
            "$.presentation.version",
            currentPresentation
                ? "Expected presentation version 2"
                : "Expected presentation version 1");
    }

    ProjectPresentationState presentation;
    presentation.schemaVersion = 2;
    const auto* layouts = objectMember(*source, "structureLayouts");
    if (layouts == nullptr || !layouts->isArray())
    {
        addError(result, "$.presentation.structureLayouts", "Expected array");
    }
    else
    {
        std::unordered_set<std::string> seenScopes;
        for (std::size_t layoutIndex = 0;
             layoutIndex < layouts->array().size();
             ++layoutIndex)
        {
            const auto& layoutValue = layouts->array()[layoutIndex];
            const auto layoutPath = "$.presentation.structureLayouts["
                + std::to_string(layoutIndex) + "]";
            if (!layoutValue.isObject())
            {
                addError(result, layoutPath, "Expected object");
                continue;
            }
            if (layoutValue.object().size() != 2)
                addError(result, layoutPath, "Expected scope and nodes only");

            ProjectStructureLayout layout;
            const auto* scope = objectMember(layoutValue, "scope");
            std::string kind;
            if (scope == nullptr || !scope->isObject()
                || !requiredStringMember(
                    result,
                    *scope,
                    "kind",
                    layoutPath + ".scope.kind",
                    kind))
            {
                if (scope == nullptr || !scope->isObject())
                    addError(result, layoutPath + ".scope", "Expected object");
                continue;
            }
            if (kind == "project")
            {
                if (scope->object().size() != 1)
                    addError(result, layoutPath + ".scope", "Project scope accepts kind only");
            }
            else if (kind == "soundDefinition")
            {
                std::string definitionId;
                if (scope->object().size() != 2
                    || !requiredStringMember(
                        result,
                        *scope,
                        "definitionId",
                        layoutPath + ".scope.definitionId",
                        definitionId)
                    || definitionId.empty())
                {
                    addError(result, layoutPath + ".scope", "Sound-definition scope requires one definitionId");
                    continue;
                }
                layout.soundDefinitionId = std::move(definitionId);
            }
            else
            {
                addError(result, layoutPath + ".scope.kind", "Expected project or soundDefinition");
                continue;
            }

            const auto scopeKey = layout.soundDefinitionId.has_value()
                ? "definition:" + *layout.soundDefinitionId
                : std::string{"project"};
            if (!seenScopes.insert(scopeKey).second)
            {
                addError(result, layoutPath + ".scope", "Duplicate presentation scope");
                continue;
            }
            auto* graph = graphForPresentationScope(
                result.document,
                layout.soundDefinitionId);
            if (graph == nullptr)
            {
                addError(result, layoutPath + ".scope", "Unknown Sound definition scope");
                continue;
            }

            const auto* nodes = objectMember(layoutValue, "nodes");
            if (nodes == nullptr || !nodes->isArray())
            {
                addError(result, layoutPath + ".nodes", "Expected array");
                continue;
            }
            std::unordered_set<std::string> seenNodes;
            for (std::size_t nodeIndex = 0;
                 nodeIndex < nodes->array().size();
                 ++nodeIndex)
            {
                const auto& nodeValue = nodes->array()[nodeIndex];
                const auto nodePath = layoutPath + ".nodes["
                    + std::to_string(nodeIndex) + "]";
                if (!nodeValue.isObject())
                {
                    addError(result, nodePath, "Expected object");
                    continue;
                }
                if (nodeValue.object().size() != 3)
                    addError(result, nodePath, "Expected nodeId, x, and y only");
                ProjectNodePresentation node;
                const auto hasId = requiredStringMember(
                    result,
                    nodeValue,
                    "nodeId",
                    nodePath + ".nodeId",
                    node.nodeId);
                const auto hasX = parseCanvasCoordinate(
                    result,
                    nodeValue,
                    "x",
                    nodePath + ".x",
                    node.position.x);
                const auto hasY = parseCanvasCoordinate(
                    result,
                    nodeValue,
                    "y",
                    nodePath + ".y",
                    node.position.y);
                if (!hasId || !hasX || !hasY)
                    continue;
                if (!seenNodes.insert(node.nodeId).second)
                {
                    addError(result, nodePath + ".nodeId", "Duplicate node presentation");
                    continue;
                }
                const auto target = std::ranges::find_if(
                    graph->nodes,
                    [&](const auto& candidate) {
                        return candidate.instanceId == node.nodeId;
                    });
                if (target == graph->nodes.end())
                {
                    addError(result, nodePath + ".nodeId", "Unknown node in presentation scope");
                    continue;
                }
                layout.nodes.push_back(std::move(node));
            }
            for (const auto& node : graph->nodes)
            {
                if (!seenNodes.contains(node.instanceId))
                {
                    addError(
                        result,
                        layoutPath + ".nodes",
                        "Missing node presentation for " + node.instanceId);
                }
            }
            presentation.structureLayouts.push_back(std::move(layout));
        }
        if (seenScopes.size() != result.document.soundDefinitions.size() + 1)
        {
            addError(
                result,
                "$.presentation.structureLayouts",
                "Expected exactly one layout for every project graph scope");
        }
    }

    const auto* controls = objectMember(*source, "performanceControls");
    if (controls == nullptr || !controls->isArray())
    {
        addError(result, "$.presentation.performanceControls", "Expected array");
    }
    else if (controls->array().size() > maximumPerformanceControlCount)
    {
        addError(result,
            "$.presentation.performanceControls",
            "Too many Performance controls");
    }
    else
    {
        std::unordered_set<std::string> seenControls;
        std::unordered_set<std::string> seenControlIds;
        std::size_t modulationSlotCount = 0;
        for (std::size_t index = 0; index < controls->array().size(); ++index)
        {
            const auto& controlValue = controls->array()[index];
            const auto path = "$.presentation.performanceControls["
                + std::to_string(index) + "]";
            if (!controlValue.isObject())
            {
                addError(result, path, "Expected object");
                continue;
            }
            ProjectPerformanceControlPresentation control;
            bool identityValid = true;
            if (!currentPresentation)
            {
                if (controlValue.object().size() != 5)
                    addError(result, path, "Expected eventName, domain, x, y, and visible only");
                std::string domain;
                const auto hasName = requiredStringMember(result,
                    controlValue,
                    "eventName",
                    path + ".eventName",
                    control.target.eventName);
                const auto hasDomain = requiredStringMember(result,
                    controlValue,
                    "domain",
                    path + ".domain",
                    domain);
                const auto parsedDomain = hasDomain
                    ? parseValueDomain(domain)
                    : std::nullopt;
                if (!parsedDomain.has_value())
                    addError(result, path + ".domain", "Expected known Event Value domain");
                identityValid = hasName && parsedDomain.has_value();
                if (parsedDomain.has_value())
                    control.target.domain = *parsedDomain;
                control.target.kind = SnapshotTargetKind::EventValue;
            }
            else
            {
                if (controlValue.object().size() != 8)
                {
                    addError(result,
                        path,
                        "Expected controlId, target, kind, displayName, x, y, visible, and modulationSlots only");
                }
                identityValid = requiredStringMember(result,
                    controlValue,
                    "controlId",
                    path + ".controlId",
                    control.controlId);
                if (identityValid
                    && !isValidProjectPerformanceIdentifier(control.controlId))
                {
                    addError(result,
                        path + ".controlId",
                        "Expected bounded Performance control identifier");
                    identityValid = false;
                }
                if (!seenControlIds.insert(control.controlId).second)
                {
                    addError(result,
                        path + ".controlId",
                        "Duplicate Performance control identifier");
                    identityValid = false;
                }
                const auto* targetValue = objectMember(controlValue, "target");
                const auto parsedTarget = targetValue == nullptr
                    ? std::optional<SnapshotTargetIdentity>{}
                    : parseSnapshotTarget(
                        result,
                        *targetValue,
                        path + ".target",
                        sourceVersion);
                if (!parsedTarget.has_value())
                {
                    if (targetValue == nullptr)
                        addError(result, path + ".target", "Expected target object");
                    identityValid = false;
                }
                else
                {
                    control.target = *parsedTarget;
                    if (!isProjectPerformanceTargetAvailable(
                            result.document, control.target))
                    {
                        addError(result,
                            path + ".target",
                            "Performance target is unavailable in this project");
                        identityValid = false;
                    }
                }
                std::string kind;
                if (!requiredStringMember(result,
                        controlValue,
                        "kind",
                        path + ".kind",
                        kind))
                {
                    identityValid = false;
                }
                else if (kind == "knob")
                    control.kind = ProjectPerformanceControlKind::Knob;
                else if (kind == "fader")
                    control.kind = ProjectPerformanceControlKind::Fader;
                else if (kind == "gate")
                    control.kind = ProjectPerformanceControlKind::Gate;
                else if (kind == "toggle")
                    control.kind = ProjectPerformanceControlKind::Toggle;
                else if (kind == "macro")
                    control.kind = ProjectPerformanceControlKind::Macro;
                else
                {
                    addError(result, path + ".kind", "Expected known Performance control kind");
                    identityValid = false;
                }
                if (!requiredStringMember(result,
                        controlValue,
                        "displayName",
                        path + ".displayName",
                        control.displayName)
                    || control.displayName.empty()
                    || control.displayName.size()
                        > maximumPerformanceDisplayNameBytes)
                {
                    addError(result,
                        path + ".displayName",
                        "Expected bounded non-empty Performance display name");
                    identityValid = false;
                }
            }
            const auto hasX = parseCanvasCoordinate(
                result,
                controlValue,
                "x",
                path + ".x",
                control.position.x);
            const auto hasY = parseCanvasCoordinate(
                result,
                controlValue,
                "y",
                path + ".y",
                control.position.y);
            const auto* visible = objectMember(controlValue, "visible");
            if (visible == nullptr || !visible->isBoolean())
                addError(result, path + ".visible", "Expected boolean");
            else
                control.visible = visible->boolean();
            if (!identityValid || !hasX || !hasY
                || visible == nullptr || !visible->isBoolean())
            {
                continue;
            }
            if (currentPresentation)
            {
                const auto* slots = objectMember(controlValue, "modulationSlots");
                if (slots == nullptr || !slots->isArray())
                {
                    addError(result, path + ".modulationSlots", "Expected array");
                    continue;
                }
                if (slots->array().size()
                    > maximumPerformanceModulationSlotsPerControl)
                {
                    addError(result,
                        path + ".modulationSlots",
                        "Too many modulation slots for one Performance control");
                    continue;
                }
                modulationSlotCount += slots->array().size();
                if (modulationSlotCount > maximumPerformanceModulationSlotCount)
                {
                    addError(result,
                        "$.presentation.performanceControls",
                        "Too many Performance modulation slots");
                    continue;
                }
                std::unordered_set<std::string> destinations;
                for (std::size_t slotIndex = 0;
                    slotIndex < slots->array().size();
                    ++slotIndex)
                {
                    const auto& slotValue = slots->array()[slotIndex];
                    const auto slotPath = path + ".modulationSlots["
                        + std::to_string(slotIndex) + "]";
                    if (!slotValue.isObject()
                        || slotValue.object().size() != 2)
                    {
                        addError(result,
                            slotPath,
                            "Expected target and depth only");
                        continue;
                    }
                    const auto* destinationValue = objectMember(slotValue, "target");
                    const auto destination = destinationValue == nullptr
                        ? std::optional<SnapshotTargetIdentity>{}
                        : parseSnapshotTarget(
                            result,
                            *destinationValue,
                            slotPath + ".target",
                            sourceVersion);
                    const auto* depth = objectMember(slotValue, "depth");
                    if (!destination.has_value()
                        || depth == nullptr || !depth->isNumber()
                        || !std::isfinite(depth->number())
                        || depth->number() < -1.0 || depth->number() > 1.0)
                    {
                        addError(result,
                            slotPath,
                            "Expected available target and finite depth in [-1, 1]");
                        continue;
                    }
                    const auto destinationId =
                        snapshotTargetIdentityString(*destination);
                    if (*destination == control.target
                        || !isProjectPerformanceTargetAvailable(
                            result.document, *destination)
                        || !destinations.insert(destinationId).second)
                    {
                        addError(result,
                            slotPath + ".target",
                            "Duplicate, self, or unavailable modulation target");
                        continue;
                    }
                    control.modulationSlots.push_back(
                        {*destination, depth->number()});
                }
                if (control.kind != ProjectPerformanceControlKind::Macro
                    && !control.modulationSlots.empty())
                {
                    addError(result,
                        path + ".modulationSlots",
                        "Only macro controls accept modulation slots");
                    continue;
                }
            }
            const auto key = snapshotTargetIdentityString(control.target);
            if (!seenControls.insert(key).second)
            {
                addError(result, path, "Duplicate Performance control presentation");
                continue;
            }
            presentation.performanceControls.push_back(std::move(control));
        }
        const auto allEventsPresented = std::ranges::all_of(
            result.document.virtualControls,
            [&](const auto& virtualControl) {
                SnapshotTargetIdentity target;
                target.kind = SnapshotTargetKind::EventValue;
                target.eventName = virtualControl.eventName;
                target.domain = virtualControl.domain;
                return seenControls.contains(snapshotTargetIdentityString(target));
            });
        if (!allEventsPresented)
        {
            addError(
                result,
                "$.presentation.performanceControls",
                "Expected exactly one presentation for every virtual control");
        }
    }

    if (currentPresentation)
    {
        const auto* meters = objectMember(*source, "performanceMeters");
        if (meters == nullptr || !meters->isArray())
        {
            addError(result, "$.presentation.performanceMeters", "Expected array");
        }
        else if (meters->array().size() > maximumPerformanceMeterCount)
        {
            addError(result,
                "$.presentation.performanceMeters",
                "Too many Performance meters");
        }
        else
        {
            std::unordered_set<std::string> meterIds;
            for (std::size_t index = 0; index < meters->array().size(); ++index)
            {
                const auto& value = meters->array()[index];
                const auto path = "$.presentation.performanceMeters["
                    + std::to_string(index) + "]";
                if (!value.isObject() || value.object().size() != 5)
                {
                    addError(result,
                        path,
                        "Expected meterId, outputChannel, x, y, and visible only");
                    continue;
                }
                ProjectPerformanceMeterPresentation meter;
                if (!requiredStringMember(result,
                        value,
                        "meterId",
                        path + ".meterId",
                        meter.meterId)
                    || !isValidProjectPerformanceIdentifier(meter.meterId)
                    || !meterIds.insert(meter.meterId).second)
                {
                    addError(result,
                        path + ".meterId",
                        "Expected unique bounded Performance meter identifier");
                    continue;
                }
                const auto* channel = objectMember(value, "outputChannel");
                std::int32_t outputChannel = 0;
                if (channel == nullptr || !channel->isNumber()
                    || !parseSigned32BitIntegerToken(
                        channel->numberToken(), outputChannel)
                    || outputChannel <= 0)
                {
                    addError(result,
                        path + ".outputChannel",
                        "Expected positive integer output channel");
                    continue;
                }
                meter.outputChannel = static_cast<std::uint32_t>(outputChannel);
                const auto hasX = parseCanvasCoordinate(
                    result, value, "x", path + ".x", meter.position.x);
                const auto hasY = parseCanvasCoordinate(
                    result, value, "y", path + ".y", meter.position.y);
                const auto* visible = objectMember(value, "visible");
                if (visible == nullptr || !visible->isBoolean())
                    addError(result, path + ".visible", "Expected boolean");
                else
                    meter.visible = visible->boolean();
                if (hasX && hasY && visible != nullptr && visible->isBoolean())
                    presentation.performanceMeters.push_back(std::move(meter));
            }
        }
    }
    result.document.presentation = std::move(presentation);
    if (projectPerformancePresentationHasCycle(result.document))
    {
        addError(result,
            "$.presentation.performanceControls",
            "Performance macro routing contains a cycle");
    }
}

bool isValidSnapshotIdentity(std::string_view value)
{
    if (value.empty() || !isAsciiEventNameStart(value.front()))
        return false;
    return std::ranges::all_of(
        value.substr(1),
        [](const auto character) {
            return isAsciiEventNameContinue(character)
                || character == '-';
        });
}

bool parseSnapshotIdentifier(
    ProjectLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::string& output)
{
    if (!requiredStringMember(result, object, key, path, output))
        return false;
    if (output.size() > maximumSnapshotIdentifierBytes
        || !isValidSnapshotIdentity(output))
    {
        addError(result, path, "Expected bounded snapshot identifier");
        return false;
    }
    return true;
}

bool parseSnapshotName(
    ProjectLoadResult& result,
    const json::Value& object,
    const std::string& path,
    std::string& output)
{
    if (!requiredStringMember(result, object, "name", path, output))
        return false;
    if (output.empty() || output.size() > maximumSnapshotNameBytes)
    {
        addError(result, path, "Expected non-empty bounded snapshot name");
        return false;
    }
    return true;
}

bool parseSnapshotInstancePath(
    ProjectLoadResult& result,
    const json::Value& object,
    const std::string& path,
    std::vector<std::string>& output)
{
    const auto* source = objectMember(object, "instancePath");
    if (source == nullptr || !source->isArray())
    {
        addError(result, path, "Expected string array");
        return false;
    }
    bool valid = true;
    output.clear();
    output.reserve(source->array().size());
    for (std::size_t index = 0; index < source->array().size(); ++index)
    {
        const auto& value = source->array()[index];
        const auto valuePath = path + "[" + std::to_string(index) + "]";
        if (!value.isString()
            || value.string().size() > maximumSnapshotIdentifierBytes
            || !isValidSnapshotIdentity(value.string()))
        {
            addError(result, valuePath, "Expected bounded structural identifier");
            valid = false;
            continue;
        }
        output.push_back(value.string());
    }
    return valid;
}

bool parseNullableSnapshotIdentifier(
    ProjectLoadResult& result,
    const json::Value& object,
    std::string_view key,
    const std::string& path,
    std::optional<std::string>& output)
{
    const auto* source = objectMember(object, key);
    if (source == nullptr)
    {
        addError(result, path, "Expected string or null");
        return false;
    }
    if (source->isNull())
    {
        output.reset();
        return true;
    }
    if (!source->isString()
        || source->string().size() > maximumSnapshotIdentifierBytes
        || !isValidSnapshotIdentity(source->string()))
    {
        addError(result, path, "Expected bounded structural identifier or null");
        return false;
    }
    output = source->string();
    return true;
}

std::optional<SnapshotScopeIdentity> parseSnapshotScope(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path)
{
    if (!source.isObject())
    {
        addError(result, path, "Expected object");
        return std::nullopt;
    }
    std::string kind;
    if (!requiredStringMember(result, source, "kind", path + ".kind", kind))
        return std::nullopt;

    SnapshotScopeIdentity scope;
    if (kind == "project")
    {
        if (source.object().size() != 1)
        {
            addError(result, path, "Project snapshot scope accepts kind only");
            return std::nullopt;
        }
        return scope;
    }
    if (kind == "definition")
    {
        scope.kind = SnapshotScopeKind::Definition;
        if (source.object().size() != 3)
            addError(result, path, "Definition snapshot scope requires kind, instancePath, and definitionId only");
        const auto hasPath = parseSnapshotInstancePath(
            result,
            source,
            path + ".instancePath",
            scope.instancePath);
        const auto hasDefinition = parseSnapshotIdentifier(
            result,
            source,
            "definitionId",
            path + ".definitionId",
            scope.definitionId);
        if (!hasPath || !hasDefinition || scope.instancePath.empty()
            || source.object().size() != 3)
        {
            if (hasPath && scope.instancePath.empty())
                addError(result, path + ".instancePath", "Definition snapshot scope requires a concrete instance path");
            return std::nullopt;
        }
        return scope;
    }
    if (kind == "node")
    {
        scope.kind = SnapshotScopeKind::Node;
        if (source.object().size() != 4)
            addError(result, path, "Node snapshot scope requires kind, instancePath, containingDefinitionId, and nodeId only");
        const auto hasPath = parseSnapshotInstancePath(
            result,
            source,
            path + ".instancePath",
            scope.instancePath);
        const auto hasContainingDefinition = parseNullableSnapshotIdentifier(
            result,
            source,
            "containingDefinitionId",
            path + ".containingDefinitionId",
            scope.containingDefinitionId);
        const auto hasNode = parseSnapshotIdentifier(
            result,
            source,
            "nodeId",
            path + ".nodeId",
            scope.nodeId);
        const auto hasConsistentContainer = scope.instancePath.empty()
            ? !scope.containingDefinitionId.has_value()
            : scope.containingDefinitionId.has_value();
        if (!hasConsistentContainer)
            addError(result, path, "Node snapshot path and containing definition must identify the same graph layer");
        if (!hasPath || !hasContainingDefinition || !hasNode
            || !hasConsistentContainer || source.object().size() != 4)
        {
            return std::nullopt;
        }
        return scope;
    }
    addError(result, path + ".kind", "Expected project, definition, or node snapshot scope");
    return std::nullopt;
}

std::optional<SnapshotTargetIdentity> parseSnapshotTarget(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    int sourceVersion)
{
    if (!source.isObject())
    {
        addError(result, path, "Expected object");
        return std::nullopt;
    }
    std::string kind;
    if (!requiredStringMember(result, source, "kind", path + ".kind", kind))
        return std::nullopt;

    SnapshotTargetIdentity target;
    if (kind == "eventValue")
    {
        target.kind = SnapshotTargetKind::EventValue;
        if (source.object().size() != 3)
            addError(result, path, "Event Value target requires kind, eventName, and domain only");
        std::string domain;
        const auto hasName = parseSnapshotIdentifier(
            result,
            source,
            "eventName",
            path + ".eventName",
            target.eventName);
        if (hasName && !isValidEventName(target.eventName))
        {
            addError(result, path + ".eventName", "Expected Event Value identifier");
            return std::nullopt;
        }
        const auto hasDomain = requiredStringMember(
            result,
            source,
            "domain",
            path + ".domain",
            domain);
        const auto parsedDomain = hasDomain
            ? parseValueDomain(domain)
            : std::nullopt;
        if (!parsedDomain.has_value())
            addError(result, path + ".domain", "Expected known Event Value domain");
        if (!hasName || !parsedDomain.has_value()
            || source.object().size() != 3)
        {
            return std::nullopt;
        }
        target.domain = *parsedDomain;
        return target;
    }
    if (kind == "nodeParameter")
    {
        target.kind = SnapshotTargetKind::NodeParameter;
        if (source.object().size() != 5)
            addError(result, path, "Node-parameter target requires kind, instancePath, containingDefinitionId, nodeId, and parameterId only");
        const auto hasPath = parseSnapshotInstancePath(
            result,
            source,
            path + ".instancePath",
            target.instancePath);
        const auto hasContainingDefinition = parseNullableSnapshotIdentifier(
            result,
            source,
            "containingDefinitionId",
            path + ".containingDefinitionId",
            target.containingDefinitionId);
        const auto hasNode = parseSnapshotIdentifier(
            result,
            source,
            "nodeId",
            path + ".nodeId",
            target.nodeId);
        const auto hasParameter = parseSnapshotIdentifier(
            result,
            source,
            "parameterId",
            path + ".parameterId",
            target.parameterId);
        const auto hasConsistentContainer = target.instancePath.empty()
            ? !target.containingDefinitionId.has_value()
            : target.containingDefinitionId.has_value();
        if (!hasConsistentContainer)
            addError(result, path, "Node-parameter path and containing definition must identify the same graph layer");
        if (!hasPath || !hasContainingDefinition || !hasNode || !hasParameter
            || !hasConsistentContainer || source.object().size() != 5)
        {
            return std::nullopt;
        }
        return target;
    }
    if (kind == "soundParameter")
    {
        target.kind = SnapshotTargetKind::SoundParameter;
        if (source.object().size() != 4)
            addError(result, path, "Sound-parameter target requires kind, instancePath, definitionId, and parameterId only");
        const auto hasPath = parseSnapshotInstancePath(
            result,
            source,
            path + ".instancePath",
            target.instancePath);
        const auto hasDefinition = parseSnapshotIdentifier(
            result,
            source,
            "definitionId",
            path + ".definitionId",
            target.definitionId);
        const auto hasParameter = parseSnapshotIdentifier(
            result,
            source,
            "parameterId",
            path + ".parameterId",
            target.parameterId);
        if (!hasPath || !hasDefinition || !hasParameter
            || target.instancePath.empty() || source.object().size() != 4)
        {
            if (hasPath && target.instancePath.empty())
                addError(result, path + ".instancePath", "Sound-parameter target requires a concrete instance path");
            return std::nullopt;
        }
        return target;
    }
    if (kind == "variable" || kind == "soundVariable")
    {
        const auto isSoundVariable = kind == "soundVariable";
        if (isSoundVariable && sourceVersion < versionTwentyNine)
        {
            addError(
                result,
                path + ".kind",
                "Sound-variable snapshot targets require format 29");
            return std::nullopt;
        }
        target.kind = isSoundVariable
            ? SnapshotTargetKind::SoundVariable
            : SnapshotTargetKind::Variable;
        const auto hasVariable = parseSnapshotIdentifier(
            result,
            source,
            "variableId",
            path + ".variableId",
            target.variableId);
        const auto hasCanonicalVariable = hasVariable
            && isValidProjectVariableIdentifier(target.variableId);
        if (hasVariable && !hasCanonicalVariable)
        {
            addError(
                result,
                path + ".variableId",
                "Expected variable identifier");
        }
        const auto* instancePath = objectMember(source, "instancePath");
        const auto* definitionId = objectMember(source, "definitionId");
        const auto* ownerKind = objectMember(source, "ownerKind");
        if (ownerKind != nullptr)
        {
            if (sourceVersion < versionTwentyEight
                || !ownerKind->isString()
                || ownerKind->string() != "workspaceSession"
                || instancePath != nullptr
                || definitionId != nullptr
                || !hasCanonicalVariable
                || source.object().size() != 3U)
            {
                addError(
                    result,
                    path,
                    isSoundVariable
                        ? "Workspace-session Sound-variable target requires kind, ownerKind workspaceSession, and variableId only in format 29"
                        : "Workspace-session variable target requires kind, ownerKind workspaceSession, and variableId only in format 28 or later");
                return std::nullopt;
            }
            target.variableOwnerKind =
                SnapshotVariableOwnerKind::WorkspaceSession;
            return target;
        }
        if (instancePath == nullptr && definitionId == nullptr)
        {
            if (!hasCanonicalVariable || source.object().size() != 2U)
            {
                addError(
                    result,
                    path,
                    isSoundVariable
                        ? "Project Sound-variable target requires kind and variableId only"
                        : "Project-variable target requires kind and variableId only");
                return std::nullopt;
            }
            return target;
        }
        const auto hasPath = parseSnapshotInstancePath(
            result,
            source,
            path + ".instancePath",
            target.instancePath);
        const auto hasDefinition = parseSnapshotIdentifier(
            result,
            source,
            "definitionId",
            path + ".definitionId",
            target.definitionId);
        if (!hasCanonicalVariable || !hasPath || !hasDefinition
            || target.instancePath.empty() || source.object().size() != 4U)
        {
            addError(
                result,
                path,
                isSoundVariable
                    ? "Sound-owned Sound-variable target requires kind, instancePath, definitionId, and variableId only"
                    : "Sound-owned scalar-variable target requires kind, instancePath, definitionId, and variableId only");
            return std::nullopt;
        }
        return target;
    }
    addError(result, path + ".kind", "Expected eventValue, nodeParameter, soundParameter, variable, or soundVariable target");
    return std::nullopt;
}

std::optional<SnapshotValue> parseSnapshotValue(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path)
{
    if (!source.isObject())
    {
        addError(result, path, "Expected object");
        return std::nullopt;
    }
    std::string kind;
    if (!requiredStringMember(result, source, "kind", path + ".kind", kind))
        return std::nullopt;

    SnapshotValue value;
    if (kind == "number")
    {
        if (source.object().size() != 3)
            addError(result, path, "Numeric snapshot value requires kind, domain, and value only");
        std::string domain;
        const auto hasDomain = requiredStringMember(
            result,
            source,
            "domain",
            path + ".domain",
            domain);
        const auto parsedDomain = hasDomain
            ? parseValueDomain(domain)
            : std::nullopt;
        if (!parsedDomain.has_value())
            addError(result, path + ".domain", "Expected known value domain");
        const auto hasValue = requiredFiniteNumberMember(
            result,
            source,
            "value",
            path + ".value",
            value.number);
        if (!parsedDomain.has_value() || !hasValue
            || source.object().size() != 3)
        {
            return std::nullopt;
        }
        value.domain = *parsedDomain;
        return value;
    }
    if (kind == "choice")
    {
        value.kind = SnapshotValueKind::Choice;
        if (source.object().size() != 2)
            addError(result, path, "Choice snapshot value requires kind and value only");
        if (!requiredStringMember(
                result,
                source,
                "value",
                path + ".value",
                value.choice)
            || value.choice.empty()
            || value.choice.size() > maximumSnapshotNameBytes
            || source.object().size() != 2)
        {
            if (value.choice.empty())
                addError(result, path + ".value", "Expected non-empty bounded choice token");
            return std::nullopt;
        }
        return value;
    }
    if (kind == "boolean")
    {
        value.kind = SnapshotValueKind::Boolean;
        if (source.object().size() != 2)
            addError(result, path, "Boolean snapshot value requires kind and value only");
        const auto* boolean = objectMember(source, "value");
        if (boolean == nullptr || !boolean->isBoolean())
        {
            addError(result, path + ".value", "Expected boolean");
            return std::nullopt;
        }
        if (source.object().size() != 2)
            return std::nullopt;
        value.boolean = boolean->boolean();
        return value;
    }
    addError(result, path + ".kind", "Expected number, choice, or boolean snapshot value");
    return std::nullopt;
}

void parseSnapshotLibrary(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "snapshotLibrary");
    if (source == nullptr || !source->isObject())
    {
        addError(result, "$.snapshotLibrary", "Expected required object");
        return;
    }
    if (source->object().size() != 2)
        addError(result, "$.snapshotLibrary", "Expected version and banks only");
    const auto* version = objectMember(*source, "version");
    if (version == nullptr || !version->isNumber()
        || version->number() != 1.0)
    {
        addError(result, "$.snapshotLibrary.version", "Expected snapshot-library version 1");
    }
    const auto* banks = objectMember(*source, "banks");
    if (banks == nullptr || !banks->isArray())
    {
        addError(result, "$.snapshotLibrary.banks", "Expected array");
    }
    else if (banks->array().size() > maximumSnapshotBankCount)
    {
        addError(
            result,
            "$.snapshotLibrary.banks",
            "Snapshot bank count exceeds the supported limit");
    }
    else
    {
        const auto snapshotErrorCountBefore = result.errors.size();
        ProjectSnapshotLibraryState library;
        std::unordered_set<std::string> bankIds;
        for (std::size_t bankIndex = 0;
             bankIndex < banks->array().size();
             ++bankIndex)
        {
            const auto& bankSource = banks->array()[bankIndex];
            const auto bankPath = "$.snapshotLibrary.banks["
                + std::to_string(bankIndex) + "]";
            if (!bankSource.isObject())
            {
                addError(result, bankPath, "Expected object");
                continue;
            }
            if (bankSource.object().size() != 6)
                addError(result, bankPath, "Expected id, name, scope, smoothingSeconds, parameterPolicies, and snapshots only");

            ProjectSnapshotBank bank;
            auto valid = parseSnapshotIdentifier(
                result,
                bankSource,
                "id",
                bankPath + ".id",
                bank.bankId);
            valid = parseSnapshotName(
                        result,
                        bankSource,
                        bankPath + ".name",
                        bank.displayName)
                && valid;
            const auto* scopeSource = objectMember(bankSource, "scope");
            const auto scope = scopeSource == nullptr
                ? std::optional<SnapshotScopeIdentity>{}
                : parseSnapshotScope(result, *scopeSource, bankPath + ".scope");
            if (scopeSource == nullptr)
                addError(result, bankPath + ".scope", "Expected object");
            if (scope.has_value())
                bank.scope = *scope;
            else
                valid = false;
            valid = requiredFiniteNumberMember(
                        result,
                        bankSource,
                        "smoothingSeconds",
                        bankPath + ".smoothingSeconds",
                        bank.smoothingSeconds)
                && valid;
            if (bank.smoothingSeconds < 0.0
                || bank.smoothingSeconds > maximumSnapshotSmoothingSeconds)
            {
                addError(result, bankPath + ".smoothingSeconds", "Expected smoothing time from 0 through 30 seconds");
                valid = false;
            }
            const auto* policies = objectMember(bankSource, "parameterPolicies");
            if (policies == nullptr || !policies->isArray())
            {
                addError(result, bankPath + ".parameterPolicies", "Expected array");
                valid = false;
            }
            else if (policies->array().size() > maximumSnapshotPoliciesPerBank)
            {
                addError(result, bankPath + ".parameterPolicies", "Snapshot policy count exceeds the supported limit");
                valid = false;
            }
            else
            {
                for (std::size_t policyIndex = 0;
                     policyIndex < policies->array().size();
                     ++policyIndex)
                {
                    const auto& policySource = policies->array()[policyIndex];
                    const auto policyPath = bankPath + ".parameterPolicies["
                        + std::to_string(policyIndex) + "]";
                    if (!policySource.isObject())
                    {
                        addError(result, policyPath, "Expected object");
                        valid = false;
                        continue;
                    }
                    if (policySource.object().size() != 3)
                    {
                        addError(result, policyPath, "Expected target, locked, and range only");
                        valid = false;
                    }
                    const auto* targetSource = objectMember(policySource, "target");
                    const auto target = targetSource == nullptr
                        ? std::optional<SnapshotTargetIdentity>{}
                        : parseSnapshotTarget(
                            result,
                            *targetSource,
                            policyPath + ".target",
                            sourceVersion);
                    if (targetSource == nullptr)
                        addError(result, policyPath + ".target", "Expected object");
                    const auto* locked = objectMember(policySource, "locked");
                    if (locked == nullptr || !locked->isBoolean())
                        addError(result, policyPath + ".locked", "Expected boolean");
                    const auto* range = objectMember(policySource, "range");
                    SnapshotParameterPolicy policy;
                    bool rangeValid = range != nullptr;
                    if (range == nullptr)
                    {
                        addError(result, policyPath + ".range", "Expected object or null");
                    }
                    else if (!range->isNull())
                    {
                        if (!range->isObject() || range->object().size() != 2)
                        {
                            addError(result, policyPath + ".range", "Expected minimum and maximum only");
                            rangeValid = false;
                        }
                        else
                        {
                            SnapshotParameterRange parsedRange;
                            const auto hasMinimum = requiredFiniteNumberMember(
                                result,
                                *range,
                                "minimum",
                                policyPath + ".range.minimum",
                                parsedRange.minimum);
                            const auto hasMaximum = requiredFiniteNumberMember(
                                result,
                                *range,
                                "maximum",
                                policyPath + ".range.maximum",
                                parsedRange.maximum);
                            rangeValid = hasMinimum && hasMaximum
                                && parsedRange.minimum <= parsedRange.maximum;
                            if (hasMinimum && hasMaximum
                                && parsedRange.minimum > parsedRange.maximum)
                            {
                                addError(result, policyPath + ".range", "Snapshot range minimum must not exceed maximum");
                            }
                            if (rangeValid)
                                policy.range = parsedRange;
                        }
                    }
                    if (!target.has_value()
                        || locked == nullptr || !locked->isBoolean()
                        || !rangeValid || policySource.object().size() != 3)
                    {
                        valid = false;
                        continue;
                    }
                    if (std::ranges::any_of(
                            bank.parameterPolicies,
                            [&](const auto& candidate) {
                                return candidate.target == *target;
                            }))
                    {
                        addError(result, policyPath + ".target", "Duplicate snapshot policy target");
                        valid = false;
                        continue;
                    }
                    policy.target = *target;
                    policy.locked = locked->boolean();
                    bank.parameterPolicies.push_back(std::move(policy));
                }
            }

            const auto* snapshots = objectMember(bankSource, "snapshots");
            if (snapshots == nullptr || !snapshots->isArray())
            {
                addError(result, bankPath + ".snapshots", "Expected array");
                valid = false;
            }
            else if (snapshots->array().size() > maximumSnapshotsPerBank)
            {
                addError(result, bankPath + ".snapshots", "Snapshot count exceeds the supported limit");
                valid = false;
            }
            else
            {
                std::unordered_set<std::string> snapshotIds;
                for (std::size_t snapshotIndex = 0;
                     snapshotIndex < snapshots->array().size();
                     ++snapshotIndex)
                {
                    const auto& snapshotSource = snapshots->array()[snapshotIndex];
                    const auto snapshotPath = bankPath + ".snapshots["
                        + std::to_string(snapshotIndex) + "]";
                    if (!snapshotSource.isObject())
                    {
                        addError(result, snapshotPath, "Expected object");
                        valid = false;
                        continue;
                    }
                    if (snapshotSource.object().size() != 3)
                    {
                        addError(result, snapshotPath, "Expected id, name, and values only");
                        valid = false;
                    }
                    ProjectSnapshot snapshot;
                    auto snapshotValid = parseSnapshotIdentifier(
                        result,
                        snapshotSource,
                        "id",
                        snapshotPath + ".id",
                        snapshot.snapshotId);
                    snapshotValid = parseSnapshotName(
                                        result,
                                        snapshotSource,
                                        snapshotPath + ".name",
                                        snapshot.displayName)
                        && snapshotValid;
                    if (!snapshotIds.insert(snapshot.snapshotId).second)
                    {
                        addError(result, snapshotPath + ".id", "Duplicate snapshot identity in bank");
                        snapshotValid = false;
                    }
                    const auto* values = objectMember(snapshotSource, "values");
                    if (values == nullptr || !values->isArray())
                    {
                        addError(result, snapshotPath + ".values", "Expected array");
                        snapshotValid = false;
                    }
                    else if (values->array().size() > maximumSnapshotValuesPerSnapshot)
                    {
                        addError(result, snapshotPath + ".values", "Snapshot value count exceeds the supported limit");
                        snapshotValid = false;
                    }
                    else
                    {
                        for (std::size_t valueIndex = 0;
                             valueIndex < values->array().size();
                             ++valueIndex)
                        {
                            const auto& valueSource = values->array()[valueIndex];
                            const auto valuePath = snapshotPath + ".values["
                                + std::to_string(valueIndex) + "]";
                            if (!valueSource.isObject()
                                || valueSource.object().size() != 2)
                            {
                                addError(result, valuePath, "Expected target and value only");
                                snapshotValid = false;
                                continue;
                            }
                            const auto* targetSource = objectMember(valueSource, "target");
                            const auto target = targetSource == nullptr
                                ? std::optional<SnapshotTargetIdentity>{}
                                : parseSnapshotTarget(
                                    result,
                                    *targetSource,
                                    valuePath + ".target",
                                    sourceVersion);
                            if (targetSource == nullptr)
                                addError(result, valuePath + ".target", "Expected object");
                            const auto* typedValueSource = objectMember(valueSource, "value");
                            const auto typedValue = typedValueSource == nullptr
                                ? std::optional<SnapshotValue>{}
                                : parseSnapshotValue(result, *typedValueSource, valuePath + ".value");
                            if (typedValueSource == nullptr)
                                addError(result, valuePath + ".value", "Expected object");
                            if (!target.has_value() || !typedValue.has_value())
                            {
                                snapshotValid = false;
                                continue;
                            }
                            if (std::ranges::any_of(
                                    snapshot.values,
                                    [&](const auto& candidate) {
                                        return candidate.target == *target;
                                    }))
                            {
                                addError(result, valuePath + ".target", "Duplicate snapshot value target");
                                snapshotValid = false;
                                continue;
                            }
                            snapshot.values.push_back({*target, *typedValue});
                        }
                    }
                    if (!snapshotValid || snapshotSource.object().size() != 3)
                    {
                        valid = false;
                        continue;
                    }
                    bank.snapshots.push_back(std::move(snapshot));
                }
            }
            if (!bankIds.insert(bank.bankId).second)
            {
                addError(result, bankPath + ".id", "Duplicate snapshot bank identity");
                valid = false;
            }
            if (!valid || bankSource.object().size() != 6)
                continue;
            library.banks.push_back(std::move(bank));
        }
        if (result.errors.size() == snapshotErrorCountBefore)
            result.document.snapshotLibrary = std::move(library);
        else
            addError(result, "$.snapshotLibrary.banks", "Invalid snapshot bank collection");
}
}

void parseSampleMapLibrary(
    ProjectLoadResult& result,
    const json::Value& root)
{
    const auto errorCountBefore = result.errors.size();
    const auto* source = objectMember(root, "sampleMapLibrary");
    if (source == nullptr || !source->isObject())
    {
        addError(result, "$.sampleMapLibrary", "Expected required object");
        return;
    }
    if (source->object().size() != 2)
        addError(result, "$.sampleMapLibrary", "Expected version and maps only");

    std::uint64_t schemaVersion = 0;
    if (!requiredUnsignedJsonIntegerMember(
            result,
            *source,
            "version",
            "$.sampleMapLibrary.version",
            1,
            schemaVersion)
        || schemaVersion != 1)
    {
        addError(
            result,
            "$.sampleMapLibrary.version",
            "Expected sample-map library version 1");
    }

    const auto* maps = objectMember(*source, "maps");
    if (maps == nullptr || !maps->isArray())
    {
        addError(result, "$.sampleMapLibrary.maps", "Expected array");
        return;
    }
    if (maps->array().size() > maximumProjectSampleMaps)
    {
        addError(
            result,
            "$.sampleMapLibrary.maps",
            "Sample-map count exceeds the supported limit");
        return;
    }

    const auto parseLoop = [&](
                               const json::Value& object,
                               std::string_view key,
                               const std::string& path,
                               std::optional<ProjectSampleLoop>& output) {
        const auto* value = objectMember(object, key);
        if (value == nullptr)
        {
            addError(result, path, "Expected object or null");
            return false;
        }
        if (value->isNull())
        {
            output.reset();
            return true;
        }
        if (!value->isObject())
        {
            addError(result, path, "Expected object or null");
            return false;
        }
        auto valid = value->object().size() == 3;
        if (!valid)
        {
            addError(
                result,
                path,
                "Expected mode, startFrame, and endFrameExclusive only");
        }
        ProjectSampleLoop loop;
        std::string mode;
        valid = requiredStringMember(
                    result,
                    *value,
                    "mode",
                    path + ".mode",
                    mode)
            && valid;
        if (mode == "forward")
            loop.mode = ProjectSampleLoopMode::Forward;
        else if (mode == "alternate")
            loop.mode = ProjectSampleLoopMode::Alternate;
        else
        {
            addError(
                result,
                path + ".mode",
                "Expected forward or alternate");
            valid = false;
        }
        valid = requiredUnsignedJsonIntegerMember(
                    result,
                    *value,
                    "startFrame",
                    path + ".startFrame",
                    maximumProjectAssetJsonInteger,
                    loop.startFrame)
            && valid;
        valid = requiredUnsignedJsonIntegerMember(
                    result,
                    *value,
                    "endFrameExclusive",
                    path + ".endFrameExclusive",
                    maximumProjectAssetJsonInteger,
                    loop.endFrameExclusive)
            && valid;
        if (valid)
            output = loop;
        return valid;
    };

    ProjectSampleMapLibraryState library;
    library.schemaVersion = static_cast<std::uint32_t>(schemaVersion);
    std::size_t totalRegionCount = 0;
    for (std::size_t mapIndex = 0;
         mapIndex < maps->array().size();
         ++mapIndex)
    {
        const auto& mapSource = maps->array()[mapIndex];
        const auto mapPath = "$.sampleMapLibrary.maps["
            + std::to_string(mapIndex) + "]";
        if (!mapSource.isObject())
        {
            addError(result, mapPath, "Expected object");
            continue;
        }
        auto valid = mapSource.object().size() == 3;
        if (!valid)
            addError(result, mapPath, "Expected id, name, and regions only");

        ProjectSampleMap map;
        valid = requiredStringMember(
                    result,
                    mapSource,
                    "id",
                    mapPath + ".id",
                    map.mapId)
            && valid;
        valid = requiredStringMember(
                    result,
                    mapSource,
                    "name",
                    mapPath + ".name",
                    map.displayName)
            && valid;
        const auto* regions = objectMember(mapSource, "regions");
        if (regions == nullptr || !regions->isArray())
        {
            addError(result, mapPath + ".regions", "Expected array");
            valid = false;
        }
        else if (regions->array().size() > maximumProjectSampleRegions
                 || totalRegionCount
                         > maximumProjectSampleRegions
                             - regions->array().size())
        {
            addError(
                result,
                mapPath + ".regions",
                "Sample-region count exceeds the supported limit");
            valid = false;
        }
        else
        {
            totalRegionCount += regions->array().size();
            for (std::size_t regionIndex = 0;
                 regionIndex < regions->array().size();
                 ++regionIndex)
            {
                const auto& regionSource = regions->array()[regionIndex];
                const auto regionPath = mapPath + ".regions["
                    + std::to_string(regionIndex) + "]";
                if (!regionSource.isObject())
                {
                    addError(result, regionPath, "Expected object");
                    valid = false;
                    continue;
                }
                auto regionValid = regionSource.object().size() == 10;
                if (!regionValid)
                {
                    addError(
                        result,
                        regionPath,
                        "Expected id, assetId, note/velocity bounds, reverse, and loop fields only");
                }

                ProjectSampleRegion region;
                regionValid = requiredStringMember(
                                  result,
                                  regionSource,
                                  "id",
                                  regionPath + ".id",
                                  region.regionId)
                    && regionValid;
                regionValid = requiredStringMember(
                                  result,
                                  regionSource,
                                  "assetId",
                                  regionPath + ".assetId",
                                  region.assetId)
                    && regionValid;
                const auto parseMidi = [&](
                                           std::string_view key,
                                           const std::string& path,
                                           std::uint8_t& output,
                                           std::uint64_t minimum) {
                    std::uint64_t value = 0;
                    if (!requiredUnsignedJsonIntegerMember(
                            result,
                            regionSource,
                            key,
                            path,
                            127,
                            value)
                        || value < minimum)
                    {
                        if (value < minimum)
                            addError(result, path, "Expected MIDI value from 1 through 127");
                        return false;
                    }
                    output = static_cast<std::uint8_t>(value);
                    return true;
                };
                regionValid = parseMidi(
                                  "rootNote",
                                  regionPath + ".rootNote",
                                  region.rootNote,
                                  0)
                    && regionValid;
                regionValid = parseMidi(
                                  "lowNote",
                                  regionPath + ".lowNote",
                                  region.lowNote,
                                  0)
                    && regionValid;
                regionValid = parseMidi(
                                  "highNote",
                                  regionPath + ".highNote",
                                  region.highNote,
                                  0)
                    && regionValid;
                regionValid = parseMidi(
                                  "lowVelocity",
                                  regionPath + ".lowVelocity",
                                  region.lowVelocity,
                                  1)
                    && regionValid;
                regionValid = parseMidi(
                                  "highVelocity",
                                  regionPath + ".highVelocity",
                                  region.highVelocity,
                                  1)
                    && regionValid;
                const auto* reverse = objectMember(regionSource, "reverse");
                if (reverse == nullptr || !reverse->isBoolean())
                {
                    addError(result, regionPath + ".reverse", "Expected boolean");
                    regionValid = false;
                }
                else
                {
                    region.reverse = reverse->boolean();
                }
                regionValid = parseLoop(
                                  regionSource,
                                  "sustainLoop",
                                  regionPath + ".sustainLoop",
                                  region.sustainLoop)
                    && regionValid;
                regionValid = parseLoop(
                                  regionSource,
                                  "releaseLoop",
                                  regionPath + ".releaseLoop",
                                  region.releaseLoop)
                    && regionValid;
                if (regionValid)
                    map.regions.push_back(std::move(region));
                else
                    valid = false;
            }
        }
        if (valid)
            library.maps.push_back(std::move(map));
    }

    if (result.errors.size() != errorCountBefore)
        return;
    result.document.sampleMapLibrary = std::move(library);
    for (const auto& diagnostic :
         projectSampleMapDiagnostics(result.document))
    {
        addError(result, diagnostic.path, diagnostic.message);
    }
}

// Format 156: how the project's fields read rounded and truncated, a beat and a number without units
// given to inSOSPitch; each is absent where it is the default, as in every
// project before format 156.
void parseFieldReadingSettings(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    auto& definitions = result.document.expressionDefinitions;
    if (const auto* rule = objectMember(root, "roundingRule"); rule != nullptr)
    {
        if (sourceVersion < versionOneHundredFiftySix)
            addError(result, "$.roundingRule", "roundingRule requires format 156");
        else if (!rule->isString() || rule->string() != "manualTable")
            addError(result, "$.roundingRule", "Expected manualTable");
        else
            definitions.rounding = talas::expr::RoundingRule::ManualTable;
    }
    if (const auto* tempo = objectMember(root, "fieldBeatTempoBpm"); tempo != nullptr)
    {
        if (sourceVersion < versionOneHundredFiftySix)
            addError(result, "$.fieldBeatTempoBpm", "fieldBeatTempoBpm requires format 156");
        else if (!tempo->isNumber() || !std::isfinite(tempo->number())
            || tempo->number() < talas::expr::minimumFieldBeatTempoBpm
            || tempo->number() > talas::expr::maximumFieldBeatTempoBpm)
        {
            addError(result, "$.fieldBeatTempoBpm", "fieldBeatTempoBpm must be from 1 to 1000");
        }
        else
            definitions.fieldBeatTempoBpm = tempo->number();
    }
    if (const auto* sosPitch = objectMember(root, "unitlessSOSPitch"); sosPitch != nullptr)
    {
        if (sourceVersion < versionOneHundredFiftySix)
            addError(result, "$.unitlessSOSPitch", "unitlessSOSPitch requires format 156");
        else if (!sosPitch->isString() || sosPitch->string() != "linear")
            addError(result, "$.unitlessSOSPitch", "Expected linear");
        else
            definitions.unitlessSOSPitch = talas::expr::UnitlessSOSPitch::Linear;
    }
}

void parseExpressionDefinitions(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    // The timecode frame rate that converts SMPTE times to seconds, from
    // format 68; 30 before.
    std::uint8_t timecodeFramesPerSecond = 30;
    if (const auto* rate = objectMember(root, "timecodeFramesPerSecond"); rate != nullptr)
    {
        std::int32_t parsedRate = 0;
        if (sourceVersion < versionSixtyEight)
        {
            addError(result, "$.timecodeFramesPerSecond",
                "Timecode frames per second requires format 68");
        }
        else if (!rate->isNumber()
            || !parseSigned32BitIntegerToken(rate->numberToken(), parsedRate)
            || (parsedRate != 24 && parsedRate != 25 && parsedRate != 30))
        {
            addError(result, "$.timecodeFramesPerSecond",
                "Timecode frames per second must be 24, 25 or 30");
        }
        else
        {
            timecodeFramesPerSecond = static_cast<std::uint8_t>(parsedRate);
        }
    }
    result.document.expressionDefinitions.timecodeFramesPerSecond = timecodeFramesPerSecond;
    const auto* source = objectMember(root, "expressionDefinitions");
    if (sourceVersion < versionTwentyFive)
    {
        if (source != nullptr)
        {
            addError(
                result,
                "$.expressionDefinitions",
                "Expression definitions require format 25");
        }
        return;
    }
    if (source == nullptr || !source->isArray())
    {
        addError(
            result,
            "$.expressionDefinitions",
            "Expected required expression definition array");
        return;
    }

    std::size_t aggregateSourceBytes = 0;
    for (std::size_t index = 0; index < source->array().size(); ++index)
    {
        const auto& candidate = source->array()[index];
        const auto* body = candidate.isObject()
            ? objectMember(candidate, "source")
            : nullptr;
        if (body != nullptr && body->isString())
        {
            if (body->string().size()
                > talas::expr::ExpressionLimits::maximumDefinitionSourceBytesTotal
                    - aggregateSourceBytes)
            {
                addError(
                    result,
                    "$.expressionDefinitions[" + std::to_string(index)
                        + "].source",
                    "Expression definition sources exceed their fixed aggregate byte budget");
                return;
            }
            aggregateSourceBytes += body->string().size();
        }
    }

    const auto errorCountBefore = result.errors.size();
    talas::expr::ExpressionDefinitionLibrary library;
    library.timecodeFramesPerSecond = timecodeFramesPerSecond;
    library.definitions.reserve(source->array().size());
    for (std::size_t definitionIndex = 0;
         definitionIndex < source->array().size();
         ++definitionIndex)
    {
        const auto& definitionSource = source->array()[definitionIndex];
        const auto definitionPath = "$.expressionDefinitions["
            + std::to_string(definitionIndex) + "]";
        if (!definitionSource.isObject()
            || definitionSource.object().size() != 4U)
        {
            addError(
                result,
                definitionPath,
                "Expected name, parameters, resultDomain, and source only");
            continue;
        }

        talas::expr::ExpressionDefinition definition;
        auto valid = requiredStringMember(
            result,
            definitionSource,
            "name",
            definitionPath + ".name",
            definition.name);
        valid = requiredStringMember(
                    result,
                    definitionSource,
                    "source",
                    definitionPath + ".source",
                    definition.source)
            && valid;

        const auto* resultDomain =
            objectMember(definitionSource, "resultDomain");
        if (resultDomain == nullptr || !resultDomain->isString())
        {
            addError(
                result,
                definitionPath + ".resultDomain",
                "Expected value domain");
            valid = false;
        }
        else if (const auto parsedDomain =
                     parseValueDomain(resultDomain->string()))
        {
            definition.resultDomain = *parsedDomain;
        }
        else
        {
            addError(
                result,
                definitionPath + ".resultDomain",
                "Unknown value domain");
            valid = false;
        }

        const auto* parameters = objectMember(definitionSource, "parameters");
        if (parameters == nullptr || !parameters->isArray())
        {
            addError(
                result,
                definitionPath + ".parameters",
                "Expected parameter array");
            valid = false;
        }
        else
        {
            definition.parameters.reserve(parameters->array().size());
            for (std::size_t parameterIndex = 0;
                 parameterIndex < parameters->array().size();
                 ++parameterIndex)
            {
                const auto& parameterSource =
                    parameters->array()[parameterIndex];
                const auto parameterPath = definitionPath + ".parameters["
                    + std::to_string(parameterIndex) + "]";
                if (!parameterSource.isObject()
                    || parameterSource.object().size() != 2U)
                {
                    addError(
                        result,
                        parameterPath,
                        "Expected name and domain only");
                    valid = false;
                    continue;
                }
                talas::expr::ExpressionDefinitionParameter parameter;
                auto parameterValid = requiredStringMember(
                    result,
                    parameterSource,
                    "name",
                    parameterPath + ".name",
                    parameter.name);
                const auto* domain = objectMember(parameterSource, "domain");
                if (domain == nullptr || !domain->isString())
                {
                    addError(
                        result,
                        parameterPath + ".domain",
                        "Expected value domain");
                    parameterValid = false;
                }
                else if (const auto parsedDomain =
                             parseValueDomain(domain->string()))
                {
                    parameter.domain = *parsedDomain;
                }
                else
                {
                    addError(
                        result,
                        parameterPath + ".domain",
                        "Unknown value domain");
                    parameterValid = false;
                }
                if (parameterValid)
                    definition.parameters.push_back(std::move(parameter));
                else
                    valid = false;
            }
        }
        if (valid)
            library.definitions.push_back(std::move(definition));
    }

    if (result.errors.size() != errorCountBefore)
        return;
    const auto validated = talas::expr::parse(
        "0",
        result.document.expressionLanguage,
        library);
    if (!validated.ok())
    {
        for (const auto& diagnostic : validated.diagnostics)
        {
            const auto path = diagnostic.definitionIndex.has_value()
                    && *diagnostic.definitionIndex
                        < library.definitions.size()
                ? "$.expressionDefinitions["
                    + library.definitions[*diagnostic.definitionIndex].name
                    + "].source"
                : "$.expressionDefinitions";
            addError(
                result,
                path,
                diagnostic.message + " at bytes "
                    + std::to_string(diagnostic.range.offset) + ":"
                    + std::to_string(diagnostic.range.length));
        }
        return;
    }
    std::ranges::sort(
        library.definitions,
        {},
        &talas::expr::ExpressionDefinition::name);
    result.document.expressionDefinitions = std::move(library);
}

void parseVariableLibrary(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "variableLibrary");
    if (sourceVersion < versionTwentySeven)
    {
        if (source != nullptr)
        {
            addError(
                result,
                "$.variableLibrary",
                "Variable library requires format 27");
        }
        result.document.variableLibrary = {};
        return;
    }
    if (source == nullptr || !source->isObject()
        || source->object().size() != 2U)
    {
        addError(
            result,
            "$.variableLibrary",
            "Expected version and variables only");
        return;
    }

    ProjectVariableLibrary library;
    const auto* version = objectMember(*source, "version");
    std::int32_t parsedVersion = 0;
    if (version == nullptr || !version->isNumber()
        || !parseSigned32BitIntegerToken(
            version->numberToken(), parsedVersion)
        || parsedVersion
            != (sourceVersion == versionTwentySeven ? 1 : 2))
    {
        addError(
            result,
            "$.variableLibrary.version",
            sourceVersion == versionTwentySeven
                ? "Expected exact variable library version 1"
                : "Expected exact variable library version 2");
    }
    else
    {
        library.version = 2;
    }

    const auto* variables = objectMember(*source, "variables");
    if (variables == nullptr || !variables->isArray())
    {
        addError(
            result,
            "$.variableLibrary.variables",
            "Expected variable declaration array");
        return;
    }
    const auto errorCountBefore = result.errors.size();
    library.variables.reserve(variables->array().size());
    for (std::size_t index = 0;
         index < variables->array().size();
         ++index)
    {
        const auto& candidate = variables->array()[index];
        const auto path = "$.variableLibrary.variables["
            + std::to_string(index) + "]";
        if (!candidate.isObject() || candidate.object().size() != 6U)
        {
            addError(
                result,
                path,
                "Expected variableId, displayName, domain, initialValue, owner, and snapshotEligible only");
            continue;
        }

        ProjectVariableDeclaration declaration;
        auto valid = requiredStringMember(
            result,
            candidate,
            "variableId",
            path + ".variableId",
            declaration.variableId);
        valid = requiredStringMember(
                    result,
                    candidate,
                    "displayName",
                    path + ".displayName",
                    declaration.displayName)
            && valid;

        const auto* domain = objectMember(candidate, "domain");
        if (domain == nullptr || !domain->isString())
        {
            addError(result, path + ".domain", "Expected value domain");
            valid = false;
        }
        else if (const auto parsedDomain = parseValueDomain(domain->string()))
        {
            declaration.domain = *parsedDomain;
        }
        else
        {
            addError(result, path + ".domain", "Unknown value domain");
            valid = false;
        }

        const auto* initialValue = objectMember(candidate, "initialValue");
        if (initialValue == nullptr || !initialValue->isNumber()
            || !std::isfinite(initialValue->number()))
        {
            addError(
                result,
                path + ".initialValue",
                "Expected finite numeric initial value");
            valid = false;
        }
        else
        {
            declaration.initialValue = initialValue->number();
        }

        const auto* owner = objectMember(candidate, "owner");
        const auto* ownerKind = owner != nullptr && owner->isObject()
            ? objectMember(*owner, "kind")
            : nullptr;
        if (owner == nullptr || !owner->isObject()
            || ownerKind == nullptr || !ownerKind->isString())
        {
            addError(result, path + ".owner", "Expected variable owner");
            valid = false;
        }
        else if (ownerKind->string() == "project")
        {
            if (owner->object().size() != 1U)
            {
                addError(
                    result,
                    path + ".owner",
                    "Project owner accepts kind only");
                valid = false;
            }
            declaration.owner = {};
        }
        else if (ownerKind->string() == "soundDefinition")
        {
            if (owner->object().size() != 2U
                || !requiredStringMember(
                    result,
                    *owner,
                    "definitionId",
                    path + ".owner.definitionId",
                    declaration.owner.definitionId))
            {
                addError(
                    result,
                    path + ".owner",
                    "Sound-definition owner requires kind and definitionId only");
                valid = false;
            }
            declaration.owner.kind =
                ProjectVariableOwnerKind::SoundDefinition;
        }
        else if (sourceVersion >= versionTwentyEight
                 && ownerKind->string() == "midiVoice")
        {
            const auto* definitionId = objectMember(
                *owner,
                "definitionId");
            const auto expectedMembers = definitionId == nullptr
                ? 2U
                : 3U;
            if (owner->object().size() != expectedMembers
                || !requiredStringMember(
                    result,
                    *owner,
                    "nodeId",
                    path + ".owner.nodeId",
                    declaration.owner.nodeId)
                || (definitionId != nullptr
                    && !requiredStringMember(
                        result,
                        *owner,
                        "definitionId",
                        path + ".owner.definitionId",
                        declaration.owner.definitionId)))
            {
                addError(
                    result,
                    path + ".owner",
                    "MIDI Voice owner requires kind, nodeId, and optional definitionId only");
                valid = false;
            }
            declaration.owner.kind =
                ProjectVariableOwnerKind::MidiVoice;
        }
        else if (sourceVersion >= versionTwentyEight
                 && ownerKind->string() == "workspaceSession")
        {
            if (owner->object().size() != 1U)
            {
                addError(
                    result,
                    path + ".owner",
                    "Workspace-session owner accepts kind only");
                valid = false;
            }
            declaration.owner.kind =
                ProjectVariableOwnerKind::WorkspaceSession;
        }
        else
        {
            addError(
                result,
                path + ".owner.kind",
                "Unknown variable owner kind");
            valid = false;
        }

        const auto* snapshotEligible =
            objectMember(candidate, "snapshotEligible");
        if (snapshotEligible == nullptr || !snapshotEligible->isBoolean())
        {
            addError(
                result,
                path + ".snapshotEligible",
                "Expected snapshot eligibility boolean");
            valid = false;
        }
        else
        {
            declaration.snapshotEligible = snapshotEligible->boolean();
        }
        if (valid)
            library.variables.push_back(std::move(declaration));
    }
    if (result.errors.size() != errorCountBefore)
        return;
    result.document.variableLibrary = std::move(library);
}

void parseSoundVariableLibrary(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "soundVariableLibrary");
    if (sourceVersion < versionTwentyNine)
    {
        if (source != nullptr)
        {
            addError(
                result,
                "$.soundVariableLibrary",
                "Sound-variable library requires format 29");
        }
        result.document.soundVariableLibrary = {};
        return;
    }
    if (source == nullptr || !source->isObject()
        || source->object().size() != 2U)
    {
        addError(
            result,
            "$.soundVariableLibrary",
            "Expected version and variables only");
        return;
    }

    ProjectSoundVariableLibrary library;
    const auto* version = objectMember(*source, "version");
    std::int32_t parsedVersion = 0;
    if (version == nullptr || !version->isNumber()
        || !parseSigned32BitIntegerToken(
            version->numberToken(), parsedVersion)
        || parsedVersion != 1)
    {
        addError(
            result,
            "$.soundVariableLibrary.version",
            "Expected exact Sound-variable library version 1");
    }

    const auto* variables = objectMember(*source, "variables");
    if (variables == nullptr || !variables->isArray())
    {
        addError(
            result,
            "$.soundVariableLibrary.variables",
            "Expected Sound-variable declaration array");
        return;
    }
    const auto errorCountBefore = result.errors.size();
    library.variables.reserve(variables->array().size());
    for (std::size_t index = 0;
         index < variables->array().size();
         ++index)
    {
        const auto& candidate = variables->array()[index];
        const auto path = "$.soundVariableLibrary.variables["
            + std::to_string(index) + "]";
        if (!candidate.isObject() || candidate.object().size() != 7U)
        {
            addError(
                result,
                path,
                "Expected variableId, displayName, kind, owner, candidateDefinitionIds, initialSelectedOrdinal, and snapshotEligible only");
            continue;
        }

        ProjectSoundVariableDeclaration declaration;
        auto valid = requiredStringMember(
            result,
            candidate,
            "variableId",
            path + ".variableId",
            declaration.variableId);
        valid = requiredStringMember(
                    result,
                    candidate,
                    "displayName",
                    path + ".displayName",
                    declaration.displayName)
            && valid;
        const auto* kind = objectMember(candidate, "kind");
        if (kind == nullptr || !kind->isString())
        {
            addError(result, path + ".kind", "Expected Sound-variable kind");
            valid = false;
        }
        else if (kind->string() == "sound")
        {
            declaration.kind = ProjectSoundVariableKind::Sound;
        }
        else if (kind->string() == "soundCollection")
        {
            declaration.kind = ProjectSoundVariableKind::SoundCollection;
        }
        else
        {
            addError(result, path + ".kind", "Unknown Sound-variable kind");
            valid = false;
        }

        const auto* owner = objectMember(candidate, "owner");
        const auto* ownerKind = owner != nullptr && owner->isObject()
            ? objectMember(*owner, "kind")
            : nullptr;
        if (owner == nullptr || !owner->isObject()
            || ownerKind == nullptr || !ownerKind->isString())
        {
            addError(result, path + ".owner", "Expected Sound-variable owner");
            valid = false;
        }
        else if (ownerKind->string() == "project"
                 || ownerKind->string() == "workspaceSession")
        {
            if (owner->object().size() != 1U)
            {
                addError(
                    result,
                    path + ".owner",
                    "Global Sound-variable owner accepts kind only");
                valid = false;
            }
            declaration.owner.kind = ownerKind->string() == "project"
                ? ProjectVariableOwnerKind::Project
                : ProjectVariableOwnerKind::WorkspaceSession;
        }
        else if (ownerKind->string() == "soundDefinition")
        {
            if (owner->object().size() != 2U
                || !requiredStringMember(
                    result,
                    *owner,
                    "definitionId",
                    path + ".owner.definitionId",
                    declaration.owner.definitionId))
            {
                addError(
                    result,
                    path + ".owner",
                    "Sound-definition owner requires kind and definitionId only");
                valid = false;
            }
            declaration.owner.kind =
                ProjectVariableOwnerKind::SoundDefinition;
        }
        else if (ownerKind->string() == "midiVoice")
        {
            const auto* definitionId = objectMember(*owner, "definitionId");
            const auto expectedMembers = definitionId == nullptr ? 2U : 3U;
            if (owner->object().size() != expectedMembers
                || !requiredStringMember(
                    result,
                    *owner,
                    "nodeId",
                    path + ".owner.nodeId",
                    declaration.owner.nodeId)
                || (definitionId != nullptr
                    && !requiredStringMember(
                        result,
                        *owner,
                        "definitionId",
                        path + ".owner.definitionId",
                        declaration.owner.definitionId)))
            {
                addError(
                    result,
                    path + ".owner",
                    "MIDI Voice owner requires kind, nodeId, and optional definitionId only");
                valid = false;
            }
            declaration.owner.kind = ProjectVariableOwnerKind::MidiVoice;
        }
        else
        {
            addError(
                result,
                path + ".owner.kind",
                "Unknown Sound-variable owner kind");
            valid = false;
        }

        const auto* candidateIds =
            objectMember(candidate, "candidateDefinitionIds");
        if (candidateIds == nullptr || !candidateIds->isArray())
        {
            addError(
                result,
                path + ".candidateDefinitionIds",
                "Expected Sound definition ID array");
            valid = false;
        }
        else
        {
            declaration.candidateDefinitionIds.reserve(
                candidateIds->array().size());
            for (std::size_t candidateIndex = 0;
                 candidateIndex < candidateIds->array().size();
                 ++candidateIndex)
            {
                const auto& definitionId =
                    candidateIds->array()[candidateIndex];
                if (!definitionId.isString())
                {
                    addError(
                        result,
                        path + ".candidateDefinitionIds["
                            + std::to_string(candidateIndex) + "]",
                        "Expected Sound definition ID string");
                    valid = false;
                    continue;
                }
                declaration.candidateDefinitionIds.push_back(
                    definitionId.string());
            }
        }

        const auto* initialSelectedOrdinal =
            objectMember(candidate, "initialSelectedOrdinal");
        std::int32_t parsedOrdinal = 0;
        if (initialSelectedOrdinal == nullptr
            || !initialSelectedOrdinal->isNumber()
            || !parseSigned32BitIntegerToken(
                initialSelectedOrdinal->numberToken(),
                parsedOrdinal)
            || parsedOrdinal < 0
            || parsedOrdinal > std::numeric_limits<std::uint16_t>::max())
        {
            addError(
                result,
                path + ".initialSelectedOrdinal",
                "Expected exact unsigned 16-bit selection ordinal");
            valid = false;
        }
        else
        {
            declaration.initialSelectedOrdinal =
                static_cast<std::uint16_t>(parsedOrdinal);
        }

        const auto* snapshotEligible =
            objectMember(candidate, "snapshotEligible");
        if (snapshotEligible == nullptr || !snapshotEligible->isBoolean())
        {
            addError(
                result,
                path + ".snapshotEligible",
                "Expected snapshot eligibility boolean");
            valid = false;
        }
        else
        {
            declaration.snapshotEligible = snapshotEligible->boolean();
        }
        if (valid)
            library.variables.push_back(std::move(declaration));
    }
    if (result.errors.size() != errorCountBefore)
        return;
    result.document.soundVariableLibrary = std::move(library);
}

bool parseGeneratorOutputProfile(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    ProjectGeneratorOutputProfile& output,
    int sourceVersion)
{
    if (!source.isObject() || source.object().size() != 4U)
    {
        addError(
            result,
            path,
            "Expected version, timelineStepMicroseconds, scoreStepTicks, and bindings only");
        return false;
    }
    bool valid = true;
    std::uint64_t profileVersion = 0;
    valid = requiredExactUnsignedIntegerMember(
                result,
                source,
                "version",
                path + ".version",
                projectGeneratorOutputProfileVersion,
                profileVersion)
        && valid;
    if (profileVersion != projectGeneratorOutputProfileVersion)
    {
        addError(
            result,
            path + ".version",
            "Expected exact generator output profile version 1");
        valid = false;
    }
    std::uint64_t timelineStep = 0;
    valid = requiredExactUnsignedIntegerMember(
                result,
                source,
                "timelineStepMicroseconds",
                path + ".timelineStepMicroseconds",
                maximumProjectGeneratorTimelineMicroseconds,
                timelineStep)
        && valid;
    std::uint64_t scoreStep = 0;
    valid = requiredExactUnsignedIntegerMember(
                result,
                source,
                "scoreStepTicks",
                path + ".scoreStepTicks",
                std::numeric_limits<std::uint64_t>::max(),
                scoreStep)
        && valid;
    const auto* bindings = objectMember(source, "bindings");
    if (bindings == nullptr || !bindings->isArray()
        || bindings->array().size()
            > maximumProjectGeneratorTerminalBindings)
    {
        addError(
            result,
            path + ".bindings",
            "Expected bounded generator terminal-binding array");
        return false;
    }

    ProjectGeneratorOutputProfile candidate;
    candidate.version = static_cast<int>(profileVersion);
    candidate.timelineStepMicroseconds = timelineStep;
    candidate.scoreStepTicks = scoreStep;
    candidate.bindings.reserve(bindings->array().size());
    for (std::size_t bindingIndex = 0;
         bindingIndex < bindings->array().size();
         ++bindingIndex)
    {
        const auto& bindingSource = bindings->array()[bindingIndex];
        const auto bindingPath = path + ".bindings["
            + std::to_string(bindingIndex) + "]";
        if (!bindingSource.isObject()
            || bindingSource.object().size() != 2U)
        {
            addError(
                result,
                bindingPath,
                "Expected terminal and actions only");
            valid = false;
            continue;
        }
        ProjectGeneratorTerminalBinding binding;
        auto bindingValid = requiredStringMember(
            result,
            bindingSource,
            "terminal",
            bindingPath + ".terminal",
            binding.terminal);
        const auto* actions = objectMember(bindingSource, "actions");
        if (actions == nullptr || !actions->isArray()
            || actions->array().size()
                > maximumProjectGeneratorActionsPerTerminal)
        {
            addError(
                result,
                bindingPath + ".actions",
                "Expected bounded generator output-action array");
            bindingValid = false;
        }
        else
        {
            binding.actions.reserve(actions->array().size());
            for (std::size_t actionIndex = 0;
                 actionIndex < actions->array().size();
                 ++actionIndex)
            {
                const auto& actionSource = actions->array()[actionIndex];
                const auto actionPath = bindingPath + ".actions["
                    + std::to_string(actionIndex) + "]";
                if (!actionSource.isObject())
                {
                    addError(result, actionPath, "Expected generator output action object");
                    bindingValid = false;
                    continue;
                }
                std::string kind;
                if (!requiredStringMember(
                        result,
                        actionSource,
                        "kind",
                        actionPath + ".kind",
                        kind))
                {
                    bindingValid = false;
                    continue;
                }
                if (kind == "timelineEventValue")
                {
                    if (actionSource.object().size() != 5U)
                    {
                        addError(
                            result,
                            actionPath,
                            "Expected kind, eventName, domain, value, and interpolation only");
                        bindingValid = false;
                        continue;
                    }
                    ProjectGeneratorTimelineEventAction action;
                    auto actionValid = requiredStringMember(
                        result,
                        actionSource,
                        "eventName",
                        actionPath + ".eventName",
                        action.eventName);
                    std::string domain;
                    actionValid = requiredStringMember(
                                      result,
                                      actionSource,
                                      "domain",
                                      actionPath + ".domain",
                                      domain)
                        && actionValid;
                    const auto parsedDomain = parseValueDomain(domain);
                    if (!parsedDomain.has_value())
                    {
                        addError(result, actionPath + ".domain", "Unknown value domain");
                        actionValid = false;
                    }
                    else
                    {
                        action.domain = *parsedDomain;
                    }
                    actionValid = requiredFiniteNumberMember(
                                      result,
                                      actionSource,
                                      "value",
                                      actionPath + ".value",
                                      action.value)
                        && actionValid;
                    std::string interpolation;
                    actionValid = requiredStringMember(
                                      result,
                                      actionSource,
                                      "interpolation",
                                      actionPath + ".interpolation",
                                      interpolation)
                        && actionValid;
                    const auto parsedInterpolation =
                        parseAutomationInterpolation(interpolation);
                    if (!parsedInterpolation.has_value())
                    {
                        addError(
                            result,
                            actionPath + ".interpolation",
                            "Unknown automation interpolation");
                        actionValid = false;
                    }
                    else
                    {
                        action.interpolation = *parsedInterpolation;
                    }
                    if (actionValid)
                        binding.actions.push_back(std::move(action));
                    bindingValid = actionValid && bindingValid;
                }
                else if (kind == "scoreClip")
                {
                    if (actionSource.object().size() != 8U)
                    {
                        addError(
                            result,
                            actionPath,
                            "Expected closed Score clip action fields only");
                        bindingValid = false;
                        continue;
                    }
                    ProjectGeneratorScoreClipAction action;
                    auto actionValid = requiredStringMember(
                        result,
                        actionSource,
                        "trackId",
                        actionPath + ".trackId",
                        action.trackId);
                    actionValid = requiredStringMember(
                                      result,
                                      actionSource,
                                      "soundDefinitionId",
                                      actionPath + ".soundDefinitionId",
                                      action.soundDefinitionId)
                        && actionValid;
                    std::uint64_t repeatCount = 0;
                    actionValid = requiredExactUnsignedIntegerMember(
                                      result,
                                      actionSource,
                                      "durationTicks",
                                      actionPath + ".durationTicks",
                                      std::numeric_limits<std::uint64_t>::max(),
                                      action.durationTicks)
                        && actionValid;
                    actionValid = requiredExactUnsignedIntegerMember(
                                      result,
                                      actionSource,
                                      "contentOffsetTicks",
                                      actionPath + ".contentOffsetTicks",
                                      std::numeric_limits<std::uint64_t>::max(),
                                      action.contentOffsetTicks)
                        && actionValid;
                    actionValid = requiredExactUnsignedIntegerMember(
                                      result,
                                      actionSource,
                                      "fadeInTicks",
                                      actionPath + ".fadeInTicks",
                                      std::numeric_limits<std::uint64_t>::max(),
                                      action.fadeInTicks)
                        && actionValid;
                    actionValid = requiredExactUnsignedIntegerMember(
                                      result,
                                      actionSource,
                                      "fadeOutTicks",
                                      actionPath + ".fadeOutTicks",
                                      std::numeric_limits<std::uint64_t>::max(),
                                      action.fadeOutTicks)
                        && actionValid;
                    actionValid = requiredExactUnsignedIntegerMember(
                                      result,
                                      actionSource,
                                      "repeatCount",
                                      actionPath + ".repeatCount",
                                      maximumScoreClipRepeats,
                                      repeatCount)
                        && actionValid;
                    action.repeatCount = static_cast<std::uint32_t>(repeatCount);
                    if (actionValid)
                        binding.actions.push_back(std::move(action));
                    bindingValid = actionValid && bindingValid;
                }
                else if (kind == "scoreAutomationPoint")
                {
                    if (actionSource.object().size() != 5U)
                    {
                        addError(
                            result,
                            actionPath,
                            "Expected closed Score automation action fields only");
                        bindingValid = false;
                        continue;
                    }
                    ProjectGeneratorScoreAutomationAction action;
                    auto actionValid = requiredStringMember(
                        result,
                        actionSource,
                        "laneId",
                        actionPath + ".laneId",
                        action.laneId);
                    std::string domain;
                    actionValid = requiredStringMember(
                                      result,
                                      actionSource,
                                      "domain",
                                      actionPath + ".domain",
                                      domain)
                        && actionValid;
                    const auto parsedDomain = parseValueDomain(domain);
                    if (!parsedDomain.has_value())
                    {
                        addError(result, actionPath + ".domain", "Unknown value domain");
                        actionValid = false;
                    }
                    else
                    {
                        action.domain = *parsedDomain;
                    }
                    actionValid = requiredFiniteNumberMember(
                                      result,
                                      actionSource,
                                      "value",
                                      actionPath + ".value",
                                      action.value)
                        && actionValid;
                    std::string interpolation;
                    actionValid = requiredStringMember(
                                      result,
                                      actionSource,
                                      "interpolation",
                                      actionPath + ".interpolation",
                                      interpolation)
                        && actionValid;
                    const auto parsedInterpolation =
                        parseAutomationInterpolation(interpolation);
                    if (!parsedInterpolation.has_value())
                    {
                        addError(
                            result,
                            actionPath + ".interpolation",
                            "Unknown automation interpolation");
                        actionValid = false;
                    }
                    else
                    {
                        action.interpolation = *parsedInterpolation;
                    }
                    if (actionValid)
                        binding.actions.push_back(std::move(action));
                    bindingValid = actionValid && bindingValid;
                }
                else if (kind == "scoreMarker")
                {
                    if (actionSource.object().size() != 3U)
                    {
                        addError(
                            result,
                            actionPath,
                            "Expected kind, title, and durationTicks only");
                        bindingValid = false;
                        continue;
                    }
                    ProjectGeneratorScoreMarkerAction action;
                    auto actionValid = requiredStringMember(
                        result,
                        actionSource,
                        "title",
                        actionPath + ".title",
                        action.title);
                    const auto* duration = objectMember(
                        actionSource,
                        "durationTicks");
                    if (duration == nullptr)
                    {
                        addError(
                            result,
                            actionPath + ".durationTicks",
                            "Expected null or exact positive duration");
                        actionValid = false;
                    }
                    else if (!duration->isNull())
                    {
                        std::uint64_t parsedDuration = 0;
                        if (!requiredExactUnsignedIntegerMember(
                                result,
                                actionSource,
                                "durationTicks",
                                actionPath + ".durationTicks",
                                std::numeric_limits<std::uint64_t>::max(),
                                parsedDuration))
                        {
                            actionValid = false;
                        }
                        else if (parsedDuration == 0)
                        {
                            addError(
                                result,
                                actionPath + ".durationTicks",
                                "Expected null or exact positive duration");
                            actionValid = false;
                        }
                        else
                        {
                            action.durationTicks = parsedDuration;
                        }
                    }
                    if (actionValid)
                        binding.actions.push_back(std::move(action));
                    bindingValid = actionValid && bindingValid;
                }
                else if (kind == "graphNode"
                    && sourceVersion >= versionThirtyTwo)
                {
                    if (actionSource.object().size() != 2U)
                    {
                        addError(
                            result,
                            actionPath,
                            "Expected kind and nodeTypeId only");
                        bindingValid = false;
                        continue;
                    }
                    ProjectGeneratorGraphNodeAction action;
                    const auto actionValid = requiredStringMember(
                        result,
                        actionSource,
                        "nodeTypeId",
                        actionPath + ".nodeTypeId",
                        action.nodeTypeId);
                    if (actionValid)
                        binding.actions.push_back(std::move(action));
                    bindingValid = actionValid && bindingValid;
                }
                else
                {
                    addError(
                        result,
                        actionPath + ".kind",
                        "Unknown generator output action kind");
                    bindingValid = false;
                }
            }
        }
        if (bindingValid)
            candidate.bindings.push_back(std::move(binding));
        valid = bindingValid && valid;
    }
    if (valid)
        output = std::move(candidate);
    return valid;
}

void parseGeneratorLibrary(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "generatorLibrary");
    if (sourceVersion < versionThirty)
    {
        if (source != nullptr)
        {
            addError(
                result,
                "$.generatorLibrary",
                "Generator library requires format 30");
        }
        result.document.generatorLibrary = {};
        return;
    }
    if (source == nullptr || !source->isObject()
        || source->object().size() != 2U)
    {
        addError(
            result,
            "$.generatorLibrary",
            "Expected version and programs only");
        return;
    }

    ProjectGeneratorLibrary library;
    const auto* version = objectMember(*source, "version");
    std::int32_t parsedVersion = 0;
    const auto expectedLibraryVersion = sourceVersion == versionThirty
        ? 1
        : projectGeneratorLibraryVersion;
    if (version == nullptr || !version->isNumber()
        || !parseSigned32BitIntegerToken(
            version->numberToken(), parsedVersion)
        || parsedVersion != expectedLibraryVersion)
    {
        addError(
            result,
            "$.generatorLibrary.version",
            sourceVersion == versionThirty
                ? "Expected exact generator library version 1"
                : "Expected exact generator library version 2");
    }

    const auto* programs = objectMember(*source, "programs");
    if (programs == nullptr || !programs->isArray())
    {
        addError(
            result,
            "$.generatorLibrary.programs",
            "Expected generator program array");
        return;
    }

    const auto errorCountBefore = result.errors.size();
    library.programs.reserve(programs->array().size());
    for (std::size_t index = 0; index < programs->array().size(); ++index)
    {
        const auto& candidate = programs->array()[index];
        const auto path = "$.generatorLibrary.programs["
            + std::to_string(index) + "]";
        const auto expectedFieldCount = sourceVersion == versionThirty
            ? 7U
            : 8U;
        if (!candidate.isObject()
            || candidate.object().size() != expectedFieldCount)
        {
            addError(
                result,
                path,
                sourceVersion == versionThirty
                    ? "Expected generatorId, displayName, terminals, grammar, seed, maximumRewritePasses, and maximumOutputSymbols only"
                    : "Expected generatorId, displayName, terminals, grammar, seed, maximumRewritePasses, maximumOutputSymbols, and output only");
            continue;
        }

        ProjectGeneratorProgram program;
        auto valid = requiredStringMember(
            result,
            candidate,
            "generatorId",
            path + ".generatorId",
            program.generatorId);
        valid = requiredStringMember(
                    result,
                    candidate,
                    "displayName",
                    path + ".displayName",
                    program.displayName)
            && valid;

        std::string terminalText;
        if (requiredStringMember(
                result,
                candidate,
                "terminals",
                path + ".terminals",
                terminalText))
        {
            if (!terminalText.empty())
            {
                const auto terminalCount = 1U
                    + static_cast<std::size_t>(std::ranges::count(
                        terminalText, '\n'));
                if (terminalCount
                    > talas::generative::maximumContextFreeSymbols)
                {
                    addError(
                        result,
                        path + ".terminals",
                        "Terminal declaration count exceeds its fixed limit");
                    valid = false;
                }
                std::size_t begin = 0;
                while (valid && begin <= terminalText.size())
                {
                    const auto end = terminalText.find('\n', begin);
                    const auto count = end == std::string::npos
                        ? terminalText.size() - begin
                        : end - begin;
                    if (count
                        > talas::generative::maximumContextFreeSymbolBytes)
                    {
                        addError(
                            result,
                            path + ".terminals",
                            "Terminal identifier exceeds its fixed byte limit");
                        valid = false;
                        break;
                    }
                    program.source.terminals.emplace_back(
                        terminalText.substr(begin, count));
                    if (end == std::string::npos)
                        break;
                    begin = end + 1U;
                }
            }
        }
        else
        {
            valid = false;
        }
        valid = requiredStringMember(
                    result,
                    candidate,
                    "grammar",
                    path + ".grammar",
                    program.source.grammar)
            && valid;

        std::uint64_t seed = 0;
        valid = requiredExactUnsignedIntegerMember(
                    result,
                    candidate,
                    "seed",
                    path + ".seed",
                    std::numeric_limits<std::uint64_t>::max(),
                    seed)
            && valid;
        program.source.seed = seed;

        std::uint64_t rewritePasses = 0;
        valid = requiredExactUnsignedIntegerMember(
                    result,
                    candidate,
                    "maximumRewritePasses",
                    path + ".maximumRewritePasses",
                    talas::generative::maximumContextFreeRewritePasses,
                    rewritePasses)
            && valid;
        program.source.maximumRewritePasses =
            static_cast<std::uint16_t>(rewritePasses);

        std::uint64_t outputSymbols = 0;
        valid = requiredExactUnsignedIntegerMember(
                    result,
                    candidate,
                    "maximumOutputSymbols",
                    path + ".maximumOutputSymbols",
                    talas::generative::maximumContextFreeOutputSymbols,
                    outputSymbols)
            && valid;
        program.source.maximumOutputSymbols =
            static_cast<std::uint16_t>(outputSymbols);
        if (sourceVersion >= versionThirtyOne)
        {
            const auto* output = objectMember(candidate, "output");
            if (output == nullptr)
            {
                addError(
                    result,
                    path + ".output",
                    "Expected required generator output profile");
                valid = false;
            }
            else
            {
                valid = parseGeneratorOutputProfile(
                            result,
                            *output,
                            path + ".output",
                            program.output,
                            sourceVersion)
                    && valid;
            }
        }
        if (valid)
            library.programs.push_back(std::move(program));
    }
    if (result.errors.size() != errorCountBefore)
        return;
    result.document.generatorLibrary = std::move(library);
}

bool parseLiveSetTarget(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    ProjectLiveSetCellTarget& output)
{
    if (!source.isObject())
    {
        addError(result, path, "Expected closed live-set target object");
        return false;
    }
    std::string kind;
    if (!requiredStringMember(result, source, "kind", path + ".kind", kind))
        return false;
    if (kind == "sound")
    {
        if (source.object().size() != 3U)
        {
            addError(result, path, "Expected kind, soundDefinitionId, and sourceInstanceId only");
            return false;
        }
        ProjectLiveSetSoundTarget target;
        auto valid = requiredStringMember(
            result,
            source,
            "soundDefinitionId",
            path + ".soundDefinitionId",
            target.soundDefinitionId);
        const auto* instance = objectMember(source, "sourceInstanceId");
        if (instance == nullptr
            || (!instance->isNull() && !instance->isString()))
        {
            addError(result, path + ".sourceInstanceId", "Expected null or exact source instance ID");
            valid = false;
        }
        else if (instance->isString())
        {
            target.sourceInstanceId = instance->string();
        }
        if (valid) output = std::move(target);
        return valid;
    }
    if (kind == "snapshot")
    {
        if (source.object().size() != 3U)
        {
            addError(result, path, "Expected kind, bankId, and snapshotId only");
            return false;
        }
        ProjectLiveSetSnapshotTarget target;
        auto valid = requiredStringMember(result, source, "bankId", path + ".bankId", target.bankId);
        valid = requiredStringMember(result, source, "snapshotId", path + ".snapshotId", target.snapshotId) && valid;
        if (valid) output = std::move(target);
        return valid;
    }
    if (kind == "scoreRegion")
    {
        if (source.object().size() != 2U)
        {
            addError(result, path, "Expected kind and markerId only");
            return false;
        }
        ProjectLiveSetScoreRegionTarget target;
        const auto valid = requiredStringMember(result, source, "markerId", path + ".markerId", target.markerId);
        if (valid) output = std::move(target);
        return valid;
    }
    if (kind == "trackMute")
    {
        if (source.object().size() != 2U)
        {
            addError(result, path, "Expected kind and muted only");
            return false;
        }
        const auto* muted = objectMember(source, "muted");
        if (muted == nullptr || !muted->isBoolean())
        {
            addError(result, path + ".muted", "Expected exact boolean mute state");
            return false;
        }
        output = ProjectLiveSetTrackMuteTarget{muted->boolean()};
        return true;
    }
    if (kind == "stop")
    {
        if (source.object().size() != 1U)
        {
            addError(result, path, "Expected stop kind only");
            return false;
        }
        output = ProjectLiveSetStopTarget{};
        return true;
    }
    addError(result, path + ".kind", "Unknown live-set target kind");
    return false;
}

bool parseLiveSetMapping(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    ProjectLiveSetTriggerMapping& output)
{
    if (!source.isObject())
    {
        addError(result, path, "Expected closed live-set mapping object");
        return false;
    }
    std::string kind;
    if (!requiredStringMember(result, source, "kind", path + ".kind", kind))
        return false;
    if (kind == "midiProgram")
    {
        if (source.object().size() != 4U)
        {
            addError(result, path, "Expected kind, sourceIdentifier, channel, and program only");
            return false;
        }
        ProjectLiveSetMidiProgramMapping mapping;
        auto valid = requiredStringMember(
            result,
            source,
            "sourceIdentifier",
            path + ".sourceIdentifier",
            mapping.sourceIdentifier);
        for (const auto [name, destination] : {
                 std::pair{"channel", &mapping.channel},
                 std::pair{"program", &mapping.program}})
        {
            const auto* value = objectMember(source, name);
            std::int32_t parsed = 0;
            if (value == nullptr || !value->isNumber()
                || !parseSigned32BitIntegerToken(value->numberToken(), parsed))
            {
                addError(result, path + "." + name, "Expected exact signed integer");
                valid = false;
            }
            else
            {
                *destination = parsed;
            }
        }
        if (valid) output = std::move(mapping);
        return valid;
    }
    if (kind == "osc")
    {
        if (source.object().size() != 2U)
        {
            addError(result, path, "Expected kind and address only");
            return false;
        }
        ProjectLiveSetOscMapping mapping;
        const auto valid = requiredStringMember(result, source, "address", path + ".address", mapping.address);
        if (valid) output = std::move(mapping);
        return valid;
    }
    if (kind == "workbench")
    {
        if (source.object().size() != 1U)
        {
            addError(result, path, "Expected Workbench mapping kind only");
            return false;
        }
        output = ProjectLiveSetWorkbenchMapping{};
        return true;
    }
    addError(result, path + ".kind", "Unknown live-set mapping kind");
    return false;
}

bool parseLiveToolTrigger(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    ProjectLiveToolTrigger& output)
{
    if (!source.isObject() || source.object().size() != 7U)
    {
        addError(result, path, "Expected closed live-tool trigger object");
        return false;
    }
    std::string kind;
    std::string domain;
    auto valid = requiredStringMember(result, source, "kind", path + ".kind", kind);
    valid = requiredStringMember(result, source, "eventName", path + ".eventName", output.eventName) && valid;
    valid = requiredStringMember(result, source, "eventDomain", path + ".eventDomain", domain) && valid;
    valid = requiredStringMember(result, source, "variableId", path + ".variableId", output.variableId) && valid;
    if (const auto parsed = parseLiveToolTriggerKind(kind)) output.kind = *parsed;
    else { addError(result, path + ".kind", "Unknown live-tool trigger kind"); valid = false; }
    if (const auto parsed = parseValueDomain(domain)) output.eventDomain = *parsed;
    else { addError(result, path + ".eventDomain", "Unknown Event Value domain"); valid = false; }
    const auto* midi = objectMember(source, "midiProgram");
    if (midi == nullptr || !midi->isObject() || midi->object().size() != 3U)
    {
        addError(result, path + ".midiProgram", "Expected closed MIDI Program mapping");
        valid = false;
    }
    else
    {
        valid = requiredStringMember(result, *midi, "sourceIdentifier", path + ".midiProgram.sourceIdentifier", output.midiProgram.sourceIdentifier) && valid;
        for (const auto [name, destination] : {
                 std::pair{"channel", &output.midiProgram.channel},
                 std::pair{"program", &output.midiProgram.program}})
        {
            const auto* value = objectMember(*midi, name);
            std::int32_t parsed = 0;
            if (value == nullptr || !value->isNumber()
                || !parseSigned32BitIntegerToken(value->numberToken(), parsed))
            { addError(result, path + ".midiProgram." + name, "Expected exact signed integer"); valid = false; }
            else *destination = parsed;
        }
    }
    const auto* osc = objectMember(source, "osc");
    if (osc == nullptr || !osc->isObject() || osc->object().size() != 1U)
    { addError(result, path + ".osc", "Expected exact OSC mapping"); valid = false; }
    else valid = requiredStringMember(result, *osc, "address", path + ".osc.address", output.osc.address) && valid;
    std::uint64_t period = 0;
    valid = requiredExactUnsignedIntegerMember(
                result,
                source,
                "periodTicks",
                path + ".periodTicks",
                maximumProjectLiveSetQuantizationTicks,
                period) && valid;
    output.periodTicks = period;
    return valid;
}

bool parseLiveToolCommand(
    ProjectLoadResult& result,
    const json::Value& source,
    const std::string& path,
    ProjectLiveToolCommand& output)
{
    if (!source.isObject() || source.object().size() != 7U)
    {
        addError(result, path, "Expected closed live-tool command object");
        return false;
    }
    std::string kind;
    std::string domain;
    auto valid = requiredStringMember(result, source, "kind", path + ".kind", kind);
    valid = requiredStringMember(result, source, "liveSetId", path + ".liveSetId", output.liveSetId) && valid;
    valid = requiredStringMember(result, source, "cellId", path + ".cellId", output.cellId) && valid;
    valid = requiredStringMember(result, source, "variableId", path + ".variableId", output.variableId) && valid;
    valid = requiredStringMember(result, source, "eventName", path + ".eventName", output.eventName) && valid;
    valid = requiredStringMember(result, source, "eventDomain", path + ".eventDomain", domain) && valid;
    valid = requiredFiniteNumberMember(result, source, "value", path + ".value", output.value) && valid;
    if (const auto parsed = parseLiveToolCommandKind(kind)) output.kind = *parsed;
    else { addError(result, path + ".kind", "Unknown live-tool command kind"); valid = false; }
    if (const auto parsed = parseValueDomain(domain)) output.eventDomain = *parsed;
    else { addError(result, path + ".eventDomain", "Unknown Event Value domain"); valid = false; }
    return valid;
}

void parseLiveSetLibrary(
    ProjectLoadResult& result,
    const json::Value& root,
    int sourceVersion)
{
    const auto* source = objectMember(root, "liveSetLibrary");
    if (sourceVersion < versionThirtyFour)
    {
        if (source != nullptr)
            addError(result, "$.liveSetLibrary", "Live-set library requires format 34");
        result.document.liveSetLibrary = {};
        return;
    }
    if (source == nullptr || !source->isObject()
        || source->object().size() != 3U)
    {
        addError(result, "$.liveSetLibrary", "Expected version, liveSets, and tools only");
        return;
    }
    ProjectLiveSetLibrary library;
    std::uint64_t version = 0;
    if (!requiredExactUnsignedIntegerMember(
            result,
            *source,
            "version",
            "$.liveSetLibrary.version",
            1,
            version)
        || version != 1U)
    {
        addError(result, "$.liveSetLibrary.version", "Expected exact live-set library version 1");
    }
    const auto* liveSets = objectMember(*source, "liveSets");
    const auto* tools = objectMember(*source, "tools");
    if (liveSets == nullptr || !liveSets->isArray())
        addError(result, "$.liveSetLibrary.liveSets", "Expected live-set array");
    if (tools == nullptr || !tools->isArray())
        addError(result, "$.liveSetLibrary.tools", "Expected live-tool array");
    if (liveSets == nullptr || !liveSets->isArray()
        || tools == nullptr || !tools->isArray())
        return;

    const auto errorsBefore = result.errors.size();
    library.liveSets.reserve(liveSets->array().size());
    for (std::size_t liveSetIndex = 0;
         liveSetIndex < liveSets->array().size();
         ++liveSetIndex)
    {
        const auto& candidate = liveSets->array()[liveSetIndex];
        const auto path = "$.liveSetLibrary.liveSets[" + std::to_string(liveSetIndex) + "]";
        if (!candidate.isObject()
            || (candidate.object().size() != 3U
                && candidate.object().size() != 4U))
        { addError(result, path, "Expected liveSetId, displayName, cells, and optional routes only"); continue; }
        ProjectLiveSet liveSet;
        auto valid = requiredStringMember(result, candidate, "liveSetId", path + ".liveSetId", liveSet.liveSetId);
        valid = requiredStringMember(result, candidate, "displayName", path + ".displayName", liveSet.displayName) && valid;
        const auto* cells = objectMember(candidate, "cells");
        if (cells == nullptr || !cells->isArray())
        { addError(result, path + ".cells", "Expected cell array"); valid = false; }
        else
        {
            liveSet.cells.reserve(cells->array().size());
            for (std::size_t cellIndex = 0; cellIndex < cells->array().size(); ++cellIndex)
            {
                const auto& cellSource = cells->array()[cellIndex];
                const auto cellPath = path + ".cells[" + std::to_string(cellIndex) + "]";
                if (!cellSource.isObject() || cellSource.object().size() != 7U)
                { addError(result, cellPath, "Expected closed live-set cell object"); valid = false; continue; }
                ProjectLiveSetCell cell;
                auto cellValid = requiredStringMember(result, cellSource, "cellId", cellPath + ".cellId", cell.cellId);
                cellValid = requiredStringMember(result, cellSource, "displayName", cellPath + ".displayName", cell.displayName) && cellValid;
                std::uint64_t row = 0;
                std::uint64_t column = 0;
                cellValid = requiredExactUnsignedIntegerMember(result, cellSource, "row", cellPath + ".row", projectLiveSetRows - 1U, row) && cellValid;
                cellValid = requiredExactUnsignedIntegerMember(result, cellSource, "column", cellPath + ".column", projectLiveSetColumns - 1U, column) && cellValid;
                cell.row = static_cast<std::uint32_t>(row);
                cell.column = static_cast<std::uint32_t>(column);
                const auto* quantization = objectMember(cellSource, "quantization");
                if (quantization == nullptr || !quantization->isObject()
                    || quantization->object().size() != 2U)
                { addError(result, cellPath + ".quantization", "Expected kind and tickInterval only"); cellValid = false; }
                else
                {
                    std::string kind;
                    cellValid = requiredStringMember(result, *quantization, "kind", cellPath + ".quantization.kind", kind) && cellValid;
                    if (const auto parsed = parseLiveSetQuantization(kind)) cell.quantization.kind = *parsed;
                    else { addError(result, cellPath + ".quantization.kind", "Unknown quantization kind"); cellValid = false; }
                    cellValid = requiredExactUnsignedIntegerMember(result, *quantization, "tickInterval", cellPath + ".quantization.tickInterval", maximumProjectLiveSetQuantizationTicks, cell.quantization.tickInterval) && cellValid;
                }
                const auto* target = objectMember(cellSource, "target");
                if (target == nullptr) { addError(result, cellPath + ".target", "Missing live-set target"); cellValid = false; }
                else cellValid = parseLiveSetTarget(result, *target, cellPath + ".target", cell.target) && cellValid;
                if (sourceVersion < versionThirtySeven
                    && std::holds_alternative<ProjectLiveSetTrackMuteTarget>(cell.target))
                { addError(result, cellPath + ".target", "Live-set trackMute requires project format 37"); cellValid = false; }
                const auto* mappings = objectMember(cellSource, "mappings");
                if (mappings == nullptr || !mappings->isArray())
                { addError(result, cellPath + ".mappings", "Expected mapping array"); cellValid = false; }
                else
                {
                    cell.mappings.reserve(mappings->array().size());
                    for (std::size_t mappingIndex = 0; mappingIndex < mappings->array().size(); ++mappingIndex)
                    {
                        ProjectLiveSetTriggerMapping mapping;
                        if (parseLiveSetMapping(result, mappings->array()[mappingIndex], cellPath + ".mappings[" + std::to_string(mappingIndex) + "]", mapping))
                            cell.mappings.push_back(std::move(mapping));
                        else cellValid = false;
                    }
                }
                if (cellValid) liveSet.cells.push_back(std::move(cell));
                valid = cellValid && valid;
            }
        }
        const auto* routes = objectMember(candidate, "routes");
        if (routes != nullptr && sourceVersion < versionThirtySeven)
        {
            addError(result, path + ".routes", "Live-set routes require project format 37");
            valid = false;
        }
        else if (routes != nullptr)
        {
            if (!routes->isArray()
                || routes->array().size() > maximumProjectLiveSetRoutes)
            {
                addError(result, path + ".routes", "Expected bounded route array");
                valid = false;
            }
            else
            {
                liveSet.routes.reserve(routes->array().size());
                for (std::size_t routeIndex = 0;
                     routeIndex < routes->array().size(); ++routeIndex)
                {
                    const auto& routeSource = routes->array()[routeIndex];
                    const auto routePath = path + ".routes["
                        + std::to_string(routeIndex) + "]";
                    if (!routeSource.isObject()
                        || routeSource.object().size() != 2U)
                    {
                        addError(result, routePath,
                                 "Expected fromColumn and toColumn only");
                        valid = false;
                        continue;
                    }
                    std::uint64_t from = 0;
                    std::uint64_t to = 0;
                    const auto routeValid =
                        requiredExactUnsignedIntegerMember(
                            result, routeSource, "fromColumn",
                            routePath + ".fromColumn",
                            projectLiveSetColumns - 1U, from)
                        && requiredExactUnsignedIntegerMember(
                            result, routeSource, "toColumn",
                            routePath + ".toColumn",
                            projectLiveSetColumns - 1U, to);
                    if (routeValid)
                        liveSet.routes.push_back({
                            static_cast<std::uint32_t>(from),
                            static_cast<std::uint32_t>(to)});
                    valid = routeValid && valid;
                }
            }
        }
        else if (candidate.object().size() != 3U)
        {
            addError(result, path + ".routes", "Expected route array");
            valid = false;
        }
        if (valid) library.liveSets.push_back(std::move(liveSet));
    }

    library.tools.reserve(tools->array().size());
    for (std::size_t toolIndex = 0; toolIndex < tools->array().size(); ++toolIndex)
    {
        const auto& candidate = tools->array()[toolIndex];
        const auto path = "$.liveSetLibrary.tools[" + std::to_string(toolIndex) + "]";
        if (!candidate.isObject() || candidate.object().size() != 6U)
        { addError(result, path, "Expected closed live-tool object"); continue; }
        ProjectLiveTool tool;
        auto valid = requiredStringMember(result, candidate, "toolId", path + ".toolId", tool.toolId);
        valid = requiredStringMember(result, candidate, "displayName", path + ".displayName", tool.displayName) && valid;
        valid = requiredStringMember(result, candidate, "initialStateId", path + ".initialStateId", tool.initialStateId) && valid;
        const auto* states = objectMember(candidate, "states");
        const auto* variables = objectMember(candidate, "variables");
        const auto* transitions = objectMember(candidate, "transitions");
        if (states == nullptr || !states->isArray()
            || variables == nullptr || !variables->isArray()
            || transitions == nullptr || !transitions->isArray())
        { addError(result, path, "Expected state, variable, and transition arrays"); continue; }
        tool.states.reserve(states->array().size());
        for (std::size_t index = 0; index < states->array().size(); ++index)
        {
            const auto& stateSource = states->array()[index];
            const auto statePath = path + ".states[" + std::to_string(index) + "]";
            if (!stateSource.isObject() || stateSource.object().size() != 2U)
            { addError(result, statePath, "Expected stateId and displayName only"); valid = false; continue; }
            ProjectLiveToolState state;
            auto stateValid = requiredStringMember(result, stateSource, "stateId", statePath + ".stateId", state.stateId);
            stateValid = requiredStringMember(result, stateSource, "displayName", statePath + ".displayName", state.displayName) && stateValid;
            if (stateValid) tool.states.push_back(std::move(state));
            valid = stateValid && valid;
        }
        tool.variables.reserve(variables->array().size());
        for (std::size_t index = 0; index < variables->array().size(); ++index)
        {
            const auto& variableSource = variables->array()[index];
            const auto variablePath = path + ".variables[" + std::to_string(index) + "]";
            if (!variableSource.isObject() || variableSource.object().size() != 6U)
            { addError(result, variablePath, "Expected closed live-tool variable object"); valid = false; continue; }
            ProjectLiveToolVariable variable;
            std::string kind;
            auto variableValid = requiredStringMember(result, variableSource, "variableId", variablePath + ".variableId", variable.variableId);
            variableValid = requiredStringMember(result, variableSource, "displayName", variablePath + ".displayName", variable.displayName) && variableValid;
            variableValid = requiredStringMember(result, variableSource, "kind", variablePath + ".kind", kind) && variableValid;
            variableValid = requiredFiniteNumberMember(result, variableSource, "initialNumber", variablePath + ".initialNumber", variable.initialNumber) && variableValid;
            variableValid = requiredBooleanMember(result, variableSource, "initialBoolean", variablePath + ".initialBoolean", variable.initialBoolean) && variableValid;
            if (const auto parsed = parseLiveToolVariableKind(kind)) variable.kind = *parsed;
            else { addError(result, variablePath + ".kind", "Unknown live-tool variable kind"); variableValid = false; }
            const auto* stateId = objectMember(variableSource, "stateId");
            if (stateId == nullptr || (!stateId->isNull() && !stateId->isString()))
            { addError(result, variablePath + ".stateId", "Expected null or state identity"); variableValid = false; }
            else if (stateId->isString()) variable.stateId = stateId->string();
            if (variableValid) tool.variables.push_back(std::move(variable));
            valid = variableValid && valid;
        }
        tool.transitions.reserve(transitions->array().size());
        for (std::size_t index = 0; index < transitions->array().size(); ++index)
        {
            const auto& transitionSource = transitions->array()[index];
            const auto transitionPath = path + ".transitions[" + std::to_string(index) + "]";
            if (!transitionSource.isObject() || transitionSource.object().size() != 6U)
            { addError(result, transitionPath, "Expected closed live-tool transition object"); valid = false; continue; }
            ProjectLiveToolTransition transition;
            auto transitionValid = requiredStringMember(result, transitionSource, "transitionId", transitionPath + ".transitionId", transition.transitionId);
            transitionValid = requiredStringMember(result, transitionSource, "fromStateId", transitionPath + ".fromStateId", transition.fromStateId) && transitionValid;
            transitionValid = requiredStringMember(result, transitionSource, "toStateId", transitionPath + ".toStateId", transition.toStateId) && transitionValid;
            transitionValid = requiredStringMember(result, transitionSource, "guardExpression", transitionPath + ".guardExpression", transition.guardExpression) && transitionValid;
            const auto* trigger = objectMember(transitionSource, "trigger");
            if (trigger == nullptr) { addError(result, transitionPath + ".trigger", "Missing live-tool trigger"); transitionValid = false; }
            else transitionValid = parseLiveToolTrigger(result, *trigger, transitionPath + ".trigger", transition.trigger) && transitionValid;
            const auto* commands = objectMember(transitionSource, "commands");
            if (commands == nullptr || !commands->isArray())
            { addError(result, transitionPath + ".commands", "Expected command array"); transitionValid = false; }
            else
            {
                transition.commands.reserve(commands->array().size());
                for (std::size_t commandIndex = 0; commandIndex < commands->array().size(); ++commandIndex)
                {
                    ProjectLiveToolCommand command;
                    if (parseLiveToolCommand(result, commands->array()[commandIndex], transitionPath + ".commands[" + std::to_string(commandIndex) + "]", command))
                        transition.commands.push_back(std::move(command));
                    else transitionValid = false;
                }
            }
            if (transitionValid) tool.transitions.push_back(std::move(transition));
            valid = transitionValid && valid;
        }
        if (valid) library.tools.push_back(std::move(tool));
    }
    if (result.errors.size() == errorsBefore)
        result.document.liveSetLibrary = std::move(library);
}
}

std::string serializeProjectDocument(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    if (expressionLanguageName(document.expressionLanguage).empty())
        throw std::invalid_argument("Invalid project expression language");
    if (const auto variables = validateProjectVariableLibrary(document);
        !variables.ok())
    {
        throw std::invalid_argument(
            variables.diagnostics.empty()
                ? "Invalid project variable library"
                : variables.diagnostics.front().message);
    }
    if (const auto soundVariables =
            validateProjectSoundVariableLibrary(document);
        !soundVariables.ok())
    {
        throw std::invalid_argument(
            soundVariables.diagnostics.empty()
                ? "Invalid project Sound-variable library"
                : soundVariables.diagnostics.front().message);
    }
    if (const auto generators =
            validateProjectGeneratorLibrary(document.generatorLibrary);
        !generators.ok())
    {
        throw std::invalid_argument(
            generators.diagnostics.empty()
                ? "Invalid project generator library"
                : generators.diagnostics.front().message);
    }
    if (const auto liveSets = validateProjectLiveSetLibrary(document);
        !liveSets.ok())
    {
        throw std::invalid_argument(
            liveSets.diagnostics.empty()
                ? "Invalid project live-set library"
                : liveSets.diagnostics.front().message);
    }
    if (const auto targets =
            validateProjectGeneratorOutputTargets(
                document,
                registry);
        !targets.ok())
    {
        throw std::invalid_argument(
            targets.diagnostics.empty()
                ? "Generator target diagnostics exceed the fixed limit"
                : targets.diagnostics.front().message);
    }
    if (const auto definitions = talas::expr::parse(
            "0",
            document.expressionLanguage,
            document.expressionDefinitions);
        !definitions.ok())
    {
        throw std::invalid_argument(
            definitions.diagnostics.empty()
                ? "Invalid expression definition library"
                : definitions.diagnostics.front().message);
    }
    if (const auto expressionAudit = auditProjectExpressions(
            document,
            document.expressionLanguage);
        !expressionAudit.ok())
    {
        throw std::invalid_argument(
            expressionAudit.diagnostics.empty()
                ? "Project expression diagnostics exceed the fixed limit"
                : expressionAudit.diagnostics.front().path + ": "
                    + expressionAudit.diagnostics.front().message);
    }
    if (document.partialTrackEdits.size()
        > maximumProjectPartialTrackEditSets)
    {
        throw std::invalid_argument(
            "Partial-track edit-set count exceeds the fixed limit");
    }
    std::unordered_set<std::string> partialTrackEditSetIds;
    std::unordered_set<std::string> partialTrackEditAssetIds;
    for (const auto& edits : document.partialTrackEdits)
    {
        if (const auto editError =
                validateProjectPartialTrackEditSet(edits))
        {
            throw std::invalid_argument(editError->message);
        }
        const auto asset = std::ranges::find(
            document.assets,
            edits.assetId,
            &ProjectAsset::id);
        if (asset == document.assets.end()
            || !partialTrackEditSetMatchesAsset(edits, *asset))
        {
            throw std::invalid_argument(
                "Partial-track edit set does not match its exact source asset");
        }
        if (!partialTrackEditSetIds.insert(edits.editSetId).second
            || !partialTrackEditAssetIds.insert(edits.assetId).second)
        {
            throw std::invalid_argument(
                "Partial-track edit identities and target assets must be unique");
        }
    }
    if (const auto error =
            projectMusicalTimeValidationError(document.timeline.musicalTime))
    {
        throw std::invalid_argument(*error);
    }
    std::vector<std::string_view> soundDefinitionIds;
    soundDefinitionIds.reserve(document.soundDefinitions.size());
    for (const auto& definition : document.soundDefinitions)
        soundDefinitionIds.push_back(definition.definitionId);
    if (const auto scoreError =
            projectScoreValidationError(document.timeline.score, soundDefinitionIds))
    {
        throw std::invalid_argument(scoreError->message);
    }
    if (const auto artifactError =
            validateProjectScoreRenderArtifacts(document))
    {
        throw std::invalid_argument(artifactError->message);
    }
    if (!isValidOscIngressSettings(document.deviceSettings.oscIngress))
        throw std::invalid_argument("Invalid OSC ingress settings");
    if (!isValidOscEgressSettings(document.deviceSettings.oscEgress)
        || !isValidOscEndpointPair(
            document.deviceSettings.oscIngress,
            document.deviceSettings.oscEgress))
    {
        throw std::invalid_argument("Invalid OSC feedback settings");
    }
    std::unordered_set<std::string> oscAddresses;
    std::size_t oscMappingCount = 0;
    std::unordered_set<std::string> oscEgressAddresses;
    std::size_t oscEgressMappingCount = 0;
    for (const auto& control : document.virtualControls)
    {
        if (control.oscMapping.has_value()
            && (!isValidOscControlMapping(*control.oscMapping)
                || ++oscMappingCount > maximumOscControlMappings
                || !oscAddresses.insert(control.oscMapping->address).second))
        {
            throw std::invalid_argument(
                "Invalid or duplicate OSC control mapping");
        }
        if (control.oscEgressMapping.has_value()
            && (!isValidOscEgressMapping(*control.oscEgressMapping)
                || ++oscEgressMappingCount > maximumOscControlMappings
                || !oscEgressAddresses.insert(
                    control.oscEgressMapping->address).second))
        {
            throw std::invalid_argument(
                "Invalid or duplicate OSC feedback mapping");
        }
    }
    std::string output;
    output += "{\n";
    appendQuotedField(output, 2, "format", projectFormat, true);
    appendIntegerField(output, 2, "version", supportedVersion, true);
    appendQuotedField(
        output,
        2,
        "expressionLanguage",
        expressionLanguageName(document.expressionLanguage),
        true);
    // Written only when it is not the default, 30.
    if (document.expressionDefinitions.timecodeFramesPerSecond != 30)
    {
        appendIntegerField(
            output,
            2,
            "timecodeFramesPerSecond",
            document.expressionDefinitions.timecodeFramesPerSecond,
            true);
    }
    // Format 156: each written only when it is not the default.
    if (document.expressionDefinitions.rounding == talas::expr::RoundingRule::ManualTable)
        appendQuotedField(output, 2, "roundingRule", "manualTable", true);
    if (document.expressionDefinitions.fieldBeatTempoBpm.has_value())
        appendNumberField(output, 2, "fieldBeatTempoBpm", *document.expressionDefinitions.fieldBeatTempoBpm, true);
    if (document.expressionDefinitions.unitlessSOSPitch == talas::expr::UnitlessSOSPitch::Linear)
        appendQuotedField(output, 2, "unitlessSOSPitch", "linear", true);
    appendExpressionDefinitions(output, document.expressionDefinitions);
    output += ",\n";
    appendVariableLibrary(output, document.variableLibrary);
    output += ",\n";
    appendSoundVariableLibrary(output, document.soundVariableLibrary);
    output += ",\n";
    appendGeneratorLibrary(output, document.generatorLibrary);
    output += ",\n";
    appendLiveSetLibrary(output, document.liveSetLibrary);
    output += ",\n";
    output += "  \"metadata\": {\n";
    appendQuotedField(output, 4, "title", document.metadata.title, false);
    output += "  },\n";
    appendPresentation(output, document);
    output += ",\n";
    appendSnapshotLibrary(output, document.snapshotLibrary);
    output += ",\n";
    appendSampleMapLibrary(output, document.sampleMapLibrary);
    output += ",\n";
    appendVirtualControls(output, document.virtualControls);
    output += ",\n";
    // Format 142: the widgets kept dormant, once there are some.
    if (!document.dormantVirtualControls.empty())
    {
        appendVirtualControls(output, document.dormantVirtualControls, "dormantVirtualControls");
        output += ",\n";
    }
    // Format 125: the surface's arrangement once it is not automatic.
    if (document.virtualControlLayout != VirtualControlLayout::Automatic)
        appendQuotedField(output, 2, "virtualControlLayout",
            document.virtualControlLayout == VirtualControlLayout::Fit ? "fit" : "fixed", true);
    // Format 126: the layouts made in the VCS Editor, once there are some.
    if (!document.virtualControlLayoutPages.empty())
    {
        output += "  \"virtualControlLayoutPages\": [\n";
        for (std::size_t index = 0; index < document.virtualControlLayoutPages.size(); ++index)
        {
            const auto& page = document.virtualControlLayoutPages[index];
            output += "    {\"name\": ";
            appendQuoted(output, page.name);
            output += ", \"eventNames\": [";
            for (std::size_t name = 0; name < page.eventNames.size(); ++name)
            {
                if (name != 0)
                    output += ", ";
                appendQuoted(output, page.eventNames[name]);
            }
            output += "]}";
            output += index + 1 < document.virtualControlLayoutPages.size() ? ",\n" : "\n";
        }
        output += "  ],\n";
    }
    // Format 155: what the layouts keep of
    // their own, once one keeps something.
    if (!document.virtualControlLayoutSettings.empty())
    {
        output += "  \"virtualControlLayoutSettings\": [\n";
        for (std::size_t index = 0; index < document.virtualControlLayoutSettings.size(); ++index)
        {
            const auto& settings = document.virtualControlLayoutSettings[index];
            if (!isValidVirtualControlLayoutSettings(settings))
                throw std::invalid_argument("Invalid layout settings: " + settings.layout);
            output += "    {\n";
            appendQuotedField(output, 6, "layout", settings.layout, true);
            if (!settings.presetBankId.empty())
                appendQuotedField(output, 6, "presetBankId", settings.presetBankId, true);
            if (settings.arrangement.has_value())
                appendQuotedField(output, 6, "arrangement",
                    *settings.arrangement == VirtualControlLayout::Fit       ? "fit"
                        : *settings.arrangement == VirtualControlLayout::Fixed ? "fixed"
                                                                               : "automatic",
                    true);
            output += "      \"widgets\": [";
            for (std::size_t at = 0; at < settings.widgets.size(); ++at)
            {
                const auto& widget = settings.widgets[at];
                output += at == 0 ? "\n" : ",\n";
                output += "        {\n";
                appendQuotedField(output, 10, "eventName", widget.eventName, true);
                appendQuotedField(output, 10, "kind", virtualControlKindName(widget.kind), true);
                appendNumberField(output, 10, "minimum", widget.minimum, true);
                appendNumberField(output, 10, "maximum", widget.maximum, widget.widget != VirtualControlWidget{});
                if (widget.widget != VirtualControlWidget{})
                    appendVirtualControlWidget(output, widget.widget, 10);
                output += "        }";
            }
            output += settings.widgets.empty() ? "]\n" : "\n      ]\n";
            output += index + 1 < document.virtualControlLayoutSettings.size() ? "    },\n" : "    }\n";
        }
        output += "  ],\n";
    }
    // Format 137: the surface's texts and boxes, once there are some.
    if (!document.virtualControlDecorations.empty())
    {
        output += "  \"virtualControlDecorations\": [\n";
        for (std::size_t index = 0; index < document.virtualControlDecorations.size(); ++index)
        {
            const auto& decoration = document.virtualControlDecorations[index];
            output += "    {\"kind\": ";
            appendQuoted(output, decoration.kind == VirtualControlDecorationKind::Box ? "box" : "text");
            output += ", \"text\": ";
            appendQuoted(output, decoration.text);
            output += ", \"bounds\": [" + std::to_string(decoration.bounds.left) + ", "
                + std::to_string(decoration.bounds.top) + ", " + std::to_string(decoration.bounds.width) + ", "
                + std::to_string(decoration.bounds.height) + "]}";
            output += index + 1 < document.virtualControlDecorations.size() ? ",\n" : "\n";
        }
        output += "  ],\n";
    }
    // Format 98: the project's own global map, written only when it has one.
    if (!document.globalMap.empty())
        appendQuotedField(output, 2, "globalMap", document.globalMap, true);
    // Format 100: ramps that start at their first value, a legacy choice
    // written only for projects that keep it.
    if (document.rampsStartAtFirstValue)
        output += "  \"rampsStartAtFirstValue\": true,\n";
    // Format 101: Mixers without a known duration, a legacy choice written
    // only for projects that keep it.
    if (document.mixerDurationsUnknown)
        output += "  \"mixerDurationsUnknown\": true,\n";
    // Format 102: the same for the Mixer.
    if (document.manyInputMixerDurationsUnknown)
        output += "  \"manyInputMixerDurationsUnknown\": true,\n";
    // Format 103: ramps that hold while their receiver is off.
    if (document.rampsHoldUntriggered)
        output += "  \"rampsHoldUntriggered\": true,\n";
    // Format 104: GraphicalEnvelope loops that play their pass out.
    if (document.graphicalEnvelopeLoopsPlayOut)
        output += "  \"graphicalEnvelopeLoopsPlayOut\": true,\n";
    if (document.banksReadLogSpectraAsHertz)
        output += "  \"banksReadLogSpectraAsHertz\": true,\n";
    // Format 114: random expressions that draw the same numbers.
    if (document.randomExpressionsShareSeeds)
        output += "  \"randomExpressionsShareSeeds\": true,\n";
    // Format 116: !KeyPitch the key's number alone.
    if (document.keyPitchIgnoresPitchBend)
        output += "  \"keyPitchIgnoresPitchBend\": true,\n";
    // Format 117: !Random and !Random1000 Event Values of their own.
    if (document.randomEventSourcesAreFaders)
        output += "  \"randomEventSourcesAreFaders\": true,\n";
    // Format 118: neg:zero:pos: selecting at once.
    if (document.signSelectionsSelectAtOnce)
        output += "  \"signSelectionsSelectAtOnce\": true,\n";
    // Format 119: time functions running on through branches not taken.
    if (document.timeRunsOnInSkippedBranches)
        output += "  \"timeRunsOnInSkippedBranches\": true,\n";
    // Format 121: Smalltalk numbers all floating point.
    if (document.smalltalkNumbersAreFloats)
        output += "  \"smalltalkNumbersAreFloats\": true,\n";
    // Format 122: !RealTime and !Time as faders, and functions of time
    // counting from the start of the whole Sound.
    if (document.timeEventSourcesAreFaders)
        output += "  \"timeEventSourcesAreFaders\": true,\n";
    if (document.timeFunctionsCountFromTheWholeSound)
        output += "  \"timeFunctionsCountFromTheWholeSound\": true,\n";
    // Format 153: !Time and !LocalTime at real time inside TimeControls and
    // TimeStoppers.
    if (document.timeEventsIgnoreTimeControls)
        output += "  \"timeEventsIgnoreTimeControls\": true,\n";
    // Format 156: choices written only when on: pasted Sounds read within
    // -1 and 1 and as they stood just before the millisecond, and the
    // inverse of 0 infinite.
    if (document.pastedSoundsSaturateAtUnity)
        output += "  \"pastedSoundsSaturateAtUnity\": true,\n";
    if (document.pastedSoundsReadThePreviousMillisecond)
        output += "  \"pastedSoundsReadThePreviousMillisecond\": true,\n";
    if (document.inverseOfZeroIsInfinite)
        output += "  \"inverseOfZeroIsInfinite\": true,\n";
    appendAssets(output, document.assets);
    output += ",\n";
    appendPartialTrackEdits(output, document.partialTrackEdits);
    output += ",\n";
    appendTimeline(output, document.timeline);
    output += ",\n";
    appendDeviceSettings(output, document.deviceSettings);
    output += ",\n";
    appendSoundDefinitions(output, document.soundDefinitions);
    output += ",\n";
    // Format 140: the Sound collections, once there are some.
    if (!document.soundCollections.empty())
    {
        output += "  \"soundCollections\": [\n";
        for (std::size_t index = 0; index < document.soundCollections.size(); ++index)
        {
            const auto& collection = document.soundCollections[index];
            output += "    {\n";
            appendQuotedField(output, 6, "id", collection.collectionId, true);
            appendQuotedField(output, 6, "name", collection.name, true);
            appendStringArrayField(output, 6, "sounds", collection.soundIds, true);
            appendStringArrayField(output, 6, "collections", collection.collectionIds, false);
            output += index + 1 < document.soundCollections.size() ? "    },\n" : "    }\n";
        }
        output += "  ],\n";
    }
    appendGraphField(output, "graph", document.graph, 2, false);
    output += "}";
    return output;
}

std::string serializeProjectDocument(const ProjectDocument& document)
{
    return serializeProjectDocument(
        document,
        talas::core::makeBuiltinNodeRegistry());
}

ProjectLoadResult parseProjectDocument(
    std::string_view jsonText,
    const talas::core::NodeRegistry& registry)
{
    ProjectLoadResult result;
    json::ParseLimits limits;
    limits.maximumNestingDepth = json::defaultMaximumNestingDepth;
    const std::array boundedStringMembers{
        json::ParseLimits::BoundedStringMember{
            "sourceIdentifier", maximumMidiSourceIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "address", maximumOscAddressBytes},
        json::ParseLimits::BoundedStringMember{
            "trackId", maximumScoreIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "clipId", maximumScoreIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "laneId", maximumScoreIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "markerId", maximumScoreIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "ownerId", maximumScoreIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "outputTrackId", maximumScoreIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "inputTrackId", maximumScoreIdentifierBytes},
        json::ParseLimits::BoundedStringMember{
            "soundDefinitionId", maximumScoreIdentifierBytes, "clips"},
        json::ParseLimits::BoundedStringMember{
            "sourceInstanceId",
            maximumScoreIdentifierBytes,
            "clips",
            {},
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(),
            true},
        json::ParseLimits::BoundedStringMember{
            "eventName", maximumScoreIdentifierBytes, "automation"},
        json::ParseLimits::BoundedStringMember{
            "displayName", maximumScoreDisplayNameBytes, "tracks"},
        json::ParseLimits::BoundedStringMember{
            "title", maximumScoreMarkerTitleBytes, "markers"},
        json::ParseLimits::BoundedStringMember{
            "kind", 32U},
        json::ParseLimits::BoundedStringMember{
            "timeDomain", 32U, "tracks"},
        json::ParseLimits::BoundedStringMember{
            "overlapMode", 32U, "tracks"},
        json::ParseLimits::BoundedStringMember{
            "scope", 32U, "automation"},
        json::ParseLimits::BoundedStringMember{
            "domain", 32U},
        json::ParseLimits::BoundedStringMember{
            "interpolation", 32U},
        json::ParseLimits::BoundedStringMember{
            "editSetId", maximumProjectPartialTrackEditIdBytes},
        json::ParseLimits::BoundedStringMember{
            "editId", maximumProjectPartialTrackEditIdBytes},
        json::ParseLimits::BoundedStringMember{
            "assetId", maximumProjectPartialTrackAssetIdBytes, "partialTrackEdits"},
        json::ParseLimits::BoundedStringMember{
            "sourceContentHash", 64U, "partialTrackEdits"},
        json::ParseLimits::BoundedStringMember{
            "sourceFingerprint",
            scoreClipRenderFingerprintBytes,
            "renderArtifact"},
        json::ParseLimits::BoundedStringMember{
            "assetId",
            maximumScoreClipRenderAssetIdBytes,
            "channels",
            "renderArtifact"},
        json::ParseLimits::BoundedStringMember{
            "expressionLanguage", 16U},
        json::ParseLimits::BoundedStringMember{
            "variableId",
            maximumProjectVariableIdentifierBytes,
            "variables"},
        json::ParseLimits::BoundedStringMember{
            "displayName",
            maximumProjectVariableDisplayNameBytes,
            "variables"},
        json::ParseLimits::BoundedStringMember{
            "definitionId",
            maximumProjectVariableIdentifierBytes,
            "owner",
            "variables"},
        json::ParseLimits::BoundedStringMember{
            "name",
            talas::expr::ExpressionLimits::maximumDefinitionNameBytes,
            "expressionDefinitions"},
        json::ParseLimits::BoundedStringMember{
            "source",
            talas::expr::ExpressionLimits::maximumDefinitionSourceBytes,
            "expressionDefinitions"},
        json::ParseLimits::BoundedStringMember{
            "name",
            talas::expr::ExpressionLimits::maximumDefinitionNameBytes,
            "parameters",
            "expressionDefinitions"},
        json::ParseLimits::BoundedStringMember{
            "generatorId",
            talas::generative::maximumContextFreeSymbolBytes,
            "programs",
            "generatorLibrary"},
        json::ParseLimits::BoundedStringMember{
            "displayName",
            talas::generative::maximumContextFreeDisplayNameBytes,
            "programs",
            "generatorLibrary"},
        json::ParseLimits::BoundedStringMember{
            "terminals",
            talas::generative::maximumContextFreeSymbols
                * (talas::generative::maximumContextFreeSymbolBytes + 1U),
            "programs",
            "generatorLibrary",
            0U,
            talas::generative::maximumContextFreeProjectSourceBytes},
        json::ParseLimits::BoundedStringMember{
            "grammar",
            talas::generative::maximumContextFreeSourceBytes,
            "programs",
            "generatorLibrary",
            0U,
            talas::generative::maximumContextFreeProjectSourceBytes},
        json::ParseLimits::BoundedStringMember{
            "terminal",
            talas::generative::maximumContextFreeSymbolBytes,
            "bindings",
            "output"},
        json::ParseLimits::BoundedStringMember{
            "eventName",
            maximumScoreIdentifierBytes,
            "actions",
            "bindings"},
        json::ParseLimits::BoundedStringMember{
            "soundDefinitionId",
            maximumScoreIdentifierBytes,
            "actions",
            "bindings"},
        json::ParseLimits::BoundedStringMember{
            "title",
            maximumScoreMarkerTitleBytes,
            "actions",
            "bindings"},
        json::ParseLimits::BoundedStringMember{
            "nodeTypeId",
            maximumProjectGeneratorNodeTypeIdBytes,
            "actions",
            "bindings"},
        json::ParseLimits::BoundedStringMember{
            "liveSetId", maximumProjectLiveSetIdentifierBytes,
            "liveSets", "liveSetLibrary"},
        json::ParseLimits::BoundedStringMember{
            "liveSetId", maximumProjectLiveSetIdentifierBytes,
            "commands", "transitions"},
        json::ParseLimits::BoundedStringMember{
            "cellId", maximumProjectLiveSetIdentifierBytes,
            "cells", "liveSets"},
        json::ParseLimits::BoundedStringMember{
            "cellId", maximumProjectLiveSetIdentifierBytes,
            "commands", "transitions"},
        json::ParseLimits::BoundedStringMember{
            "displayName", maximumProjectLiveSetDisplayNameBytes,
            "liveSets", "liveSetLibrary"},
        json::ParseLimits::BoundedStringMember{
            "displayName", maximumProjectLiveSetDisplayNameBytes,
            "cells", "liveSets"},
        json::ParseLimits::BoundedStringMember{
            "displayName", maximumProjectLiveSetDisplayNameBytes,
            "tools", "liveSetLibrary"},
        json::ParseLimits::BoundedStringMember{
            "displayName", maximumProjectLiveSetDisplayNameBytes,
            "states", "tools"},
        json::ParseLimits::BoundedStringMember{
            "displayName", maximumProjectLiveSetDisplayNameBytes,
            "variables", "tools"},
        json::ParseLimits::BoundedStringMember{
            "soundDefinitionId", maximumProjectLiveSetIdentifierBytes,
            "target", "cells"},
        json::ParseLimits::BoundedStringMember{
            "sourceInstanceId", maximumProjectLiveSetIdentifierBytes,
            "target", "cells",
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(), true},
        json::ParseLimits::BoundedStringMember{
            "bankId", maximumProjectLiveSetIdentifierBytes,
            "target", "cells"},
        json::ParseLimits::BoundedStringMember{
            "snapshotId", maximumProjectLiveSetIdentifierBytes,
            "target", "cells"},
        json::ParseLimits::BoundedStringMember{
            "toolId", maximumProjectLiveSetIdentifierBytes,
            "tools", "liveSetLibrary"},
        json::ParseLimits::BoundedStringMember{
            "initialStateId", maximumProjectLiveSetIdentifierBytes,
            "tools", "liveSetLibrary"},
        json::ParseLimits::BoundedStringMember{
            "stateId", maximumProjectLiveSetIdentifierBytes,
            "states", "tools"},
        json::ParseLimits::BoundedStringMember{
            "stateId", maximumProjectLiveSetIdentifierBytes,
            "variables", "tools",
            std::numeric_limits<std::size_t>::max(),
            std::numeric_limits<std::size_t>::max(), true},
        json::ParseLimits::BoundedStringMember{
            "variableId", maximumProjectLiveSetIdentifierBytes,
            "variables", "tools"},
        json::ParseLimits::BoundedStringMember{
            "variableId", maximumProjectLiveSetIdentifierBytes,
            "trigger", "transitions"},
        json::ParseLimits::BoundedStringMember{
            "variableId", maximumProjectLiveSetIdentifierBytes,
            "commands", "transitions"},
        json::ParseLimits::BoundedStringMember{
            "transitionId", maximumProjectLiveSetIdentifierBytes,
            "transitions", "tools"},
        json::ParseLimits::BoundedStringMember{
            "fromStateId", maximumProjectLiveSetIdentifierBytes,
            "transitions", "tools"},
        json::ParseLimits::BoundedStringMember{
            "toStateId", maximumProjectLiveSetIdentifierBytes,
            "transitions", "tools"},
        json::ParseLimits::BoundedStringMember{
            "guardExpression", maximumProjectLiveToolGuardBytes,
            "transitions", "tools"},
        json::ParseLimits::BoundedStringMember{
            "eventName", maximumProjectLiveSetIdentifierBytes,
            "trigger", "transitions"},
        json::ParseLimits::BoundedStringMember{
            "eventName", maximumProjectLiveSetIdentifierBytes,
            "commands", "transitions"},
        json::ParseLimits::BoundedStringMember{
            "eventDomain", 32U, "trigger", "transitions"},
        json::ParseLimits::BoundedStringMember{
            "eventDomain", 32U, "commands", "transitions"},
    };
    // Slot zero is reserved for the one C9 aggregate inspected-record
    // budget. The entry using that storage is not itself aggregate-counted.
    constexpr std::size_t liveSetAggregateContainerBudgetIndex = 0U;
    const std::array boundedContainerMembers{
        json::ParseLimits::BoundedContainerMember{
            "tracks", maximumScoreTracks},
        json::ParseLimits::BoundedContainerMember{
            "clips", maximumScoreClips},
        json::ParseLimits::BoundedContainerMember{
            "automation", maximumScoreAutomationLanes},
        json::ParseLimits::BoundedContainerMember{
            "markers", maximumScoreMarkers},
        json::ParseLimits::BoundedContainerMember{
            "points", maximumScoreAutomationPoints, true},
        json::ParseLimits::BoundedContainerMember{
            "channels", maximumScoreChannels, false, "renderArtifact"},
        json::ParseLimits::BoundedContainerMember{
            "partialTrackEdits", maximumProjectPartialTrackEditSets},
        json::ParseLimits::BoundedContainerMember{
            "pointOverrides", maximumProjectPartialTrackPointOverrides},
        json::ParseLimits::BoundedContainerMember{
            "insertedPoints", maximumProjectPartialTrackInsertedPoints},
        json::ParseLimits::BoundedContainerMember{
            "trackStates", maximumProjectPartialTrackStates},
        json::ParseLimits::BoundedContainerMember{
            "expressionDefinitions",
            talas::expr::ExpressionLimits::maximumDefinitions},
        json::ParseLimits::BoundedContainerMember{
            "variables", maximumProjectVariableCount, false, "variableLibrary"},
        json::ParseLimits::BoundedContainerMember{
            "variables", maximumProjectSoundVariableCount, false, "soundVariableLibrary"},
        json::ParseLimits::BoundedContainerMember{
            "candidateDefinitionIds", maximumProjectSoundVariableCandidates},
        json::ParseLimits::BoundedContainerMember{
            "parameters",
            talas::expr::ExpressionLimits::maximumDefinitionParameters,
            false,
            "expressionDefinitions"},
        json::ParseLimits::BoundedContainerMember{
            "programs",
            talas::generative::maximumContextFreePrograms,
            false,
            "generatorLibrary"},
        json::ParseLimits::BoundedContainerMember{
            "bindings",
            maximumProjectGeneratorTerminalBindings,
            false,
            "output"},
        json::ParseLimits::BoundedContainerMember{
            "actions",
            maximumProjectGeneratorActionsPerTerminal,
            false,
            "bindings"},
        json::ParseLimits::BoundedContainerMember{
            "liveSets", maximumProjectLiveSets, false,
            "liveSetLibrary", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
        json::ParseLimits::BoundedContainerMember{
            "tools", maximumProjectLiveTools, false,
            "liveSetLibrary", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
        json::ParseLimits::BoundedContainerMember{
            "cells", projectLiveSetRows * projectLiveSetColumns, false,
            "liveSets", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
        json::ParseLimits::BoundedContainerMember{
            "mappings", maximumProjectLiveSetMappingsPerCell, false,
            "cells", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
        json::ParseLimits::BoundedContainerMember{
            "states", maximumProjectLiveToolStates, false,
            "tools", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
        json::ParseLimits::BoundedContainerMember{
            "variables", maximumProjectLiveToolVariables, false,
            "tools", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
        json::ParseLimits::BoundedContainerMember{
            "transitions", maximumProjectLiveToolTransitions, false,
            "tools", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
        json::ParseLimits::BoundedContainerMember{
            "commands", maximumProjectLiveToolCommandsPerTransition, false,
            "transitions", liveSetAggregateContainerBudgetIndex,
            maximumProjectLiveSetInspectedRecords},
    };
    constexpr auto maximumLiveSetJsonValues =
        maximumProjectLiveSetInspectedRecords * 24U;
    constexpr auto maximumLiveSetDecodedStringBytes =
        maximumProjectLiveSetInspectedRecords
        * (maximumProjectLiveToolGuardBytes + 512U);
    const std::array boundedSubtreeMembers{
        json::ParseLimits::BoundedSubtreeMember{
            "liveSetLibrary",
            {},
            maximumLiveSetJsonValues,
            12U,
            maximumLiveSetDecodedStringBytes,
            64U,
            64U},
    };
    limits.boundedStringMembers = boundedStringMembers;
    limits.boundedContainerMembers = boundedContainerMembers;
    limits.boundedSubtreeMembers = boundedSubtreeMembers;
    const auto parsed = json::parse(jsonText, limits);
    if (!parsed.ok())
    {
        for (const auto& error : parsed.errors)
            addError(result, error.path.empty() ? "$" : error.path, error.message);
        return result;
    }

    if (!parsed.value.isObject())
    {
        addError(result, "$", "Expected root object");
        return result;
    }

    const auto* format = objectMember(parsed.value, "format");
    if (format == nullptr || !format->isString() || format->string() != projectFormat)
        addError(result, "$.format", "Expected talas.project format");

    int sourceVersion = 0;
    const auto* version = objectMember(parsed.value, "version");
    if (version == nullptr || !version->isNumber())
    {
        addError(result, "$.version", "Expected numeric version");
    }
    else if (std::int32_t parsedVersion = 0;
             !parseSigned32BitIntegerToken(
                 version->numberToken(),
                 parsedVersion))
    {
        addError(result, "$.version", "Expected integral numeric version");
    }
    else if (parsedVersion != versionOne
             && parsedVersion != versionTwo
             && parsedVersion != versionThree
             && parsedVersion != versionFour
             && parsedVersion != versionFive
             && parsedVersion != versionSix
             && parsedVersion != versionSeven
             && parsedVersion != versionEight
             && parsedVersion != versionNine
             && parsedVersion != versionTen
             && parsedVersion != versionEleven
             && parsedVersion != versionTwelve
             && parsedVersion != versionThirteen
             && parsedVersion != versionFourteen
             && parsedVersion != versionFifteen
             && parsedVersion != versionSixteen
             && parsedVersion != versionSeventeen
             && parsedVersion != versionEighteen
             && parsedVersion != versionNineteen
             && parsedVersion != versionTwenty
             && parsedVersion != versionTwentyOne
             && parsedVersion != versionTwentyTwo
             && parsedVersion != versionTwentyThree
             && parsedVersion != versionTwentyFour
             && parsedVersion != versionTwentyFive
             && parsedVersion != versionTwentySix
             && parsedVersion != versionTwentySeven
             && parsedVersion != versionTwentyEight
             && parsedVersion != versionTwentyNine
             && parsedVersion != versionThirty
             && parsedVersion != versionThirtyOne
             && parsedVersion != versionThirtyTwo
             && parsedVersion != versionThirtyThree
             && parsedVersion != versionThirtyFour
             && parsedVersion != versionThirtyFive
             && parsedVersion != versionThirtySix
             && parsedVersion != versionThirtySeven
             && parsedVersion != versionThirtyEight
             && parsedVersion != versionThirtyNine
             && parsedVersion != versionForty
             && parsedVersion != versionFortyOne
             && parsedVersion != versionFortyTwo
             && parsedVersion != versionFortyThree
             && parsedVersion != versionFortyFour
             && parsedVersion != versionFortyFive
             && parsedVersion != versionFortySix
             && parsedVersion != versionFortySeven
             && parsedVersion != versionFortyEight
             && parsedVersion != versionFortyNine
             && parsedVersion != versionFifty
             && parsedVersion != versionFiftyOne
             && parsedVersion != versionFiftyTwo
             && parsedVersion != versionFiftyThree
             && parsedVersion != versionFiftyFour
             && parsedVersion != versionFiftyFive
             && parsedVersion != versionFiftySix
             && parsedVersion != versionFiftySeven
             && parsedVersion != versionFiftyEight
             && parsedVersion != versionFiftyNine
             && parsedVersion != versionSixty
             && parsedVersion != versionSixtyOne
             && parsedVersion != versionSixtyTwo
             && parsedVersion != versionSixtyThree
             && parsedVersion != versionSixtyFour
             && parsedVersion != versionSixtyFive
             && parsedVersion != versionSixtySix
             && parsedVersion != versionSixtySeven
             && parsedVersion != versionSixtyEight
             && parsedVersion != versionSixtyNine
             && parsedVersion != versionSeventy
             && parsedVersion != versionSeventyOne
             && parsedVersion != versionSeventyTwo
             && parsedVersion != versionSeventyThree
             && parsedVersion != versionSeventyFour
             && parsedVersion != versionSeventyFive
             && parsedVersion != versionSeventySix
             && parsedVersion != versionSeventySeven
             && parsedVersion != versionSeventyEight
             && parsedVersion != versionSeventyNine
             && parsedVersion != versionEighty
             && parsedVersion != versionEightyOne
             && parsedVersion != versionEightyTwo
             && parsedVersion != versionEightyThree
             && parsedVersion != versionEightyFour
             && parsedVersion != versionEightyFive
             && parsedVersion != versionEightySix
             && parsedVersion != versionEightySeven
             && parsedVersion != versionEightyEight
             && parsedVersion != versionEightyNine
             && parsedVersion != versionNinety
             && parsedVersion != versionNinetyOne
             && parsedVersion != versionNinetyTwo
             && parsedVersion != versionNinetyThree
             && parsedVersion != versionNinetyFour
             && parsedVersion != versionNinetyFive
             && parsedVersion != versionNinetySix
             && parsedVersion != versionNinetySeven
             && parsedVersion != versionNinetyEight
             && parsedVersion != versionNinetyNine
             && parsedVersion != versionOneHundred
             && parsedVersion != versionOneHundredOne
             && parsedVersion != versionOneHundredTwo
             && parsedVersion != versionOneHundredThree
             && parsedVersion != versionOneHundredFour
             && parsedVersion != versionOneHundredFive
             && parsedVersion != versionOneHundredSix
             && parsedVersion != versionOneHundredSeven
             && parsedVersion != versionOneHundredEight
             && parsedVersion != versionOneHundredNine
             && parsedVersion != versionOneHundredTen
             && parsedVersion != versionOneHundredEleven
             && parsedVersion != versionOneHundredTwelve
             && parsedVersion != versionOneHundredThirteen
             && parsedVersion != versionOneHundredFourteen
             && parsedVersion != versionOneHundredFifteen
             && parsedVersion != versionOneHundredSixteen
             && parsedVersion != versionOneHundredSeventeen
             && parsedVersion != versionOneHundredEighteen
             && parsedVersion != versionOneHundredNineteen
             && parsedVersion != versionOneHundredTwenty
             && parsedVersion != versionOneHundredTwentyOne
             && parsedVersion != versionOneHundredTwentyTwo
             && parsedVersion != versionOneHundredTwentyThree
             && parsedVersion != versionOneHundredTwentyFour
             && parsedVersion != versionOneHundredTwentyFive
             && parsedVersion != versionOneHundredTwentySix
             && parsedVersion != versionOneHundredTwentySeven
             && parsedVersion != versionOneHundredTwentyEight
             && parsedVersion != versionOneHundredTwentyNine
             && parsedVersion != versionOneHundredThirty
             && parsedVersion != versionOneHundredThirtyOne
             && parsedVersion != versionOneHundredThirtyTwo
             && parsedVersion != versionOneHundredThirtyThree
             && parsedVersion != versionOneHundredThirtyFour
             && parsedVersion != versionOneHundredThirtyFive
             && parsedVersion != versionOneHundredThirtySix
             && parsedVersion != versionOneHundredThirtySeven
             && parsedVersion != versionOneHundredThirtyEight
             && parsedVersion != versionOneHundredThirtyNine
             && parsedVersion != versionOneHundredForty
             && parsedVersion != versionOneHundredFortyOne
             && parsedVersion != versionOneHundredFortyTwo
             && parsedVersion != versionOneHundredFortyThree
             && parsedVersion != versionOneHundredFortyFour
             && parsedVersion != versionOneHundredFortyFive
             && parsedVersion != versionOneHundredFortySix
             && parsedVersion != versionOneHundredFortySeven
             && parsedVersion != versionOneHundredFortyEight
             && parsedVersion != versionOneHundredFortyNine
             && parsedVersion != versionOneHundredFifty
             && parsedVersion != versionOneHundredFiftyOne
             && parsedVersion != versionOneHundredFiftyTwo
             && parsedVersion != versionOneHundredFiftyThree
             && parsedVersion != versionOneHundredFiftyFour
             && parsedVersion != versionOneHundredFiftyFive
             && parsedVersion != supportedVersion)
    {
        addError(result, "$.version", "Unsupported project version");
    }
    else
    {
        sourceVersion = parsedVersion;
        result.document.formatVersion = supportedVersion;
        result.document.expressionLanguage =
            sourceVersion <= versionTwentyTwo
            ? talas::expr::ExpressionLanguageVersion::LegacyV1
            : sourceVersion == versionTwentyThree
                ? talas::expr::ExpressionLanguageVersion::PureV2
                : sourceVersion == versionTwentyFour
                    ? talas::expr::ExpressionLanguageVersion::PureV3
                    : sourceVersion == versionTwentyFive
                        ? talas::expr::ExpressionLanguageVersion::PureV4
                        : sourceVersion == versionTwentySix
                            ? talas::expr::ExpressionLanguageVersion::SessionV1
                        : sourceVersion <= versionThirtyFour
                            ? talas::expr::ExpressionLanguageVersion::VariablesV1
                            : talas::expr::ExpressionLanguageVersion::VariablesV2;
    }

    const auto* expressionLanguage =
        objectMember(parsed.value, "expressionLanguage");
    if (sourceVersion > 0 && sourceVersion <= versionTwentyTwo)
    {
        if (expressionLanguage != nullptr)
        {
            addError(
                result,
                "$.expressionLanguage",
                "Expression language requires format 23");
        }
    }
    // Every format from 23 on writes its expression language.
    else if (sourceVersion >= versionTwentyThree)
    {
        if (expressionLanguage == nullptr || !expressionLanguage->isString())
        {
            addError(
                result,
                "$.expressionLanguage",
                "Expected required expression language");
        }
        else if (const auto parsedLanguage =
                     parseExpressionLanguage(expressionLanguage->string()))
        {
            if (sourceVersion < versionThirtySix
                && *parsedLanguage == talas::expr::ExpressionLanguageVersion::VoiceV1)
            {
                addError(result, "$.expressionLanguage",
                    "VoiceV1 expression language requires format 36");
            }
            else if (sourceVersion < versionThirtyEight
                && *parsedLanguage == talas::expr::ExpressionLanguageVersion::PureV5)
            {
                addError(result, "$.expressionLanguage",
                    "PureV5 expression language requires format 38");
            }
            else if (sourceVersion < versionThirtySeven
                && *parsedLanguage == talas::expr::ExpressionLanguageVersion::VariablesV3)
            {
                addError(result, "$.expressionLanguage",
                    "VariablesV3 expression language requires format 37");
            }
            else if (sourceVersion < versionForty
                && *parsedLanguage == talas::expr::ExpressionLanguageVersion::VariablesV4)
            {
                addError(result, "$.expressionLanguage",
                    "VariablesV4 expression language requires format 40");
            }
            else if (sourceVersion < versionFortyTwo
                && *parsedLanguage == talas::expr::ExpressionLanguageVersion::ValV1)
            {
                addError(result, "$.expressionLanguage",
                    "ValV1 expression language requires format 42");
            }
            else if (sourceVersion < versionThirtyFive
                && *parsedLanguage
                    == talas::expr::ExpressionLanguageVersion::VariablesV2)
            {
                addError(
                    result,
                    "$.expressionLanguage",
                    "VariablesV2 expression language requires format 35");
            }
            else if (sourceVersion == versionTwentyThree
                && (*parsedLanguage
                        == talas::expr::ExpressionLanguageVersion::PureV3
                    || *parsedLanguage
                        == talas::expr::ExpressionLanguageVersion::PureV4
                    || *parsedLanguage
                        == talas::expr::ExpressionLanguageVersion::SessionV1
                    || *parsedLanguage
                        == talas::expr::ExpressionLanguageVersion::VariablesV1))
            {
                addError(
                    result,
                    "$.expressionLanguage",
                    *parsedLanguage
                            == talas::expr::ExpressionLanguageVersion::VariablesV1
                        ? "VariablesV1 expression language requires format 27"
                        : *parsedLanguage
                                  == talas::expr::ExpressionLanguageVersion::SessionV1
                            ? "SessionV1 expression language requires format 26"
                        : *parsedLanguage
                                  == talas::expr::ExpressionLanguageVersion::PureV4
                            ? "PureV4 expression language requires format 25"
                            : "PureV3 expression language requires format 24");
            }
            else if (sourceVersion == versionTwentyFour
                     && (*parsedLanguage
                             == talas::expr::ExpressionLanguageVersion::PureV4
                         || *parsedLanguage
                             == talas::expr::ExpressionLanguageVersion::SessionV1
                         || *parsedLanguage
                             == talas::expr::ExpressionLanguageVersion::VariablesV1))
            {
                addError(
                    result,
                    "$.expressionLanguage",
                    *parsedLanguage
                            == talas::expr::ExpressionLanguageVersion::VariablesV1
                        ? "VariablesV1 expression language requires format 27"
                        : *parsedLanguage
                                  == talas::expr::ExpressionLanguageVersion::SessionV1
                            ? "SessionV1 expression language requires format 26"
                        : "PureV4 expression language requires format 25");
            }
            else if (sourceVersion == versionTwentyFive
                     && (*parsedLanguage
                             == talas::expr::ExpressionLanguageVersion::SessionV1
                         || *parsedLanguage
                             == talas::expr::ExpressionLanguageVersion::VariablesV1))
            {
                addError(
                    result,
                    "$.expressionLanguage",
                    *parsedLanguage
                            == talas::expr::ExpressionLanguageVersion::VariablesV1
                        ? "VariablesV1 expression language requires format 27"
                        : "SessionV1 expression language requires format 26");
            }
            else if (sourceVersion == versionTwentySix
                     && *parsedLanguage
                         == talas::expr::ExpressionLanguageVersion::VariablesV1)
            {
                addError(
                    result,
                    "$.expressionLanguage",
                    "VariablesV1 expression language requires format 27");
            }
            else
            {
                result.document.expressionLanguage = *parsedLanguage;
            }
        }
        else
        {
            addError(
                result,
                "$.expressionLanguage",
                sourceVersion == versionTwentyThree
                    ? "Expected legacyV1 or pureV2 expression language"
                    : sourceVersion == versionTwentyFour
                        ? "Expected legacyV1, pureV2, or pureV3 expression language"
                        : sourceVersion == versionTwentyFive
                            ? "Expected legacyV1, pureV2, pureV3, or pureV4 expression language"
                            : sourceVersion == versionTwentySix
                                ? "Expected legacyV1, pureV2, pureV3, pureV4, or sessionV1 expression language"
                                : sourceVersion < versionThirtyFive
                                    ? "Expected legacyV1, pureV2, pureV3, pureV4, sessionV1, or variablesV1 expression language"
                                    : sourceVersion == versionThirtyFive
                                        ? "Expected legacyV1, pureV2, pureV3, pureV4, sessionV1, variablesV1, or variablesV2 expression language"
                                        : sourceVersion == versionThirtySix
                                            ? "Expected legacyV1, pureV2, pureV3, pureV4, sessionV1, variablesV1, variablesV2, or voiceV1 expression language"
                                        : sourceVersion == versionThirtySeven
                                            ? "Expected legacyV1, pureV2, pureV3, pureV4, sessionV1, variablesV1, variablesV2, voiceV1, or variablesV3 expression language"
                                        : sourceVersion == versionThirtyEight
                                            ? "Expected legacyV1, pureV2, pureV3, pureV4, pureV5, sessionV1, variablesV1, variablesV2, voiceV1, or variablesV3 expression language"
                                        : sourceVersion == versionThirtyNine
                                            ? "Expected legacyV1, pureV2, pureV3, pureV4, pureV5, sessionV1, variablesV1, variablesV2, voiceV1, or variablesV3 expression language"
                                        : sourceVersion < versionFortyTwo
                                            ? "Expected legacyV1, pureV2, pureV3, pureV4, pureV5, sessionV1, variablesV1, variablesV2, voiceV1, variablesV3, or variablesV4 expression language"
                                            : "Expected legacyV1, pureV2, pureV3, pureV4, pureV5, sessionV1, variablesV1, variablesV2, voiceV1, variablesV3, variablesV4, or valV1 expression language");
        }
    }

    if (sourceVersion > 0)
    {
        parseExpressionDefinitions(result, parsed.value, sourceVersion);
        parseFieldReadingSettings(result, parsed.value, sourceVersion);
    }

    if (sourceVersion > 0 && sourceVersion <= versionTen)
    {
        if (objectMember(parsed.value, "presentation") != nullptr)
            addError(result, "$.presentation", "Presentation requires format 11");
        if (objectMember(parsed.value, "snapshotLibrary") != nullptr)
            addError(result, "$.snapshotLibrary", "Snapshot library requires format 11");
    }
    if (sourceVersion > 0 && sourceVersion <= versionEleven
        && objectMember(parsed.value, "sampleMapLibrary") != nullptr)
    {
        addError(
            result,
            "$.sampleMapLibrary",
            "Sample-map library requires format 12");
    }

    parseMetadata(result, parsed.value);
    parseVirtualControls(result, parsed.value, sourceVersion);
    parseVirtualControls(result, parsed.value, sourceVersion, true);
    if (sourceVersion >= versionFour)
        parseAssets(result, parsed.value, sourceVersion);
    if (sourceVersion > 0)
        parsePartialTrackEdits(result, parsed.value, sourceVersion);
    parseTimeline(result, parsed.value, sourceVersion);
    if (sourceVersion >= versionFive)
        parseDeviceSettings(result, parsed.value, sourceVersion);
    std::size_t preservedComponentCount = 0;
    std::size_t preservedComponentStateBytes = 0;
    if (sourceVersion >= versionEight)
    {
        parseSoundDefinitions(
            result,
            parsed.value,
            sourceVersion,
            preservedComponentCount,
            preservedComponentStateBytes);
    }
    if (sourceVersion > 0)
        parseVariableLibrary(result, parsed.value, sourceVersion);
    if (sourceVersion > 0)
        parseSoundVariableLibrary(result, parsed.value, sourceVersion);
    if (sourceVersion > 0)
        parseGeneratorLibrary(result, parsed.value, sourceVersion);
    if (sourceVersion > 0)
        parseLiveSetLibrary(result, parsed.value, sourceVersion);
    if (const auto* ramps = objectMember(parsed.value, "rampsStartAtFirstValue"); ramps != nullptr)
    {
        if (sourceVersion < versionOneHundred)
            addError(result, "$.rampsStartAtFirstValue", "rampsStartAtFirstValue requires format 100");
        else if (!ramps->isBoolean() || !ramps->boolean())
            addError(result, "$.rampsStartAtFirstValue", "Expected true");
        else
            result.document.rampsStartAtFirstValue = true;
    }
    if (const auto* mixers = objectMember(parsed.value, "mixerDurationsUnknown"); mixers != nullptr)
    {
        if (sourceVersion < versionOneHundredOne)
            addError(result, "$.mixerDurationsUnknown", "mixerDurationsUnknown requires format 101");
        else if (!mixers->isBoolean() || !mixers->boolean())
            addError(result, "$.mixerDurationsUnknown", "Expected true");
        else
            result.document.mixerDurationsUnknown = true;
    }
    if (const auto* mixers = objectMember(parsed.value, "manyInputMixerDurationsUnknown"); mixers != nullptr)
    {
        if (sourceVersion < versionOneHundredTwo)
            addError(result, "$.manyInputMixerDurationsUnknown", "manyInputMixerDurationsUnknown requires format 102");
        else if (!mixers->isBoolean() || !mixers->boolean())
            addError(result, "$.manyInputMixerDurationsUnknown", "Expected true");
        else
            result.document.manyInputMixerDurationsUnknown = true;
    }
    if (const auto* pages = objectMember(parsed.value, "virtualControlLayoutPages"); pages != nullptr)
    {
        if (sourceVersion < versionOneHundredTwentySix)
            addError(result, "$.virtualControlLayoutPages", "virtualControlLayoutPages requires format 126");
        else if (!pages->isArray() || pages->array().size() > 256)
            addError(result, "$.virtualControlLayoutPages", "Expected at most 256 layouts");
        else
        {
            for (std::size_t index = 0; index < pages->array().size(); ++index)
            {
                const auto& item = pages->array()[index];
                const auto path = "$.virtualControlLayoutPages[" + std::to_string(index) + "]";
                const auto* name = item.isObject() ? objectMember(item, "name") : nullptr;
                const auto* names = item.isObject() ? objectMember(item, "eventNames") : nullptr;
                if (!item.isObject() || item.object().size() != 2 || name == nullptr || !name->isString() || name->string().empty()
                    || name->string().size() > maximumVirtualControlLabelBytes || names == nullptr || !names->isArray()
                    || !std::ranges::all_of(names->array(), [](const json::Value& value) { return value.isString(); }))
                {
                    addError(result, path, "Expected a name and the Event Values the layout shows");
                    continue;
                }
                VirtualControlLayoutPage page{name->string(), {}};
                for (const auto& value : names->array())
                    page.eventNames.push_back(value.string());
                result.document.virtualControlLayoutPages.push_back(std::move(page));
            }
        }
    }
    if (const auto* layouts = objectMember(parsed.value, "virtualControlLayoutSettings"); layouts != nullptr)
        parseVirtualControlLayoutSettings(result, *layouts, sourceVersion);
    if (const auto* collections = objectMember(parsed.value, "soundCollections"); collections != nullptr)
    {
        if (sourceVersion < versionOneHundredForty)
            addError(result, "$.soundCollections", "soundCollections requires format 140");
        else if (!collections->isArray())
            addError(result, "$.soundCollections", "Expected an array of Sound collections");
        else
        {
            for (std::size_t index = 0; index < collections->array().size(); ++index)
            {
                const auto& item = collections->array()[index];
                const auto path = "$.soundCollections[" + std::to_string(index) + "]";
                const auto* id = item.isObject() ? objectMember(item, "id") : nullptr;
                const auto* name = item.isObject() ? objectMember(item, "name") : nullptr;
                const auto* sounds = item.isObject() ? objectMember(item, "sounds") : nullptr;
                const auto* nested = item.isObject() ? objectMember(item, "collections") : nullptr;
                const auto strings = [](const json::Value* values) {
                    return values != nullptr && values->isArray()
                        && std::ranges::all_of(values->array(), [](const json::Value& value) { return value.isString(); });
                };
                if (!item.isObject() || item.object().size() != 4 || id == nullptr || !id->isString() || name == nullptr
                    || !name->isString() || !strings(sounds) || !strings(nested))
                {
                    addError(result, path, "Expected a collection's id, name, Sounds and collections");
                    continue;
                }
                ProjectSoundCollection collection;
                collection.collectionId = id->string();
                collection.name = name->string();
                for (const auto& value : sounds->array())
                    collection.soundIds.push_back(value.string());
                for (const auto& value : nested->array())
                    collection.collectionIds.push_back(value.string());
                result.document.soundCollections.push_back(std::move(collection));
            }
        }
    }
    if (const auto* decorations = objectMember(parsed.value, "virtualControlDecorations"); decorations != nullptr)
    {
        if (sourceVersion < versionOneHundredThirtySeven)
            addError(result, "$.virtualControlDecorations", "virtualControlDecorations requires format 137");
        else if (!decorations->isArray() || decorations->array().size() > maximumVirtualControlDecorations)
            addError(result, "$.virtualControlDecorations", "Expected at most 64 texts and boxes");
        else
        {
            for (std::size_t index = 0; index < decorations->array().size(); ++index)
            {
                const auto& item = decorations->array()[index];
                const auto path = "$.virtualControlDecorations[" + std::to_string(index) + "]";
                const auto* kind = item.isObject() ? objectMember(item, "kind") : nullptr;
                const auto* text = item.isObject() ? objectMember(item, "text") : nullptr;
                const auto* bounds = item.isObject() ? objectMember(item, "bounds") : nullptr;
                std::array<std::int32_t, 4> values{};
                const auto placed = bounds != nullptr && bounds->isArray() && bounds->array().size() == 4
                    && std::ranges::all_of(std::views::iota(std::size_t{0}, std::size_t{4}), [&](std::size_t at) {
                           const auto& value = bounds->array()[at];
                           return value.isNumber() && parseSigned32BitIntegerToken(value.numberToken(), values[at]);
                       });
                if (!item.isObject() || item.object().size() != 3 || kind == nullptr || !kind->isString()
                    || (kind->string() != "text" && kind->string() != "box") || text == nullptr || !text->isString()
                    || text->string().size() > maximumVirtualControlDecorationTextBytes || !placed || values[2] < 1
                    || values[3] < 1)
                {
                    addError(result, path, "Expected a kind (text or box), its text and its left, top, width and height");
                    continue;
                }
                VirtualControlDecoration decoration{
                    kind->string() == "box" ? VirtualControlDecorationKind::Box : VirtualControlDecorationKind::Text,
                    text->string(),
                    VirtualControlBounds{values[0], values[1], values[2], values[3]},
                };
                if (!isValidVirtualControlDecoration(decoration))
                {
                    addError(result, path, "A text or box lies on the surface, its text without control characters");
                    continue;
                }
                result.document.virtualControlDecorations.push_back(std::move(decoration));
            }
        }
    }
    if (const auto* layout = objectMember(parsed.value, "virtualControlLayout"); layout != nullptr)
    {
        if (sourceVersion < versionOneHundredTwentyFive)
            addError(result, "$.virtualControlLayout", "virtualControlLayout requires format 125");
        else if (layout->isString() && layout->string() == "fixed")
            result.document.virtualControlLayout = VirtualControlLayout::Fixed;
        else if (layout->isString() && layout->string() == "fit")
            result.document.virtualControlLayout = VirtualControlLayout::Fit;
        else
            addError(result, "$.virtualControlLayout", "Expected fixed or fit");
    }
    if (const auto* ramps = objectMember(parsed.value, "rampsHoldUntriggered"); ramps != nullptr)
    {
        if (sourceVersion < versionOneHundredThree)
            addError(result, "$.rampsHoldUntriggered", "rampsHoldUntriggered requires format 103");
        else if (!ramps->isBoolean() || !ramps->boolean())
            addError(result, "$.rampsHoldUntriggered", "Expected true");
        else
            result.document.rampsHoldUntriggered = true;
    }
    if (const auto* loops = objectMember(parsed.value, "graphicalEnvelopeLoopsPlayOut"); loops != nullptr)
    {
        if (sourceVersion < versionOneHundredFour)
            addError(result, "$.graphicalEnvelopeLoopsPlayOut", "graphicalEnvelopeLoopsPlayOut requires format 104");
        else if (!loops->isBoolean() || !loops->boolean())
            addError(result, "$.graphicalEnvelopeLoopsPlayOut", "Expected true");
        else
            result.document.graphicalEnvelopeLoopsPlayOut = true;
    }
    if (const auto* spectra = objectMember(parsed.value, "banksReadLogSpectraAsHertz"); spectra != nullptr)
    {
        if (sourceVersion < versionOneHundredEight)
            addError(result, "$.banksReadLogSpectraAsHertz", "banksReadLogSpectraAsHertz requires format 108");
        else if (!spectra->isBoolean() || !spectra->boolean())
            addError(result, "$.banksReadLogSpectraAsHertz", "Expected true");
        else
            result.document.banksReadLogSpectraAsHertz = true;
    }
    if (const auto* shared = objectMember(parsed.value, "randomExpressionsShareSeeds"); shared != nullptr)
    {
        if (sourceVersion < versionOneHundredFourteen)
            addError(result, "$.randomExpressionsShareSeeds", "randomExpressionsShareSeeds requires format 114");
        else if (!shared->isBoolean() || !shared->boolean())
            addError(result, "$.randomExpressionsShareSeeds", "Expected true");
        else
            result.document.randomExpressionsShareSeeds = true;
    }
    if (const auto* unbent = objectMember(parsed.value, "keyPitchIgnoresPitchBend"); unbent != nullptr)
    {
        if (sourceVersion < versionOneHundredSixteen)
            addError(result, "$.keyPitchIgnoresPitchBend", "keyPitchIgnoresPitchBend requires format 116");
        else if (!unbent->isBoolean() || !unbent->boolean())
            addError(result, "$.keyPitchIgnoresPitchBend", "Expected true");
        else
            result.document.keyPitchIgnoresPitchBend = true;
    }
    if (const auto* faders = objectMember(parsed.value, "randomEventSourcesAreFaders"); faders != nullptr)
    {
        if (sourceVersion < versionOneHundredSeventeen)
            addError(result, "$.randomEventSourcesAreFaders", "randomEventSourcesAreFaders requires format 117");
        else if (!faders->isBoolean() || !faders->boolean())
            addError(result, "$.randomEventSourcesAreFaders", "Expected true");
        else
            result.document.randomEventSourcesAreFaders = true;
    }
    if (const auto* atOnce = objectMember(parsed.value, "signSelectionsSelectAtOnce"); atOnce != nullptr)
    {
        if (sourceVersion < versionOneHundredEighteen)
            addError(result, "$.signSelectionsSelectAtOnce", "signSelectionsSelectAtOnce requires format 118");
        else if (!atOnce->isBoolean() || !atOnce->boolean())
            addError(result, "$.signSelectionsSelectAtOnce", "Expected true");
        else
            result.document.signSelectionsSelectAtOnce = true;
    }
    if (const auto* runsOn = objectMember(parsed.value, "timeRunsOnInSkippedBranches"); runsOn != nullptr)
    {
        if (sourceVersion < versionOneHundredNineteen)
            addError(result, "$.timeRunsOnInSkippedBranches", "timeRunsOnInSkippedBranches requires format 119");
        else if (!runsOn->isBoolean() || !runsOn->boolean())
            addError(result, "$.timeRunsOnInSkippedBranches", "Expected true");
        else
            result.document.timeRunsOnInSkippedBranches = true;
    }
    if (const auto* floats = objectMember(parsed.value, "smalltalkNumbersAreFloats"); floats != nullptr)
    {
        if (sourceVersion < versionOneHundredTwentyOne)
            addError(result, "$.smalltalkNumbersAreFloats", "smalltalkNumbersAreFloats requires format 121");
        else if (!floats->isBoolean() || !floats->boolean())
            addError(result, "$.smalltalkNumbersAreFloats", "Expected true");
        else
            result.document.smalltalkNumbersAreFloats = true;
    }
    for (const auto& [name, flag] : {std::pair{"timeEventSourcesAreFaders", &ProjectDocument::timeEventSourcesAreFaders},
             std::pair{"timeFunctionsCountFromTheWholeSound", &ProjectDocument::timeFunctionsCountFromTheWholeSound}})
    {
        const auto* member = objectMember(parsed.value, name);
        if (member == nullptr)
            continue;
        const auto path = std::string{"$."} + name;
        if (sourceVersion < versionOneHundredTwentyTwo)
            addError(result, path, std::string{name} + " requires format 122");
        else if (!member->isBoolean() || !member->boolean())
            addError(result, path, "Expected true");
        else
            result.document.*flag = true;
    }
    if (const auto* member = objectMember(parsed.value, "timeEventsIgnoreTimeControls"))
    {
        if (sourceVersion < versionOneHundredFiftyThree)
            addError(result, "$.timeEventsIgnoreTimeControls", "timeEventsIgnoreTimeControls requires format 153");
        else if (!member->isBoolean() || !member->boolean())
            addError(result, "$.timeEventsIgnoreTimeControls", "Expected true");
        else
            result.document.timeEventsIgnoreTimeControls = true;
    }
    // Format 156: choices that a project of an earlier format has off, so
    // it plays as it did.
    for (const auto& [name, flag] : {std::pair{"pastedSoundsSaturateAtUnity", &ProjectDocument::pastedSoundsSaturateAtUnity},
             std::pair{"pastedSoundsReadThePreviousMillisecond", &ProjectDocument::pastedSoundsReadThePreviousMillisecond},
             std::pair{"inverseOfZeroIsInfinite", &ProjectDocument::inverseOfZeroIsInfinite}})
    {
        const auto* member = objectMember(parsed.value, name);
        if (member == nullptr)
            continue;
        const auto path = std::string{"$."} + name;
        if (sourceVersion < versionOneHundredFiftySix)
            addError(result, path, std::string{name} + " requires format 156");
        else if (!member->isBoolean() || !member->boolean())
            addError(result, path, "Expected true");
        else
            result.document.*flag = true;
    }
    if (sourceVersion < versionOneHundredNine)
    {
        for (std::size_t definition = 0; definition < result.document.soundDefinitions.size(); ++definition)
        {
            const auto& exposed = result.document.soundDefinitions[definition].exposedParameters;
            if (std::ranges::any_of(exposed, [](const ProjectSoundExposedParameter& parameter) {
                    return std::ranges::any_of(parameter.bindings, [](const ProjectSoundParameterBinding& binding) {
                        return binding.target == ProjectSoundParameterTarget::Variable;
                    });
                }))
            {
                addError(result, "$.soundDefinitions[" + std::to_string(definition) + "]",
                    "variable bindings require format 109");
            }
        }
    }
    if (const auto* globalMap = objectMember(parsed.value, "globalMap"); globalMap != nullptr)
    {
        if (sourceVersion < versionNinetyEight)
            addError(result, "$.globalMap", "A project's global map requires format 98");
        else if (!globalMap->isString() || globalMap->string().empty())
            addError(result, "$.globalMap", "Expected the global map's text");
        else if (const auto map = parseProjectGlobalMap(globalMap->string()); !map.ok())
            addError(result, "$.globalMap", "Global map line " + std::to_string(map.line) + ": " + map.error);
        else
            result.document.globalMap = globalMap->string();
    }
    if (sourceVersion >= versionTwentyOne && result.ok())
    {
        std::vector<std::string_view> soundDefinitionIds;
        soundDefinitionIds.reserve(result.document.soundDefinitions.size());
        for (const auto& definition : result.document.soundDefinitions)
            soundDefinitionIds.push_back(definition.definitionId);
        if (const auto scoreError = projectScoreValidationError(
                result.document.timeline.score,
                soundDefinitionIds))
        {
            const auto path = scoreError->entityId.empty()
                ? std::string{"$.timeline.score"}
                : "$.timeline.score[" + scoreError->entityId + "]";
            addError(result, path, scoreError->message);
        }
        if (result.ok())
        {
            if (const auto artifactError =
                    validateProjectScoreRenderArtifacts(result.document))
            {
                addError(
                    result,
                    "$.timeline.score.clips[" + artifactError->path
                        + "].renderArtifact",
                    artifactError->message);
            }
        }
    }
    parseGraph(
        result,
        parsed.value,
        sourceVersion,
        preservedComponentCount,
        preservedComponentStateBytes);
    if (result.ok())
    {
        if (const auto sourceError =
                validateProjectScoreSourceBindings(result.document))
        {
            addError(
                result,
                "$.timeline.score.clips[" + sourceError->entityId + "]",
                sourceError->message);
        }
    }
    if (result.ok())
    {
        const auto validation = validateProjectVariableLibrary(
            result.document);
        for (const auto& diagnostic : validation.diagnostics)
            addError(result, diagnostic.path, diagnostic.message);
        if (validation.truncated)
        {
            addError(
                result,
                "$.variableLibrary",
                "Variable diagnostics exceed the fixed limit");
        }
    }
    if (result.ok())
    {
        const auto validation = validateProjectSoundVariableLibrary(
            result.document);
        for (const auto& diagnostic : validation.diagnostics)
            addError(result, diagnostic.path, diagnostic.message);
        if (validation.truncated)
        {
            addError(
                result,
                "$.soundVariableLibrary",
                "Sound-variable diagnostics exceed the fixed limit");
        }
    }
    if (result.ok())
    {
        const auto validation = validateProjectGeneratorLibrary(
            result.document.generatorLibrary);
        for (const auto& diagnostic : validation.diagnostics)
        {
            addError(
                result,
                diagnostic.path,
                diagnostic.message + " at bytes "
                    + std::to_string(diagnostic.sourceRange.begin) + ":"
                    + std::to_string(
                        diagnostic.sourceRange.end
                            - diagnostic.sourceRange.begin));
        }
        if (validation.truncated)
        {
            addError(
                result,
                "$.generatorLibrary",
                "Generator diagnostics exceed the fixed limit");
        }
    }
    if (result.ok())
    {
        const auto validation =
            validateProjectGeneratorOutputTargets(
                result.document,
                registry);
        for (const auto& diagnostic : validation.diagnostics)
            addError(result, diagnostic.path, diagnostic.message);
        if (validation.truncated)
        {
            addError(
                result,
                "$.generatorLibrary",
                "Generator target diagnostics exceed the fixed limit");
        }
    }
    if (sourceVersion >= versionEleven)
    {
        rejectFormatElevenInlinePositions(result, parsed.value);
        parsePresentation(result, parsed.value, sourceVersion);
        if (result.ok() && sourceVersion < supportedVersion)
            reconcileProjectPresentation(result.document);
        parseSnapshotLibrary(result, parsed.value, sourceVersion);
    }
    else if (sourceVersion > 0)
    {
        migrateLegacyPresentation(result.document);
    }
    if (sourceVersion >= versionTwelve)
        parseSampleMapLibrary(result, parsed.value);
    if (result.ok())
    {
        const auto validation = validateProjectLiveSetLibrary(result.document);
        for (const auto& diagnostic : validation.diagnostics)
            addError(result, diagnostic.path, diagnostic.message);
        if (validation.truncated)
        {
            addError(
                result,
                "$.liveSetLibrary",
                "Live-set diagnostics exceed the fixed limit");
        }
    }
    if (result.ok())
        migrateSoundParameterAliases(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionThirtyEight)
        migrateLegacyMidiMapperVoiceModels(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion <= versionThirtyEight)
        migrateLegacyAnalogSequencerIndices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion <= versionForty)
        migrateLegacyAnalogSequencerStepZeroModes(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFortyThree)
        migrateLegacyMixers(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFortyFour)
        migrateLegacyTunableVocoderSpacing(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFortyFive)
        migrateLegacyTimePacing(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFortySix)
        migrateLegacyOverlappingMixers(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFortySeven)
        migrateLegacyTrackerUnits(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFortyEight)
        migrateLegacyArcTans(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFortyNine)
        migrateLegacyTrackerScales(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFifty)
        migrateLegacyGrainDurations(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftyOne)
        migrateLegacyTimeOffsets(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftyTwo)
        migrateLegacyFrequencyScaleFormants(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftyThree)
        migrateLegacyTrackSelectorUnits(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftyFour)
        migrateLegacyAnalysisFilters(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftyFive)
        migrateLegacySpectralShapes(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftySix)
        migrateLegacyDiskPlayerDurations(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftySeven)
        migrateLegacyLiveSpectralTracks(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftyEight)
        migrateLegacyFilterBankBandwidths(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionFiftyNine)
        migrateLegacyCloudBankGrainStarts(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixty)
        migrateTwoFormantVoiceExcitation(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtyOne)
        migrateSpectrumModifierFreqOffsetUnits(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtyTwo)
        migrateFftPacking(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtyThree)
        migrateReResonatorFilterForm(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtyFour)
        migrateTimeOffsetOffsetMeans(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtyFive)
        migrateContextFreeGrammarInputs(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtySix)
        migrateSetDurationStartTimes(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtySeven)
        migrateMidiOutputs(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSixtyNine)
        migrateReverbSectionSmoothing(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventy)
        migrateHarmonicResonatorPrezero(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventyOne)
        migrateFormatSeventyOneChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventyTwo)
        migrateHarmonicResonatorTuning(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventyThree)
        migrateFormatSeventyThreeChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventyFour)
        migrateFormatSeventyFourChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventyFive)
        migrateFormatSeventyFiveChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventySix)
        migrateFormatSeventySixChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventySeven)
        migrateFormatSeventySevenChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventyEight)
        migrateFormatSeventyEightChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionSeventyNine)
        migrateFormatSeventyNineChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEighty)
        migrateFormatEightyChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightyOne)
        migrateFormatEightyOneChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightyTwo)
        migrateFormatEightyTwoChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightyThree)
        migrateFormatEightyThreeChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightyFour)
        migrateFormatEightyFourChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightyFive)
        migrateFormatEightyFiveChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightySix)
        migrateFormatEightySixChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightySeven)
        migrateFormatEightySevenChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightyEight)
        migrateFormatEightyEightChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionEightyNine)
        migrateFormatEightyNineChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionNinety)
        migrateSoundCollectionMembers(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionNinetyOne)
        migrateFormatNinetyOneChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionNinetyTwo)
        migrateFormatNinetyTwoChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionNinetyThree)
        migrateFormatNinetyThreeChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionNinetyFour)
        migrateFormatNinetyFourChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionNinetyFive)
        migrateFormatNinetyFiveChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionNinetySix)
        migrateFormatNinetySixChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundred)
        migrateDynamicRangeControllerRatioLaw(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredOne)
        migrateFormatOneHundredOneChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredTwo)
        migrateFormatOneHundredTwoChoices(result);
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredSeven)
        migrateFormatOneHundredSevenChoices(result);
    // Projects before format 100 start ramp, ramp:, fullRamp and fullRamp:
    // at their first value; they now rest at 1 until their receiver first
    // becomes positive. Projects before format 103 hold a
    // ramp or full ramp while its receiver is off; they now run on to their
    // end once triggered. A project that names no ramp needs no
    // legacy choice.
    const auto namesRamp = [&] {
        const auto inGraph = [](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [](const ProjectNode& node) {
                return std::ranges::any_of(node.parameters, [](const ProjectParameterExpression& parameter) {
                    const auto mentions = [](const std::optional<std::string>& text) {
                        return text.has_value()
                            && (text->find("ramp") != std::string::npos || text->find("Ramp") != std::string::npos);
                    };
                    return mentions(parameter.expression) || mentions(parameter.text);
                });
            });
        };
        return inGraph(result.document.graph)
            || std::ranges::any_of(result.document.soundDefinitions,
                [&](const ProjectSoundDefinition& definition) { return inGraph(definition.graph); });
    };
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundred)
        result.document.rampsStartAtFirstValue = namesRamp();
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredThree)
        result.document.rampsHoldUntriggered = namesRamp();
    // Projects before format 104 with a looped GraphicalEnvelope play the
    // loop's pass out when Gate falls; it now jumps to the loop's end.
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredFour)
    {
        const auto loops = [](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [](const ProjectNode& node) {
                return node.typeId == "talas.graphicalEnvelope"
                    && std::ranges::any_of(node.parameters, [](const ProjectParameterExpression& parameter) {
                           return parameter.parameterId == "envelope" && parameter.text.has_value()
                               && parameter.text->find("loopEnd") != std::string::npos;
                       });
            });
        };
        result.document.graphicalEnvelopeLoopsPlayOut = loops(result.document.graph)
            || std::ranges::any_of(result.document.soundDefinitions,
                [&](const ProjectSoundDefinition& definition) { return loops(definition.graph); });
    }
    // In projects before format 108 an OscillatorBank reads a log
    // spectrum's note numbers as hertz; one that makes no log spectrum needs
    // no legacy choice.
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredEight)
    {
        const auto logged = [](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [](const ProjectNode& node) {
                if (node.typeId == "talas.spectrumLinearToLog")
                    return true;
                return (node.typeId == "talas.syntheticSpectrumFromSounds"
                           || node.typeId == "talas.syntheticSpectrumFromArray")
                    && std::ranges::any_of(node.parameters, [](const ProjectParameterExpression& parameter) {
                           return parameter.parameterId == "logScale" && parameter.boolean == std::optional<bool>{true};
                       });
            });
        };
        result.document.banksReadLogSpectraAsHertz = logged(result.document.graph)
            || std::ranges::any_of(result.document.soundDefinitions,
                [&](const ProjectSoundDefinition& definition) { return logged(definition.graph); });
    }
    // Projects before format 106 read a Val .5 as 0.5; Val now needs the
    // leading zero, so they gain it.
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredSix
        && result.document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1)
    {
        const auto addZeros = [](ProjectParameterExpression& parameter) {
            if (parameter.expression.has_value())
                parameter.expression = talas::expr::withLeadingZeros(*parameter.expression);
        };
        const auto inGraph = [&](ProjectGraph& graph) {
            for (auto& node : graph.nodes)
                std::ranges::for_each(node.parameters, addZeros);
        };
        inGraph(result.document.graph);
        for (auto& definition : result.document.soundDefinitions)
        {
            inGraph(definition.graph);
            for (auto& exposed : definition.exposedParameters)
                addZeros(exposed.defaultValue);
        }
    }
    // In projects before format 110 removeUnits gave a bpm frequency's
    // hertz; it now gives its beats per minute, so they write hz between to
    // keep the number they had.
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredTen
        && result.document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1)
    {
        const auto inHertz = [](ProjectParameterExpression& parameter) {
            if (parameter.expression.has_value())
                parameter.expression = talas::expr::withBpmRemoveUnitsInHertz(*parameter.expression);
        };
        const auto inGraph = [&](ProjectGraph& graph) {
            for (auto& node : graph.nodes)
                std::ranges::for_each(node.parameters, inHertz);
        };
        inGraph(result.document.graph);
        for (auto& definition : result.document.soundDefinitions)
        {
            inGraph(definition.graph);
            for (auto& exposed : definition.exposedParameters)
                inHertz(exposed.defaultValue);
        }
    }
    // In projects before format 111 a number written right before d was the
    // pitch d of that octave; d now marks a double, 8.1758d being 8.1758
    //, so they put a space between to keep their pitch.
    // removeUnits also gave the hertz of a bpm frequency a number multiplied
    // or divided; it now gives its beats per minute, so they write
    // hz after such a bpm.
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredEleven
        && result.document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1)
    {
        const auto apart = [](ProjectParameterExpression& parameter) {
            if (parameter.expression.has_value())
                parameter.expression = talas::expr::withBpmTimesNumbersRemoveUnitsInHertz(
                    talas::expr::withPitchDApart(*parameter.expression));
        };
        const auto inGraph = [&](ProjectGraph& graph) {
            for (auto& node : graph.nodes)
                std::ranges::for_each(node.parameters, apart);
        };
        inGraph(result.document.graph);
        for (auto& definition : result.document.soundDefinitions)
        {
            inGraph(definition.graph);
            for (auto& exposed : definition.exposedParameters)
                apart(exposed.defaultValue);
        }
    }
    // In projects before format 112 a bpm: beat went straight to 1; it now
    // dips to -1 for five milliseconds first, stealing its voice as !KeyDown
    // does, so they take each bpm: message's abs.
    if (result.ok() && sourceVersion > 0
        && sourceVersion < versionOneHundredTwelve
        && result.document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1)
    {
        const auto steady = [](ProjectParameterExpression& parameter) {
            if (parameter.expression.has_value())
                parameter.expression = talas::expr::withMetronomesWithoutVoiceStealing(*parameter.expression);
        };
        const auto inGraph = [&](ProjectGraph& graph) {
            for (auto& node : graph.nodes)
                std::ranges::for_each(node.parameters, steady);
        };
        inGraph(result.document.graph);
        for (auto& definition : result.document.soundDefinitions)
        {
            inGraph(definition.graph);
            for (auto& exposed : definition.exposedParameters)
                steady(exposed.defaultValue);
        }
    }
    // In Val projects before format 114 fields reading the same random
    // expression drew the same numbers; each now draws its own. One with a
    // random message keeps drawing alike.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFourteen
        && result.document.expressionLanguage == talas::expr::ExpressionLanguageVersion::ValV1)
    {
        const auto drawsAtRandom = [](const ProjectParameterExpression& parameter) {
            if (!parameter.expression.has_value())
                return false;
            std::string text = *parameter.expression;
            std::ranges::transform(text, text.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return text.find("random") != std::string::npos || text.find("normal") != std::string::npos
                || text.find("randexp") != std::string::npos || text.find("chaotic") != std::string::npos;
        };
        const auto inGraph = [&](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [&](const ProjectNode& node) {
                return std::ranges::any_of(node.parameters, drawsAtRandom);
            });
        };
        result.document.randomExpressionsShareSeeds = inGraph(result.document.graph)
            || std::ranges::any_of(result.document.soundDefinitions, [&](const ProjectSoundDefinition& definition) {
                   return inGraph(definition.graph)
                       || std::ranges::any_of(definition.exposedParameters,
                           [&](const auto& exposed) { return drawsAtRandom(exposed.defaultValue); });
               });
    }
    // In projects before format 115 Random new gave the same stream on every
    // run; it now gives another each time, so their
    // fields write that stream's seed.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFifteen)
    {
        const auto repeatable = [](ProjectParameterExpression& parameter) {
            for (auto* field : {&parameter.expression, &parameter.text})
            {
                if (field->has_value() && field->value().find("Random new") != std::string::npos)
                    *field = smalltalk::withRandomNewRepeatable(**field);
            }
        };
        const auto inGraph = [&](ProjectGraph& graph) {
            for (auto& node : graph.nodes)
                std::ranges::for_each(node.parameters, repeatable);
        };
        inGraph(result.document.graph);
        for (auto& definition : result.document.soundDefinitions)
        {
            inGraph(definition.graph);
            for (auto& exposed : definition.exposedParameters)
                repeatable(exposed.defaultValue);
        }
    }
    // In Val projects before format 116 !KeyPitch was the key's number
    // alone; it now follows the pitch wheel. One
    // that reads it keeps the key alone.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredSixteen)
        result.document.keyPitchIgnoresPitchBend = readsKeyPitch(result.document);
    // In Val projects before format 117 !Random and !Random1000 were
    // Event Values of their own; they now draw every millisecond. One that
    // reads them keeps its faders.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredSeventeen)
        result.document.randomEventSourcesAreFaders = readsRandomEventSources(result.document);
    // In Val projects before format 118 neg:zero:pos: selected at once;
    // it now starts at 0 until its receiver first changes. One that holds it
    // keeps selecting at once.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredEighteen)
        result.document.signSelectionsSelectAtOnce = holdsSignSelections(result.document);
    // In Val projects before format 119 a time function ran on through
    // a branch not taken; it now stops and carries on from where it left off
    //. One holding a conditional and state keeps running on.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredNineteen)
        result.document.timeRunsOnInSkippedBranches = holdsTimeInSkippedBranches(result.document);
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredTwenty)
        migrateLegacyMidiVoicePolyphony(result);
    // In projects before format 121 every Smalltalk number was floating
    // point; integers and ratios of them are now exact.
    // One that holds Smalltalk keeps its floating point.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredTwentyOne)
        result.document.smalltalkNumbersAreFloats = holdsSmalltalkPrograms(result.document);
    // In projects before format 122 !RealTime and !Time were Event Values of
    // their own in Val, and every function of time counted from the
    // start of the whole Sound; they are now the whole Sound's clocks, and
    // each function of time counts from the start of its own Sound. One
    // that reads them or holds a function of time keeps what it did.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredTwentyTwo)
    {
        result.document.timeEventSourcesAreFaders = readsTimeEventSources(result.document);
        result.document.timeFunctionsCountFromTheWholeSound = holdsTimeFunctions(result.document);
    }
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFortyFour)
        migrateFormatOneHundredFortyFourChoices(result);
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFortyEight)
        migrateGainOverflow(result);
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFortyNine)
        migrateLimeInterpreterInputs(result);
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFifty)
        migratePhaseShiftOverflow(result);
    // Projects before format 154 wrote their caches
    // in 16 bits, and go on doing so.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFiftyFour)
        result.document.timeline.cacheWordSize = 16;
    // A Script of a project before format 155 whose
    // events put a value against a keyword's colon keeps playing.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFiftyFive)
        migrateScriptKeywordSpacing(result);
    // Projects before format 153 kept !Time and !LocalTime at real time
    // inside TimeControls and TimeStoppers; since, they count the time those
    // let pass. One holding either keeps
    // what it did.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFiftyThree)
    {
        const auto holds = [](const ProjectGraph& graph) {
            return std::ranges::any_of(graph.nodes, [](const ProjectNode& node) {
                return node.typeId == "talas.timeControl" || node.typeId == "talas.timeStopper";
            });
        };
        result.document.timeEventsIgnoreTimeControls = holds(result.document.graph)
            || std::ranges::any_of(result.document.soundDefinitions, [&](const ProjectSoundDefinition& definition) {
                   return holds(definition.graph);
               });
    }
    // Projects before format 152 sent a MIDIOutputController's empty Value
    // as controller 1; since, the prototype sends its Amp as controller 7
    //.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFiftyTwo)
    {
        migrateLegacyDefault(result, "talas.midiOutputController", "controllerNumber", "1");
        migrateLegacyTextDefault(result, "talas.midiOutputController", "value", "");
    }
    // Projects before format 151 held an ADSR's constant Gate to the end of
    // its Sound; since, its release ends with the Sound.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredFiftyOne)
    {
        migrateLegacyChoice(result, "talas.adsr", "constantGate", "legacyHolds",
            "ADSR constantGate requires project format 151");
    }
    // Since format 113 a Val Event Value's name is not case sensitive
    //. A project before it keeps names differing only in
    // case apart, each after the first gaining a number; one since cannot
    // have two Virtual Controls so named.
    if (result.ok() && sourceVersion > 0 && sourceVersion < versionOneHundredThirteen)
        keepEventValueSpellingsApart(result.document);
    else if (result.ok())
    {
        if (const auto twin = findEventValueSpellingTwin(result.document))
            addError(result, "$.virtualControls[" + std::to_string(*twin) + "].eventName",
                "Duplicate Event Value name, differing only in case");
    }
    if (result.ok() && sourceVersion == versionOne)
        migrateVersionOne(result.document);
    if (result.ok())
    {
        const auto expressionAudit = auditProjectExpressions(
            result.document,
            result.document.expressionLanguage);
        for (const auto& diagnostic : expressionAudit.diagnostics)
        {
            addError(
                result,
                diagnostic.path,
                diagnostic.message + " at bytes "
                    + std::to_string(diagnostic.range.offset) + ":"
                    + std::to_string(diagnostic.range.length));
        }
        if (expressionAudit.truncated)
        {
            addError(
                result,
                "$",
                "Project expression diagnostics exceed the fixed limit");
        }
    }
    return result;
}

ProjectLoadResult parseProjectDocument(std::string_view jsonText)
{
    return parseProjectDocument(
        jsonText, talas::core::makeBuiltinNodeRegistry());
}
}
