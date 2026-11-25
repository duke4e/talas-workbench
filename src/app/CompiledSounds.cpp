#include "talas/app/CompiledSounds.h"
#include "talas/app/SoundBrowserAudition.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectAssetPaths.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <charconv>
#include <set>
#include <variant>

namespace talas::app
{
namespace
{
constexpr std::string_view header = "talas.compiled-sound 1";

std::string_view restOfLine(std::string_view& text)
{
    const auto end = text.find('\n');
    const auto line = text.substr(0, end);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    return line;
}

std::optional<std::string> field(std::string_view& text, std::string_view name)
{
    const auto line = restOfLine(text);
    if (!line.starts_with(name) || line.size() <= name.size() || line[name.size()] != ' ')
        return std::nullopt;
    return std::string{line.substr(name.size() + 1)};
}

// The Sounds the program plays: the root graph's and those they use.
std::set<std::string> soundsUsed(const talas::project::ProjectDocument& document)
{
    std::set<std::string> used;
    std::vector<const talas::project::ProjectGraph*> graphs{&document.graph};
    while (!graphs.empty())
    {
        const auto* graph = graphs.back();
        graphs.pop_back();
        for (const auto& node : graph->nodes)
        {
            if (!node.soundDefinitionId.has_value() || !used.insert(*node.soundDefinitionId).second)
                continue;
            const auto definition = std::ranges::find(document.soundDefinitions, *node.soundDefinitionId,
                &talas::project::ProjectSoundDefinition::definitionId);
            if (definition != document.soundDefinitions.end())
                graphs.push_back(&definition->graph);
        }
    }
    return used;
}

// The files the program reads: those its Sounds name, the sample maps' and
// the edited spectra's.
std::set<std::string> filesUsed(const talas::project::ProjectDocument& document, const std::set<std::string>& sounds)
{
    std::set<std::string> used;
    const auto add = [&](const talas::project::ProjectGraph& graph) {
        for (const auto& node : graph.nodes)
            for (const auto& parameter : node.parameters)
                if (parameter.resource.has_value())
                    used.insert(*parameter.resource);
    };
    add(document.graph);
    for (const auto& definition : document.soundDefinitions)
        if (sounds.contains(definition.definitionId))
            add(definition.graph);
    for (const auto& map : document.sampleMapLibrary.maps)
        for (const auto& region : map.regions)
            used.insert(region.assetId);
    for (const auto& edits : document.partialTrackEdits)
        used.insert(edits.assetId);
    return used;
}

// What a compiled Sound plays: its program without the Sounds and files it
// does not use, its layout or where its files are, which differ between
// the project and a compiled copy where the contents do not.
talas::project::ProjectDocument programOf(talas::project::ProjectDocument document)
{
    const auto sounds = soundsUsed(document);
    const auto files = filesUsed(document, sounds);
    std::erase_if(document.soundDefinitions, [&](const auto& definition) {
        return !sounds.contains(definition.definitionId);
    });
    std::erase_if(document.assets, [&](const auto& asset) { return !files.contains(asset.id); });
    for (auto& asset : document.assets)
        asset.path.clear();
    document.presentation = {};
    document.metadata = {};
    return document;
}
}

CompiledSoundResult compileSound(const talas::project::ProjectDocument& document,
    std::string_view soundDefinitionId, const std::optional<std::filesystem::path>& projectFile,
    CompiledSoundTarget target, std::optional<std::string_view> sourceInstanceId)
{
    const auto definition = std::ranges::find(document.soundDefinitions, soundDefinitionId,
        &talas::project::ProjectSoundDefinition::definitionId);
    if (definition == document.soundDefinitions.end())
        return {std::nullopt, "The project has no Sound " + std::string{soundDefinitionId}};
    auto prepared = talas::project::projectSoundPreparationDocument(document, soundDefinitionId, sourceInstanceId);
    if (!prepared.ok())
        return {std::nullopt, prepared.error.empty() ? "The Sound has no output to compile" : prepared.error};
    // The files it reads, as they are on the disk now.
    for (auto& asset : prepared.document.assets)
    {
        const auto path = talas::project::resolveProjectAssetPath(asset, projectFile);
        if (path.empty())
            continue;
        asset.path = talas::project::pathToUtf8(path);
        if (const auto hash = talas::project::calculateProjectAssetSha256(path); hash.ok())
            asset.contentHash = hash.contentHash;
    }
    CompiledSound compiled{std::string{soundDefinitionId}, definition->displayName, std::move(target),
        std::move(prepared.document)};
    if (compiled.name.empty())
        compiled.name = compiled.soundDefinitionId;
    return {std::move(compiled), {}};
}

std::vector<CompiledSoundFileCopy> withFileCopies(CompiledSound& compiled, const std::filesystem::path& compiledFile)
{
    const auto folder = compiledFile.parent_path() / (talas::project::pathToUtf8(compiledFile.stem()) + " files");
    std::vector<CompiledSoundFileCopy> copies;
    std::set<std::filesystem::path> taken;
    const auto files = filesUsed(compiled.document, soundsUsed(compiled.document));
    for (auto& asset : compiled.document.assets)
    {
        if (asset.path.empty() || !files.contains(asset.id))
            continue;
        const auto from = talas::project::pathFromUtf8(asset.path);
        if (const auto copied = std::ranges::find(copies, from, &CompiledSoundFileCopy::from); copied != copies.end())
        {
            asset.path = talas::project::pathToUtf8(copied->to);
            continue;
        }
        auto to = folder / from.filename();
        for (int number = 2; taken.contains(to); ++number)
            to = folder / (talas::project::pathToUtf8(from.stem()) + " " + std::to_string(number)
                + talas::project::pathToUtf8(from.extension()));
        taken.insert(to);
        copies.push_back({from, to});
        asset.path = talas::project::pathToUtf8(to);
    }
    return copies;
}

std::string writeCompiledSound(const CompiledSound& compiled)
{
    std::string text{header};
    text += "\nsound " + compiled.soundDefinitionId;
    text += "\nname " + compiled.name;
    text += "\nformat " + std::to_string(compiled.target.projectFormat);
    text += "\napplication " + compiled.target.application;
    text += "\nsystem " + compiled.target.system;
    text += "\nprogram\n";
    text += talas::project::serializeProjectDocument(compiled.document);
    return text;
}

std::optional<CompiledSound> readCompiledSound(std::string_view text)
{
    if (restOfLine(text) != header)
        return std::nullopt;
    CompiledSound compiled;
    const auto sound = field(text, "sound");
    const auto name = field(text, "name");
    const auto format = field(text, "format");
    const auto application = field(text, "application");
    const auto system = field(text, "system");
    if (!sound || !name || !format || !application || !system || restOfLine(text) != "program")
        return std::nullopt;
    const auto [end, failed] = std::from_chars(format->data(), format->data() + format->size(),
        compiled.target.projectFormat);
    if (failed != std::errc{} || end != format->data() + format->size())
        return std::nullopt;
    auto program = talas::project::parseProjectDocument(text);
    if (!program.ok())
        return std::nullopt;
    compiled.soundDefinitionId = *sound;
    compiled.name = *name;
    compiled.target.application = *application;
    compiled.target.system = *system;
    compiled.document = std::move(program.document);
    return compiled;
}

bool compiledSoundCurrent(const CompiledSound& compiled, const CompiledSound& now)
{
    return compiled.soundDefinitionId == now.soundDefinitionId && compiled.target == now.target
        && talas::project::serializeProjectDocument(programOf(compiled.document))
        == talas::project::serializeProjectDocument(programOf(now.document));
}

std::filesystem::path compiledGridSoundPath(const std::filesystem::path& folder,
    const talas::project::ProjectLiveSet& set, const talas::project::ProjectLiveSetCell& cell)
{
    return folder / set.liveSetId / (cell.cellId + compiledSoundExtension);
}

std::vector<std::string> gridCellsToCompile(const talas::project::ProjectLiveSet& set,
    const std::map<std::string, bool>& currentByCell, bool allSounds)
{
    std::vector<std::string> cells;
    for (const auto& cell : set.cells)
    {
        if (!std::holds_alternative<talas::project::ProjectLiveSetSoundTarget>(cell.target))
            continue;
        const auto current = currentByCell.find(cell.cellId);
        if (allSounds || current == currentByCell.end() || !current->second)
            cells.push_back(cell.cellId);
    }
    return cells;
}

std::filesystem::path compiledSoundGridFileName(std::string_view gridName)
{
    return talas::project::pathFromUtf8(std::string{gridName} + compiledSoundGridExtension);
}

std::optional<talas::project::ProjectDocument> compiledSoundGridFile(
    const talas::project::ProjectDocument& project, std::string_view liveSetId)
{
    const auto& sets = project.liveSetLibrary.liveSets;
    const auto set = std::ranges::find(sets, liveSetId, &talas::project::ProjectLiveSet::liveSetId);
    if (set == sets.end())
        return std::nullopt;
    auto file = project;
    file.liveSetLibrary.liveSets = {*set};
    file.liveSetLibrary.tools.clear();
    return file;
}

CompiledSoundGridOpened withCompiledSoundGridFile(const talas::project::ProjectDocument& project,
    const talas::project::ProjectDocument& gridFile, const talas::core::NodeRegistry& registry)
{
    CompiledSoundGridOpened opened;
    if (gridFile.liveSetLibrary.liveSets.size() != 1)
    {
        opened.diagnostic = "A Compiled Sound Grid file holds one grid";
        return opened;
    }
    auto document = project;
    auto set = gridFile.liveSetLibrary.liveSets.front();
    // Each Sound a box plays comes in once, as the Sound Browser brings one.
    std::map<std::string, std::string> imported;
    for (auto& cell : set.cells)
    {
        auto* sound = std::get_if<talas::project::ProjectLiveSetSoundTarget>(&cell.target);
        if (sound == nullptr)
            continue;
        auto found = imported.find(sound->soundDefinitionId);
        if (found == imported.end())
        {
            const auto brought = soundBrowserImport(document, gridFile, sound->soundDefinitionId, {}, std::nullopt, registry);
            if (!brought.ok())
            {
                opened.diagnostic = brought.diagnostic;
                return opened;
            }
            document = *brought.document;
            found = imported.emplace(sound->soundDefinitionId, brought.rootDefinitionId).first;
        }
        sound->soundDefinitionId = found->second;
    }
    auto& sets = document.liveSetLibrary.liveSets;
    const auto taken = [&](const std::string& id) {
        return std::ranges::find(sets, id, &talas::project::ProjectLiveSet::liveSetId) != sets.end();
    };
    const auto base = set.liveSetId;
    for (int suffix = 2; taken(set.liveSetId); ++suffix)
        set.liveSetId = base + std::to_string(suffix);
    opened.liveSetId = set.liveSetId;
    sets.push_back(std::move(set));
    opened.document = std::move(document);
    return opened;
}

std::optional<ToolGridBox> toolGridBox(const talas::project::ProjectDocument& document, std::string_view gridFile,
    std::size_t number, std::string_view name)
{
    const auto& sets = document.liveSetLibrary.liveSets;
    const auto stem = talas::project::pathToUtf8(talas::project::pathFromUtf8(std::string{gridFile}).stem());
    auto set = std::ranges::find_if(sets, [&](const auto& candidate) {
        return candidate.displayName == stem || candidate.liveSetId == stem;
    });
    if (set == sets.end() && sets.size() == 1)
        set = sets.begin();
    if (set == sets.end())
        return std::nullopt;
    for (const auto& cell : set->cells)
    {
        const auto* sound = std::get_if<talas::project::ProjectLiveSetSoundTarget>(&cell.target);
        if (sound == nullptr)
            continue;
        const auto program = static_cast<std::size_t>(cell.row) * talas::project::projectLiveSetColumns + cell.column;
        const auto definition = std::ranges::find(document.soundDefinitions, sound->soundDefinitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        const auto named = !name.empty()
            && (cell.displayName == name
                || (definition != document.soundDefinitions.end()
                    && (definition->displayName == name || definition->definitionId == name)));
        if ((number != 0 && program + 1 == number) || named)
            return ToolGridBox{&*set, &cell};
    }
    return std::nullopt;
}
}
