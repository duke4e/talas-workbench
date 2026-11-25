#include "talas/app/SoundFileWindowModel.h"

#include "talas/app/GraphBranchLayout.h"
#include "talas/app/PrototypeBrowserModel.h"
#include "talas/project/ProjectValidation.h"
#include "talas/project/ProjectVariables.h"

#include <algorithm>
#include <functional>
#include <iterator>

namespace talas::app
{
std::vector<SoundFileIcon> soundFileIcons(const talas::project::ProjectDocument& document,
    const std::optional<std::string>& collectionId, const std::optional<std::string>& editing)
{
    return soundFileIcons(document, collectionId, editing.has_value() ? std::vector<std::string>{*editing} : std::vector<std::string>{});
}

std::vector<SoundFileIcon> soundFileIcons(const talas::project::ProjectDocument& document,
    const std::optional<std::string>& collectionId, const std::vector<std::string>& editing)
{
    const auto named = [&editing](const talas::project::ProjectSoundDefinition& definition) {
        return SoundFileIcon{definition.definitionId,
            definition.displayName.empty() ? definition.definitionId : definition.displayName, false,
            std::ranges::find(editing, definition.definitionId) != editing.end()};
    };
    std::vector<SoundFileIcon> icons;
    if (collectionId.has_value())
    {
        const auto collection = std::ranges::find(
            document.soundCollections, *collectionId, &talas::project::ProjectSoundCollection::collectionId);
        if (collection == document.soundCollections.end())
            return icons;
        for (const auto& nested : collection->collectionIds)
        {
            const auto found = std::ranges::find(
                document.soundCollections, nested, &talas::project::ProjectSoundCollection::collectionId);
            if (found != document.soundCollections.end())
                icons.push_back({found->collectionId, found->name, true});
        }
        for (const auto& sound : collection->soundIds)
        {
            const auto found = std::ranges::find(document.soundDefinitions, sound, &talas::project::ProjectSoundDefinition::definitionId);
            if (found != document.soundDefinitions.end())
                icons.push_back(named(*found));
        }
        return icons;
    }
    const auto held = [&](const std::string& id, bool sound) {
        return std::ranges::any_of(document.soundCollections, [&](const talas::project::ProjectSoundCollection& collection) {
            const auto& members = sound ? collection.soundIds : collection.collectionIds;
            return std::ranges::find(members, id) != members.end();
        });
    };
    for (const auto& collection : document.soundCollections)
    {
        if (!held(collection.collectionId, false))
            icons.push_back({collection.collectionId, collection.name, true});
    }
    for (const auto& definition : document.soundDefinitions)
    {
        if (!held(definition.definitionId, true))
            icons.push_back(named(definition));
    }
    return icons;
}

std::optional<std::string> soundFileParentCollection(
    const talas::project::ProjectDocument& document, std::string_view collectionId)
{
    for (const auto& collection : document.soundCollections)
    {
        if (std::ranges::find(collection.collectionIds, collectionId) != collection.collectionIds.end())
            return collection.collectionId;
    }
    return std::nullopt;
}

SoundFileDoubleClick soundFileDoubleClick(const SoundFileIcon& icon) noexcept
{
    if (icon.collection)
        return SoundFileDoubleClick::OpenCollection;
    return icon.beingEdited ? SoundFileDoubleClick::BringEditorToFront : SoundFileDoubleClick::EditSound;
}

const talas::project::ProjectGraph* soundEditorSignalFlow(
    const talas::project::ProjectDocument& document, const std::optional<std::string>& editedDefinitionId) noexcept
{
    if (!editedDefinitionId.has_value())
        return &document.graph;
    const auto edited = std::ranges::find(
        document.soundDefinitions, *editedDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
    return edited == document.soundDefinitions.end() ? nullptr : &edited->graph;
}

std::vector<std::string> soundFileClick(std::vector<std::string> selection, std::string_view definitionId, bool shift)
{
    if (!shift)
        return definitionId.empty() ? std::vector<std::string>{} : std::vector<std::string>{std::string{definitionId}};
    if (definitionId.empty())
        return selection;
    if (const auto found = std::ranges::find(selection, definitionId); found != selection.end())
        selection.erase(found);
    else
        selection.emplace_back(definitionId);
    return selection;
}

std::vector<std::string> soundFileBoxSelection(
    std::vector<std::string> selection, const std::vector<std::string>& within, bool shift)
{
    if (!shift)
        return within;
    for (const auto& id : within)
        selection = soundFileClick(std::move(selection), id, true);
    return selection;
}

std::vector<std::string> variablesToAskFor(const talas::project::ProjectDocument& document,
    const std::vector<std::pair<std::string, double>>& environment, VariablePlayOrigin origin)
{
    auto names = talas::project::undeclaredProjectVariables(document);
    if (origin == VariablePlayOrigin::SoundEditor)
        std::erase_if(names, [&](const std::string& name) {
            return std::ranges::any_of(environment, [&](const auto& entry) { return entry.first == name; });
        });
    return names;
}

std::optional<talas::project::ProjectDocument> soundFileWindowMoved(
    talas::project::ProjectDocument document, const std::vector<std::string>& definitionIds, std::string_view before)
{
    auto& definitions = document.soundDefinitions;
    const auto moving = [&](const talas::project::ProjectSoundDefinition& definition) {
        return std::ranges::find(definitionIds, definition.definitionId) != definitionIds.end();
    };
    std::vector<talas::project::ProjectSoundDefinition> moved;
    std::vector<talas::project::ProjectSoundDefinition> staying;
    for (auto& definition : definitions)
        (moving(definition) ? moved : staying).push_back(std::move(definition));
    if (moved.empty())
        return std::nullopt;
    const auto at = std::ranges::find(staying, before, &talas::project::ProjectSoundDefinition::definitionId);
    staying.insert(at, std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
    definitions = std::move(staying);
    return document;
}

std::vector<CustomPrototypeCategory> customPrototypeStrip(const talas::project::ProjectDocument& soundFile)
{
    const auto prototype = [&](const std::string& id) -> std::optional<CustomPrototype> {
        const auto found = std::ranges::find(soundFile.soundDefinitions, id, &talas::project::ProjectSoundDefinition::definitionId);
        if (found == soundFile.soundDefinitions.end())
            return std::nullopt;
        return CustomPrototype{found->definitionId, found->displayName.empty() ? found->definitionId : found->displayName};
    };
    std::vector<CustomPrototypeCategory> strip;
    for (const auto& collection : soundFile.soundCollections)
    {
        CustomPrototypeCategory category{collection.name, {}};
        for (const auto& sound : collection.soundIds)
        {
            if (const auto found = prototype(sound))
                category.sounds.push_back(*found);
        }
        strip.push_back(std::move(category));
    }
    CustomPrototypeCategory general{"General", {}};
    for (const auto& definition : soundFile.soundDefinitions)
    {
        const auto held = std::ranges::any_of(soundFile.soundCollections, [&](const talas::project::ProjectSoundCollection& collection) {
            return std::ranges::find(collection.soundIds, definition.definitionId) != collection.soundIds.end();
        });
        if (!held)
            general.sounds.push_back(*prototype(definition.definitionId));
    }
    if (!general.sounds.empty())
        strip.push_back(std::move(general));
    return strip;
}

std::optional<talas::project::ProjectSoundDefinition> soundsInTurn(
    const talas::project::ProjectDocument& document, const std::vector<std::string>& selected)
{
    std::vector<const talas::project::ProjectSoundDefinition*> sounds;
    const auto addSound = [&](std::string_view id) {
        const auto found = std::ranges::find(document.soundDefinitions, id, &talas::project::ProjectSoundDefinition::definitionId);
        if (found != document.soundDefinitions.end() && std::ranges::find(sounds, &*found) == sounds.end())
            sounds.push_back(&*found);
    };
    std::vector<std::string> opened;
    const std::function<void(std::string_view)> add = [&](std::string_view id) {
        const auto collection = std::ranges::find(document.soundCollections, id, &talas::project::ProjectSoundCollection::collectionId);
        if (collection == document.soundCollections.end())
        {
            addSound(id);
            return;
        }
        if (std::ranges::find(opened, id) != opened.end())
            return;
        opened.emplace_back(id);
        for (const auto& inner : collection->collectionIds)
            add(inner);
        for (const auto& sound : collection->soundIds)
            addSound(sound);
    };
    for (const auto& id : selected)
        add(id);
    if (sounds.size() < 2)
        return std::nullopt;
    talas::project::ProjectSoundDefinition turn;
    turn.definitionId = numberedCopyName("soundsInTurn", [&](std::string_view id) {
        return std::ranges::any_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& other) {
            return other.definitionId == id;
        });
    });
    turn.displayName = "Sounds in turn";
    turn.graph.nodes.push_back({"inTurn", "talas.concatenation", {}});
    for (std::size_t index = 0; index < sounds.size(); ++index)
    {
        const auto output = std::ranges::find(sounds[index]->outputs, talas::core::PortType::Audio, &talas::project::ProjectSoundPort::type);
        if (output == sounds[index]->outputs.end())
            continue;
        const auto instance = "sound" + std::to_string(index + 1);
        turn.graph.nodes.push_back({instance, {}, {}, sounds[index]->definitionId});
        turn.graph.connections.push_back({instance, output->portId, "inTurn", "inputs"});
    }
    if (turn.graph.connections.size() < 2)
        return std::nullopt;
    turn.outputs = {{"audioOut", talas::core::PortType::Audio, "inTurn", "leftOut"}};
    return turn;
}

std::vector<std::string> soundFileSelectionInOrder(
    const std::vector<SoundFileIcon>& icons, const std::vector<std::string>& selection)
{
    std::vector<std::string> ordered;
    for (const auto& icon : icons)
    {
        if (std::ranges::find(selection, icon.definitionId) != selection.end())
            ordered.push_back(icon.definitionId);
    }
    return ordered;
}

namespace
{
constexpr std::string_view signalFlowSoundDragPrefix = "talas.signalFlowSound:";
}

std::string signalFlowSoundDragDescription(const std::optional<std::string>& scope, std::string_view nodeId)
{
    return std::string{signalFlowSoundDragPrefix} + (scope.has_value() ? "1" + *scope : std::string{"0"}) + '\n'
        + std::string{nodeId};
}

std::optional<SignalFlowSoundDrag> signalFlowSoundDrag(std::string_view description)
{
    if (!description.starts_with(signalFlowSoundDragPrefix))
        return std::nullopt;
    const auto rest = description.substr(signalFlowSoundDragPrefix.size());
    const auto newline = rest.find('\n');
    if (newline == std::string_view::npos || newline == 0 || newline + 1 == rest.size()
        || (rest.front() != '0' && rest.front() != '1') || (rest.front() == '0' && newline != 1)
        || (rest.front() == '1' && newline == 1))
        return std::nullopt;
    SignalFlowSoundDrag drag;
    if (rest.front() == '1')
        drag.scope = std::string{rest.substr(1, newline - 1)};
    drag.nodeId = std::string{rest.substr(newline + 1)};
    return drag;
}

talas::project::ProjectGraphFragment soundFileWindowCopied(const std::vector<std::string>& definitionIds)
{
    talas::project::ProjectGraphFragment fragment;
    for (std::size_t index = 0; index < definitionIds.size(); ++index)
    {
        talas::project::ProjectNode node;
        node.instanceId = definitionIds[index];
        node.soundDefinitionId = definitionIds[index];
        fragment.nodes.push_back(std::move(node));
        fragment.positions.push_back({static_cast<std::int32_t>(index) * 200, 0});
    }
    return fragment;
}

std::optional<talas::project::ProjectGraphFragment> prototypeCopied(const talas::core::NodeRegistry& registry, std::string_view typeId)
{
    if (registry.findNodeType(typeId) == nullptr)
        return std::nullopt;
    const auto dot = typeId.rfind('.');
    talas::project::ProjectGraphFragment fragment;
    fragment.nodes.push_back({std::string{dot == std::string_view::npos ? typeId : typeId.substr(dot + 1)}, std::string{typeId}, {}});
    fragment.positions.push_back({0, 0});
    return fragment;
}

std::vector<std::string> soundsOfCopy(const talas::project::ProjectDocument& document, const talas::project::ProjectGraphFragment& fragment)
{
    std::vector<std::string> sounds;
    for (const auto& node : fragment.nodes)
    {
        if (!node.soundDefinitionId.has_value()
            || std::ranges::none_of(document.soundDefinitions, [&](const auto& definition) {
                   return definition.definitionId == *node.soundDefinitionId;
               }))
            return {};
        if (std::ranges::find(sounds, *node.soundDefinitionId) == sounds.end())
            sounds.push_back(*node.soundDefinitionId);
    }
    return sounds;
}

std::optional<SignalFlowSoundCopy> soundCopiedFromFragment(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const talas::project::ProjectGraphFragment& fragment)
{
    // The copy as a signal flow of its own, beside the project's Sounds.
    auto copied = document;
    copied.graph = {fragment.nodes, fragment.connections};
    for (auto node = fragment.nodes.rbegin(); node != fragment.nodes.rend(); ++node)
    {
        const auto feeds = std::ranges::any_of(fragment.connections, [&](const talas::core::Connection& line) {
            return line.fromNode == node->instanceId;
        });
        if (feeds)
            continue;
        if (auto sound = soundCopiedFromSignalFlow(copied, registry, {std::nullopt, node->instanceId}))
            return sound;
    }
    return std::nullopt;
}

std::optional<talas::project::ProjectDocument> soundSharedInPlaceOf(const talas::project::ProjectDocument& original,
    const talas::core::NodeRegistry& registry, const SignalFlowSoundDrag& drag, std::string_view replacedId)
{
    if (drag.nodeId == replacedId)
        return std::nullopt;
    auto document = original;
    auto* graph = &document.graph;
    talas::project::ProjectSoundDefinition* holder = nullptr;
    if (drag.scope.has_value())
    {
        const auto found = std::ranges::find(document.soundDefinitions, *drag.scope, &talas::project::ProjectSoundDefinition::definitionId);
        if (found == document.soundDefinitions.end())
            return std::nullopt;
        holder = &*found;
        graph = &holder->graph;
    }
    const auto shared = std::ranges::find(graph->nodes, drag.nodeId, &talas::project::ProjectNode::instanceId);
    if (shared == graph->nodes.end()
        || std::ranges::find(graph->nodes, replacedId, &talas::project::ProjectNode::instanceId) == graph->nodes.end())
        return std::nullopt;
    // The Sound's output: the one it already feeds by, else its first audio
    // output.
    std::string output;
    if (const auto feeding = std::ranges::find(graph->connections, drag.nodeId, &talas::core::Connection::fromNode);
        feeding != graph->connections.end())
        output = feeding->fromPort;
    else if (const auto* descriptor = registry.findNodeType(shared->typeId))
    {
        const auto audio = std::ranges::find(descriptor->outputs, talas::core::PortType::Audio, &talas::core::PortDescriptor::type);
        if (audio != descriptor->outputs.end())
            output = audio->id;
    }
    if (output.empty())
        return std::nullopt;
    const auto reached = [&](std::string_view from, bool downstream) {
        std::vector<std::string> found{std::string{from}};
        for (std::size_t index = 0; index < found.size(); ++index)
        {
            for (const auto& line : graph->connections)
            {
                const auto& near = downstream ? line.fromNode : line.toNode;
                const auto& far = downstream ? line.toNode : line.fromNode;
                if (near == found[index] && std::ranges::find(found, far) == found.end())
                    found.push_back(far);
            }
        }
        return found;
    };
    if (const auto fed = reached(replacedId, true); std::ranges::find(fed, drag.nodeId) != fed.end())
        return std::nullopt;
    const auto feeders = reached(replacedId, false);
    std::vector<std::string> removed{std::string{replacedId}};
    for (auto& line : graph->connections)
    {
        if (line.fromNode == replacedId)
        {
            line.fromNode = drag.nodeId;
            line.fromPort = output;
        }
    }
    std::erase_if(graph->connections, [&](const talas::core::Connection& line) { return line.toNode == replacedId; });
    // The Sounds that fed only the one replaced go with it.
    for (auto pruned = true; pruned;)
    {
        pruned = false;
        for (const auto& feeder : feeders)
        {
            if (feeder == replacedId || feeder == drag.nodeId || std::ranges::find(removed, feeder) != removed.end()
                || std::ranges::any_of(graph->connections, [&](const talas::core::Connection& line) { return line.fromNode == feeder; })
                || (holder != nullptr && std::ranges::find(holder->outputs, feeder, &talas::project::ProjectSoundPort::nodeId) != holder->outputs.end()))
                continue;
            removed.push_back(feeder);
            std::erase_if(graph->connections, [&](const talas::core::Connection& line) { return line.toNode == feeder; });
            pruned = true;
        }
    }
    std::erase_if(graph->nodes, [&](const talas::project::ProjectNode& node) {
        return std::ranges::find(removed, node.instanceId) != removed.end();
    });
    if (holder != nullptr)
    {
        if (std::ranges::any_of(holder->inputs, [&](const talas::project::ProjectSoundPort& input) {
                return std::ranges::find(removed, input.nodeId) != removed.end();
            }))
            return std::nullopt;
        for (auto& sound : holder->outputs)
        {
            if (sound.nodeId == replacedId)
            {
                sound.nodeId = drag.nodeId;
                sound.nodePortId = output;
            }
        }
    }
    talas::project::reconcileProjectPresentation(document);
    return document;
}

std::optional<std::string> parameterValueOfDraggedSound(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const SignalFlowSoundDrag& drag, std::string_view parameterId)
{
    const auto* graph = &document.graph;
    if (drag.scope.has_value())
    {
        const auto found = std::ranges::find(document.soundDefinitions, *drag.scope, &talas::project::ProjectSoundDefinition::definitionId);
        if (found == document.soundDefinitions.end())
            return std::nullopt;
        graph = &found->graph;
    }
    const auto node = std::ranges::find(graph->nodes, drag.nodeId, &talas::project::ProjectNode::instanceId);
    const auto* descriptor = node == graph->nodes.end() ? nullptr : registry.findNodeType(node->typeId);
    if (descriptor == nullptr)
        return std::nullopt;
    const auto parameter = std::ranges::find(descriptor->parameters, parameterId, &talas::core::ParameterDescriptor::id);
    if (parameter == descriptor->parameters.end() || parameter->kind != talas::core::ParameterKind::Expression)
        return std::nullopt;
    const auto set = std::ranges::find(node->parameters, parameterId, &talas::project::ProjectParameterExpression::parameterId);
    return set != node->parameters.end() && set->expression.has_value() ? *set->expression : parameter->defaultExpression;
}

std::optional<SignalFlowSoundCopy> soundCopiedFromSignalFlow(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, const SignalFlowSoundDrag& drag)
{
    const talas::project::ProjectSoundDefinition* holder = nullptr;
    if (drag.scope.has_value())
    {
        const auto found = std::ranges::find(document.soundDefinitions, *drag.scope, &talas::project::ProjectSoundDefinition::definitionId);
        if (found == document.soundDefinitions.end())
            return std::nullopt;
        holder = &*found;
    }
    const auto& graph = holder != nullptr ? holder->graph : document.graph;
    const auto node = std::ranges::find(graph.nodes, drag.nodeId, &talas::project::ProjectNode::instanceId);
    if (node == graph.nodes.end())
        return std::nullopt;
    // The Sound and every Sound feeding it.
    std::vector<std::string> held{drag.nodeId};
    for (std::size_t index = 0; index < held.size(); ++index)
    {
        for (const auto& line : graph.connections)
        {
            if (line.toNode == held[index] && std::ranges::find(held, line.fromNode) == held.end())
                held.push_back(line.fromNode);
        }
    }
    const auto isHeld = [&](std::string_view id) { return std::ranges::find(held, id) != held.end(); };
    SignalFlowSoundCopy copy;
    auto& sound = copy.definition;
    const auto taken = [&](std::string_view id) {
        return std::ranges::any_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& other) {
            return other.definitionId == id || other.displayName == id;
        });
    };
    sound.definitionId = taken(drag.nodeId) ? numberedCopyName(drag.nodeId, taken) : drag.nodeId;
    sound.displayName = sound.definitionId;
    for (const auto& member : graph.nodes)
    {
        if (isHeld(member.instanceId))
            sound.graph.nodes.push_back(member);
    }
    for (const auto& line : graph.connections)
    {
        if (isHeld(line.fromNode) && isHeld(line.toNode))
            sound.graph.connections.push_back(line);
    }
    if (holder != nullptr)
    {
        for (const auto& input : holder->inputs)
        {
            if (isHeld(input.nodeId))
                sound.inputs.push_back(input);
        }
    }
    if (node->soundDefinitionId.has_value())
    {
        const auto inner = std::ranges::find(document.soundDefinitions, *node->soundDefinitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (inner != document.soundDefinitions.end())
        {
            for (const auto& output : inner->outputs)
            {
                if (output.type == talas::core::PortType::Audio)
                    sound.outputs.push_back({output.portId, talas::core::PortType::Audio, drag.nodeId, output.portId});
            }
        }
    }
    else if (const auto* type = registry.findNodeType(node->typeId))
    {
        for (const auto& output : type->outputs)
        {
            if (output.type == talas::core::PortType::Audio)
                sound.outputs.push_back({output.id, talas::core::PortType::Audio, drag.nodeId, output.id});
        }
    }
    if (sound.outputs.empty())
        return std::nullopt;
    copy.layout.soundDefinitionId = sound.definitionId;
    if (const auto* layout = talas::project::findProjectStructureLayout(document, drag.scope))
    {
        for (const auto& placed : layout->nodes)
        {
            if (isHeld(placed.nodeId))
                copy.layout.nodes.push_back(placed);
        }
    }
    return copy;
}

std::vector<std::string> soundRemovalChoices(std::size_t pending)
{
    if (pending > 1)
        return {"Yes", "Remove all", "Cancel"};
    return {"Yes", "Cancel"};
}

std::string soundRemovalQuestion(std::string_view name, const std::vector<std::string>& inputs)
{
    if (inputs.empty())
        return "Remove " + std::string{name} + "?";
    if (inputs.size() == 1)
        return "Replace " + std::string{name} + " with its input " + inputs.front() + "?";
    return "Replace " + std::string{name} + " with one of its inputs?";
}

std::optional<SoundRemovalAnswer> soundRemovalAnswer(std::string_view choice) noexcept
{
    if (choice == "Yes")
        return SoundRemovalAnswer::Yes;
    if (choice == "Remove all")
        return SoundRemovalAnswer::RemoveAll;
    if (choice == "Cancel")
        return SoundRemovalAnswer::Cancel;
    return std::nullopt;
}

SoundRemoval soundRemovalAnswered(SoundRemoval removal, SoundRemovalAnswer answer)
{
    if (removal.pending.empty())
        return removal;
    switch (answer)
    {
    case SoundRemovalAnswer::Yes:
        removal.removed.push_back(removal.pending.front());
        removal.pending.erase(removal.pending.begin());
        break;
    case SoundRemovalAnswer::RemoveAll:
        removal.removed.insert(removal.removed.end(), removal.pending.begin(), removal.pending.end());
        removal.pending.clear();
        break;
    case SoundRemovalAnswer::Cancel:
        removal.pending.clear();
        break;
    }
    return removal;
}

std::size_t soundFileColumns(int width, int cell) noexcept
{
    if (cell <= 0 || width < cell)
        return 1;
    return static_cast<std::size_t>(width / cell);
}

SoundFileCell soundFileCell(std::size_t index, std::size_t columns) noexcept
{
    columns = std::max<std::size_t>(columns, 1);
    return {index % columns, index / columns};
}

std::optional<talas::project::ProjectSoundDefinition> soundForPrototype(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view typeId, std::map<std::string, unsigned>* copyNumbers)
{
    // AR Drag&Drop is a Sound of its own too, its VCA
    // shaping a Noise as a prototype's input, so that it plays and opens from
    // the Prototypes.
    if (isPrototypeAssembly(typeId))
    {
        const auto descriptor = prototypeAssemblyDescriptor(registry, typeId);
        auto assembly = prototypeAssembly(registry, typeId, [](std::string_view name) { return name == "noise"; });
        if (!descriptor.has_value() || !assembly.has_value())
            return std::nullopt;
        talas::project::ProjectSoundDefinition sound;
        const auto stem = std::string{"arDragDrop"};
        auto number = 0U;
        if (copyNumbers != nullptr)
        {
            const auto last = copyNumbers->find(std::string{typeId});
            number = (last == copyNumbers->end() ? 0U : last->second) + 1;
        }
        const auto taken = [&](std::string_view id, std::string_view name) {
            return std::ranges::any_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& other) {
                return (!id.empty() && other.definitionId == id) || (!name.empty() && other.displayName == name);
            });
        };
        if (copyNumbers == nullptr)
        {
            sound.definitionId = numberedCopyName(stem, [&](std::string_view id) { return taken(id, {}); });
            sound.displayName = numberedCopyName(descriptor->displayName, [&](std::string_view name) { return taken({}, name); });
        }
        else
        {
            while (taken(stem + std::to_string(number), descriptor->displayName + std::to_string(number)))
                ++number;
            sound.definitionId = stem + std::to_string(number);
            sound.displayName = descriptor->displayName + std::to_string(number);
            (*copyNumbers)[std::string{typeId}] = number;
        }
        const auto head = assembly->head.instanceId;
        sound.graph.connections = {{"noise", "audioOut", head, assembly->headInput},
            {assembly->source.instanceId, assembly->sourceOutput, head, assembly->headControl}};
        sound.graph.nodes = {std::move(assembly->head), std::move(assembly->source), {"noise", "talas.noise", {}}};
        sound.outputs.push_back({"audioOut", talas::core::PortType::Audio, head, "audioOut"});
        return sound;
    }
    const auto* type = registry.findNodeType(typeId);
    if (type == nullptr)
        return std::nullopt;
    const auto dot = typeId.rfind('.');
    const auto stem = std::string{dot == std::string_view::npos ? typeId : typeId.substr(dot + 1)};
    talas::project::ProjectSoundDefinition sound;
    auto number = 0U;
    if (copyNumbers == nullptr)
    {
        sound.definitionId = numberedCopyName(stem, [&](std::string_view id) {
            return std::ranges::any_of(document.soundDefinitions,
                [&](const talas::project::ProjectSoundDefinition& other) { return other.definitionId == id; });
        });
        sound.displayName = numberedCopyName(type->displayName, [&](std::string_view name) {
            return std::ranges::any_of(document.soundDefinitions,
                [&](const talas::project::ProjectSoundDefinition& other) { return other.displayName == name; });
        });
    }
    else
    {
        // On from the number the prototype last gave a copy.
        const auto last = copyNumbers->find(std::string{typeId});
        number = (last == copyNumbers->end() ? 0U : last->second) + 1;
        while (std::ranges::any_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& other) {
            return other.definitionId == stem + std::to_string(number)
                || other.displayName == type->displayName + std::to_string(number);
        }))
            ++number;
        sound.definitionId = stem + std::to_string(number);
        sound.displayName = type->displayName + std::to_string(number);
    }
    sound.graph.nodes.push_back({stem, std::string{typeId}, {}});
    // Its required audio inputs take noise, so that the prototype comes
    // with an input of its own; one of another kind makes no Sound.
    for (const auto& input : type->inputs)
    {
        if (!input.required || input.compilerPopulated)
            continue;
        if (input.type != talas::core::PortType::Audio)
            return std::nullopt;
        const auto source = stem == "noise" ? std::string{"input"} : std::string{"noise"};
        if (std::ranges::find(sound.graph.nodes, source, &talas::project::ProjectNode::instanceId) == sound.graph.nodes.end())
            sound.graph.nodes.push_back({source, "talas.noise", {}});
        sound.graph.connections.push_back({source, "audioOut", stem, input.id});
    }
    for (const auto& output : type->outputs)
    {
        if (output.type == talas::core::PortType::Audio)
            sound.outputs.push_back({output.id, talas::core::PortType::Audio, stem, output.id});
    }
    // A Sound that plays without an audio output, as
    // a SoundToGlobalController or a MIDI output Sound does, is a Sound of
    // its own too, so that a double-click in the Prototypes opens it with
    // its fields to write in (the controller's Value takes a Val
    // program); any other prototype without one makes none.
    if (sound.outputs.empty() && !talas::project::isNoAudioTerminalNodeType(typeId))
        return std::nullopt;
    if (copyNumbers != nullptr)
        (*copyNumbers)[std::string{typeId}] = number;
    return sound;
}

namespace
{
[[nodiscard]] const talas::project::ProjectSoundDefinition* soundOf(
    const talas::project::ProjectDocument& document, std::string_view definitionId)
{
    const auto found = std::ranges::find(document.soundDefinitions, definitionId, &talas::project::ProjectSoundDefinition::definitionId);
    return found == document.soundDefinitions.end() ? nullptr : &*found;
}

[[nodiscard]] OpenSoundEditor* editorOn(SoundEditorWindows& windows, std::string_view definitionId)
{
    const auto found = std::ranges::find(windows.editors, definitionId, &OpenSoundEditor::definitionId);
    return found == windows.editors.end() ? nullptr : &*found;
}

// FNV-1a, 64 bits, over the kind of window and its name.
[[nodiscard]] std::uint64_t windowNumber(std::string_view kind, std::string_view name) noexcept
{
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](std::string_view part) {
        for (const auto character : part)
        {
            hash ^= static_cast<unsigned char>(character);
            hash *= 1099511628211ULL;
        }
    };
    mix(kind);
    mix(std::string_view{"/"});
    mix(name);
    return hash == 0 ? 1 : hash;
}

// A variable's name is a letter, then letters and
// digits.
[[nodiscard]] bool variableName(std::string_view name) noexcept
{
    const auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return !name.empty() && letter(name.front())
        && std::ranges::all_of(name, [&](char c) { return letter(c) || digit(c); });
}

[[nodiscard]] bool isSoundVariable(const talas::project::ProjectNode& node) noexcept
{
    return !node.soundDefinitionId.has_value()
        && (node.typeId == "talas.soundCollectionVariable" || node.typeId == "talas.variable");
}

[[nodiscard]] std::optional<std::string> variableIdOf(const talas::project::ProjectNode& node)
{
    const auto named = std::ranges::find(node.parameters, std::string_view{"variableId"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (named == node.parameters.end() || !named->text.has_value() || named->text->empty())
        return std::nullopt;
    return *named->text;
}

void nameVariable(talas::project::ProjectNode& node, std::string_view name)
{
    const auto found = std::ranges::find(node.parameters, std::string_view{"variableId"},
        &talas::project::ProjectParameterExpression::parameterId);
    if (found != node.parameters.end())
    {
        found->text = std::string{name};
        return;
    }
    talas::project::ProjectParameterExpression named;
    named.parameterId = "variableId";
    named.text = std::string{name};
    node.parameters.push_back(std::move(named));
}
}

SoundEditorOpening openSoundEditor(
    SoundEditorWindows& windows, const talas::project::ProjectDocument& document, std::string_view definitionId)
{
    const auto* sound = soundOf(document, definitionId);
    if (sound == nullptr)
        return SoundEditorOpening::Refused;
    windows.front = std::string{definitionId};
    if (editorOn(windows, definitionId) != nullptr)
        return SoundEditorOpening::BroughtToFront;
    windows.editors.push_back({std::string{definitionId}, {std::string{definitionId}}, *sound});
    return SoundEditorOpening::Opened;
}

const OpenSoundEditor* soundEditorOn(const SoundEditorWindows& windows, std::string_view definitionId) noexcept
{
    const auto found = std::ranges::find(windows.editors, definitionId, &OpenSoundEditor::definitionId);
    return found == windows.editors.end() ? nullptr : &*found;
}

const OpenSoundEditor* frontSoundEditor(const SoundEditorWindows& windows) noexcept
{
    return windows.front.has_value() ? soundEditorOn(windows, *windows.front) : nullptr;
}

void closeSoundEditor(SoundEditorWindows& windows, std::string_view definitionId)
{
    std::erase_if(windows.editors, [&](const OpenSoundEditor& editor) { return editor.definitionId == definitionId; });
    if (windows.front == definitionId)
        windows.front.reset();
}

void followSoundEditorPath(
    SoundEditorWindows& windows, const talas::project::ProjectDocument& document, const std::vector<std::string>& path)
{
    if (path.empty() || openSoundEditor(windows, document, path.front()) == SoundEditorOpening::Refused)
    {
        windows.front.reset();
        return;
    }
    editorOn(windows, path.front())->path = path;
}

void reconcileSoundEditors(SoundEditorWindows& windows, const talas::project::ProjectDocument& document)
{
    std::erase_if(windows.editors,
        [&](const OpenSoundEditor& editor) { return soundOf(document, editor.definitionId) == nullptr; });
    for (auto& editor : windows.editors)
    {
        const auto gone = std::ranges::find_if(
            editor.path, [&](const std::string& definitionId) { return soundOf(document, definitionId) == nullptr; });
        editor.path.erase(gone, editor.path.end());
        if (editor.path.empty())
            editor.path = {editor.definitionId};
    }
    if (windows.front.has_value() && soundEditorOn(windows, *windows.front) == nullptr)
        windows.front.reset();
}

void soundEditorsOnNewDocument(SoundEditorWindows& windows, const talas::project::ProjectDocument& document)
{
    std::erase_if(windows.editors, [&](const OpenSoundEditor& editor) { return windows.front != editor.definitionId; });
    reconcileSoundEditors(windows, document);
    soundEditorsSaved(windows, document);
}

void soundEditorsSaved(SoundEditorWindows& windows, const talas::project::ProjectDocument& document)
{
    for (auto& editor : windows.editors)
    {
        if (const auto* sound = soundOf(document, editor.definitionId))
            editor.opened = *sound;
    }
}

std::vector<std::string> soundsBeingEdited(const SoundEditorWindows& windows)
{
    std::vector<std::string> sounds;
    sounds.reserve(windows.editors.size());
    for (const auto& editor : windows.editors)
        sounds.push_back(editor.definitionId);
    return sounds;
}

std::string soundEditorTitle(const talas::project::ProjectDocument& document, const OpenSoundEditor& editor)
{
    const auto* sound = soundOf(document, editor.definitionId);
    return sound == nullptr || sound->displayName.empty() ? editor.definitionId : sound->displayName;
}

std::uint64_t soundEditorHistoryWindow() noexcept
{
    return windowNumber("sound editors", {});
}

std::uint64_t namedHistoryWindow(std::string_view windowName) noexcept
{
    return windowNumber("window", windowName);
}

const talas::project::ProjectNode* soundFileVariable(const talas::project::ProjectDocument& document, std::string_view definitionId)
{
    const auto* sound = soundOf(document, definitionId);
    if (sound == nullptr || sound->graph.nodes.size() != 1 || !isSoundVariable(sound->graph.nodes.front()))
        return nullptr;
    return &sound->graph.nodes.front();
}

std::optional<talas::project::ProjectSoundDefinition> soundFileVariableRenamed(
    const talas::project::ProjectDocument& document, std::string_view definitionId, std::string_view name)
{
    if (soundFileVariable(document, definitionId) == nullptr || !variableName(name))
        return std::nullopt;
    auto renamed = *soundOf(document, definitionId);
    renamed.displayName = std::string{name};
    nameVariable(renamed.graph.nodes.front(), name);
    return renamed;
}

std::optional<talas::project::ProjectNode> soundFileSoundForInput(const talas::project::ProjectDocument& document,
    std::optional<std::string_view> scope, std::string_view definitionId)
{
    const auto* sound = soundOf(document, definitionId);
    if (sound == nullptr)
        return std::nullopt;
    const talas::project::ProjectGraph* graph = &document.graph;
    if (scope.has_value())
    {
        const auto* holder = soundOf(document, *scope);
        if (holder == nullptr)
            return std::nullopt;
        graph = &holder->graph;
    }
    const auto taken = [&](std::string_view id) {
        return std::ranges::find(graph->nodes, id, &talas::project::ProjectNode::instanceId) != graph->nodes.end();
    };
    const auto free = [&](const std::string& base) { return taken(base) ? numberedCopyName(base, taken) : base; };
    if (const auto* variable = soundFileVariable(document, definitionId))
    {
        auto node = *variable;
        const auto name = variableIdOf(node).value_or(sound->displayName.empty() ? sound->definitionId : sound->displayName);
        nameVariable(node, name);
        node.instanceId = free(name);
        return node;
    }
    talas::project::ProjectNode node;
    node.instanceId = free(sound->definitionId);
    node.soundDefinitionId = sound->definitionId;
    return node;
}
}
