#include "talas/app/MemoryWarnings.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <map>
#include <set>
#include <string_view>

namespace talas::app
{
namespace
{
const talas::project::ProjectParameterExpression* parameterOf(
    const talas::project::ProjectNode& node, std::string_view parameterId)
{
    const auto found = std::ranges::find(node.parameters, parameterId,
        &talas::project::ProjectParameterExpression::parameterId);
    return found == node.parameters.end() ? nullptr : &*found;
}

// The parameter a reader names its memory by, or none for a Sound that
// reads no MemoryWriter.
std::string_view readerResourceParameter(std::string_view typeId)
{
    if (typeId == "talas.sample" || typeId == "talas.sampleCloud"
        || typeId == "talas.timeFrequencyScale")
        return "sample";
    if (typeId == "talas.functionGenerator" || typeId == "talas.triggeredTableRead")
        return "wavetable";
    if (typeId == "talas.waveshaper")
        return "shapingFunction";
    return {};
}

std::optional<double> constantOf(std::string_view expression)
{
    while (!expression.empty() && expression.front() == ' ')
        expression.remove_prefix(1);
    for (const std::string_view unit : {" ms", " s", "ms", "s"})
    {
        if (expression.ends_with(unit))
        {
            expression.remove_suffix(unit.size());
            break;
        }
    }
    while (!expression.empty() && expression.back() == ' ')
        expression.remove_suffix(1);
    double value = 0.0;
    const auto* end = expression.data() + expression.size();
    const auto [parsed, error] = std::from_chars(expression.data(), end, value);
    if (error != std::errc{} || parsed != end)
        return std::nullopt;
    return value;
}

bool hasInputOn(const talas::project::ProjectGraph& graph,
    const std::string& nodeId, std::string_view portId)
{
    return std::ranges::any_of(graph.connections, [&](const auto& connection) {
        return connection.toNode == nodeId && connection.toPort == portId;
    });
}

// A reader waits for its Gate or Trigger when either is fed by a Sound or is
// not a constant.
bool waitsForGateOrTrigger(const talas::project::ProjectGraph& graph,
    const talas::project::ProjectNode& node)
{
    for (const std::string_view portId : {"gate", "trigger"})
    {
        if (hasInputOn(graph, node.instanceId, portId))
            return true;
        const auto* parameter = parameterOf(node, portId);
        if (parameter != nullptr && parameter->expression.has_value()
            && !constantOf(*parameter->expression).has_value())
            return true;
    }
    return false;
}

// A TimeOffset with an offset, or a SetDuration with a StartTime, starts
// what it is fed late.
bool startsLate(const talas::project::ProjectGraph& graph,
    const talas::project::ProjectNode& node)
{
    const std::string_view parameterId = node.typeId == "talas.timeOffset" ? "offset"
        : node.typeId == "talas.setDuration" ? "startTime" : "";
    if (parameterId.empty())
        return false;
    if (hasInputOn(graph, node.instanceId, parameterId))
        return true;
    const auto* parameter = parameterOf(node, parameterId);
    if (parameter == nullptr || !parameter->expression.has_value())
        return false;
    const auto value = constantOf(*parameter->expression);
    return !value.has_value() || *value > 0.0;
}
}

std::vector<MemoryWarning> memoryWarnings(const talas::project::ProjectGraph& graph)
{
    std::map<std::string, std::string> recordingByWriter;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId != "talas.memoryWriter")
            continue;
        const auto* name = parameterOf(node, "recordingName");
        if (name != nullptr && name->resource.has_value() && !name->resource->empty())
            recordingByWriter.emplace(node.instanceId, *name->resource);
    }
    if (recordingByWriter.empty())
        return {};

    const auto nodeById = [&](const std::string& id) -> const talas::project::ProjectNode* {
        const auto found = std::ranges::find(graph.nodes, id, &talas::project::ProjectNode::instanceId);
        return found == graph.nodes.end() ? nullptr : &*found;
    };
    // Whether some path from the node to the output starts it on time.
    std::map<std::string, bool> onTime;
    std::set<std::string> visiting;
    std::function<bool(const std::string&)> reachesOutputOnTime = [&](const std::string& id) {
        if (const auto known = onTime.find(id); known != onTime.end())
            return known->second;
        if (!visiting.insert(id).second)
            return false;
        bool feedsSomething = false;
        bool result = false;
        for (const auto& connection : graph.connections)
        {
            if (connection.fromNode != id)
                continue;
            feedsSomething = true;
            const auto* next = nodeById(connection.toNode);
            if (next == nullptr || startsLate(graph, *next))
                continue;
            if (reachesOutputOnTime(connection.toNode))
            {
                result = true;
                break;
            }
        }
        visiting.erase(id);
        if (!feedsSomething)
            result = true;
        onTime[id] = result;
        return result;
    };

    std::set<std::string> readRecordings;
    std::vector<MemoryWarning> warnings;
    for (const auto& node : graph.nodes)
    {
        const auto parameterId = readerResourceParameter(node.typeId);
        if (parameterId.empty())
            continue;
        const auto* fromWriter = parameterOf(node, "fromMemoryWriter");
        const auto* name = parameterOf(node, parameterId);
        if (fromWriter == nullptr || fromWriter->boolean != true
            || name == nullptr || !name->resource.has_value())
            continue;
        const auto written = std::ranges::any_of(recordingByWriter,
            [&](const auto& writer) { return writer.second == *name->resource; });
        if (!written)
            continue;
        readRecordings.insert(*name->resource);
        if (waitsForGateOrTrigger(graph, node) || !reachesOutputOnTime(node.instanceId))
            continue;
        warnings.push_back({MemoryWarningKind::ReadBeforeWritten, node.instanceId, *name->resource,
            node.instanceId + " reads " + *name->resource
                + " before anything is written there; put a TimeOffset on it"});
    }
    for (const auto& node : graph.nodes)
    {
        const auto writer = recordingByWriter.find(node.instanceId);
        if (writer == recordingByWriter.end() || readRecordings.contains(writer->second))
            continue;
        warnings.push_back({MemoryWarningKind::WrittenNeverRead, node.instanceId, writer->second,
            node.instanceId + " writes " + writer->second + " but no Sound reads it"});
    }
    return warnings;
}

std::string memoryWarningsStatus(const talas::project::ProjectGraph& graph)
{
    const auto warnings = memoryWarnings(graph);
    if (warnings.empty())
        return {};
    auto status = " - warning: " + warnings.front().message;
    if (warnings.size() > 1)
        status += " (and " + std::to_string(warnings.size() - 1) + " more)";
    return status;
}
}
