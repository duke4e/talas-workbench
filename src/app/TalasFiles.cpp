#include "talas/app/TalasFiles.h"

#include "talas/app/TimelineMidiImport.h"
#include "talas/project/SosSpectrumFile.h"
#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ToolFile.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <sstream>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string lowered(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

[[nodiscard]] std::string kindKey(TalasFileKind kind)
{
    switch (kind)
    {
    case TalasFileKind::Sample: return "sample";
    case TalasFileKind::Spectrum: return "spectrum";
    case TalasFileKind::Ga: return "ga";
    case TalasFileKind::Re: return "re";
    case TalasFileKind::Midi: return "midi";
    }
    return "sample";
}

[[nodiscard]] std::optional<TalasFileKind> kindFromKey(std::string_view key)
{
    for (const auto kind : {TalasFileKind::Sample, TalasFileKind::Spectrum, TalasFileKind::Ga, TalasFileKind::Re, TalasFileKind::Midi})
    {
        if (kindKey(kind) == key)
            return kind;
    }
    return std::nullopt;
}

[[nodiscard]] std::vector<std::uint8_t> readBytes(const std::filesystem::path& path, std::size_t limit = 64U << 20U)
{
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes;
    if (!input)
        return bytes;
    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(std::max<std::streamoff>(0, input.tellg()));
    if (size > limit)
        return bytes;
    input.seekg(0);
    bytes.resize(size);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return bytes;
}

[[nodiscard]] std::optional<talas::project::ProjectAsset> inspectedSample(const std::filesystem::path& path)
{
    talas::project::ProjectAsset manifest;
    manifest.id = "organizer";
    manifest.kind = talas::project::ProjectAssetKind::Audio;
    manifest.path = path.string();
    const auto inspected = talas::project::inspectProjectAsset(manifest, path);
    if (!inspected.error.has_value())
        return inspected.asset;
    return std::nullopt;
}
}

std::optional<TalasFileKind> talasFileKindOf(const std::filesystem::path& path)
{
    const auto name = lowered(path.filename().string());
    const auto extension = lowered(path.extension().string());
    if (name.ends_with(" ga.wav"))
        return TalasFileKind::Ga;
    if (name.ends_with(" re.txt"))
        return TalasFileKind::Re;
    if (extension == ".talaspartial" || extension == ".spc")
        return TalasFileKind::Spectrum;
    if (extension == ".mid" || extension == ".midi")
        return TalasFileKind::Midi;
    if (extension == ".wav" || extension == ".aif" || extension == ".aiff" || extension == ".sd2"
        || extension == ".sf" || extension == ".sd1")
        return TalasFileKind::Sample;
    return std::nullopt;
}

std::string_view talasFileKindColourName(TalasFileKind kind) noexcept
{
    switch (kind)
    {
    case TalasFileKind::Sample: return "turquoise";
    case TalasFileKind::Spectrum: return "purple";
    case TalasFileKind::Ga: return "red-brown";
    case TalasFileKind::Re: return "yellow-orange";
    case TalasFileKind::Midi: return "green";
    }
    return "turquoise";
}

std::uint32_t talasFileKindColour(TalasFileKind kind) noexcept
{
    switch (kind)
    {
    case TalasFileKind::Sample: return 0xff40e0d0u;
    case TalasFileKind::Spectrum: return 0xffb060e0u;
    case TalasFileKind::Ga: return 0xffb05a3cu;
    case TalasFileKind::Re: return 0xfff0b030u;
    case TalasFileKind::Midi: return 0xff50c060u;
    }
    return 0xff40e0d0u;
}

void FileOrganizerModel::setRoots(std::vector<std::filesystem::path> roots)
{
    roots_ = std::move(roots);
}

void FileOrganizerModel::toggle(const std::filesystem::path& folder)
{
    if (!open_.erase(folder))
        open_.insert(folder);
}

bool FileOrganizerModel::isOpen(const std::filesystem::path& folder) const
{
    return open_.contains(folder);
}

std::vector<FileOrganizerRow> FileOrganizerModel::rows(std::size_t limit) const
{
    std::vector<FileOrganizerRow> rows;
    const auto add = [&](const auto& self, const std::filesystem::path& folder, std::size_t depth) -> void {
        if (rows.size() >= limit)
            return;
        const auto open = open_.contains(folder);
        rows.push_back({folder, depth, true, open, std::nullopt});
        if (!open)
            return;
        std::vector<std::filesystem::path> folders;
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error))
        {
            std::error_code typeError;
            if (entry.is_directory(typeError))
                folders.push_back(entry.path());
            else if (entry.is_regular_file(typeError) && talasFileKindOf(entry.path()).has_value())
                files.push_back(entry.path());
        }
        const auto byName = [](const auto& left, const auto& right) {
            return lowered(left.filename().string()) < lowered(right.filename().string());
        };
        std::ranges::sort(folders, byName);
        std::ranges::sort(files, byName);
        const auto addFile = [&](const std::filesystem::path& file) {
            if (rows.size() < limit)
                rows.push_back({file, depth + 1, false, false, talasFileKindOf(file)});
        };
        if (foldersFirst_)
        {
            for (const auto& child : folders)
                self(self, child, depth + 1);
            for (const auto& file : files)
                addFile(file);
            return;
        }
        // Folders among the files, each by name.
        std::vector<std::filesystem::path> all = folders;
        all.insert(all.end(), files.begin(), files.end());
        std::ranges::stable_sort(all, byName);
        for (const auto& child : all)
        {
            if (std::ranges::find(folders, child) != folders.end())
                self(self, child, depth + 1);
            else
                addFile(child);
        }
    };
    for (const auto& root : roots_)
        add(add, root, 0);
    return rows;
}

std::vector<std::string> talasFileInformation(const std::filesystem::path& path)
{
    std::vector<std::string> lines{path.string()};
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (!error)
    {
        const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            std::chrono::file_clock::to_sys(modified));
        const auto seconds = std::chrono::system_clock::to_time_t(system);
        char text[64];
        std::strftime(text, sizeof text, "Modified %Y-%m-%d %H:%M:%S", std::localtime(&seconds));
        lines.emplace_back(text);
    }
    const auto kind = talasFileKindOf(path);
    if (!kind.has_value())
        return lines;
    char text[160];
    switch (*kind)
    {
    case TalasFileKind::Sample:
    case TalasFileKind::Ga:
        if (const auto sample = inspectedSample(path); sample.has_value() && sample->sampleRate > 0.0)
        {
            std::snprintf(text, sizeof text, "%u channel%s · %.0f Hz · %llu frames · %.3f s", sample->channelCount,
                sample->channelCount == 1 ? "" : "s", sample->sampleRate,
                static_cast<unsigned long long>(sample->frameCount),
                static_cast<double>(sample->frameCount) / sample->sampleRate);
            lines.emplace_back(text);
        }
        break;
    case TalasFileKind::Spectrum:
    {
        const auto bytes = readBytes(path);
        const auto decoded = talas::project::decodePartialTrackAsset(bytes);
        if (decoded.ok())
        {
            std::set<std::uint64_t> tracks;
            for (const auto& frame : decoded.asset.frames)
            {
                for (const auto& point : frame.points)
                    tracks.insert(point.trackId);
            }
            std::snprintf(text, sizeof text, "%zu tracks · %zu frames · %.3f s", tracks.size(), decoded.asset.frames.size(),
                decoded.asset.sampleRate > 0.0 ? static_cast<double>(decoded.asset.sourceFrameCount) / decoded.asset.sampleRate : 0.0);
            lines.emplace_back(text);
        }
        break;
    }
    case TalasFileKind::Midi:
    {
        const auto bytes = readBytes(path);
        if (const auto seconds = standardMidiFileSeconds(bytes))
        {
            std::string channels;
            for (const auto channel : standardMidiFileChannels(bytes))
                channels += (channels.empty() ? "" : ", ") + std::to_string(channel);
            std::snprintf(text, sizeof text, "%.3f s · channels %s", *seconds, channels.empty() ? "none" : channels.c_str());
            lines.emplace_back(text);
        }
        break;
    }
    case TalasFileKind::Re:
        lines.emplace_back("RE resonator table");
        break;
    }
    return lines;
}

std::string writeWorkbenchPreferences(const WorkbenchPreferences& preferences)
{
    std::ostringstream out;
    const auto flag = [&](std::string_view key, bool value) { out << key << '=' << (value ? "yes" : "no") << '\n'; };
    out << "talas-preferences 1\n";
    flag("showFullPathNames", preferences.showFullPathNames);
    flag("showAllDigits", preferences.showAllDigits);
    flag("largeFieldIcons", preferences.largeFieldIcons);
    flag("showEightChannelsInDspStatus", preferences.showEightChannels);
    flag("openVirtualControlsOnPlay", preferences.openVirtualControlsOnPlay);
    flag("systemOutputLevelPad", preferences.padSystemOutput);
    flag("notifyCompilerWarningsImmediately", preferences.notifyCompilerWarnings);
    flag("showHelpOnStartup", preferences.showHelpOnStartup);
    flag("showUntitledOnStartup", preferences.showUntitledOnStartup);
    flag("showSoundBrowserOnStartup", preferences.showSoundBrowserOnStartup);
    flag("updateDisplayDuringDiskOperations", preferences.updateDisplayDuringDiskOperations);
    flag("allowStackTraces", preferences.allowStackTraces);
    flag("escapeLearnsPitch", preferences.learnOneKeyAsPitch);
    flag("graySelection", preferences.graySelection);
    out << "defaultMidiInputChannel=" << preferences.defaultMidiInputChannel << '\n';
    out << "vcsProgramChangeChannel=" << preferences.vcsProgramChangeChannel << '\n';
    out << "spectrumAnalyzerLength=" << preferences.spectrumAnalyzer.length << '\n';
    out << "spectrumAnalyzerWindow=" << spectrumAnalyzerWindowKey(preferences.spectrumAnalyzer.window) << '\n';
    // The speakers on one line, a semicolon between them.
    if (!preferences.speakerPlacement.empty())
    {
        auto speakers = talas::project::speakerPlacementText(preferences.speakerPlacement);
        std::ranges::replace(speakers, '\n', ';');
        out << "speakerPlacement=" << speakers << '\n';
    }
    for (const auto& folder : preferences.frequentlyUsedFolders)
        out << "frequentlyUsedFolder=" << folder << '\n';
    for (const auto& [kind, application] : preferences.fileEditors)
        out << "fileEditor." << kindKey(kind) << '=' << application << '\n';
    for (const auto& [prototype, number] : preferences.prototypeCopyNumbers)
        out << "prototypeCopy." << prototype << '=' << number << '\n';
    return out.str();
}

std::vector<std::filesystem::path> foldersInside(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    for (std::filesystem::directory_iterator entry{folder, std::filesystem::directory_options::skip_permission_denied, error}, end;
         !error && entry != end; entry.increment(error))
    {
        std::error_code typeError;
        const auto name = entry->path().filename().string();
        if (!name.empty() && name.front() != '.' && entry->is_directory(typeError) && !typeError)
            folders.push_back(entry->path());
    }
    const auto lower = [](const std::filesystem::path& path) {
        auto name = path.filename().string();
        std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return name;
    };
    std::ranges::sort(folders, {}, lower);
    return folders;
}

WorkbenchPreferences withFrequentlyUsedFolder(WorkbenchPreferences preferences, const std::filesystem::path& folder)
{
    const auto text = talas::project::pathToUtf8(folder.lexically_normal());
    if (!text.empty() && std::ranges::find(preferences.frequentlyUsedFolders, text) == preferences.frequentlyUsedFolders.end())
        preferences.frequentlyUsedFolders.push_back(text);
    return preferences;
}

WorkbenchPreferences withoutFrequentlyUsedFolder(WorkbenchPreferences preferences, std::size_t index)
{
    if (index < preferences.frequentlyUsedFolders.size())
        preferences.frequentlyUsedFolders.erase(preferences.frequentlyUsedFolders.begin() + static_cast<std::ptrdiff_t>(index));
    return preferences;
}

WorkbenchPreferences startupWorkbenchPreferences(
    std::string_view text, const WorkbenchPreferences& defaults, bool capsLockHeld)
{
    return capsLockHeld ? defaults : readWorkbenchPreferences(text, defaults);
}

WorkbenchPreferences readWorkbenchPreferences(std::string_view text, const WorkbenchPreferences& defaults)
{
    if (!text.starts_with("talas-preferences 1"))
        return defaults;
    WorkbenchPreferences preferences;
    std::istringstream in{std::string{text}};
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line))
    {
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const auto key = line.substr(0, equals);
        const auto value = line.substr(equals + 1);
        const auto flag = [&](bool& field) {
            if (value == "yes" || value == "no")
                field = value == "yes";
        };
        if (key == "showFullPathNames")
            flag(preferences.showFullPathNames);
        else if (key == "showAllDigits")
            flag(preferences.showAllDigits);
        else if (key == "largeFieldIcons")
            flag(preferences.largeFieldIcons);
        else if (key == "showEightChannelsInDspStatus")
            flag(preferences.showEightChannels);
        else if (key == "openVirtualControlsOnPlay")
            flag(preferences.openVirtualControlsOnPlay);
        else if (key == "systemOutputLevelPad")
            flag(preferences.padSystemOutput);
        else if (key == "notifyCompilerWarningsImmediately")
            flag(preferences.notifyCompilerWarnings);
        else if (key == "showHelpOnStartup")
            flag(preferences.showHelpOnStartup);
        else if (key == "showUntitledOnStartup")
            flag(preferences.showUntitledOnStartup);
        else if (key == "showSoundBrowserOnStartup")
            flag(preferences.showSoundBrowserOnStartup);
        else if (key == "updateDisplayDuringDiskOperations")
            flag(preferences.updateDisplayDuringDiskOperations);
        else if (key == "allowStackTraces")
            flag(preferences.allowStackTraces);
        else if (key == "escapeLearnsPitch")
            flag(preferences.learnOneKeyAsPitch);
        else if (key == "graySelection")
            flag(preferences.graySelection);
        else if (key == "defaultMidiInputChannel")
        {
            if (value.size() <= 2 && !value.empty() && std::ranges::all_of(value, [](char digit) { return digit >= '0' && digit <= '9'; }))
                if (const auto channel = std::stoi(value); channel >= 1 && channel <= 16)
                    preferences.defaultMidiInputChannel = channel;
        }
        else if (key == "vcsProgramChangeChannel")
        {
            if (value.size() <= 2 && !value.empty() && std::ranges::all_of(value, [](char digit) { return digit >= '0' && digit <= '9'; }))
                if (const auto channel = std::stoi(value); channel >= 0 && channel <= 16)
                    preferences.vcsProgramChangeChannel = channel;
        }
        else if (key == "frequentlyUsedFolder" && !value.empty())
            preferences.frequentlyUsedFolders.push_back(value);
        else if (key == "spectrumAnalyzerLength" && (value == "64" || value == "128" || value == "256"))
            preferences.spectrumAnalyzer.length = static_cast<unsigned>(std::stoul(value));
        else if (key == "spectrumAnalyzerWindow" && spectrumAnalyzerWindowFromKey(value).has_value())
            preferences.spectrumAnalyzer.window = *spectrumAnalyzerWindowFromKey(value);
        else if (key == "speakerPlacement")
        {
            auto speakers = value;
            std::ranges::replace(speakers, ';', '\n');
            if (auto placement = talas::project::parseSpeakerPlacement(speakers))
                preferences.speakerPlacement = std::move(*placement);
        }
        else if (key.starts_with("fileEditor."))
        {
            if (const auto kind = kindFromKey(key.substr(11)))
                preferences.fileEditors[*kind] = value;
        }
        else if (key.starts_with("prototypeCopy.") && key.size() > 14 && !value.empty() && value.size() <= 9
            && std::ranges::all_of(value, [](char digit) { return digit >= '0' && digit <= '9'; }))
        {
            preferences.prototypeCopyNumbers[key.substr(14)] = static_cast<unsigned>(std::stoul(value));
        }
    }
    return preferences;
}

std::string preferencesReleaseLine(std::string_view version, std::string_view compilerDate)
{
    static constexpr std::array<std::string_view, 12> months{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    auto line = "Talas " + std::string{version};
    const auto month = compilerDate.size() == 11 ? std::ranges::find(months, compilerDate.substr(0, 3)) : months.end();
    const auto day = compilerDate.size() == 11 ? std::string{compilerDate.substr(4, 2)} : std::string{};
    const auto year = compilerDate.size() == 11 ? compilerDate.substr(7) : std::string_view{};
    const auto digits = [](std::string_view text) { return std::ranges::all_of(text, [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }); };
    if (month == months.end() || compilerDate[3] != ' ' || compilerDate[6] != ' ' || !digits(year)
        || !(day[0] == ' ' || std::isdigit(static_cast<unsigned char>(day[0])) != 0) || !digits(day.substr(1)))
        return line;
    char date[16];
    std::snprintf(date, sizeof date, "%.4s-%02d-%02d", std::string{year}.c_str(),
        static_cast<int>(month - months.begin()) + 1, std::stoi(day));
    return line + ", released " + date;
}

float systemOutputGain(const WorkbenchPreferences& preferences) noexcept
{
    // -6 dB: 10^(-6/20).
    return preferences.padSystemOutput ? 0.501187234F : 1.0F;
}

std::string displayedFileName(std::string_view path, const WorkbenchPreferences& preferences)
{
    if (preferences.showFullPathNames)
        return std::string{path};
    return std::filesystem::path{std::string{path}}.filename().string();
}

std::string displayedNumber(double value, const WorkbenchPreferences& preferences)
{
    char text[48];
    std::snprintf(text, sizeof text, preferences.showAllDigits ? "%.12g" : "%.6g", value);
    return text;
}

TalasFileOpening talasFileOpening(TalasFileKind kind, const WorkbenchPreferences& preferences, bool fromOpenCommand)
{
    if (const auto found = preferences.fileEditors.find(kind); !fromOpenCommand && found != preferences.fileEditors.end())
        return found->second.empty() ? TalasFileOpening::AskForExternalEditor : TalasFileOpening::ExternalEditor;
    switch (kind)
    {
    case TalasFileKind::Sample:
    case TalasFileKind::Ga:
        return TalasFileOpening::SampleEditor;
    case TalasFileKind::Spectrum:
        return TalasFileOpening::SpectrumEditor;
    case TalasFileKind::Midi:
        return TalasFileOpening::MidiTimeline;
    case TalasFileKind::Re:
        return TalasFileOpening::None;
    }
    return TalasFileOpening::None;
}

std::optional<AnalysisExampleSound> soundForTalasFile(
    const talas::project::ProjectDocument& document, const std::filesystem::path& path)
{
    const auto kind = talasFileKindOf(path);
    if (!kind.has_value())
        return std::nullopt;
    const auto seconds = [](const std::filesystem::path& file) {
        const auto sample = inspectedSample(file);
        return sample.has_value() && sample->sampleRate > 0.0
            ? static_cast<double>(sample->frameCount) / sample->sampleRate
            : 2.0;
    };
    switch (*kind)
    {
    case TalasFileKind::Sample:
        return sampleFileSound(document, path);
    case TalasFileKind::Spectrum:
        return spectralAnalysisExampleSound(document, path);
    case TalasFileKind::Ga:
        // Middle C, as the analysis has no pitch of its own on disk.
        return gaAnalysisExampleSound(document, path, 261.6256, seconds(path));
    case TalasFileKind::Re:
    {
        auto excitation = path;
        auto name = path.stem().string();
        name = name.substr(0, name.size() - 3) + " EX.wav";
        excitation.replace_filename(name);
        std::error_code error;
        const auto hasExcitation = std::filesystem::is_regular_file(excitation, error);
        return reAnalysisExampleSound(document, path, hasExcitation ? excitation : std::filesystem::path{},
            hasExcitation ? seconds(excitation) : 2.0);
    }
    case TalasFileKind::Midi:
    {
        const auto bytes = readBytes(path);
        if (!standardMidiFileSeconds(bytes).has_value())
            return std::nullopt;
        return midiFileOrchestraSound(document, path, standardMidiFileChannels(bytes));
    }
    }
    return std::nullopt;
}

namespace
{
constexpr std::string_view fileDragPrefix = "talasFile:";
}

std::string talasFileDragDescription(const std::filesystem::path& path)
{
    return std::string{fileDragPrefix} + path.string();
}

std::optional<std::filesystem::path> talasFileFromDrag(std::string_view description)
{
    if (!description.starts_with(fileDragPrefix) || description.size() == fileDragPrefix.size())
        return std::nullopt;
    return std::filesystem::path{std::string{description.substr(fileDragPrefix.size())}};
}

std::optional<std::string> talasFileFieldValue(const std::filesystem::path& path, talas::expr::ValueDomain domain)
{
    if (domain != talas::expr::ValueDomain::Seconds && domain != talas::expr::ValueDomain::Samples)
        return std::nullopt;
    std::optional<double> seconds;
    switch (talasFileKindOf(path).value_or(TalasFileKind::Re))
    {
    case TalasFileKind::Sample:
    case TalasFileKind::Ga:
        if (const auto sample = inspectedSample(path); sample.has_value() && sample->sampleRate > 0.0)
            seconds = static_cast<double>(sample->frameCount) / sample->sampleRate;
        break;
    case TalasFileKind::Midi:
    {
        const auto bytes = readBytes(path);
        seconds = standardMidiFileSeconds(bytes);
        break;
    }
    case TalasFileKind::Spectrum:
    {
        // A spectrum gives the length of the sample analysed.
        const auto decoded = talas::project::decodePartialTrackAsset(readBytes(path));
        if (decoded.ok() && decoded.asset.sampleRate > 0.0)
            seconds = static_cast<double>(decoded.asset.sourceFrameCount) / decoded.asset.sampleRate;
        break;
    }
    case TalasFileKind::Re:
        break;
    }
    if (!seconds.has_value())
        return std::nullopt;
    char text[48];
    std::snprintf(text, sizeof text, "%.6g s", *seconds);
    return std::string{text};
}

std::optional<TalasFilePlayer> talasFilePlayer(const std::filesystem::path& path)
{
    TalasFilePlayer player;
    auto sound = soundForTalasFile(player.soundFile, path);
    if (!sound.has_value())
        return std::nullopt;
    for (const auto& resource : sound->resources)
    {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(resource.file, error).lexically_normal();
        auto extension = resource.file.extension().string();
        std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        talas::project::ProjectAsset asset;
        asset.id = resource.file.filename().string() + "#" + std::to_string(player.soundFile.assets.size() + 1);
        asset.kind = extension == ".mid" || extension == ".midi" ? talas::project::ProjectAssetKind::Binary
            : talas::project::isSpectrumFilePath(resource.file)        ? talas::project::ProjectAssetKind::PartialSpectrum
                                                                     : talas::project::ProjectAssetKind::Table;
        asset.path = (error ? resource.file : absolute).string();
        auto& nodes = sound->definition.graph.nodes;
        const auto node = std::ranges::find(nodes, resource.nodeId, &talas::project::ProjectNode::instanceId);
        if (node == nodes.end())
            continue;
        auto parameter = std::ranges::find(node->parameters, resource.parameterId, &talas::project::ProjectParameterExpression::parameterId);
        if (parameter == node->parameters.end())
            parameter = node->parameters.insert(node->parameters.end(), {resource.parameterId, {}, {}, {}, {}, {}});
        parameter->resource = asset.id;
        player.soundFile.assets.push_back(std::move(asset));
    }
    player.definitionId = sound->definition.definitionId;
    player.name = sound->definition.displayName.empty() ? sound->definition.definitionId : sound->definition.displayName;
    player.soundFile.soundDefinitions.push_back(std::move(sound->definition));
    player.soundFile.virtualControls = std::move(sound->controls);
    return player;
}

std::vector<TalasFileType> talasFileTypes(bool playableOnly)
{
    // Play... plays samples, Sound files, spectra and MIDI files,
    // an analysis through the Sound that resynthesises it.
    std::vector<TalasFileType> types{
        {"Sample file", "*.wav;*.aif;*.aiff;*.sd1;*.sd2;*.sf"},
        {"Sound file", "*.talas.json"},
        {"Timeline", "*.talastimeline"},
        {"Spectrum file", "*.talaspartial;*.spc"},
        {"MIDI file", "*.mid;*.midi"},
    };
    // A compiled Sound file plays but does not open.
    if (playableOnly)
    {
        types.push_back({"Compiled Sound file", "*.talascompiled"});
        return types;
    }
    types.push_back({"RE analysis", "* RE.txt"});
    types.push_back({"Project package", "*.talaspkg"});
    // A Tool, which Open any... runs.
    types.push_back({"Tool", "*.talastool"});
    // A Compiled Sound Grid's own file, which joins the project.
    types.push_back({"Compiled Sound grid", "*.prg"});
    types.push_back({"Text file", "*.txt"});
    types.push_back({"Global map", "*.map"});
    return types;
}

std::vector<TalasOpenFileType> talasOpenFileTypes()
{
    // A Tool on the disk runs as Use tool or Any
    // openable file, and opens in the Tool editor as Edit tool.
    std::string any;
    for (const auto& type : talasFileTypes(false))
        any += (any.empty() ? "" : ";") + type.wildcard;
    const auto tool = std::string{"*"} + std::string{talas::project::tool::toolFileExtension};
    return {
        {"Sound file", "*.talas.json;*.talaspkg", TalasOpenAction::Project},
        {"Timeline", "*.talastimeline", TalasOpenAction::Project},
        {"Edit Tool", tool, TalasOpenAction::EditTool},
        {"Use Tool", tool, TalasOpenAction::UseTool},
        {"Any openable file", any, TalasOpenAction::AnyOpenableFile},
    };
}

bool isTimelineFile(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension == timelineFileExtension;
}

bool talasFileIsGlobalMap(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".map";
}

std::optional<std::string> globalMapFileText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input || !talasFileIsGlobalMap(path))
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

std::optional<std::filesystem::path> defaultGlobalMapFile(const std::filesystem::path& folder)
{
    const auto maps = folder / "Global Maps";
    const auto file = maps / "Default.map";
    std::error_code error;
    if (std::filesystem::is_regular_file(file, error))
        return file;
    std::filesystem::create_directories(maps, error);
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << talas::project::defaultGlobalMapText();
    if (!output)
        return std::nullopt;
    return file;
}

bool talasFileOpensAsText(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".txt" && talasFileKindOf(path) != std::optional<TalasFileKind>{TalasFileKind::Re};
}

double signalFlowIconScale(bool large) noexcept
{
    return large ? 1.0 : 0.5;
}

std::filesystem::path diskRecorderFile(std::string_view fileName, const std::filesystem::path& programFolder, bool aiff)
{
    return diskRecorderFileOfFormat(fileName, programFolder, aiff ? ".aif" : ".wav");
}

std::filesystem::path diskRecorderFileOfFormat(
    std::string_view fileName, const std::filesystem::path& programFolder, std::string_view extension)
{
    auto file = talas::project::pathFromUtf8(fileName);
    if (!file.is_absolute())
        file = programFolder / file;
    if (!file.has_extension())
        file += talas::project::pathFromUtf8(extension);
    return file.lexically_normal();
}

bool programChangeSelectsVcsPreset(int vcsChannel, int channel) noexcept
{
    return vcsChannel == 0 || vcsChannel == channel;
}

SpectrumAnalyzerSettings spectrumDisplaySettings(
    bool infoAnalyzer, const SpectrumAnalyzerSettings& preferences, const SpectrumAnalyzerSettings& panel) noexcept
{
    return infoAnalyzer ? preferences : panel;
}

// The four further windows after the three offered
// before them, so that a preferences file keeps its choice.
const std::vector<SpectrumAnalyzerWindow>& spectrumAnalyzerWindows()
{
    static const std::vector<SpectrumAnalyzerWindow> windows{SpectrumAnalyzerWindow::Rectangular,
        SpectrumAnalyzerWindow::Hann, SpectrumAnalyzerWindow::Blackman, SpectrumAnalyzerWindow::LinearEnvelope,
        SpectrumAnalyzerWindow::Hamming, SpectrumAnalyzerWindow::LongKaiser};
    return windows;
}

std::string_view spectrumAnalyzerWindowKey(SpectrumAnalyzerWindow window) noexcept
{
    switch (window)
    {
    case SpectrumAnalyzerWindow::Rectangular: return "rectangular";
    case SpectrumAnalyzerWindow::Hann: return "hann";
    case SpectrumAnalyzerWindow::Blackman: return "blackman";
    case SpectrumAnalyzerWindow::LinearEnvelope: return "linearEnvelope";
    case SpectrumAnalyzerWindow::Hamming: return "hamming";
    case SpectrumAnalyzerWindow::LongKaiser: return "longKaiser";
    }
    return "hann";
}

std::string_view spectrumAnalyzerWindowLabel(SpectrumAnalyzerWindow window) noexcept
{
    switch (window)
    {
    case SpectrumAnalyzerWindow::Rectangular: return "Rectangular";
    case SpectrumAnalyzerWindow::Hann: return "Hann";
    case SpectrumAnalyzerWindow::Blackman: return "Blackman";
    case SpectrumAnalyzerWindow::LinearEnvelope: return "LinearEnvelope";
    case SpectrumAnalyzerWindow::Hamming: return "Hamming";
    case SpectrumAnalyzerWindow::LongKaiser: return "Long Kaiser";
    }
    return "Hann";
}

std::optional<SpectrumAnalyzerWindow> spectrumAnalyzerWindowFromKey(std::string_view key) noexcept
{
    for (const auto window : {SpectrumAnalyzerWindow::Rectangular, SpectrumAnalyzerWindow::Hann,
             SpectrumAnalyzerWindow::Blackman, SpectrumAnalyzerWindow::LinearEnvelope, SpectrumAnalyzerWindow::Hamming,
             SpectrumAnalyzerWindow::LongKaiser})
    {
        if (spectrumAnalyzerWindowKey(window) == key)
            return window;
    }
    return std::nullopt;
}

int soundFieldIconSize(bool large) noexcept
{
    return large ? 40 : 20;
}

std::vector<SoundFieldIconPlace> soundFieldIconPlaces(std::size_t count, int width, bool large)
{
    // Room for the name below the icon, and beside it.
    const auto cellWidth = soundFieldIconSize(large) + 52;
    const auto cellHeight = soundFieldIconSize(large) + 16;
    const auto columns = static_cast<std::size_t>(std::max(1, width / cellWidth));
    std::vector<SoundFieldIconPlace> places;
    for (std::size_t index = 0; index < count; ++index)
    {
        places.push_back({static_cast<int>(index % columns) * cellWidth, static_cast<int>(index / columns) * cellHeight,
            cellWidth, cellHeight});
    }
    return places;
}

std::vector<std::string> talasNewFileTypes()
{
    return {"Sound file", "Timeline", "Tool", "Sample file", "Global map", "Text file", "Compiled Sound grid"};
}

FileOrganizerFolderLook fileOrganizerFolderLook(bool open) noexcept
{
    return open ? FileOrganizerFolderLook{0xff000000u, true} : FileOrganizerFolderLook{0xff808080u, false};
}

std::vector<ViewChoice> viewChoices(ViewedWindow window)
{
    switch (window)
    {
    case ViewedWindow::SoundFileWindow:
        return {{"Icons", "Large icons"}, {"Icons", "Small icons"}};
    case ViewedWindow::SoundEditor:
        return {{"Size", "Large icons"}, {"Size", "Small icons"}, {"Icons", "Compact"}, {"Icons", "With ports"},
            {"Icons", "With ports and wires"}, {"Connections", "Curved"}, {"Connections", "Radial"},
            {"Connections", "Right angle"}, {"Parameters", "Parameters beside the signal flow"},
            {"Parameters", "Parameters below the signal flow"}, {"Prototypes", "Prototypes at the left"},
            {"Prototypes", "Prototypes across the top"}};
    case ViewedWindow::FileOrganizer:
        return {{"Folders", "Folders first"}, {"Folders", "Folders among the files"}};
    }
    return {};
}

namespace
{
[[nodiscard]] std::vector<std::filesystem::path> filesInNamedFolders(const std::vector<std::filesystem::path>& folders,
    const std::filesystem::path& name, std::initializer_list<std::string_view> extensions)
{
    std::vector<std::filesystem::path> files;
    const auto take = [&](const std::filesystem::path& folder) {
        std::error_code error;
        if (!std::filesystem::is_directory(folder, error))
            return;
        for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        {
            const auto extension = lowered(entry.path().extension().string());
            if (entry.is_regular_file(error) && std::ranges::find(extensions, std::string_view{extension}) != extensions.end()
                && std::ranges::find(files, entry.path()) == files.end())
                files.push_back(entry.path());
        }
    };
    for (const auto& folder : folders)
    {
        if (folder.filename() == name)
            take(folder);
        take(folder / name);
    }
    std::ranges::sort(files, [](const auto& left, const auto& right) {
        return left.filename().string() < right.filename().string();
    });
    return files;
}
}

std::vector<std::filesystem::path> toolsInToolsFolders(const std::vector<std::filesystem::path>& folders)
{
    return filesInNamedFolders(folders, "Tools", {".talastool"});
}

std::vector<std::filesystem::path> documentationInDocumentationFolders(const std::vector<std::filesystem::path>& folders)
{
    return filesInNamedFolders(folders, "Documentation", {".pdf", ".md", ".html"});
}

bool talasFileIsOfType(const std::filesystem::path& path, const TalasFileType& type)
{
    const auto name = lowered(path.filename().string());
    std::string_view patterns = type.wildcard;
    while (!patterns.empty())
    {
        const auto end = patterns.find(';');
        auto pattern = lowered(std::string{patterns.substr(0, end)});
        patterns = end == std::string_view::npos ? std::string_view{} : patterns.substr(end + 1);
        if (pattern.starts_with("*") && name.ends_with(pattern.substr(1)))
            return true;
    }
    return false;
}

std::size_t cycledWindowIndex(std::size_t count, std::size_t current, int step) noexcept
{
    if (count == 0)
        return 0;
    const auto signedCount = static_cast<long long>(count);
    const auto next = (static_cast<long long>(current % count) + step) % signedCount;
    return static_cast<std::size_t>(next < 0 ? next + signedCount : next);
}
}
