#include "talas/project/ProjectLocalMapControlDiscovery.h"

#include "talas/project/ProjectMidiMapperMap.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <algorithm>
#include <cmath>
#include <ranges>
#include <string_view>
#include <utility>

namespace talas::project
{
ProjectLocalMapControlDiscoveryResult discoverProjectLocalMapControls(
    const ProjectDocument& document,
    const talas::core::NodeRegistry& registry)
{
    ProjectLocalMapControlDiscoveryResult result;
    const auto expanded = expandProjectSounds(document, registry);
    if (!expanded.ok())
    {
        for (const auto& error : expanded.validation.errors)
        {
            result.diagnostics.push_back({error.nodeId, error.message});
        }
        return result;
    }

    for (const auto& node : expanded.document.graph.nodes)
    {
        if (node.typeId != "talas.midiMapper")
            continue;
        const auto map = std::ranges::find(
            node.parameters,
            std::string_view{"map"},
            &ProjectParameterExpression::parameterId);
        if (map == node.parameters.end() || !map->text.has_value())
            continue;
        const auto parsed = parseProjectMidiMapperMap(*map->text);
        if (!parsed.ok())
        {
            result.diagnostics.push_back({
                node.instanceId,
                "MIDIMapper Map cannot be projected into local controls"});
            continue;
        }
        const auto identity = std::ranges::find(
            expanded.manifest.nodes,
            node.instanceId,
            &ExpandedSoundNodeIdentity::flatNodeId);
        if (identity == expanded.manifest.nodes.end())
        {
            result.diagnostics.push_back({
                node.instanceId,
                "Expanded MIDIMapper instance identity is missing"});
            continue;
        }
        for (std::size_t index = 0;
             index < parsed.localAssociationCount; ++index)
        {
            const auto& association = parsed.localAssociations[index];
            ProjectLocalMapControl control;
            control.flatMapperNodeId = node.instanceId;
            control.mapperInstancePath = identity->instancePath;
            control.mapperDefinitionId = identity->definitionId;
            control.mapperLocalNodeId = identity->localNodeId;
            control.eventName = association.localEventValue;
            control.sourceName = association.localEventSource;
            control.channel = association.channel;
            control.displayAs = association.displayAs;
            control.taper = association.taper;
            const auto virtualControl = std::ranges::find(
                document.virtualControls,
                control.eventName,
                &VirtualControl::eventName);
            if (virtualControl != document.virtualControls.end())
                control.domain = virtualControl->domain;
            if (association.hasRange())
            {
                std::array<double, 2> range{};
                if (!parseProjectMidiMapperMapBound(
                        association.minimumText, range[0])
                    || !parseProjectMidiMapperMapBound(
                        association.maximumText, range[1])
                    || !std::isfinite(range[1] - range[0]))
                {
                    result.diagnostics.push_back({
                        node.instanceId,
                        "MIDIMapper Map range cannot be projected into a local control"});
                    continue;
                }
                control.range = range;
            }
            if (association.hasGrid())
            {
                double step = 0.0;
                if (!parseProjectMidiMapperMapBound(
                        association.gridText, step))
                {
                    result.diagnostics.push_back({
                        node.instanceId,
                        "MIDIMapper Map grid cannot be projected into a local control"});
                    continue;
                }
                control.grid = step;
            }
            result.controls.push_back(std::move(control));
        }
    }
    return result;
}
}
