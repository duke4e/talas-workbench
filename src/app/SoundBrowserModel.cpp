#include "talas/app/SoundBrowserModel.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace talas::app
{
namespace
{
constexpr std::size_t maximumSoundFileBytes = 64U << 20U;
constexpr std::size_t maximumFindRows = 1U << 16U;

[[nodiscard]] std::string lowered(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

[[nodiscard]] std::size_t indexOf(SoundBrowserKind kind) noexcept
{
    return static_cast<std::size_t>(kind);
}

[[nodiscard]] std::optional<talas::project::ProjectDocument> readSoundFile(const std::filesystem::path& path)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximumSoundFileBytes)
        return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto loaded = talas::project::parseProjectDocument(text);
    if (!loaded.ok())
        return std::nullopt;
    return std::move(loaded.document);
}

[[nodiscard]] std::vector<std::uint8_t> headBytes(const std::filesystem::path& path, std::size_t limit)
{
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(limit);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(limit));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, input.gcount())));
    return bytes;
}

[[nodiscard]] std::uint32_t littleEndian32(const std::vector<std::uint8_t>& bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

[[nodiscard]] bool readsKeys(std::string_view expression) noexcept
{
    for (const auto* key : {"!KeyDown", "!KeyPitch", "!KeyVelocity", "!KeyNumber", "!KeyTimbre"})
    {
        if (expression.find(key) != std::string_view::npos)
            return true;
    }
    return false;
}

[[nodiscard]] bool definitionExpectsMidi(
    const talas::project::ProjectDocument& document, std::string_view definitionId, std::set<std::string>& visited)
{
    if (!visited.insert(std::string{definitionId}).second)
        return false;
    const auto definition = std::ranges::find(
        document.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return false;
    for (const auto& node : definition->graph.nodes)
    {
        if (node.typeId == "talas.midiVoice")
            return true;
        for (const auto& parameter : node.parameters)
        {
            if (parameter.expression.has_value() && readsKeys(*parameter.expression))
                return true;
        }
        if (node.soundDefinitionId.has_value() && definitionExpectsMidi(document, *node.soundDefinitionId, visited))
            return true;
    }
    return false;
}

[[nodiscard]] std::string fileName(const std::filesystem::path& path)
{
    auto name = path.filename().string();
    if (name.ends_with(".talas.json"))
        name.resize(name.size() - std::string_view{".talas.json"}.size());
    return name;
}
}

namespace
{
// The expressions of an expressions file, the file's name its category.
[[nodiscard]] std::vector<ExpressionLibraryEntry> expressionsOf(const std::filesystem::path& file)
{
    std::ifstream input(file, std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    auto entries = parseExpressionsFile(text, file.stem().string());
    return entries.has_value() ? std::move(*entries) : std::vector<ExpressionLibraryEntry>{};
}
}

SoundBrowserActivation soundBrowserActivation(SoundBrowserKind kind) noexcept
{
    switch (kind)
    {
    case SoundBrowserKind::Folder: return SoundBrowserActivation::Spawn;
    case SoundBrowserKind::SoundFile: return SoundBrowserActivation::OpenSoundFileWindow;
    case SoundBrowserKind::Sound: return SoundBrowserActivation::Edit;
    case SoundBrowserKind::Sample:
    case SoundBrowserKind::Spectrum: return SoundBrowserActivation::OpenInEditor;
    case SoundBrowserKind::Ga:
    case SoundBrowserKind::Re:
    case SoundBrowserKind::Midi: return SoundBrowserActivation::Play;
    case SoundBrowserKind::ExpressionsFile: return SoundBrowserActivation::Toggle;
    case SoundBrowserKind::Expression: return SoundBrowserActivation::Nothing;
    }
    return SoundBrowserActivation::Play;
}

std::optional<SoundBrowserKind> soundBrowserKindOf(const std::filesystem::path& path)
{
    if (lowered(path.filename().string()).ends_with(".talas.json") || isTimelineFile(path))
        return SoundBrowserKind::SoundFile;
    const auto kind = talasFileKindOf(path);
    // A text file beginning with EXPR holds expressions.
    if (!kind.has_value() && lowered(path.extension().string()) == ".txt")
    {
        std::ifstream input(path, std::ios::binary);
        std::string head(4, '\0');
        input.read(head.data(), 4);
        if (input.gcount() == 4 && head == "EXPR")
            return SoundBrowserKind::ExpressionsFile;
        return std::nullopt;
    }
    if (!kind.has_value())
        return std::nullopt;
    switch (*kind)
    {
    case TalasFileKind::Sample: return SoundBrowserKind::Sample;
    case TalasFileKind::Spectrum: return SoundBrowserKind::Spectrum;
    case TalasFileKind::Ga: return SoundBrowserKind::Ga;
    case TalasFileKind::Re: return SoundBrowserKind::Re;
    case TalasFileKind::Midi: return SoundBrowserKind::Midi;
    }
    return std::nullopt;
}

namespace
{
constexpr std::string_view browserSoundDragPrefix = "talas.browserSound:";
}

std::string soundBrowserDragDescription(const SoundBrowserRow& row)
{
    switch (row.kind)
    {
    case SoundBrowserKind::Folder:
    case SoundBrowserKind::SoundFile:
        return {};
    case SoundBrowserKind::Sound:
        return std::string{browserSoundDragPrefix} + row.definitionId + '\n' + talas::project::pathToUtf8(row.path);
    case SoundBrowserKind::ExpressionsFile:
        return {};
    case SoundBrowserKind::Expression:
    {
        // Dropped in a field as if pasted.
        const auto entry = soundBrowserExpression(row);
        return entry.has_value() ? expressionLibraryDragDescription(*entry) : std::string{};
    }
    default:
        return talasFileDragDescription(row.path);
    }
}

std::optional<SoundBrowserDrag> soundBrowserDrag(std::string_view description)
{
    if (description.starts_with(browserSoundDragPrefix))
    {
        const auto rest = description.substr(browserSoundDragPrefix.size());
        const auto newline = rest.find('\n');
        if (newline == std::string_view::npos || newline == 0 || newline + 1 == rest.size())
            return std::nullopt;
        return SoundBrowserDrag{talas::project::pathFromUtf8(rest.substr(newline + 1)), std::string{rest.substr(0, newline)}};
    }
    const auto file = talasFileFromDrag(description);
    const auto kind = file.has_value() ? soundBrowserKindOf(*file) : std::nullopt;
    if (!kind.has_value() || *kind == SoundBrowserKind::Folder || *kind == SoundBrowserKind::SoundFile
        || *kind == SoundBrowserKind::Sound)
        return std::nullopt;
    return SoundBrowserDrag{*file, {}};
}

std::uint32_t soundBrowserKindColour(SoundBrowserKind kind) noexcept
{
    switch (kind)
    {
    case SoundBrowserKind::Folder: return 0xffd0d0d0u;
    case SoundBrowserKind::SoundFile: return 0xff4a7dffu;
    case SoundBrowserKind::Sound: return 0xff6f9bffu;
    case SoundBrowserKind::Sample: return talasFileKindColour(TalasFileKind::Sample);
    case SoundBrowserKind::Spectrum: return talasFileKindColour(TalasFileKind::Spectrum);
    case SoundBrowserKind::Ga: return talasFileKindColour(TalasFileKind::Ga);
    case SoundBrowserKind::Re: return talasFileKindColour(TalasFileKind::Re);
    case SoundBrowserKind::Midi: return talasFileKindColour(TalasFileKind::Midi);
    case SoundBrowserKind::ExpressionsFile:
    case SoundBrowserKind::Expression: return 0xffe0a040u;
    }
    return 0xffd0d0d0u;
}

SoundBrowserRowLook soundBrowserRowLook(const SoundBrowserRow& row) noexcept
{
    const auto opens = row.kind == SoundBrowserKind::Folder || row.kind == SoundBrowserKind::SoundFile
        || row.kind == SoundBrowserKind::ExpressionsFile;
    SoundBrowserRowLook look;
    if (row.kind != SoundBrowserKind::Folder)
        look.colour = row.kind == SoundBrowserKind::SoundFile && isTimelineFile(row.path)
            ? soundBrowserTimelineColour
            : soundBrowserKindColour(row.kind);
    look.faded = opens && !row.open;
    look.triangle = !opens ? SoundBrowserTriangle::None : row.open ? SoundBrowserTriangle::Down : SoundBrowserTriangle::Sideways;
    look.underlined = row.kind == SoundBrowserKind::SoundFile;
    look.arrow = row.processesInput;
    return look;
}

std::string_view soundBrowserKindName(SoundBrowserKind kind) noexcept
{
    switch (kind)
    {
    case SoundBrowserKind::Folder: return "Folder";
    case SoundBrowserKind::SoundFile: return "Sound file";
    case SoundBrowserKind::Sound: return "Sound";
    case SoundBrowserKind::Sample: return "Sample";
    case SoundBrowserKind::Spectrum: return "Spectrum";
    case SoundBrowserKind::Ga: return "GA analysis";
    case SoundBrowserKind::Re: return "RE analysis";
    case SoundBrowserKind::Midi: return "MIDI file";
    case SoundBrowserKind::ExpressionsFile: return "Expressions file";
    case SoundBrowserKind::Expression: return "Expression";
    }
    return "Folder";
}

std::string soundBrowserTitle(const std::optional<std::filesystem::path>& folder)
{
    return folder.has_value() ? folder->filename().string() : std::string{"Sound Browser"};
}

std::vector<SoundBrowserSound> soundBrowserSounds(const talas::project::ProjectDocument& document)
{
    std::vector<SoundBrowserSound> sounds;
    for (const auto& definition : document.soundDefinitions)
    {
        sounds.push_back({definition.definitionId,
            definition.displayName.empty() ? definition.definitionId : definition.displayName,
            std::ranges::any_of(definition.inputs, [](const talas::project::ProjectSoundPort& port) {
                return port.type == talas::core::PortType::Audio;
            })});
    }
    std::ranges::stable_sort(sounds, {}, [](const SoundBrowserSound& sound) { return lowered(sound.name); });
    return sounds;
}

void SoundBrowserModel::setRoots(std::vector<std::filesystem::path> roots)
{
    roots_ = std::move(roots);
}

const std::vector<std::filesystem::path>& SoundBrowserModel::roots() const noexcept
{
    return roots_;
}

void SoundBrowserModel::toggle(const std::filesystem::path& path)
{
    if (!open_.erase(path))
        open_.insert(path);
}

bool SoundBrowserModel::isOpen(const std::filesystem::path& path) const
{
    return open_.contains(path);
}

void SoundBrowserModel::showOnly(SoundBrowserKind kind)
{
    shown_.fill(false);
    shown_[indexOf(kind)] = true;
}

void SoundBrowserModel::toggleKind(SoundBrowserKind kind)
{
    shown_[indexOf(kind)] = !shown_[indexOf(kind)];
}

void SoundBrowserModel::showAll()
{
    shown_.fill(true);
}

bool SoundBrowserModel::shows(SoundBrowserKind kind) const noexcept
{
    return kind == SoundBrowserKind::Folder || shown_[indexOf(kind)];
}

const std::vector<SoundBrowserSound>& SoundBrowserModel::soundsOf(const std::filesystem::path& soundFile) const
{
    const auto found = sounds_.find(soundFile);
    if (found != sounds_.end())
        return found->second;
    const auto document = readSoundFile(soundFile);
    return sounds_.emplace(soundFile, document.has_value() ? soundBrowserSounds(*document) : std::vector<SoundBrowserSound>{})
        .first->second;
}

void SoundBrowserModel::walk(const std::filesystem::path& folder, std::size_t depth, bool openOnly, std::size_t limit,
    std::vector<SoundBrowserRow>& rows, const std::function<bool()>& stopped) const
{
    if (rows.size() >= limit || (stopped && stopped()))
        return;
    rows.push_back({folder, {}, folder.filename().string(), depth, SoundBrowserKind::Folder, open_.contains(folder), false});
    if (openOnly && !open_.contains(folder))
        return;
    std::vector<std::filesystem::path> folders;
    std::vector<std::filesystem::path> files;
    std::error_code error;
    for (const auto& entry :
        std::filesystem::directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, error))
    {
        std::error_code typeError;
        if (entry.is_directory(typeError))
            folders.push_back(entry.path());
        else if (entry.is_regular_file(typeError) && soundBrowserKindOf(entry.path()).has_value())
            files.push_back(entry.path());
    }
    const auto byName = [](const auto& left, const auto& right) {
        return lowered(left.filename().string()) < lowered(right.filename().string());
    };
    std::ranges::sort(folders, byName);
    std::ranges::sort(files, byName);
    for (const auto& child : folders)
        walk(child, depth + 1, openOnly, limit, rows, stopped);
    for (const auto& file : files)
    {
        if (rows.size() >= limit || (stopped && stopped()))
            return;
        const auto kind = *soundBrowserKindOf(file);
        if (kind == SoundBrowserKind::ExpressionsFile)
        {
            // An expressions file shows while its expressions do, and
            // opens on them.
            if (!shows(SoundBrowserKind::ExpressionsFile) && !shows(SoundBrowserKind::Expression))
                continue;
            const auto open = open_.contains(file);
            rows.push_back({file, {}, file.filename().string(), depth + 1, kind, open, false});
            if ((openOnly && !open) || !shows(SoundBrowserKind::Expression))
                continue;
            const auto entries = expressionsOf(file);
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                if (rows.size() >= limit)
                    return;
                rows.push_back({file, std::to_string(index), entries[index].name, depth + 2, SoundBrowserKind::Expression, false,
                    false});
            }
            continue;
        }
        if (kind == SoundBrowserKind::SoundFile)
        {
            // A Sound file shows while its Sounds do.
            if (!shows(SoundBrowserKind::SoundFile) && !shows(SoundBrowserKind::Sound))
                continue;
            const auto open = open_.contains(file);
            rows.push_back({file, {}, fileName(file), depth + 1, kind, open, false});
            if (openOnly && !open)
                continue;
            if (!shows(SoundBrowserKind::Sound))
                continue;
            for (const auto& sound : soundsOf(file))
            {
                if (rows.size() >= limit)
                    return;
                rows.push_back({file, sound.definitionId, sound.name, depth + 2, SoundBrowserKind::Sound, false, sound.processesInput});
            }
            continue;
        }
        if (shows(kind))
            rows.push_back({file, {}, file.filename().string(), depth + 1, kind, false, false});
    }
}

std::vector<SoundBrowserRow> SoundBrowserModel::rows(std::size_t limit) const
{
    std::vector<SoundBrowserRow> rows;
    for (const auto& root : roots_)
        walk(root, 0, true, limit, rows);
    return rows;
}

std::optional<SoundBrowserRow> SoundBrowserModel::find(
    std::string_view text, const std::optional<SoundBrowserRow>& after, const std::function<bool()>& stopped)
{
    const auto wanted = lowered(std::string{text});
    if (wanted.empty())
        return std::nullopt;
    const auto matches = [&wanted](const SoundBrowserRow& row) { return lowered(row.name).find(wanted) != std::string::npos; };
    // From the row after the selection to the end of the list, then round
    // from the top to the row before it.
    const auto search = [&](const std::vector<SoundBrowserRow>& list) -> std::optional<SoundBrowserRow> {
        auto start = list.begin();
        auto stop = list.begin();
        if (after.has_value())
        {
            const auto found = std::ranges::find_if(list, [&after](const SoundBrowserRow& row) {
                return row.path == after->path && row.definitionId == after->definitionId;
            });
            if (found != list.end())
            {
                start = std::next(found);
                stop = found;
            }
        }
        if (const auto match = std::find_if(start, list.end(), matches); match != list.end())
            return *match;
        if (const auto match = std::find_if(list.begin(), stop, matches); match != stop)
            return *match;
        return std::nullopt;
    };
    // The open folders first, then every folder.
    auto match = search(rows(maximumFindRows));
    if (!match.has_value())
    {
        std::vector<SoundBrowserRow> every;
        for (const auto& root : roots_)
            walk(root, 0, false, maximumFindRows, every, stopped);
        match = search(every);
    }
    if (!match.has_value() || (stopped && stopped()))
        return std::nullopt;
    // The folders and Sound file holding the match open to show it.
    for (auto parent = match->kind == SoundBrowserKind::Sound ? match->path : match->path.parent_path();
         !parent.empty();
         parent = parent.parent_path())
    {
        const auto underRoot = std::ranges::any_of(roots_, [&parent](const std::filesystem::path& root) {
            const auto relative = parent.lexically_relative(root);
            return !relative.empty() && *relative.begin() != "..";
        });
        if (!underRoot)
            break;
        open_.insert(parent);
        if (parent.has_parent_path() && parent.parent_path() == parent)
            break;
    }
    auto result = *match;
    result.open = open_.contains(result.path) && result.kind != SoundBrowserKind::Sound;
    return result;
}

void SoundBrowserModel::refresh()
{
    sounds_.clear();
}

std::optional<ExpressionLibraryEntry> soundBrowserExpression(const SoundBrowserRow& row)
{
    if (row.kind != SoundBrowserKind::Expression)
        return std::nullopt;
    std::size_t index = 0;
    const auto* const end = row.definitionId.data() + row.definitionId.size();
    if (row.definitionId.empty() || std::from_chars(row.definitionId.data(), end, index).ptr != end)
        return std::nullopt;
    const auto entries = expressionsOf(row.path);
    return index < entries.size() ? std::optional{entries[index]} : std::nullopt;
}

std::vector<std::string> soundBrowserInformation(const SoundBrowserRow& row)
{
    if (row.kind == SoundBrowserKind::Folder)
        return {"Folder · " + row.name};
    if (row.kind == SoundBrowserKind::Expression)
    {
        // Its comment, then its code.
        std::vector<std::string> lines{"Expression · " + row.name};
        if (const auto entry = soundBrowserExpression(row); entry.has_value())
        {
            if (!entry->comment.empty())
                lines.push_back(entry->comment);
            lines.push_back(entry->expression);
        }
        return lines;
    }
    if (row.kind == SoundBrowserKind::ExpressionsFile)
        return {"Expressions file · " + row.name};
    if (row.kind == SoundBrowserKind::Sound)
    {
        std::vector<std::string> lines{"Sound · " + row.name};
        const auto document = readSoundFile(row.path);
        if (!document.has_value())
            return lines;
        const auto definition = std::ranges::find(
            document->soundDefinitions, row.definitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document->soundDefinitions.end())
            return lines;
        // Its duration when the samples it plays give one.
        if (const auto seconds = talas::project::soundDefinitionSampleDuration(*document, row.definitionId))
        {
            char duration[64];
            std::snprintf(duration, sizeof duration, "Duration %g s", *seconds);
            lines.emplace_back(duration);
        }
        if (!definition->description.empty())
            lines.push_back(definition->description);
        if (row.processesInput)
            lines.emplace_back("Processes its input");
        if (soundExpectsMidi(*document, row.definitionId))
            lines.emplace_back("MIDI");
        return lines;
    }
    if (row.kind == SoundBrowserKind::SoundFile)
    {
        std::vector<std::string> lines{"Sound file · " + row.name};
        if (const auto document = readSoundFile(row.path))
            lines.push_back(std::to_string(document->soundDefinitions.size()) + " Sounds");
        return lines;
    }
    if (row.kind != SoundBrowserKind::Sample)
        return talasFileInformation(row.path);
    // Its type and name, folder, last change, duration, pitch and
    // samples.
    std::vector<std::string> lines{"Sample · " + row.name};
    lines.push_back("Directory " + talas::project::pathToUtf8(row.path.parent_path()));
    std::error_code error;
    if (const auto modified = std::filesystem::last_write_time(row.path, error); !error)
    {
        const auto seconds = std::chrono::system_clock::to_time_t(
            std::chrono::time_point_cast<std::chrono::system_clock::duration>(std::chrono::file_clock::to_sys(modified)));
        char text[64];
        std::strftime(text, sizeof text, "Modified %Y-%m-%d %H:%M:%S", std::localtime(&seconds));
        lines.emplace_back(text);
    }
    talas::project::ProjectAsset manifest;
    manifest.id = "browser";
    manifest.kind = talas::project::ProjectAssetKind::Audio;
    manifest.path = row.path.string();
    const auto inspected = talas::project::inspectProjectAsset(manifest, row.path);
    if (!inspected.error.has_value() && inspected.asset.sampleRate > 0.0)
    {
        char duration[64];
        std::snprintf(duration, sizeof duration, "Duration %g s",
            static_cast<double>(inspected.asset.frameCount) / inspected.asset.sampleRate);
        lines.emplace_back(duration);
    }
    lines.push_back("Pitch " + std::to_string(samplePitchNoteNumber(row.path)) + " nn");
    if (!inspected.error.has_value() && inspected.asset.sampleRate > 0.0)
    {
        const auto& asset = inspected.asset;
        const auto bits = asset.encoding.find("24") != std::string::npos ? 24
            : asset.encoding.find("32") != std::string::npos           ? 32
            : asset.encoding.find("8") != std::string::npos && asset.encoding.find("16") == std::string::npos ? 8
                                                                                                             : 16;
        char text[160];
        std::snprintf(text, sizeof text, "%llu samples · %.0f Hz · %d bits · %s",
            static_cast<unsigned long long>(asset.frameCount), asset.sampleRate, bits,
            asset.channelCount == 1 ? "mono" : asset.channelCount == 2 ? "stereo" : (std::to_string(asset.channelCount) + " channels").c_str());
        lines.emplace_back(text);
    }
    return lines;
}

bool soundExpectsMidi(const talas::project::ProjectDocument& document, std::string_view definitionId)
{
    std::set<std::string> visited;
    return definitionExpectsMidi(document, definitionId, visited);
}

unsigned samplePitchNoteNumber(const std::filesystem::path& path)
{
    const auto bytes = headBytes(path, 1U << 20U);
    if (bytes.size() < 12 || std::string_view{reinterpret_cast<const char*>(bytes.data()), 4} != "RIFF"
        || std::string_view{reinterpret_cast<const char*>(bytes.data() + 8), 4} != "WAVE")
        return 60;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size())
    {
        const std::string_view id{reinterpret_cast<const char*>(bytes.data() + offset), 4};
        const auto size = littleEndian32(bytes, offset + 4);
        if (id == "smpl" && offset + 8 + 16 <= bytes.size())
        {
            const auto note = littleEndian32(bytes, offset + 8 + 12);
            return note <= 127 ? note : 60;
        }
        offset += 8 + size + (size & 1U);
    }
    return 60;
}

SoundBrowserPlayer soundBrowserPlayer(SoundBrowserKind kind) noexcept
{
    switch (kind)
    {
    case SoundBrowserKind::Sample:
    case SoundBrowserKind::Ga:
        return SoundBrowserPlayer::Sample;
    case SoundBrowserKind::Sound:
        return SoundBrowserPlayer::Sound;
    case SoundBrowserKind::Spectrum:
    case SoundBrowserKind::Re:
    case SoundBrowserKind::Midi:
        return SoundBrowserPlayer::FileSound;
    case SoundBrowserKind::SoundFile:
        return SoundBrowserPlayer::Timeline;
    case SoundBrowserKind::Folder:
    case SoundBrowserKind::ExpressionsFile:
    case SoundBrowserKind::Expression:
        break;
    }
    return SoundBrowserPlayer::None;
}

bool soundBrowserLoops(double seconds, bool once) noexcept
{
    return !once && seconds > 0.0 && seconds < 30.0;
}

std::string soundBrowserInputText(const SoundBrowserReplacementInput& input)
{
    if (input.kind == SoundBrowserInputKind::AudioInput)
        return "Input " + std::to_string(input.audioInput);
    return input.name;
}

SoundBrowserReplacementInput nextAudioInput(SoundBrowserReplacementInput input, unsigned inputCount)
{
    const auto count = std::max(1U, inputCount);
    input.audioInput = input.kind == SoundBrowserInputKind::AudioInput ? input.audioInput % count + 1 : 1;
    input.kind = SoundBrowserInputKind::AudioInput;
    input.path.clear();
    input.definitionId.clear();
    input.name.clear();
    input.connected = true;
    return input;
}

std::optional<SoundBrowserReplacementInput> substituteInput(const SoundBrowserRow& row)
{
    if (row.kind != SoundBrowserKind::Sample && row.kind != SoundBrowserKind::Sound)
        return std::nullopt;
    SoundBrowserReplacementInput input;
    input.kind = row.kind == SoundBrowserKind::Sample ? SoundBrowserInputKind::Sample : SoundBrowserInputKind::Sound;
    input.path = row.path;
    input.definitionId = row.definitionId;
    input.name = row.name;
    input.connected = true;
    return input;
}
}
