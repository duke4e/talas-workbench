#include "talas/app/ClassEditorModel.h"
#include "talas/app/PrototypeBrowserModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/core/FromChars.h"
#include "talas/project/ProjectClassFromExample.h"

#include "talas/expr/Expression.h"

#include <sstream>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string_view trimmed(std::string_view text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] std::optional<double> number(std::string_view text)
{
    text = trimmed(text);
    if (text.empty())
        return std::nullopt;
    auto value = 0.0;
    const auto [end, failed] = talas::fromChars(text.data(), text.data() + text.size(), value);
    if (failed != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

[[nodiscard]] std::string numberText(double value)
{
    std::array<char, 64> buffer{};
    const auto [end, failed] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return failed == std::errc{} ? std::string(buffer.data(), end) : std::string{"0"};
}

// `x @ y`.
[[nodiscard]] std::optional<std::pair<double, double>> point(std::string_view text)
{
    const auto at = text.find('@');
    if (at == std::string_view::npos)
        return std::nullopt;
    const auto x = number(text.substr(0, at));
    const auto y = number(text.substr(at + 1));
    if (!x || !y)
        return std::nullopt;
    return std::pair{*x, *y};
}

constexpr std::array<std::pair<std::string_view, talas::expr::ValueDomain>, 5> domains{{
    {"scalar", talas::expr::ValueDomain::Scalar},
    {"hertz", talas::expr::ValueDomain::Hertz},
    {"seconds", talas::expr::ValueDomain::Seconds},
    {"note number", talas::expr::ValueDomain::NoteNumber},
    {"samples", talas::expr::ValueDomain::Samples},
}};

// The parameter types, in alphabetical order.
constexpr std::array<std::string_view, 2> channelNbrChoices{"Mono", "Stereo"};
constexpr std::array<std::string_view, 5> diskFormatChoices{"SDI", "SDII", "AIFF", "IRCAM", "WAVE"};
constexpr std::array<std::string_view, 2> dynamicsTypeChoices{"compressor", "expander"};
constexpr std::array<std::string_view, 2> envelopeSegmentTypeChoices{"linear", "exponential"};
constexpr std::array<std::string_view, 2> feedbackDelayTypeChoices{"comb", "allpass"};
constexpr std::array<std::string_view, 3> genericSourceChoices{"Disk", "Live", "RAM"};
constexpr std::array<std::string_view, 2> interpolationChoices{"none", "linear"};
constexpr std::array<std::string_view, 5> lowestAnalyzedFreqChoices{"Above 1F (44 hz)", "Above 2F (87 hz)", "Above 3F (172 hz)", "Above 4F (345 hz)", "Above 5F (690 hz)"};
constexpr std::array<std::string_view, 3> midiSourceChoices{"MIDIInput", "MIDIFile", "Script"};
constexpr std::array<std::string_view, 2> modulationChoices{"frequency", "none"};
constexpr std::array<std::string_view, 2> oscillatorBankInputTypeChoices{"SpectralShape", "SOSAnalysis"};
constexpr std::array<std::string_view, 2> panFunctionChoices{"linear", "power"};
constexpr std::array<std::string_view, 2> shapeSourceChoices{"wavetable", "polynomial"};
constexpr std::array<std::string_view, 4> wavetableAccessChoices{"none", "readWrite", "readOnly", "writeOnly"};
constexpr std::array<std::string_view, 3> wordSizeChoices{"8", "16", "24"};

const std::array talasTypes{
    TalasParameterType{"almostOneScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 0.9, 1.0, {}, false,
        "0.9 to 1.0"},
    TalasParameterType{"array", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any list of one or more objects"},
    TalasParameterType{"boolean", talas::core::ParameterKind::Boolean, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "true or false"},
    TalasParameterType{"channelNbr", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, channelNbrChoices, false,
        "Mono or Stereo"},
    TalasParameterType{"codeString", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "Smalltalk code"},
    TalasParameterType{"complex", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any complex number, real % imaginary"},
    TalasParameterType{"delayLine", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "wavetable name"},
    TalasParameterType{"diskFormat", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, diskFormatChoices, false,
        "SDI, SDII, AIFF, IRCAM or WAVE"},
    TalasParameterType{"doubleScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, -2.0, 2.0, {}, false,
        "-2.0 to 2.0"},
    TalasParameterType{"duration", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Samples, 1, 140737488355327, {}, false,
        "1 to 140737488355327 samp"},
    TalasParameterType{"dynamicsType", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, dynamicsTypeChoices, false,
        "compressor or expander"},
    TalasParameterType{"envelope", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "a graphical envelope"},
    TalasParameterType{"envelopeSegmentType", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, envelopeSegmentTypeChoices, false,
        "linear or exponential"},
    TalasParameterType{"feedbackDelayType", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, feedbackDelayTypeChoices, false,
        "comb or allpass"},
    TalasParameterType{"fileName", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any legal file name"},
    TalasParameterType{"frequency", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Hertz, -25000.0, 25000.0, {}, false,
        "-25000.0 hz to 25000.0 hz"},
    TalasParameterType{"genericSource", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, genericSourceChoices, false,
        "Disk, Live, or RAM"},
    TalasParameterType{"halfScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 0.0, 0.5, {}, false,
        "0.0 to 0.5"},
    TalasParameterType{"hotArray", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, true,
        "any list of one or more objects; each element in the array can be an Event Value"},
    TalasParameterType{"hotDoubleScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, -2.0, 2.0, {}, true,
        "-2.0 to 2.0"},
    TalasParameterType{"hotFrequency", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Hertz, -25000.0, 25000.0, {}, true,
        "-25000.0 hz to 25000.0 hz"},
    TalasParameterType{"hotPositiveScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 0.0, 1.0, {}, true,
        "0 to 1.0"},
    TalasParameterType{"hotScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, -1.0, 1.0, {}, true,
        "-1.0 to 1.0"},
    TalasParameterType{"hotTime", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Samples, 0, 140737488355327, {}, true,
        "0 samp to 140737488355327 samp"},
    TalasParameterType{"hotValue", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, true,
        "number or an Event Value"},
    TalasParameterType{"integer", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, -2147483647, 2147483647, {}, false,
        "-2147483647 to 2147483647"},
    TalasParameterType{"interpolation", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, interpolationChoices, false,
        "none or linear"},
    TalasParameterType{"legalName", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "a letter followed by zero or more letters and numbers"},
    TalasParameterType{"limeFileName", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any legal Lime file name"},
    TalasParameterType{"lowestAnalyzedFreq", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, lowestAnalyzedFreqChoices, false,
        "Above 1F (44 hz) to Above 5F (690 hz)"},
    TalasParameterType{"midiChannel", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 1, 16, {}, false,
        "1 to 16"},
    TalasParameterType{"midiControllerNumber", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 0, 127, {}, false,
        "0 to 127"},
    TalasParameterType{"midiFileName", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any legal MIDI file name"},
    TalasParameterType{"midiSource", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, midiSourceChoices, false,
        "MIDIInput, MIDIFile, or Script"},
    TalasParameterType{"modulation", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, modulationChoices, false,
        "frequency or none"},
    TalasParameterType{"name", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "one or more characters"},
    // The least restrictive: Smalltalk code, any object.
    TalasParameterType{"object", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any Smalltalk object"},
    TalasParameterType{"oscillatorBankInputType", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, oscillatorBankInputTypeChoices, false,
        "SpectralShape or SOSAnalysis (obsolete)"},
    TalasParameterType{"panFunction", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, panFunctionChoices, false,
        "linear or power"},
    TalasParameterType{"polesAndZeroes", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any list of complex poles and zeroes"},
    TalasParameterType{"positiveFrequency", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Hertz, 0.001, 25000.0, {}, false,
        "0.001 hz to 25000.0 hz"},
    TalasParameterType{"positiveInteger", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 0, 214748364, {}, false,
        "0 to 214748364"},
    TalasParameterType{"positiveScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 0.0, 1.0, {}, false,
        "0 to 1.0"},
    TalasParameterType{"processor", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, 1, 8, {}, false,
        "1 to 8"},
    TalasParameterType{"samplesFileList", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any list of sample file segments"},
    TalasParameterType{"samplesFileName", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any legal sample file name"},
    TalasParameterType{"scale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, -1.0, 1.0, {}, false,
        "-1.0 to 1.0"},
    TalasParameterType{"shapeSource", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, shapeSourceChoices, false,
        "wavetable or polynomial"},
    TalasParameterType{"singleWavetableDelay", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Samples, 0, 4096, {}, false,
        "0 to 4096 samp"},
    TalasParameterType{"spectrumFileName", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any legal spectrum file name"},
    TalasParameterType{"spectrumWavetable", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "spectrum wavetable name"},
    TalasParameterType{"string", talas::core::ParameterKind::Text, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "one or more characters"},
    TalasParameterType{"textFileName", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any legal text file name"},
    TalasParameterType{"time", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Samples, 0, 140737488355327, {}, false,
        "0 samp to 140737488355327 samp"},
    TalasParameterType{"upScale", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, -16.0, 16.0, {}, false,
        "-16.0 to 16.0"},
    TalasParameterType{"value", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "any Smalltalk value"},
    TalasParameterType{"wavetable", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "wavetable name"},
    TalasParameterType{"wavetableAccess", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, wavetableAccessChoices, false,
        "none, readWrite, readOnly, or writeOnly"},
    TalasParameterType{"wavetableDelay", talas::core::ParameterKind::Expression, talas::expr::ValueDomain::Samples, 0, 4096, {}, false,
        "0 samp to 4096 samp"},
    TalasParameterType{"wavetableOfStandardLength", talas::core::ParameterKind::Resource, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, {}, false,
        "name of any 4096-sample wavetable"},
    TalasParameterType{"wordSize", talas::core::ParameterKind::Choice, talas::expr::ValueDomain::Scalar, std::nullopt, std::nullopt, wordSizeChoices, false,
        "8, 16, or 24"},
};
}

bool classEditorFieldsChanged(
    const talas::project::ProjectSoundExposedParameter& parameter, const ClassEditorParameterFields& fields)
{
    std::string error;
    const auto applied = applyClassEditorFields(parameter, fields, error);
    return !applied.has_value() || *applied != parameter;
}

std::string_view classEditorDomainName(talas::expr::ValueDomain domain)
{
    for (const auto& [name, value] : domains)
    {
        if (value == domain)
            return name;
    }
    return "scalar";
}

std::string fieldRectangleText(const talas::project::ProjectSoundFieldRectangle& rectangle)
{
    return numberText(rectangle.left) + " @ " + numberText(rectangle.top) + " corner: " + numberText(rectangle.right)
        + " @ " + numberText(rectangle.bottom);
}

std::optional<talas::project::ProjectSoundFieldRectangle> parseFieldRectangle(std::string_view text)
{
    const auto corner = text.find("corner:");
    if (corner == std::string_view::npos)
        return std::nullopt;
    const auto upperLeft = point(text.substr(0, corner));
    const auto lowerRight = point(text.substr(corner + 7));
    if (!upperLeft || !lowerRight)
        return std::nullopt;
    talas::project::ProjectSoundExposedParameter probe;
    probe.field = talas::project::ProjectSoundFieldRectangle{upperLeft->first, upperLeft->second, lowerRight->first,
        lowerRight->second};
    if (!talas::project::isValidSoundParameterClassFields(probe))
        return std::nullopt;
    return probe.field;
}

std::vector<ClassEditorLayoutBox> classEditorFieldLayout(
    const talas::project::ProjectSoundDefinition& definition, std::string_view selectedParameterId)
{
    std::vector<ClassEditorLayoutBox> boxes;
    for (const auto& parameter : definition.exposedParameters)
    {
        if (parameter.field.has_value())
            boxes.push_back({parameter.displayName, *parameter.field, parameter.parameterId == selectedParameterId});
    }
    for (std::size_t first = 0; first < boxes.size(); ++first)
    {
        for (std::size_t second = first + 1; second < boxes.size(); ++second)
        {
            const auto& a = boxes[first].field;
            const auto& b = boxes[second].field;
            if (a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom)
                boxes[first].overlaps = boxes[second].overlaps = true;
        }
    }
    return boxes;
}

std::string soundParameterHelp(const talas::project::ProjectSoundExposedParameter& parameter)
{
    std::string help = parameter.comment;
    if (parameter.minimum.has_value() || parameter.maximum.has_value())
    {
        if (!help.empty())
            help += "\n";
        help += "Range " + (parameter.minimum ? numberText(*parameter.minimum) : std::string{"any"}) + " to "
            + (parameter.maximum ? numberText(*parameter.maximum) : std::string{"any"});
    }
    return help.empty() ? std::string{"No help is available."} : help;
}

ClassEditorParameterFields classEditorFields(const talas::project::ProjectSoundExposedParameter& parameter)
{
    return {
        parameter.displayName,
        std::string{classEditorDomainName(parameter.domain)},
        parameter.minimum ? numberText(*parameter.minimum) : std::string{},
        parameter.maximum ? numberText(*parameter.maximum) : std::string{},
        parameter.field ? fieldRectangleText(*parameter.field) : std::string{},
        parameter.comment,
        std::string{classEditorFieldKindName(parameter)},
    };
}

std::vector<std::string_view> classEditorFieldKinds(talas::core::ParameterKind kind)
{
    switch (kind)
    {
    case talas::core::ParameterKind::Expression:
        return {"hotValue", "value"};
    case talas::core::ParameterKind::Text:
        return {"string", "codeString", "complex", "polesAndZeroes", "grammar"};
    case talas::core::ParameterKind::Choice:
        return {"symbolList", "radioButtons", "radioButtonsTwoColumns"};
    case talas::core::ParameterKind::Boolean:
        return {"boolean"};
    case talas::core::ParameterKind::Resource:
        return {"fileName"};
    }
    return {};
}

std::string_view classEditorFieldKindName(const talas::project::ProjectSoundExposedParameter& parameter)
{
    if (parameter.fieldKind != talas::project::ProjectSoundFieldKind::Default)
        return talas::project::projectSoundFieldKindName(parameter.fieldKind);
    const auto kinds = classEditorFieldKinds(parameter.kind);
    return kinds.empty() ? std::string_view{} : kinds.front();
}

std::vector<RadioButtonPlace> radioButtonPlaces(std::size_t count, std::size_t columns)
{
    columns = std::max<std::size_t>(columns, 1);
    const auto rows = (count + columns - 1) / columns;
    std::vector<RadioButtonPlace> places;
    places.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        places.push_back({index / rows, index % rows});
    return places;
}

std::size_t radioButtonColumns(const talas::project::ProjectSoundExposedParameter& parameter) noexcept
{
    if (parameter.kind != talas::core::ParameterKind::Choice)
        return 0;
    switch (parameter.fieldKind)
    {
    case talas::project::ProjectSoundFieldKind::RadioButtons:
        return 1;
    case talas::project::ProjectSoundFieldKind::RadioButtonsTwoColumns:
        return 2;
    default:
        return 0;
    }
}

std::optional<talas::project::ProjectSoundExposedParameter> applyClassEditorFields(
    talas::project::ProjectSoundExposedParameter parameter, const ClassEditorParameterFields& fields, std::string& error)
{
    const auto name = trimmed(fields.displayName);
    if (name.empty())
    {
        error = "A parameter needs a name";
        return std::nullopt;
    }
    parameter.displayName = std::string{name};
    const auto domain = std::ranges::find(domains, trimmed(fields.domain), &std::pair<std::string_view, talas::expr::ValueDomain>::first);
    if (domain == domains.end())
    {
        error = "A parameter's type is scalar, hertz, seconds, note number or samples";
        return std::nullopt;
    }
    if (parameter.kind != talas::core::ParameterKind::Expression && domain->second != talas::expr::ValueDomain::Scalar)
    {
        error = "Only an expression parameter takes units";
        return std::nullopt;
    }
    parameter.domain = domain->second;
    const auto bound = [&](std::string_view text, std::optional<double>& destination, std::string_view what) {
        if (trimmed(text).empty())
        {
            destination.reset();
            return true;
        }
        const auto value = number(text);
        if (!value)
        {
            error = std::string{what} + " is a number, or empty for none";
            return false;
        }
        destination = value;
        return true;
    };
    if (!bound(fields.minimum, parameter.minimum, "The minimum") || !bound(fields.maximum, parameter.maximum, "The maximum"))
        return std::nullopt;
    if ((parameter.minimum || parameter.maximum) && parameter.kind != talas::core::ParameterKind::Expression)
    {
        error = "Only an expression parameter has a range";
        return std::nullopt;
    }
    if (trimmed(fields.field).empty())
        parameter.field.reset();
    else if (const auto rectangle = parseFieldRectangle(fields.field))
        parameter.field = rectangle;
    else
    {
        error = "The Field Rectangle reads left @ top corner: right @ bottom, each from 0 to 1";
        return std::nullopt;
    }
    parameter.comment = fields.comment;
    if (const auto kind = trimmed(fields.fieldKind); !kind.empty())
    {
        const auto kinds = classEditorFieldKinds(parameter.kind);
        if (std::ranges::find(kinds, kind) == kinds.end())
        {
            error = "This parameter's field is ";
            for (std::size_t index = 0; index < kinds.size(); ++index)
                error += (index == 0 ? "" : index + 1 == kinds.size() ? " or " : ", ") + std::string{kinds[index]};
            return std::nullopt;
        }
        // The field the parameter has already is no change.
        if (kind != classEditorFieldKindName(parameter))
            parameter.fieldKind = talas::project::parseProjectSoundFieldKind(kind).value_or(talas::project::ProjectSoundFieldKind::Default);
    }
    if (!talas::project::isValidSoundParameterClassFields(parameter))
    {
        error = "The minimum lies below the maximum, and a comment holds at most 4096 bytes";
        return std::nullopt;
    }
    return parameter;
}

std::span<const TalasParameterType> talasParameterTypes() noexcept
{
    return talasTypes;
}

const TalasParameterType* findTalasParameterType(std::string_view name) noexcept
{
    const auto found = std::ranges::find(talasTypes, name, &TalasParameterType::name);
    return found == talasTypes.end() ? nullptr : &*found;
}

namespace
{
// An example's name and id: the class's with " example" and Example, and
// the first number free when either is a Sound's already.
void nameExample(talas::project::ProjectSoundDefinition& example, const talas::project::ProjectDocument& document,
    const std::string& className, const std::string& classId)
{
    const auto taken = [&](const std::string& name) {
        return std::ranges::any_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& sound) {
            return sound.definitionId == name || sound.displayName == name;
        });
    };
    const auto base = className + " example";
    const auto baseId = classId + "Example";
    auto name = base;
    auto id = baseId;
    for (int number = 1; taken(name) || taken(id); ++number)
    {
        name = base + std::to_string(number);
        id = baseId + std::to_string(number);
    }
    example.definitionId = id;
    example.displayName = name;
}
}

std::optional<talas::project::ProjectSoundDefinition> classExample(
    const talas::project::ProjectDocument& document, std::string_view classId)
{
    const auto found = std::ranges::find(document.soundDefinitions, classId, &talas::project::ProjectSoundDefinition::definitionId);
    if (found == document.soundDefinitions.end())
        return std::nullopt;
    talas::project::ProjectSoundDefinition example;
    example.inputs = found->inputs;
    example.outputs = found->outputs;
    example.graph = found->graph;
    nameExample(example, document, found->displayName.empty() ? found->definitionId : found->displayName, found->definitionId);
    return example;
}

std::optional<talas::project::ProjectSoundDefinition> systemClassExample(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view prototypeId, std::string& error)
{
    error.clear();
    // A few System classes, those built from other Sounds,
    // keep the Sound they were made from.
    if (isPrototypeAssembly(prototypeId))
    {
        const auto descriptor = prototypeAssemblyDescriptor(registry, prototypeId);
        auto example = soundForPrototype(document, registry, prototypeId);
        if (!descriptor.has_value() || !example.has_value())
        {
            error = "The Sounds " + std::string{prototypeId} + " is built from are missing";
            return std::nullopt;
        }
        const auto dot = prototypeId.rfind('.');
        nameExample(*example, document, descriptor->displayName,
            std::string{dot == std::string_view::npos ? prototypeId : prototypeId.substr(dot + 1)});
        return example;
    }
    // The rest are built in, each one Sound of its own kind.
    const auto* type = registry.findNodeType(prototypeId);
    if (type == nullptr || type->provenance.origin != talas::core::NodeOrigin::BuiltIn)
    {
        error = std::string{prototypeId} + " is none of the System prototypes";
        return std::nullopt;
    }
    error = type->displayName + " is a System class built in, not made from other Sounds: it has no example to retrieve";
    return std::nullopt;
}

std::optional<std::string> systemClassOfSelection(const talas::project::ProjectDocument& document,
    const std::optional<std::string>& openDefinitionId, std::string_view selectedNodeId)
{
    const talas::project::ProjectGraph* graph = &document.graph;
    if (openDefinitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *openDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document.soundDefinitions.end())
            return std::nullopt;
        graph = &definition->graph;
    }
    const auto node = std::ranges::find(graph->nodes, selectedNodeId, &talas::project::ProjectNode::instanceId);
    if (node == graph->nodes.end() || node->soundDefinitionId.has_value())
        return std::nullopt;
    return node->typeId;
}

std::optional<std::vector<std::string>> classParametersTypedAs(talas::project::ProjectSoundDefinition& definition,
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry, std::string_view sound)
{
    // The other Sound's parameters as a class's would be.
    std::vector<talas::project::ProjectSoundExposedParameter> others;
    auto fromClass = false;
    if (const auto* prototype = registry.findNodeType(sound))
    {
        for (const auto& parameter : prototype->parameters)
        {
            talas::project::ProjectSoundExposedParameter other;
            other.parameterId = parameter.id;
            other.kind = parameter.kind;
            other.domain = parameter.domain;
            other.choices = parameter.choices;
            if (const auto* type = findTalasParameterType(parameter.id);
                type != nullptr && type->kind == parameter.kind && type->domain == parameter.domain)
            {
                other.minimum = type->minimum;
                other.maximum = type->maximum;
            }
            others.push_back(std::move(other));
        }
    }
    else if (const auto found = std::ranges::find(document.soundDefinitions, sound, &talas::project::ProjectSoundDefinition::definitionId);
             found != document.soundDefinitions.end())
    {
        others = found->exposedParameters;
        fromClass = true;
    }
    else
        return std::nullopt;
    std::vector<std::string> changed;
    for (auto& parameter : definition.exposedParameters)
    {
        const auto other = std::ranges::find(others, parameter.parameterId, &talas::project::ProjectSoundExposedParameter::parameterId);
        if (other == others.end() || other->kind != parameter.kind)
            continue;
        auto typed = parameter;
        typed.domain = other->domain;
        typed.minimum = other->minimum;
        typed.maximum = other->maximum;
        if (typed.kind == talas::core::ParameterKind::Choice)
            typed.choices = other->choices;
        // An instance of the old class gives back its
        // comments and its fields' places and kinds too.
        if (fromClass)
        {
            typed.comment = other->comment;
            typed.field = other->field;
            typed.fieldKind = other->fieldKind;
        }
        if (typed != parameter)
        {
            parameter = std::move(typed);
            changed.push_back(parameter.parameterId);
        }
    }
    return changed;
}

const TalasParameterType* talasParameterTypeOf(const talas::project::ProjectSoundExposedParameter& parameter) noexcept
{
    const auto* type = findTalasParameterType(parameter.parameterId);
    return type != nullptr && talasParameterTypeFits(parameter, *type) ? type : nullptr;
}

bool talasParameterTypeFits(
    const talas::project::ProjectSoundExposedParameter& parameter, const TalasParameterType& type) noexcept
{
    if (parameter.kind != type.kind)
        return false;
    if (type.kind != talas::core::ParameterKind::Choice)
        return true;
    return std::ranges::equal(parameter.choices, type.choices);
}

std::string_view talasParameterTypeField(const TalasParameterType& type)
{
    if (type.kind == talas::core::ParameterKind::Expression)
        return type.hot ? "hotValue" : "value";
    // Smalltalk code, a complex number, a list of poles and zeroes
    //.
    if (type.kind == talas::core::ParameterKind::Text && (type.name == "codeString" || type.name == "object"))
        return "codeString";
    if (type.kind == talas::core::ParameterKind::Text && (type.name == "complex" || type.name == "polesAndZeroes"))
        return type.name;
    return classEditorFieldKinds(type.kind).front();
}

std::optional<talas::project::ProjectSoundExposedParameter> applyTalasParameterType(
    talas::project::ProjectSoundExposedParameter parameter, const TalasParameterType& type, std::string& error)
{
    if (!talasParameterTypeFits(parameter, type))
    {
        error = "The " + std::string{type.name} + " type takes " + std::string{type.takes}
            + ", which this parameter's field does not";
        return std::nullopt;
    }
    parameter.fieldKind = talas::project::parseProjectSoundFieldKind(talasParameterTypeField(type))
                              .value_or(talas::project::ProjectSoundFieldKind::Default);
    if (type.kind != talas::core::ParameterKind::Expression)
        return withTalasParameterTypeDefault(std::move(parameter), type);
    ClassEditorParameterFields fields = classEditorFields(parameter);
    fields.domain = std::string{classEditorDomainName(type.domain)};
    fields.minimum = type.minimum ? numberText(*type.minimum) : std::string{};
    fields.maximum = type.maximum ? numberText(*type.maximum) : std::string{};
    auto typed = applyClassEditorFields(std::move(parameter), fields, error);
    if (!typed.has_value())
        return std::nullopt;
    return withTalasParameterTypeDefault(std::move(*typed), type);
}

std::optional<talas::project::ProjectParameterExpression> talasParameterTypeDefault(const TalasParameterType& type)
{
    talas::project::ProjectParameterExpression value;
    switch (type.kind)
    {
    case talas::core::ParameterKind::Expression:
    {
        // The legal value nearest 0, in the units of the type's domain.
        auto number = 0.0;
        if (type.minimum.has_value() && number < *type.minimum)
            number = *type.minimum;
        if (type.maximum.has_value() && number > *type.maximum)
            number = *type.maximum;
        std::string_view units;
        switch (type.domain)
        {
        case talas::expr::ValueDomain::Hertz: units = " hz"; break;
        case talas::expr::ValueDomain::Seconds: units = " s"; break;
        case talas::expr::ValueDomain::Samples: units = " samp"; break;
        case talas::expr::ValueDomain::NoteNumber: units = " nn"; break;
        default: break;
        }
        value.expression = numberText(number) + std::string{units};
        return value;
    }
    case talas::core::ParameterKind::Choice:
        if (type.choices.empty())
            return std::nullopt;
        value.choice = std::string{type.choices.front()};
        return value;
    case talas::core::ParameterKind::Boolean:
        value.boolean = false;
        return value;
    default:
        return std::nullopt;
    }
}

talas::project::ProjectSoundExposedParameter withTalasParameterTypeDefault(
    talas::project::ProjectSoundExposedParameter parameter, const TalasParameterType& type,
    talas::expr::ExpressionLanguageVersion language)
{
    if (!talasParameterTypeFits(parameter, type))
        return parameter;
    auto defaultValue = talasParameterTypeDefault(type);
    if (!defaultValue.has_value())
        return parameter;
    const auto& current = parameter.defaultValue;
    auto takes = false;
    switch (type.kind)
    {
    case talas::core::ParameterKind::Expression:
        if (current.expression.has_value())
        {
            const auto parsed = talas::expr::parse(*current.expression, language);
            if (parsed.ok()
                && (!parsed.expression->eventReferences().empty() || !parsed.expression->variableReferences().empty()
                    || !parsed.expression->identifiers().empty()))
            {
                // An Event Value or a variable plays as it comes, as the
                // range leaves it.
                takes = true;
            }
            else if (parsed.ok())
            {
                const auto evaluated = talas::expr::evaluate(*parsed.expression, {}, {type.domain});
                takes = evaluated.ok() && std::isfinite(evaluated.value.value)
                    && (!type.minimum.has_value() || evaluated.value.value >= *type.minimum)
                    && (!type.maximum.has_value() || evaluated.value.value <= *type.maximum);
            }
        }
        break;
    case talas::core::ParameterKind::Choice:
        takes = current.choice.has_value() && std::ranges::find(type.choices, *current.choice) != type.choices.end();
        break;
    case talas::core::ParameterKind::Boolean:
        takes = current.boolean.has_value();
        break;
    default:
        takes = true;
        break;
    }
    if (takes)
        return parameter;
    defaultValue->parameterId = current.parameterId.empty() ? parameter.parameterId : current.parameterId;
    parameter.defaultValue = std::move(*defaultValue);
    return parameter;
}

std::optional<std::string> classOfSelection(
    const talas::project::ProjectDocument& document,
    const std::optional<std::string>& openDefinitionId,
    std::string_view selectedNodeId)
{
    const talas::project::ProjectGraph* graph = &document.graph;
    if (openDefinitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *openDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document.soundDefinitions.end())
            return std::nullopt;
        graph = &definition->graph;
    }
    const auto node = std::ranges::find(graph->nodes, selectedNodeId, &talas::project::ProjectNode::instanceId);
    if (node != graph->nodes.end() && node->soundDefinitionId.has_value())
        return node->soundDefinitionId;
    return openDefinitionId;
}

std::string newClassCloseQuestion(std::string_view className)
{
    return "Save an instance of the new class " + std::string{className}
        + "? Its parameters take their default values. No starts over, without the class.";
}

std::vector<std::string> newClassInstances(const talas::project::ProjectDocument& document, std::string_view classId)
{
    std::vector<std::string> instances;
    for (const auto& node : document.graph.nodes)
    {
        if (node.soundDefinitionId == classId)
            instances.push_back(node.instanceId);
    }
    return instances;
}

std::optional<SoundFileClassInstanceSaved> classInstanceSavedInSoundFile(
    const talas::project::ProjectDocument& document, std::string_view classId)
{
    auto instance = talas::project::soundFileClassInstance(document, classId);
    if (!instance.has_value())
        return std::nullopt;
    SoundFileClassInstanceSaved saved{document, {instance->definitionId}};
    saved.document.soundDefinitions.push_back(std::move(*instance));
    return saved;
}

talas::project::ProjectSoundDefinition classWithDefaults(talas::project::ProjectSoundDefinition definition,
    const std::vector<std::pair<std::string, double>>& values)
{
    for (auto& parameter : definition.exposedParameters)
    {
        const auto variable = std::ranges::find(parameter.bindings, talas::project::ProjectSoundParameterTarget::Variable,
            &talas::project::ProjectSoundParameterBinding::target);
        if (variable == parameter.bindings.end())
            continue;
        const auto value = std::ranges::find(values, variable->variableName, &std::pair<std::string, double>::first);
        if (value != values.end())
        {
            std::ostringstream text;
            text << value->second;
            parameter.defaultValue.expression = text.str();
        }
    }
    return definition;
}
}
