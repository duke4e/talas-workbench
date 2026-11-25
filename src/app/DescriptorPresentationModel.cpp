#include "talas/app/DescriptorPresentationModel.h"

#include "talas/app/ClassEditorModel.h"

#include <algorithm>
#include <cctype>
#include <ranges>

namespace talas::app
{
namespace
{
std::string executionSupportLabel(talas::core::ExecutionSupport support)
{
    switch (support)
    {
    case talas::core::ExecutionSupport::Unsupported:
        return "Unsupported";
    case talas::core::ExecutionSupport::Subset:
        return "Subset";
    case talas::core::ExecutionSupport::Supported:
        return "Supported";
    }
    return "Unsupported";
}

std::string limitationLabel(std::string_view limitation)
{
    if (limitation == "offline-functional-subset")
        return "Offline behavior is a functional subset";
    if (limitation == "offline-compatibility-placeholder")
        return "Offline behavior is compatibility-only";
    if (limitation == "prepared-realtime-subset")
        return "Prepared realtime behavior is a subset";
    if (limitation == "prepared-realtime-unsupported")
        return "Prepared realtime is unavailable";
    return std::string{limitation};
}

std::string yesNo(bool value)
{
    return value ? "yes" : "no";
}

bool executableAdapter(const talas::core::NodeTypeDescriptor& descriptor)
{
    return descriptor.supportLevel != talas::core::NodeSupportLevel::Placeholder
        && descriptor.replacedBy.empty()
        && (descriptor.capabilities.offline
                != talas::core::ExecutionSupport::Unsupported
            || descriptor.capabilities.preparedRealtime
                != talas::core::ExecutionSupport::Unsupported);
}

DescriptorPortPresentation portPresentation(talas::core::PortType type) noexcept
{
    switch (type)
    {
    case talas::core::PortType::Audio:
        return {
            DescriptorWireColourRole::Audio,
            DescriptorPortShape::Circle,
            "A",
        };
    case talas::core::PortType::Control:
        return {
            DescriptorWireColourRole::Control,
            DescriptorPortShape::Square,
            "C",
        };
    case talas::core::PortType::Event:
    case talas::core::PortType::NoteStream:
        return {
            DescriptorWireColourRole::Event,
            DescriptorPortShape::Diamond,
            "E",
        };
    case talas::core::PortType::SampleBuffer:
    case talas::core::PortType::Wavetable:
    case talas::core::PortType::Spectrum:
    case talas::core::PortType::ComplexSpectrum:
        return {
            DescriptorWireColourRole::Data,
            DescriptorPortShape::Hexagon,
            "D",
        };
    }
    return {};
}
}

std::string descriptorPortTypeName(talas::core::PortType type)
{
    switch (type)
    {
    case talas::core::PortType::Audio:
        return "audio";
    case talas::core::PortType::Control:
        return "control";
    case talas::core::PortType::Event:
        return "event";
    case talas::core::PortType::SampleBuffer:
        return "sample buffer";
    case talas::core::PortType::Wavetable:
        return "wavetable";
    case talas::core::PortType::Spectrum:
        return "spectrum";
    case talas::core::PortType::ComplexSpectrum:
        return "complex spectrum";
    case talas::core::PortType::NoteStream:
        return "note stream";
    }
    return "unknown";
}

std::string descriptorParameterKindName(talas::core::ParameterKind kind)
{
    switch (kind)
    {
    case talas::core::ParameterKind::Expression:
        return "expression";
    case talas::core::ParameterKind::Choice:
        return "choice";
    case talas::core::ParameterKind::Boolean:
        return "boolean";
    case talas::core::ParameterKind::Resource:
        return "resource";
    case talas::core::ParameterKind::Text:
        return "text";
    }
    return "unknown";
}

std::string descriptorValueDomainName(talas::expr::ValueDomain domain)
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
        return "note number";
    case talas::expr::ValueDomain::Samples:
        return "samples";
    }
    return "unknown";
}

std::string descriptorOriginLabel(talas::core::NodeOrigin origin)
{
    switch (origin)
    {
    case talas::core::NodeOrigin::BuiltIn:
        return "Built-in";
    case talas::core::NodeOrigin::Project:
        return "Project";
    case talas::core::NodeOrigin::PreservedComponent:
        return "Preserved component";
    case talas::core::NodeOrigin::IsolatedComponent:
        return "Isolated component";
    case talas::core::NodeOrigin::External:
        return "External";
    }
    return "Unknown";
}

DescriptorWirePresentation descriptorWirePresentation(
    const talas::core::PortDescriptor& source,
    DescriptorConnectionEmphasis emphasis)
{
    DescriptorWirePresentation result;
    const auto port = portPresentation(source.type);
    result.colour = port.colour;
    result.terminalShape = port.shape;
    switch (source.signal.rate)
    {
    case talas::core::PortSignalRate::Sample:
        result.pattern = DescriptorWirePattern::Solid;
        break;
    case talas::core::PortSignalRate::Constant:
        result.pattern = DescriptorWirePattern::LongDash;
        break;
    case talas::core::PortSignalRate::Block:
        result.pattern = DescriptorWirePattern::ShortDash;
        break;
    case talas::core::PortSignalRate::Event:
        result.pattern = DescriptorWirePattern::Dotted;
        break;
    case talas::core::PortSignalRate::Frame:
        result.pattern = DescriptorWirePattern::DashDot;
        break;
    case talas::core::PortSignalRate::VariableOrUnknown:
        result.pattern = DescriptorWirePattern::SparseDotted;
        break;
    }
    switch (source.signal.edge)
    {
    case talas::core::PortEdgeSemantics::Gate:
        result.marker = DescriptorWireMarker::Gate;
        break;
    case talas::core::PortEdgeSemantics::Trigger:
        result.marker = DescriptorWireMarker::Trigger;
        break;
    case talas::core::PortEdgeSemantics::Continuous:
    case talas::core::PortEdgeSemantics::NotApplicable:
        result.marker = DescriptorWireMarker::None;
        break;
    }
    switch (emphasis)
    {
    case DescriptorConnectionEmphasis::Normal:
        break;
    case DescriptorConnectionEmphasis::SelectedIncident:
        result.width = 3.25F;
        result.outlineWidth = 2.0F;
        result.opacity = 1.0F;
        break;
    case DescriptorConnectionEmphasis::ContextDimmed:
        result.width = 1.5F;
        result.opacity = 0.34F;
        break;
    }
    return result;
}

DescriptorPortPresentation descriptorPortPresentation(
    const talas::core::PortDescriptor& port) noexcept
{
    return portPresentation(port.type);
}

std::string descriptorPortInfoHint(
    const talas::core::PortDescriptor& port,
    bool output)
{
    std::string result = output ? "Output " : "Input ";
    result += port.id;
    result += ": ";
    result += descriptorPortTypeName(port.type);
    result += output ? ", producer" : (port.required ? ", required" : ", optional");
    result += ", rate ";
    result += talas::core::portSignalRateName(port.signal.rate);
    if (port.signal.audioLayout != talas::core::AudioBusLayoutKind::NotApplicable)
    {
        result += ", layout ";
        result += talas::core::audioBusLayoutKindName(port.signal.audioLayout);
    }
    if (port.signal.edge != talas::core::PortEdgeSemantics::NotApplicable)
    {
        result += ", edge ";
        result += talas::core::portEdgeSemanticsName(port.signal.edge);
    }
    if (port.signal.spectralFormat
        != talas::core::SpectralFrameFormat::NotApplicable)
    {
        result += ", format ";
        result += talas::core::spectralFrameFormatName(
            port.signal.spectralFormat);
    }
    result += ", timeline-bearing ";
    result += yesNo(port.signal.timelineBearing);
    return result;
}

std::string prototypeParameterHelp(std::string_view typeId, std::string_view parameterId)
{
    if (typeId == "talas.noise" && parameterId == "type")
        return "white: every value equally likely, the same energy in every band of frequencies of the same width; "
               "pink: the same energy in every octave, so it sounds darker than white; "
               "hotPink: each value most likely a small step from the one before, a large step less likely";
    return {};
}

std::string descriptorParameterInfoHint(
    const talas::core::ParameterDescriptor& parameter)
{
    std::string result = parameter.id + ": ";
    result += descriptorParameterKindName(parameter.kind);
    result += ", domain ";
    result += descriptorValueDomainName(parameter.domain);
    switch (parameter.kind)
    {
    case talas::core::ParameterKind::Expression:
        result += ", default ";
        result += parameter.defaultExpression.empty()
            ? "empty expression"
            : parameter.defaultExpression;
        break;
    case talas::core::ParameterKind::Choice:
        result += ", default ";
        result += parameter.defaultChoice;
        result += ", choices ";
        for (std::size_t index = 0; index < parameter.choices.size(); ++index)
        {
            if (index != 0)
                result += ", ";
            result += parameter.choices[index];
        }
        break;
    case talas::core::ParameterKind::Boolean:
        result += parameter.defaultBoolean.value_or(false)
            ? ", default enabled"
            : ", default disabled";
        break;
    case talas::core::ParameterKind::Resource:
        result += ", project resource reference";
        if (!parameter.defaultResource.empty())
            result += ", default " + parameter.defaultResource;
        break;
    case talas::core::ParameterKind::Text:
        result += ", literal text";
        if (!parameter.defaultText.empty())
            result += ", default " + parameter.defaultText;
        break;
    }
    // The values a field of that name
    // takes, as its parameter type gives them (Scale1 a scale).
    auto name = parameter.id;
    while (!name.empty() && std::isdigit(static_cast<unsigned char>(name.back())) != 0)
        name.pop_back();
    const auto* type = findTalasParameterType(name);
    if (type == nullptr && !name.empty())
        type = findTalasParameterType("hot" + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name.front()))))
            + name.substr(1));
    if (type != nullptr && type->kind == parameter.kind && !type->takes.empty())
        result += ", takes " + std::string{type->takes};
    return result;
}

std::string descriptorSoundClassHelp(
    const talas::core::NodeTypeDescriptor& descriptor)
{
    std::string result;
    for (const auto& span : descriptorSoundClassHelpSpans(descriptor))
        result += span.text;
    return result;
}

std::vector<SoundClassHelpSpan> descriptorSoundClassHelpSpans(
    const talas::core::NodeTypeDescriptor& descriptor, const std::function<bool(std::string_view)>& constantField)
{
    std::string text = descriptor.displayName;
    if (!descriptor.category.empty())
        text += " (" + descriptor.category + ")";
    text += "\n" + descriptor.supportSummary;
    if (!descriptor.inputs.empty())
    {
        text += "\n\nInputs";
        for (const auto& port : descriptor.inputs)
            text += "\n" + descriptorPortInfoHint(port, false);
    }
    if (!descriptor.outputs.empty())
    {
        text += "\n\nOutputs";
        for (const auto& port : descriptor.outputs)
            text += "\n" + descriptorPortInfoHint(port, true);
    }
    std::vector<SoundClassHelpSpan> spans;
    if (!descriptor.parameters.empty())
        text += "\n\nParameters";
    for (const auto& parameter : descriptor.parameters)
    {
        // Each parameter's line starts with its name.
        const auto hint = descriptorParameterInfoHint(parameter);
        const auto hot = parameter.kind == talas::core::ParameterKind::Expression
            && !(constantField && constantField(parameter.id));
        text += "\n";
        if (!hot || !hint.starts_with(parameter.id))
        {
            text += hint;
            continue;
        }
        spans.push_back({std::move(text), false});
        spans.push_back({parameter.id, true});
        text = hint.substr(parameter.id.size());
    }
    if (!text.empty())
        spans.push_back({std::move(text), false});
    return spans;
}

std::string descriptorNodeCapabilitySummary(
    const talas::core::NodeTypeDescriptor& descriptor)
{
    std::string result = descriptor.supportSummary;
    result += "\nMaturity: ";
    result += talas::core::nodeSupportLevelName(descriptor.supportLevel);
    result += "\nVendor: " + descriptor.provenance.vendor;
    result += "\nOrigin: "
        + descriptorOriginLabel(descriptor.provenance.origin);
    result += "\nOffline: "
        + executionSupportLabel(descriptor.capabilities.offline);
    result += " via ";
    result += talas::core::executionBindingName(
        descriptor.capabilities.offlineBinding);
    result += "\nPrepared realtime: "
        + executionSupportLabel(descriptor.capabilities.preparedRealtime);
    result += " via ";
    result += talas::core::executionBindingName(
        descriptor.capabilities.preparedRealtimeBinding);
    result += "\nNested Sounds: ";
    result += talas::core::nestedSoundSupportName(
        descriptor.capabilities.nestedSounds);
    result += "\nState: ";
    result += talas::core::nodeStateModelName(descriptor.capabilities.state);
    result += ", reset ";
    result += yesNo(descriptor.capabilities.supportsReset);
    result += "\nTyped events: ";
    result += yesNo(descriptor.capabilities.typedEvents);
    result += ", transport-aware ";
    result += yesNo(descriptor.capabilities.transportAware);
    result += "\nLatency: ";
    result += talas::core::latencyKindName(descriptor.capabilities.latency.kind);
    if (descriptor.capabilities.latency.kind == talas::core::LatencyKind::Fixed)
    {
        result += " (";
        result += std::to_string(descriptor.capabilities.latency.fixedFrames);
        result += " frames)";
    }
    else if (descriptor.capabilities.latency.kind
             == talas::core::LatencyKind::PreparationTime)
    {
        result += " (";
        result += talas::core::preparedLatencyRuleName(
            descriptor.capabilities.latency.preparedRule);
        result += ": " + descriptor.capabilities.latency.parameterId + ")";
    }
    result += "\nResource access: ";
    result += talas::core::resourceAccessModeName(
        descriptor.capabilities.resourceAccess);
    result += "\nFan-in latency: ";
    result += talas::core::fanInLatencyPolicyName(
        descriptor.capabilities.fanInLatencyPolicy);
    if (!descriptor.capabilities.outputBusLayouts.empty())
    {
        result += "\nOutput layouts: ";
        for (std::size_t index = 0;
             index < descriptor.capabilities.outputBusLayouts.size();
             ++index)
        {
            if (index != 0)
                result += ", ";
            result += talas::core::audioBusLayoutKindName(
                descriptor.capabilities.outputBusLayouts[index]);
        }
    }
    if (!descriptor.capabilities.limitations.empty())
    {
        result += "\nLimits: ";
        for (std::size_t index = 0;
             index < descriptor.capabilities.limitations.size();
             ++index)
        {
            if (index != 0)
                result += ". ";
            result += limitationLabel(descriptor.capabilities.limitations[index]);
        }
        result += ".";
    }
    return result;
}

bool descriptorPortsCompatible(
    const talas::core::PortDescriptor& source,
    const talas::core::PortDescriptor& destination) noexcept
{
    return talas::core::portDescriptorsCompatible(source, destination);
}

std::string descriptorConnectionCompatibilitySummary(
    const talas::core::PortDescriptor& source,
    const talas::core::PortDescriptor& destination)
{
    if (source.type != destination.type)
    {
        return "Cannot connect " + descriptorPortTypeName(source.type)
            + " to " + descriptorPortTypeName(destination.type) + ".";
    }
    if (!descriptorPortsCompatible(source, destination))
    {
        return "Cannot connect spectral format "
            + std::string{talas::core::spectralFrameFormatName(
                source.signal.spectralFormat)}
            + " to "
            + std::string{talas::core::spectralFrameFormatName(
                destination.signal.spectralFormat)}
            + ".";
    }
    return "Ports are compatible.";
}

std::vector<DescriptorAdapterSuggestion> descriptorAdapterSuggestions(
    const talas::core::PortDescriptor& source,
    const talas::core::PortDescriptor& destination,
    const talas::core::NodeRegistry& registry,
    std::size_t limit)
{
    std::vector<DescriptorAdapterSuggestion> result;
    if (limit == 0 || descriptorPortsCompatible(source, destination))
        return result;
    for (const auto& candidate : registry.nodeTypes())
    {
        if (!executableAdapter(candidate))
            continue;
        const auto input = std::ranges::find_if(
            candidate.inputs,
            [&](const auto& port) {
                return descriptorPortsCompatible(source, port);
            });
        if (input == candidate.inputs.end())
            continue;
        const auto output = std::ranges::find_if(
            candidate.outputs,
            [&](const auto& port) {
                return descriptorPortsCompatible(port, destination);
            });
        if (output == candidate.outputs.end())
            continue;
        result.push_back({
            candidate.typeId,
            candidate.displayName,
            input->id,
            output->id,
        });
    }
    std::ranges::sort(result, [](const auto& left, const auto& right) {
        if (left.displayName != right.displayName)
            return left.displayName < right.displayName;
        return left.typeId < right.typeId;
    });
    result.erase(
        std::unique(
            result.begin(),
            result.end(),
            [](const auto& left, const auto& right) {
                return left.typeId == right.typeId;
            }),
        result.end());
    if (result.size() > limit)
        result.resize(limit);
    return result;
}
}
