#include "talas/app/AnalysisToolsModel.h"
#include "talas/audio/PartialTrackResynthesis.h"

#include "talas/expr/Expression.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <set>
#include <span>
#include <sstream>
#include <utility>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string uniqueDefinitionId(
    const talas::project::ProjectDocument& document,
    std::string_view base)
{
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        auto candidate = std::string{base} + std::to_string(ordinal);
        if (std::ranges::none_of(document.soundDefinitions, [&](const auto& definition) {
                return definition.definitionId == candidate;
            }))
        {
            return candidate;
        }
    }
}

[[nodiscard]] talas::project::ProjectParameterExpression expression(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(value);
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression resource(
    std::string id,
    std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(value);
    return parameter;
}

[[nodiscard]] std::string stem(const std::filesystem::path& file)
{
    auto name = talas::project::pathToUtf8(file.stem());
    return name.empty() ? std::string{"Analysis"} : name;
}
}

bool analysisToolPreviewLoops(AnalysisToolKind kind) noexcept
{
    return kind == AnalysisToolKind::Spectral;
}

std::size_t resynthesisOscillatorCount(const talas::project::PartialTrackAsset& analysis)
{
    std::set<std::uint64_t> tracks;
    for (const auto& frame : analysis.frames)
        for (const auto& point : frame.points)
            tracks.insert(point.trackId);
    return tracks.size();
}

std::optional<std::string> resynthesisCapacityWarning(const talas::project::PartialTrackAsset& analysis)
{
    std::size_t atOnce = 0;
    for (const auto& frame : analysis.frames)
        atOnce = std::max(atOnce, frame.points.size());
    if (atOnce <= talas::audio::maximumRealtimePartialTrackVoices)
        return std::nullopt;
    return std::to_string(atOnce) + " oscillators at once, more than the "
        + std::to_string(talas::audio::maximumRealtimePartialTrackVoices) + " the signal processor can play";
}

double spectralAnalysisLowestFrequencyHz(int setting) noexcept
{
    setting = std::clamp(setting, 1, spectralAnalysisLowestFrequencySettings);
    // F1 is 5 semitones below A1 (55 Hz).
    return 55.0 * std::pow(2.0, -4.0 / 12.0) * std::pow(2.0, setting - 1);
}

std::string spectralAnalysisLowestFrequencyLabel(int setting)
{
    setting = std::clamp(setting, 1, spectralAnalysisLowestFrequencySettings);
    std::ostringstream label;
    label << "Above " << setting << "F ("
          << std::lround(spectralAnalysisLowestFrequencyHz(setting)) << " Hz)";
    return label.str();
}

std::size_t spectralAnalysisTracks(int setting) noexcept
{
    setting = std::clamp(setting, 1, spectralAnalysisLowestFrequencySettings);
    return std::size_t{512} >> (setting - 1);
}

std::string spectralAnalysisFileSuffix(int setting)
{
    return " s" + std::to_string(spectralAnalysisTracks(setting)) + ".talaspartial";
}

std::vector<std::string> spectralAnalysisResponseLabels()
{
    return {"BestFreq", "BetterFreq", "BetterTime", "BestTime"};
}

std::size_t spectralAnalysisResponseBins(int response) noexcept
{
    return static_cast<std::size_t>(std::clamp(response, 0, 3)) + 1U;
}

std::filesystem::path analysisOutputPath(
    const std::filesystem::path& sample,
    std::string_view suffix)
{
    return sample.parent_path()
        / talas::project::pathFromUtf8(stem(sample) + std::string{suffix});
}

std::filesystem::path reAnalysisOutputPath(
    const std::filesystem::path& sample,
    const std::filesystem::path& talasFolder)
{
    return talasFolder / "Wavetables" / "RE" / talas::project::pathFromUtf8(stem(sample) + " RE.txt");
}

AnalysisExampleSound spectralAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& spectrumFile)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "spectrumExample");
    definition.displayName = stem(spectrumFile) + " resynthesis";
    definition.graph.nodes.push_back({
        "partials", "talas.partialTrackPlayerV1", {expression("gain", "1.0")}});
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "partials", "audioOut"});
    example.resources.push_back({"partials", "partialTrack", spectrumFile});
    return example;
}

std::vector<std::string> stereoAnalysisChoiceLabels()
{
    return {"Mono mix of the two channels", "Left channel", "Right channel", "Left and right into separate files"};
}

std::vector<std::filesystem::path> stereoAnalysisFiles(
    const std::filesystem::path& spectrumFile, StereoAnalysisChoice choice)
{
    if (choice != StereoAnalysisChoice::LeftAndRight)
        return {spectrumFile};
    const auto tagged = [&spectrumFile](std::string_view tag) {
        auto file = spectrumFile;
        file.replace_filename(talas::project::pathFromUtf8(
            talas::project::pathToUtf8(spectrumFile.stem()) + std::string{tag}
            + talas::project::pathToUtf8(spectrumFile.extension())));
        return file;
    };
    return {tagged(" L"), tagged(" R")};
}

std::vector<std::vector<double>> stereoAnalysisSamples(
    const std::vector<std::vector<double>>& channels, StereoAnalysisChoice choice)
{
    if (channels.empty())
        return {};
    if (channels.size() != 2)
        return {channels.front()};
    std::vector<std::vector<double>> samples;
    for (const auto channel : stereoAnalysisChannels(choice))
    {
        if (channel.has_value())
        {
            samples.push_back(channels[*channel]);
            continue;
        }
        auto mix = channels.front();
        for (std::size_t frame = 0; frame < mix.size() && frame < channels[1].size(); ++frame)
            mix[frame] = 0.5 * (mix[frame] + channels[1][frame]);
        samples.push_back(std::move(mix));
    }
    return samples;
}

std::vector<double> spectralAnalysisAuditionedChannel(const std::vector<std::vector<double>>& channels)
{
    return channels.empty() ? std::vector<double>{} : channels.front();
}

std::vector<AnalysisWindowParameter> reAnalysisWindowParameters()
{
    // The defaults are talas::dsp::ReAnalysisSettings's.
    return {{"Filter order", 1.0, 128.0, 32.0, {}}, {"Updates per second", 1.0, 1000.0, 100.0, {}},
        {"Averaging time", 1.0, 500.0, 20.0, "ms"}};
}

std::vector<std::optional<std::size_t>> stereoAnalysisChannels(StereoAnalysisChoice choice)
{
    switch (choice)
    {
    case StereoAnalysisChoice::MonoMix:
        return {std::nullopt};
    case StereoAnalysisChoice::Left:
        return {std::size_t{0}};
    case StereoAnalysisChoice::Right:
        return {std::size_t{1}};
    case StereoAnalysisChoice::LeftAndRight:
        return {std::size_t{0}, std::size_t{1}};
    }
    return {std::nullopt};
}

AnalysisExampleSound stereoSpectralAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& leftFile,
    const std::filesystem::path& rightFile)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "stereoSpectrumExample");
    auto name = stem(leftFile);
    if (name.ends_with(" L"))
        name.erase(name.size() - 2);
    definition.displayName = name + " stereo resynthesis";
    definition.graph.nodes = {
        {"left", "talas.partialTrackPlayerV1", {expression("gain", "1.0")}},
        {"right", "talas.partialTrackPlayerV1", {expression("gain", "1.0")}},
        {"join", "talas.channelJoin", {}},
    };
    definition.graph.connections = {{"left", "audioOut", "join", "left"}, {"right", "audioOut", "join", "right"}};
    definition.outputs = {
        {"audioOut", talas::core::PortType::Audio, "join", "leftOut"},
        {"out2", talas::core::PortType::Audio, "join", "rightOut"},
    };
    example.resources = {{"left", "partialTrack", leftFile}, {"right", "partialTrack", rightFile}};
    return example;
}

std::string harmonicAnalysisFileSuffix()
{
    return " h.talaspartial";
}

std::filesystem::path harmonicAnalysisTablePath(const std::filesystem::path& spectrumFile)
{
    auto table = spectrumFile;
    table.replace_extension(".txt");
    return table;
}

std::vector<AnalysisExampleSound> harmonicAnalysisExampleSounds(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& tableFile,
    std::size_t frames,
    std::size_t partials,
    double pitchHz,
    double durationSeconds)
{
    const auto number = [](double value) {
        std::ostringstream text;
        text << std::setprecision(10) << value;
        return text.str();
    };
    const auto duration = number(std::isfinite(durationSeconds) && durationSeconds > 0.0 ? durationSeconds : 1.0);
    const auto pitch = number(std::isfinite(pitchHz) && pitchHz > 0.0 ? pitchHz : 261.6255653) + " hz";
    const auto count = std::to_string(std::max<std::size_t>(1, partials));
    std::vector<AnalysisExampleSound> examples;
    auto withIds = document;
    for (const auto morph : {false, true})
    {
        AnalysisExampleSound example;
        auto& definition = example.definition;
        definition.definitionId = uniqueDefinitionId(withIds, "harmonicExample");
        definition.displayName = stem(tableFile) + (morph ? " morph" : "");
        std::vector<talas::project::ProjectParameterExpression> parameters{
            expression("onDuration", morph ? duration + " s + (!Gate * 100 s)" : duration + " s"),
            expression("analysisFrequency0", pitch),
            expression("analysisFrames0", std::to_string(std::max<std::size_t>(1, frames))),
            expression("nbrPartials", count),
            expression("bankSize", count),
        };
        if (morph)
        {
            parameters.push_back(expression("analysisFrequency1", pitch));
            parameters.push_back(expression("analysisFrames1", std::to_string(std::max<std::size_t>(1, frames))));
            parameters.push_back(expression("dbMorph", "!Morph"));
            parameters.push_back(expression("pchMorph", "!Morph"));
        }
        definition.graph.nodes.push_back({"sines", "talas.sumOfSines", std::move(parameters)});
        definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "sines", "audioOut"});
        example.resources.push_back({"sines", "analysis0", tableFile});
        if (morph)
            example.resources.push_back({"sines", "analysis1", tableFile});
        withIds.soundDefinitions.push_back(definition);
        examples.push_back(std::move(example));
    }
    return examples;
}

AnalysisExampleSound spectrumSumOfSinesSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& spectrumFile,
    std::size_t frames,
    std::size_t partials,
    double durationSeconds)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "spectrumSines");
    definition.displayName = stem(spectrumFile) + " sines";
    std::ostringstream duration;
    duration << std::setprecision(10)
             << (std::isfinite(durationSeconds) && durationSeconds > 0.0 ? durationSeconds : 1.0) << " s * !Stretch";
    const auto count = std::to_string(std::max<std::size_t>(1, partials));
    definition.graph.nodes = {
        {"ramp", "talas.functionGenerator",
            {expression("trigger", "1"), expression("onDuration", duration.str()), resource("wavetable", "FullRamp")}},
        {"spectrum", "talas.spectrumInRam",
            {expression("frequency", "1 hz * !FreqScale"), expression("level", "1"), expression("timeIndex", "-1"),
                expression("analysisFrames", std::to_string(std::max<std::size_t>(1, frames))),
                expression("analysisFrequency", "1 hz"), expression("nbrPartials", count), expression("firstPartial", "1")}},
        {"bank", "talas.oscillatorBank", {expression("nbrOscillators", count), expression("bankSize", count)}},
    };
    definition.graph.connections = {
        {"ramp", "value", "spectrum", "timeIndex"},
        {"spectrum", "spectrumOut", "bank", "spectrum"},
    };
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "bank", "audioOut"});
    example.resources.push_back({"spectrum", "analysis", spectrumFile});
    const auto control = [](std::string name, double minimum, double maximum) {
        talas::project::VirtualControl made;
        made.eventName = std::move(name);
        made.value = 1.0;
        made.minimum = minimum;
        made.maximum = maximum;
        return made;
    };
    example.controls = {control("Stretch", 0.1, 10.0), control("FreqScale", 0.25, 4.0)};
    return example;
}

AnalysisExampleSound gaAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& analysisFile,
    double basePitch,
    double durationSeconds)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "gaExample");
    definition.displayName = stem(analysisFile) + " oscillators";
    std::ostringstream duration;
    duration << std::setprecision(10)
             << (std::isfinite(durationSeconds) && durationSeconds > 0.0 ? durationSeconds : 1.0) << " s";
    std::ostringstream pitch;
    pitch << std::setprecision(10) << (std::isfinite(basePitch) && basePitch > 0.0 ? basePitch : 440.0) << " hz";
    definition.graph.nodes.push_back({
        "ramp", "talas.functionGenerator", {
            expression("trigger", "1"),
            expression("onDuration", duration.str()),
            resource("wavetable", "FullRamp"),
        }});
    definition.graph.nodes.push_back({"oscillators", "talas.gaOscillators", {expression("frequency", pitch.str())}});
    definition.graph.connections = {{"ramp", "value", "oscillators", "timeIndex"}};
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "oscillators", "audioOut"});
    example.resources.push_back({"oscillators", "analysis0", analysisFile});
    return example;
}

AnalysisExampleSound cdpProcessExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& sampleFile)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "cdpExample");
    definition.displayName = stem(sampleFile);
    definition.graph.nodes.push_back({"sample", "talas.sample", {}});
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "sample", "audioOut"});
    example.resources.push_back({"sample", "sample", sampleFile});
    return example;
}

AnalysisExampleSound sampleEditorExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& sampleFile)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "editedSample");
    definition.displayName = stem(sampleFile);
    definition.graph.nodes.push_back({"sample", "talas.sample", {}});
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "sample", "audioOut"});
    example.resources.push_back({"sample", "sample", sampleFile});
    return example;
}

AnalysisExampleSound sampleFileSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& sampleFile)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "sampleFile");
    definition.displayName = stem(sampleFile);
    talas::project::ProjectParameterExpression source;
    source.parameterId = "source";
    source.text = "Disk";
    definition.graph.nodes.push_back({"source", "talas.genericSource", {source}});
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "source", "leftOut"});
    example.resources.push_back({"source", "sample", sampleFile});
    return example;
}

AnalysisExampleSound reAnalysisExampleSound(
    const talas::project::ProjectDocument& document,
    const std::filesystem::path& tableFile,
    const std::filesystem::path& excitationFile,
    double durationSeconds)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "reExample");
    definition.displayName = stem(tableFile) + " resonator";
    std::ostringstream duration;
    duration << std::setprecision(10)
             << (std::isfinite(durationSeconds) && durationSeconds > 0.0 ? durationSeconds : 1.0)
             << " s / !Rate";
    definition.graph.nodes.push_back({
        "ramp", "talas.functionGenerator", {
            expression("trigger", "1"),
            expression("onDuration", duration.str()),
            resource("wavetable", "FullRamp"),
        }});
    definition.graph.nodes.push_back({"resonator", "talas.reResonator", {}});
    if (excitationFile.empty())
    {
        definition.graph.nodes.push_back({
            "excitation", "talas.noise", {expression("scale", "0.001 * !ExAmp")}});
    }
    else
    {
        definition.graph.nodes.push_back({"excitation", "talas.sample",
            {expression("frequency", "default * !Rate"), expression("scale", "!ExAmp")}});
        example.resources.push_back({"excitation", "sample", excitationFile});
    }
    definition.graph.nodes.push_back({"output", "talas.gain", {expression("left", "!Gain"), expression("right", "!Gain")}});
    definition.graph.connections = {
        {"excitation", "audioOut", "resonator", "audioIn"},
        {"ramp", "value", "resonator", "timeIndex"},
        {"resonator", "audioOut", "output", "audioIn"},
    };
    definition.outputs.push_back({
        "audioOut", talas::core::PortType::Audio, "output", "leftOut"});
    example.resources.insert(
        example.resources.begin(), {"resonator", "wavetable", tableFile});
    const auto control = [](std::string name, double maximum) {
        talas::project::VirtualControl made;
        made.eventName = std::move(name);
        made.value = 1.0;
        made.minimum = 0.0;
        made.maximum = maximum;
        return made;
    };
    example.controls = {control("Gain", 10.0), control("Rate", 2.0), control("ExAmp", 1.0)};
    return example;
}

std::optional<std::string> tuningFrequencyExpression(
    const talas::project::tunings::Tuning& tuning,
    const talas::project::tunings::KeyFrequencies& frequencies,
    std::string& error)
{
    using Method = talas::project::tunings::Method;
    const auto number = [](double value) {
        std::array<char, 32> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%.10g", value);
        return std::string{buffer.data()};
    };
    const auto array = [&](std::span<const double> values) {
        std::string text = "#(";
        for (std::size_t index = 0; index < values.size(); ++index)
            text += (index == 0 ? "" : " ") + number(values[index]);
        return text + ")";
    };
    // !KeyNumber is a note number in a voice; its number counts the keys.
    const auto steps = "(!KeyNumber removeUnits - " + std::to_string(tuning.tonicKey) + ")";
    const auto tonic = number(tuning.tonicFrequency);
    if (tuning.method == Method::EqualTempered)
    {
        return "(" + tonic + " * (2 raisedTo: (" + steps + " / " + std::to_string(tuning.degreesPerOctave) + "))) hz";
    }
    // One octave's degrees as ratios to the tonic: a file's tuning repeats
    // each octave after so many keys, at most 32.
    auto degrees = tuning.method == Method::TwoInterval ? tuning.degreesPerOctave
                                                        : static_cast<int>(talas::project::tunings::scaleDegrees);
    constexpr auto longest = static_cast<int>(talas::expr::ExpressionLimits::maximumListElements);
    if (tuning.method == Method::FromTextFile)
    {
        degrees = 0;
        for (int period = 1; period <= longest && degrees == 0; ++period)
        {
            auto repeats = true;
            for (std::size_t key = 0; repeats && key + static_cast<std::size_t>(period) < frequencies.size(); ++key)
            {
                const auto octave = frequencies[key + static_cast<std::size_t>(period)] / frequencies[key];
                repeats = std::abs(octave - 2.0) < 2.0e-5;
            }
            if (repeats)
                degrees = period;
        }
        if (degrees == 0)
        {
            error = "An example plays a file's tuning when its keys repeat each octave within 32 keys";
            return std::nullopt;
        }
    }
    if (degrees > longest)
    {
        error = "An example plays at most 32 scale degrees to the octave";
        return std::nullopt;
    }
    auto base = tuning.tonicKey;
    while (base + degrees - 1 >= static_cast<int>(talas::project::tunings::keyCount))
        base -= degrees;
    while (base < 0)
        base += degrees;
    std::vector<double> ratios;
    for (int degree = 0; degree < degrees; ++degree)
        ratios.push_back(frequencies[static_cast<std::size_t>(base + degree)] / frequencies[static_cast<std::size_t>(base)]);
    // The degrees count from the octave's first key, base.
    const auto fromBase = "(!KeyNumber removeUnits - " + std::to_string(base) + ")";
    const auto baseFrequency = number(frequencies[static_cast<std::size_t>(base)]);
    const auto count = std::to_string(degrees);
    return "(" + baseFrequency + " * (2 raisedTo: (" + fromBase + " / " + count + ") floor) * ((" + fromBase + " mod: "
        + count + ") of: " + array(ratios) + ")) hz";
}

bool tuningToolPageShows(std::optional<talas::project::tunings::Method> page, TuningToolPart part) noexcept
{
    const auto main = part == TuningToolPart::Tonic || part == TuningToolPart::Instrument
        || part == TuningToolPart::KindButtons;
    return page.has_value() != main;
}

std::optional<AnalysisExampleSound> tuningExampleSound(
    const talas::project::ProjectDocument& document,
    const talas::project::tunings::Tuning& tuning,
    const talas::project::tunings::KeyFrequencies& frequencies,
    TuningExampleInstrument instrument,
    const std::filesystem::path& sampleFile,
    std::string& error)
{
    const auto frequency = tuningFrequencyExpression(tuning, frequencies, error);
    if (!frequency)
        return std::nullopt;
    if (document.expressionLanguage != talas::expr::ExpressionLanguageVersion::ValV1)
    {
        error = "The example's expressions are Val: upgrade the project's expressions to Val first";
        return std::nullopt;
    }
    if (const auto parsed = talas::expr::parse(*frequency, document.expressionLanguage, document.expressionDefinitions);
        !parsed.ok())
    {
        error = "The tuning's frequency expression does not parse: "
            + (parsed.diagnostics.empty() ? std::string{} : parsed.diagnostics.front().message);
        return std::nullopt;
    }
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "tuningExample");
    definition.displayName = std::string{talas::project::tunings::methodName(tuning.method)} + " tuning";
    talas::project::ProjectParameterExpression source;
    source.parameterId = "source";
    source.text = "Live MIDI";
    definition.graph.nodes.push_back({"voice", "talas.midiVoice", {source, expression("polyphony", "8")}});
    if (instrument == TuningExampleInstrument::Sample)
    {
        definition.graph.nodes.push_back({"instrument", "talas.sample",
            {expression("frequency", *frequency), expression("gate", "!KeyDown"), expression("scale", "!Volume")}});
        definition.graph.connections = {{"instrument", "audioOut", "voice", "audioIn"}};
        example.resources.push_back({"instrument", "sample", sampleFile});
    }
    else
    {
        definition.graph.nodes.push_back({"envelope", "talas.adsr", {expression("gate", "!KeyDown")}});
        talas::project::ProjectNode oscillator{"instrument", "talas.oscillator",
            {expression("frequency", *frequency), expression("amplitude", "0.25 * !Volume")}};
        if (instrument == TuningExampleInstrument::Sawtooth)
        {
            talas::project::ProjectParameterExpression wavetable;
            wavetable.parameterId = "wavetable";
            wavetable.resource = "Saw21";
            oscillator.parameters.push_back(std::move(wavetable));
        }
        definition.graph.nodes.push_back(std::move(oscillator));
        definition.graph.connections = {{"envelope", "value", "instrument", "envelope"},
            {"instrument", "audioOut", "voice", "audioIn"}};
    }
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, "voice", "leftOut"});
    // The Volume fader, at its top so that the example sounds at first.
    talas::project::VirtualControl volume;
    volume.eventName = "Volume";
    volume.value = 1.0;
    volume.minimum = 0.0;
    volume.maximum = 1.0;
    example.controls.push_back(std::move(volume));
    return example;
}

std::optional<TuningTryOut> tuningTryOut(const talas::project::ProjectDocument& document,
    const talas::project::tunings::Tuning& tuning, const talas::project::tunings::KeyFrequencies& frequencies,
    TuningExampleInstrument instrument, const std::filesystem::path& sampleFile, std::string& error)
{
    auto example = tuningExampleSound(document, tuning, frequencies, instrument, sampleFile, error);
    if (!example)
        return std::nullopt;
    TuningTryOut tryOut;
    auto& played = tryOut.document;
    played.expressionLanguage = document.expressionLanguage;
    played.expressionDefinitions = document.expressionDefinitions;
    played.graph = std::move(example->definition.graph);
    played.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    played.graph.connections.push_back({"voice", "leftOut", "out", "audioIn"});
    played.graph.connections.push_back({"voice", "rightOut", "out", "rightIn"});
    played.virtualControls = std::move(example->controls);
    talas::project::reconcileProjectPresentation(played);
    tryOut.resources = std::move(example->resources);
    return tryOut;
}

std::optional<SpectrumViewWindow> spectrumViewZoomedTo(
    const SpectrumViewWindow& shown, double left, double top, double right, double bottom) noexcept
{
    const auto clamp01 = [](double value) { return std::clamp(value, 0.0, 1.0); };
    left = clamp01(left);
    right = clamp01(right);
    top = clamp01(top);
    bottom = clamp01(bottom);
    if (left > right)
        std::swap(left, right);
    if (top > bottom)
        std::swap(top, bottom);
    if (right - left < 0.01 || bottom - top < 0.01)
        return std::nullopt;
    const auto time = [&](double fraction) { return shown.lowTime + fraction * (shown.highTime - shown.lowTime); };
    const auto hz = [&](double fraction) { return shown.highHz - fraction * (shown.highHz - shown.lowHz); };
    return SpectrumViewWindow{time(left), time(right), hz(bottom), hz(top)};
}

SpectrumViewCursor spectrumViewCursor(bool controlOrCommandDown) noexcept
{
    return controlOrCommandDown ? SpectrumViewCursor::Magnifier : SpectrumViewCursor::Arrow;
}

SpectrumTraceHandles spectrumTraceHandles(bool displaySelected) noexcept
{
    return displaySelected ? SpectrumTraceHandles::Squares : SpectrumTraceHandles::None;
}

TraceViewAction traceViewAction(TraceViewKey key, bool pointSelected) noexcept
{
    switch (key)
    {
    case TraceViewKey::Return: return TraceViewAction::ShowWhole;
    case TraceViewKey::LowercaseL: return TraceViewAction::ToggleConnectedFrames;
    case TraceViewKey::Delete: return pointSelected ? TraceViewAction::DeletePoint : TraceViewAction::None;
    case TraceViewKey::Other: break;
    }
    return TraceViewAction::None;
}
}
