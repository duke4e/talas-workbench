#include "talas/app/SpectrumSynchronization.h"

#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <sstream>

namespace talas::app
{
namespace
{
[[nodiscard]] talas::project::ProjectParameterExpression expression(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(value);
    return parameter;
}

[[nodiscard]] talas::project::ProjectParameterExpression textField(std::string id, std::string value)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(value);
    return parameter;
}

[[nodiscard]] std::string uniqueDefinitionId(const talas::project::ProjectDocument& document, std::string_view base)
{
    for (std::size_t ordinal = 1;; ++ordinal)
    {
        auto candidate = std::string{base} + std::to_string(ordinal);
        if (std::ranges::none_of(document.soundDefinitions,
                [&](const auto& definition) { return definition.definitionId == candidate; }))
            return candidate;
    }
}

[[nodiscard]] std::string markerName(std::size_t frame)
{
    return "m" + std::to_string(frame);
}

// A SpectrumInRAM reading a spectrum's table through a TimeIndex input.
[[nodiscard]] talas::project::ProjectNode spectrumInRam(std::string id, const SynchronizedSpectrum& spectrum, std::size_t partials)
{
    return {std::move(id), "talas.spectrumInRam",
        {expression("frequency", "0 hz"), expression("level", "1"), expression("timeIndex", "-1"),
            expression("analysisFrames", std::to_string(std::max<std::size_t>(1, spectrum.frames))),
            expression("analysisFrequency", "1 hz"), expression("nbrPartials", std::to_string(partials)),
            expression("firstPartial", "1")}};
}

[[nodiscard]] talas::project::ProjectNode warpedTimeIndex(
    std::string id, const std::string& current, const std::string& ideal0, const std::string& ideal1, std::string morph)
{
    return {std::move(id), "talas.warpedTimeIndex",
        {textField("currentTimePoints", current), textField("idealTimePoints0", ideal0),
            textField("idealTimePoints1", ideal1), expression("trigger", "1"), expression("rate", "1"),
            expression("morph", std::move(morph))}};
}

[[nodiscard]] talas::project::ProjectNode oscillatorBank(std::string id, std::size_t partials)
{
    const auto count = std::to_string(std::max<std::size_t>(1, partials));
    return {std::move(id), "talas.oscillatorBank", {expression("nbrOscillators", count), expression("bankSize", count)}};
}
}

double spectrumFrameSeconds(const talas::project::PartialTrackAsset& spectrum, std::size_t frame) noexcept
{
    if (!(spectrum.sampleRate > 0.0))
        return 0.0;
    const auto at = frame < spectrum.frames.size() ? spectrum.frames[frame].sourceFrame : spectrum.sourceFrameCount;
    return static_cast<double>(at) / spectrum.sampleRate;
}

std::pair<std::size_t, std::size_t> spectrumScrubSlice(
    const talas::project::PartialTrackAsset& spectrum, std::size_t frame, std::size_t samples, double seconds) noexcept
{
    const auto at = static_cast<std::size_t>(std::max(0.0, spectrumFrameSeconds(spectrum, frame) * spectrum.sampleRate));
    const auto length = static_cast<std::size_t>(std::max(0.0, seconds * spectrum.sampleRate));
    const auto begin = std::min(at, samples);
    return {begin, std::min(begin + length, samples)};
}

std::vector<SpectrumMarker> autoSpectrumMarkers(const talas::project::PartialTrackAsset& spectrum, double minimumSpacingSeconds)
{
    const auto count = spectrum.frames.size();
    if (count == 0)
        return {};
    if (count == 1)
        return {{markerName(0), 0}};
    // How much the spectrum changes from each frame to the next: the sum of
    // its tracks' amplitude changes.
    std::vector<double> change(count, 0.0);
    std::map<std::uint64_t, double> previous;
    for (std::size_t index = 0; index < count; ++index)
    {
        std::map<std::uint64_t, double> amplitudes;
        for (const auto& point : spectrum.frames[index].points)
            amplitudes[point.trackId] = point.amplitude;
        if (index > 0)
        {
            double sum = 0.0;
            for (const auto& [track, amplitude] : amplitudes)
                sum += std::abs(amplitude - (previous.contains(track) ? previous.at(track) : 0.0));
            for (const auto& [track, amplitude] : previous)
            {
                if (!amplitudes.contains(track))
                    sum += amplitude;
            }
            change[index] = sum;
        }
        previous = std::move(amplitudes);
    }
    const auto mean = std::accumulate(change.begin() + 1, change.end(), 0.0) / static_cast<double>(count - 1);
    double variance = 0.0;
    for (std::size_t index = 1; index < count; ++index)
        variance += (change[index] - mean) * (change[index] - mean);
    const auto threshold = mean + 1.5 * std::sqrt(variance / static_cast<double>(count - 1));
    std::vector<std::size_t> peaks;
    for (std::size_t index = 1; index + 1 < count; ++index)
    {
        if (change[index] > threshold && change[index] >= change[index - 1] && change[index] >= change[index + 1])
            peaks.push_back(index);
    }
    std::ranges::sort(peaks, [&change](std::size_t a, std::size_t b) { return change[a] > change[b]; });
    std::vector<std::size_t> chosen{0, count - 1};
    for (const auto peak : peaks)
    {
        const auto seconds = spectrumFrameSeconds(spectrum, peak);
        if (std::ranges::all_of(chosen, [&](std::size_t frame) {
                return std::abs(spectrumFrameSeconds(spectrum, frame) - seconds) >= minimumSpacingSeconds;
            }))
            chosen.push_back(peak);
    }
    std::ranges::sort(chosen);
    std::vector<SpectrumMarker> markers;
    for (const auto frame : chosen)
        markers.push_back({markerName(frame), frame});
    return markers;
}

std::optional<std::vector<SpectrumMarker>> spectrumMarkersWith(
    std::vector<SpectrumMarker> markers, std::size_t frame, std::string name)
{
    if (std::ranges::any_of(markers, [frame](const SpectrumMarker& marker) { return marker.frame == frame; }))
        return std::nullopt;
    const auto at = std::ranges::upper_bound(markers, frame, {}, &SpectrumMarker::frame);
    markers.insert(at, {name.empty() ? markerName(frame) : std::move(name), frame});
    return markers;
}

std::optional<std::vector<SpectrumMarker>> spectrumMarkersRenamed(
    std::vector<SpectrumMarker> markers, std::size_t index, std::string name)
{
    if (index >= markers.size() || name.empty() || name.find('\n') != std::string::npos)
        return std::nullopt;
    markers[index].name = std::move(name);
    return markers;
}

std::optional<std::vector<SpectrumMarker>> spectrumMarkersWithout(std::vector<SpectrumMarker> markers, std::size_t index)
{
    if (index >= markers.size())
        return std::nullopt;
    markers.erase(markers.begin() + static_cast<std::ptrdiff_t>(index));
    return markers;
}

SpectrumMarkerColour spectrumMarkerColour(std::size_t index, bool selected) noexcept
{
    return selected ? SpectrumMarkerColour::White : static_cast<SpectrumMarkerColour>(index % 3);
}

SpectrumSyncAction spectrumSyncAction(SpectrumSyncKey key, bool spectrumLoaded, bool markerSelected) noexcept
{
    const auto withSpectrum = [&](SpectrumSyncAction action) {
        return spectrumLoaded ? action : SpectrumSyncAction::None;
    };
    switch (key)
    {
    case SpectrumSyncKey::Tab:
        return SpectrumSyncAction::SwitchView;
    case SpectrumSyncKey::Space:
        return withSpectrum(SpectrumSyncAction::Audition);
    case SpectrumSyncKey::Left:
        return withSpectrum(SpectrumSyncAction::ScrubBack);
    case SpectrumSyncKey::Right:
        return withSpectrum(SpectrumSyncAction::ScrubOn);
    case SpectrumSyncKey::M:
        return withSpectrum(SpectrumSyncAction::AddMarker);
    case SpectrumSyncKey::A:
        return withSpectrum(SpectrumSyncAction::SuggestMarkers);
    case SpectrumSyncKey::S:
        return SpectrumSyncAction::SaveMarkers;
    case SpectrumSyncKey::Delete:
        return markerSelected ? SpectrumSyncAction::DeleteMarker : SpectrumSyncAction::None;
    case SpectrumSyncKey::Return:
        return markerSelected ? SpectrumSyncAction::NameMarker : SpectrumSyncAction::ShowWhole;
    case SpectrumSyncKey::Other:
        break;
    }
    return SpectrumSyncAction::None;
}

std::filesystem::path spectrumMarkersPath(const std::filesystem::path& spectrumFile)
{
    auto path = spectrumFile;
    path += ".markers";
    return path;
}

bool writeSpectrumMarkers(
    const std::filesystem::path& spectrumFile, const std::vector<SpectrumMarker>& markers, std::string& error)
{
    std::ofstream output(spectrumMarkersPath(spectrumFile), std::ios::binary | std::ios::trunc);
    for (const auto& marker : markers)
        output << marker.frame << ' ' << marker.name << '\n';
    output.flush();
    if (!output.good())
    {
        error = "The markers could not be saved beside " + talas::project::pathToUtf8(spectrumFile.filename());
        return false;
    }
    return true;
}

std::optional<std::vector<SpectrumMarker>> readSpectrumMarkers(const std::filesystem::path& spectrumFile, std::string& error)
{
    std::ifstream input(spectrumMarkersPath(spectrumFile), std::ios::binary);
    if (!input.is_open())
        return std::nullopt;
    std::vector<SpectrumMarker> markers;
    std::string line;
    while (std::getline(input, line))
    {
        if (line.empty())
            continue;
        std::istringstream fields{line};
        std::size_t frame = 0;
        if (!(fields >> frame))
        {
            error = "The markers file has a line without a frame";
            return std::nullopt;
        }
        std::string name;
        std::getline(fields >> std::ws, name);
        auto added = spectrumMarkersWith(std::move(markers), frame, std::move(name));
        if (!added)
        {
            error = "The markers file has two markers at frame " + std::to_string(frame);
            return std::nullopt;
        }
        markers = std::move(*added);
    }
    return markers;
}

SpectrumTableUse spectrumTableUse(std::string_view typeId, std::string_view parameterId) noexcept
{
    if ((typeId == "talas.spectrumInRam" && parameterId == "analysis") || (typeId == "talas.spectrumOnDisk" && parameterId == "fileName"))
        return SpectrumTableUse::Triples;
    if (typeId == "talas.sumOfSines" && (parameterId == "analysis0" || parameterId == "analysis1"))
        return SpectrumTableUse::HarmonicAmplitudes;
    return SpectrumTableUse::None;
}

std::filesystem::path spectrumTablePath(const std::filesystem::path& spectrumFile)
{
    auto table = spectrumFile;
    table.replace_filename(talas::project::pathFromUtf8(talas::project::pathToUtf8(spectrumFile.stem()) + " table.txt"));
    return table;
}

std::vector<talas::project::ProjectParameterExpression> spectrumTableFields(
    std::string_view parameterId, SpectrumTableUse use, std::size_t frames)
{
    const auto count = std::to_string(std::max<std::size_t>(1, frames));
    switch (use)
    {
    case SpectrumTableUse::Triples:
        return {expression("analysisFrames", count), expression("analysisFrequency", "1 hz")};
    case SpectrumTableUse::HarmonicAmplitudes:
        return {expression(parameterId == "analysis1" ? "analysisFrames1" : "analysisFrames0", count)};
    case SpectrumTableUse::None:
        break;
    }
    return {};
}

bool spectrumMarkersCorrespond(const std::vector<SpectrumMarker>& guide, const std::vector<SpectrumMarker>& warped) noexcept
{
    return guide.size() >= 2 && guide.size() == warped.size();
}

std::string spectrumTimePoints(const talas::project::PartialTrackAsset& spectrum, const std::vector<SpectrumMarker>& markers)
{
    std::ostringstream text;
    text << std::setprecision(9);
    for (std::size_t index = 0; index < markers.size(); ++index)
        text << (index == 0 ? "" : " ") << '{' << spectrumFrameSeconds(spectrum, markers[index].frame) << " s}";
    return text.str();
}

AnalysisExampleSound spectrumMixExampleSound(const talas::project::ProjectDocument& document, const std::string& name,
    const SynchronizedSpectrum& guide, const SynchronizedSpectrum& warped)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "spectrumMix");
    definition.displayName = name + " mix";
    definition.graph.nodes = {
        warpedTimeIndex("guideTime", guide.timePoints, guide.timePoints, guide.timePoints, "0"),
        warpedTimeIndex("warpedTime", warped.timePoints, guide.timePoints, guide.timePoints, "0"),
        spectrumInRam("guide", guide, guide.partials),
        spectrumInRam("warped", warped, warped.partials),
        oscillatorBank("guideBank", guide.partials),
        oscillatorBank("warpedBank", warped.partials),
        {"join", "talas.channelJoin", {}},
    };
    definition.graph.connections = {
        {"guideTime", "value", "guide", "timeIndex"},
        {"warpedTime", "value", "warped", "timeIndex"},
        {"guide", "spectrumOut", "guideBank", "spectrum"},
        {"warped", "spectrumOut", "warpedBank", "spectrum"},
        {"guideBank", "audioOut", "join", "left"},
        {"warpedBank", "audioOut", "join", "right"},
    };
    definition.outputs = {
        {"audioOut", talas::core::PortType::Audio, "join", "leftOut"},
        {"out2", talas::core::PortType::Audio, "join", "rightOut"},
    };
    example.resources = {{"guide", "analysis", guide.table}, {"warped", "analysis", warped.table}};
    return example;
}

AnalysisExampleSound spectrumMorphExampleSound(const talas::project::ProjectDocument& document, const std::string& name,
    const SynchronizedSpectrum& guide, const SynchronizedSpectrum& warped)
{
    AnalysisExampleSound example;
    auto& definition = example.definition;
    definition.definitionId = uniqueDefinitionId(document, "spectrumMorph");
    definition.displayName = name + " morph";
    // The two banks of envelopes must cycle through the same partials.
    const auto partials = std::max<std::size_t>(1, std::min(guide.partials, warped.partials));
    const auto crossfade = [](std::string id) {
        return talas::project::ProjectNode{std::move(id), "talas.crossfade",
            {expression("fade", "!Morph"), expression("pan", "0"), expression("scale", "1"),
                [] {
                    talas::project::ProjectParameterExpression type;
                    type.parameterId = "type";
                    type.choice = std::string{"linear"};
                    return type;
                }()}};
    };
    definition.graph.nodes = {
        warpedTimeIndex("guideTime", guide.timePoints, guide.timePoints, warped.timePoints, "!Morph"),
        warpedTimeIndex("warpedTime", warped.timePoints, guide.timePoints, warped.timePoints, "!Morph"),
        spectrumInRam("guide", guide, partials),
        spectrumInRam("warped", warped, partials),
        crossfade("amplitudes"),
        crossfade("frequencies"),
        oscillatorBank("bank", partials),
    };
    definition.graph.connections = {
        {"guideTime", "value", "guide", "timeIndex"},
        {"warpedTime", "value", "warped", "timeIndex"},
        {"guide", "amplitudeOut", "amplitudes", "snd1"},
        {"warped", "amplitudeOut", "amplitudes", "snd2"},
        {"guide", "frequencyOut", "frequencies", "snd1"},
        {"warped", "frequencyOut", "frequencies", "snd2"},
        {"amplitudes", "leftOut", "bank", "amplitudeIn"},
        {"frequencies", "leftOut", "bank", "frequencyIn"},
    };
    definition.outputs = {{"audioOut", talas::core::PortType::Audio, "bank", "audioOut"}};
    example.resources = {{"guide", "analysis", guide.table}, {"warped", "analysis", warped.table}};
    return example;
}

bool synchronizingSpectraShows(bool continued, SynchronizingSpectraPart part) noexcept
{
    return part == SynchronizingSpectraPart::Spectra ? continued : !continued;
}
}
