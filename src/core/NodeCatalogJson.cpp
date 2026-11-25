#include "talas/core/NodeCatalogJson.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace talas::core
{
namespace
{
void appendIndent(std::string& output, int spaces)
{
    output.append(static_cast<std::size_t>(spaces), ' ');
}

char hexDigit(std::uint8_t value)
{
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + (value - 10));
}

void appendQuoted(std::string& output, std::string_view value)
{
    output.push_back('"');
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (character < 0x20)
            {
                output += "\\u00";
                output.push_back(hexDigit(static_cast<std::uint8_t>(character >> 4u)));
                output.push_back(hexDigit(static_cast<std::uint8_t>(character & 0x0fu)));
            }
            else
            {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

void appendStringField(std::string& output, int indent, std::string_view key, std::string_view value, bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    appendQuoted(output, value);
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

void appendUnsignedField(
    std::string& output,
    int indent,
    std::string_view key,
    std::size_t value,
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

std::string_view portTypeName(PortType type)
{
    switch (type)
    {
    case PortType::Audio:
        return "audio";
    case PortType::Control:
        return "control";
    case PortType::Event:
        return "event";
    case PortType::SampleBuffer:
        return "sampleBuffer";
    case PortType::Wavetable:
        return "wavetable";
    case PortType::Spectrum:
        return "spectrum";
    case PortType::ComplexSpectrum:
        return "complexSpectrum";
    case PortType::NoteStream:
        return "noteStream";
    }
    return "unknown";
}

std::string_view parameterKindName(ParameterKind kind)
{
    switch (kind)
    {
    case ParameterKind::Expression:
        return "expression";
    case ParameterKind::Choice:
        return "choice";
    case ParameterKind::Boolean:
        return "boolean";
    case ParameterKind::Resource:
        return "resource";
    case ParameterKind::Text:
        return "text";
    }
    return "unknown";
}

std::string_view valueDomainName(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
        return "scalar";
    case talas::expr::ValueDomain::Hertz:
        return "hertz";
    case talas::expr::ValueDomain::Seconds:
        return "seconds";
    case talas::expr::ValueDomain::NoteNumber:
        return "noteNumber";
    case talas::expr::ValueDomain::Samples:
        return "samples";
    }
    return "unknown";
}

void appendPorts(
    std::string& output,
    int indent,
    std::string_view key,
    const std::vector<PortDescriptor>& ports,
    bool trailingComma)
{
    std::vector<const PortDescriptor*> presentedPorts;
    presentedPorts.reserve(ports.size());
    for (const auto& port : ports)
    {
        if (!port.compilerPopulated)
            presentedPorts.push_back(&port);
    }
    appendIndent(output, indent);
    appendQuoted(output, key);
    output += ": ";
    if (presentedPorts.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < presentedPorts.size(); ++index)
    {
        const auto& port = *presentedPorts[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendStringField(output, indent + 4, "id", port.id, true);
        appendStringField(output, indent + 4, "type", portTypeName(port.type), true);
        appendBooleanField(output, indent + 4, "required", port.required, true);
        appendStringField(
            output,
            indent + 4,
            "rate",
            portSignalRateName(port.signal.rate),
            true);
        appendStringField(
            output,
            indent + 4,
            "audioLayout",
            audioBusLayoutKindName(port.signal.audioLayout),
            true);
        appendStringField(
            output,
            indent + 4,
            "edge",
            portEdgeSemanticsName(port.signal.edge),
            true);
        appendBooleanField(
            output,
            indent + 4,
            "timelineBearing",
            port.signal.timelineBearing,
            true);
        if (port.acceptsMany)
            appendBooleanField(output, indent + 4, "acceptsMany", true, true);
        appendStringField(
            output,
            indent + 4,
            "spectralFormat",
            spectralFrameFormatName(port.signal.spectralFormat),
            false);
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < presentedPorts.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendChoices(std::string& output, int indent, const std::vector<std::string>& choices, bool trailingComma)
{
    appendIndent(output, indent);
    appendQuoted(output, "choices");
    output += ": [\n";
    for (std::size_t index = 0; index < choices.size(); ++index)
    {
        appendIndent(output, indent + 2);
        appendQuoted(output, choices[index]);
        if (index + 1 < choices.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendParameterKindDefault(std::string& output, int indent, const ParameterDescriptor& parameter)
{
    switch (parameter.kind)
    {
    case ParameterKind::Expression:
        appendStringField(output, indent, "defaultExpression", parameter.defaultExpression, false);
        return;
    case ParameterKind::Choice:
        appendStringField(output, indent, "defaultChoice", parameter.defaultChoice, true);
        appendChoices(output, indent, parameter.choices, false);
        return;
    case ParameterKind::Boolean:
        appendBooleanField(output, indent, "defaultBoolean", parameter.defaultBoolean.value_or(false), false);
        return;
    case ParameterKind::Resource:
        appendStringField(output, indent, "defaultResource", parameter.defaultResource, false);
        return;
    case ParameterKind::Text:
        appendStringField(output, indent, "defaultText", parameter.defaultText, false);
        return;
    }
}

void appendParameters(std::string& output, int indent, const std::vector<ParameterDescriptor>& parameters)
{
    appendIndent(output, indent);
    output += "\"parameters\": ";
    if (parameters.empty())
    {
        output += "[]\n";
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < parameters.size(); ++index)
    {
        const auto& parameter = parameters[index];
        appendIndent(output, indent + 2);
        output += "{\n";
        appendStringField(output, indent + 4, "id", parameter.id, true);
        appendStringField(output, indent + 4, "kind", parameterKindName(parameter.kind), true);
        appendStringField(output, indent + 4, "domain", valueDomainName(parameter.domain), true);
        appendParameterKindDefault(output, indent + 4, parameter);
        appendIndent(output, indent + 2);
        output.push_back('}');
        if (index + 1 < parameters.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output += "]\n";
}

void appendLimitations(
    std::string& output,
    int indent,
    const std::vector<std::string>& limitations)
{
    appendIndent(output, indent);
    output += "\"limitations\": ";
    if (limitations.empty())
    {
        output += "[]\n";
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < limitations.size(); ++index)
    {
        appendIndent(output, indent + 2);
        appendQuoted(output, limitations[index]);
        if (index + 1 < limitations.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output += "]\n";
}

void appendOutputBusLayouts(
    std::string& output,
    int indent,
    const std::vector<AudioBusLayoutKind>& layouts,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += "\"outputBusLayouts\": ";
    if (layouts.empty())
    {
        output += "[]";
        if (trailingComma)
            output.push_back(',');
        output.push_back('\n');
        return;
    }

    output += "[\n";
    for (std::size_t index = 0; index < layouts.size(); ++index)
    {
        appendIndent(output, indent + 2);
        appendQuoted(output, audioBusLayoutKindName(layouts[index]));
        if (index + 1 < layouts.size())
            output.push_back(',');
        output.push_back('\n');
    }
    appendIndent(output, indent);
    output.push_back(']');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendCapabilities(
    std::string& output,
    int indent,
    const NodeCapabilityDescriptor& capabilities,
    bool trailingComma)
{
    appendIndent(output, indent);
    output += "\"capabilities\": {\n";
    appendStringField(
        output,
        indent + 2,
        "offline",
        executionSupportName(capabilities.offline),
        true);
    appendStringField(
        output,
        indent + 2,
        "offlineBinding",
        executionBindingName(capabilities.offlineBinding),
        true);
    appendStringField(
        output,
        indent + 2,
        "preparedRealtime",
        executionSupportName(capabilities.preparedRealtime),
        true);
    appendStringField(
        output,
        indent + 2,
        "preparedRealtimeBinding",
        executionBindingName(capabilities.preparedRealtimeBinding),
        true);
    appendStringField(
        output,
        indent + 2,
        "nestedSounds",
        nestedSoundSupportName(capabilities.nestedSounds),
        true);
    appendStringField(
        output,
        indent + 2,
        "state",
        nodeStateModelName(capabilities.state),
        true);
    appendBooleanField(
        output,
        indent + 2,
        "supportsReset",
        capabilities.supportsReset,
        true);
    appendBooleanField(
        output,
        indent + 2,
        "typedEvents",
        capabilities.typedEvents,
        true);
    appendBooleanField(
        output,
        indent + 2,
        "transportAware",
        capabilities.transportAware,
        true);
    appendIndent(output, indent + 2);
    output += "\"latency\": {\n";
    appendStringField(
        output,
        indent + 4,
        "kind",
        latencyKindName(capabilities.latency.kind),
        true);
    appendUnsignedField(
        output,
        indent + 4,
        "fixedFrames",
        capabilities.latency.fixedFrames,
        true);
    appendStringField(
        output,
        indent + 4,
        "parameterId",
        capabilities.latency.parameterId,
        true);
    appendStringField(
        output,
        indent + 4,
        "preparedRule",
        preparedLatencyRuleName(capabilities.latency.preparedRule),
        false);
    appendIndent(output, indent + 2);
    output += "},\n";
    appendStringField(
        output,
        indent + 2,
        "resourceAccess",
        resourceAccessModeName(capabilities.resourceAccess),
        true);
    appendStringField(
        output,
        indent + 2,
        "fanInLatencyPolicy",
        fanInLatencyPolicyName(capabilities.fanInLatencyPolicy),
        true);
    appendOutputBusLayouts(
        output,
        indent + 2,
        capabilities.outputBusLayouts,
        true);
    appendLimitations(
        output,
        indent + 2,
        capabilities.limitations);
    appendIndent(output, indent);
    output.push_back('}');
    if (trailingComma)
        output.push_back(',');
    output.push_back('\n');
}

void appendNode(std::string& output, const NodeTypeDescriptor& node)
{
    appendIndent(output, 4);
    output += "{\n";
    appendStringField(output, 6, "typeId", node.typeId, true);
    appendStringField(output, 6, "displayName", node.displayName, true);
    appendStringField(output, 6, "category", node.category, true);
    appendStringField(
        output,
        6,
        "vendor",
        node.provenance.vendor,
        true);
    appendStringField(
        output,
        6,
        "origin",
        nodeOriginName(node.provenance.origin),
        true);
    appendStringField(output, 6, "supportLevel", nodeSupportLevelName(node.supportLevel), true);
    appendStringField(output, 6, "supportSummary", node.supportSummary, true);
    if (!node.replacedBy.empty())
        appendStringField(output, 6, "replacedBy", node.replacedBy, true);
    appendCapabilities(output, 6, node.capabilities, true);
    appendPorts(output, 6, "inputs", node.inputs, true);
    appendPorts(output, 6, "outputs", node.outputs, true);
    appendParameters(output, 6, node.parameters);
    appendIndent(output, 4);
    output.push_back('}');
}
}

std::string serializeNodeCatalog(const NodeRegistry& registry)
{
    const auto nodes = registry.nodeTypes();
    std::string output;
    output += "{\n";
    output += "  \"nodes\": ";
    if (nodes.empty())
    {
        output += "[]\n";
        output += "}\n";
        return output;
    }

    output += "[\n";
    for (std::size_t index = 0; index < nodes.size(); ++index)
    {
        appendNode(output, nodes[index]);
        if (index + 1 < nodes.size())
            output.push_back(',');
        output.push_back('\n');
    }
    output += "  ]\n";
    output += "}\n";
    return output;
}
}
