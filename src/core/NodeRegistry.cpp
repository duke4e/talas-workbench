#include "talas/core/NodeRegistry.h"

#include "talas/core/BuiltinNodeBindings.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace talas::core
{
std::string_view portSignalRateName(PortSignalRate rate)
{
    switch (rate)
    {
    case PortSignalRate::Constant:
        return "constant";
    case PortSignalRate::Block:
        return "block";
    case PortSignalRate::Sample:
        return "sample";
    case PortSignalRate::Event:
        return "event";
    case PortSignalRate::Frame:
        return "frame";
    case PortSignalRate::VariableOrUnknown:
        return "variableOrUnknown";
    }
    return "variableOrUnknown";
}

std::string_view audioBusLayoutKindName(AudioBusLayoutKind layout)
{
    switch (layout)
    {
    case AudioBusLayoutKind::NotApplicable:
        return "notApplicable";
    case AudioBusLayoutKind::Mono:
        return "mono";
    case AudioBusLayoutKind::Stereo:
        return "stereo";
    case AudioBusLayoutKind::Discrete4:
        return "discrete4";
    case AudioBusLayoutKind::Discrete8:
        return "discrete8";
    case AudioBusLayoutKind::Discrete6:
        return "discrete6";
    case AudioBusLayoutKind::VariableOrUnknown:
        return "variableOrUnknown";
    }
    return "variableOrUnknown";
}

std::string_view portEdgeSemanticsName(PortEdgeSemantics semantics)
{
    switch (semantics)
    {
    case PortEdgeSemantics::NotApplicable:
        return "notApplicable";
    case PortEdgeSemantics::Continuous:
        return "continuous";
    case PortEdgeSemantics::Gate:
        return "gate";
    case PortEdgeSemantics::Trigger:
        return "trigger";
    }
    return "notApplicable";
}

std::string_view spectralFrameFormatName(SpectralFrameFormat format)
{
    switch (format)
    {
    case SpectralFrameFormat::NotApplicable:
        return "notApplicable";
    case SpectralFrameFormat::LegacyPartialTriples:
        return "legacyPartialTriples";
    case SpectralFrameFormat::PartialSpectrumV1:
        return "partialSpectrumV1";
    case SpectralFrameFormat::ComplexSpectrumV1:
        return "complexSpectrumV1";
    case SpectralFrameFormat::VariableOrUnknown:
        return "variableOrUnknown";
    }
    return "variableOrUnknown";
}

PortSignalContract defaultPortSignalContract(PortType type)
{
    switch (type)
    {
    case PortType::Audio:
        return {
            PortSignalRate::Sample,
            AudioBusLayoutKind::Mono,
            PortEdgeSemantics::Continuous,
            SpectralFrameFormat::NotApplicable,
            true,
        };
    case PortType::Control:
        return {
            PortSignalRate::VariableOrUnknown,
            AudioBusLayoutKind::NotApplicable,
            PortEdgeSemantics::Continuous,
            SpectralFrameFormat::NotApplicable,
            false,
        };
    case PortType::Event:
    case PortType::NoteStream:
        return {
            PortSignalRate::Event,
            AudioBusLayoutKind::NotApplicable,
            PortEdgeSemantics::NotApplicable,
            SpectralFrameFormat::NotApplicable,
            false,
        };
    case PortType::SampleBuffer:
    case PortType::Wavetable:
        return {
            PortSignalRate::Constant,
            AudioBusLayoutKind::NotApplicable,
            PortEdgeSemantics::NotApplicable,
            SpectralFrameFormat::NotApplicable,
            false,
        };
    case PortType::Spectrum:
        return {
            PortSignalRate::Frame,
            AudioBusLayoutKind::Mono,
            PortEdgeSemantics::Continuous,
            SpectralFrameFormat::LegacyPartialTriples,
            true,
        };
    case PortType::ComplexSpectrum:
        return {
            PortSignalRate::Frame,
            AudioBusLayoutKind::Mono,
            PortEdgeSemantics::Continuous,
            SpectralFrameFormat::ComplexSpectrumV1,
            true,
        };
    }
    return {};
}

PortDescriptor::PortDescriptor(
    std::string idValue,
    PortType typeValue,
    bool isRequired)
    : id(std::move(idValue)),
      type(typeValue),
      required(isRequired),
      signal(defaultPortSignalContract(typeValue))
{
}

bool portDescriptorsCompatible(
    const PortDescriptor& source,
    const PortDescriptor& destination) noexcept
{
    // Any signal may feed any input: audio and control signals are
    // both a value per frame, and compiling adapts one to the other.
    const auto signal = [](PortType type) {
        return type == PortType::Audio || type == PortType::Control;
    };
    if (signal(source.type) && signal(destination.type))
        return true;
    if (source.type != destination.type)
        return false;
    if (source.type == PortType::Spectrum
        || source.type == PortType::ComplexSpectrum)
    {
        // A timestamped spectrum, a LiveSpectralAnalysis's, feeds any
        // Spectrum input: compiling holds its latest frame for the legacy
        // reader.
        return source.signal.spectralFormat
                == destination.signal.spectralFormat
            || (source.signal.spectralFormat
                    == SpectralFrameFormat::PartialSpectrumV1
                && destination.signal.spectralFormat
                    == SpectralFrameFormat::LegacyPartialTriples);
    }
    return true;
}

PortDescriptor::PortDescriptor(
    std::string idValue,
    PortType typeValue,
    bool isRequired,
    PortSignalContract signalContract)
    : id(std::move(idValue)),
      type(typeValue),
      required(isRequired),
      signal(signalContract)
{
}

std::string_view nodeSupportLevelName(NodeSupportLevel level)
{
    switch (level)
    {
    case NodeSupportLevel::FullRuntime:
        return "fullRuntime";
    case NodeSupportLevel::FunctionalSubset:
        return "functionalSubset";
    case NodeSupportLevel::Placeholder:
        return "placeholder";
    case NodeSupportLevel::Utility:
        return "utility";
    }
    return "utility";
}

std::string_view nodeOriginName(NodeOrigin origin)
{
    switch (origin)
    {
    case NodeOrigin::BuiltIn:
        return "builtIn";
    case NodeOrigin::Project:
        return "project";
    case NodeOrigin::PreservedComponent:
        return "preservedComponent";
    case NodeOrigin::IsolatedComponent:
        return "isolatedComponent";
    case NodeOrigin::External:
        return "external";
    }
    return "unknown";
}

std::string_view executionSupportName(ExecutionSupport support)
{
    switch (support)
    {
    case ExecutionSupport::Unsupported:
        return "unsupported";
    case ExecutionSupport::Subset:
        return "subset";
    case ExecutionSupport::Supported:
        return "supported";
    }
    return "unsupported";
}

std::string_view executionBindingName(ExecutionBinding binding)
{
    switch (binding)
    {
    case ExecutionBinding::None:
        return "none";
    case ExecutionBinding::GraphExpansion:
        return "graphExpansion";
    case ExecutionBinding::LegacyAdapter:
        return "legacyAdapter";
    case ExecutionBinding::Kernel:
        return "kernel";
    case ExecutionBinding::IsolatedProcess:
        return "isolatedProcess";
    }
    return "none";
}

std::string_view nestedSoundSupportName(NestedSoundSupport support)
{
    switch (support)
    {
    case NestedSoundSupport::Unsupported:
        return "unsupported";
    case NestedSoundSupport::Expanded:
        return "expanded";
    case NestedSoundSupport::Native:
        return "native";
    }
    return "unsupported";
}

std::string_view nodeStateModelName(NodeStateModel model)
{
    switch (model)
    {
    case NodeStateModel::Unknown:
        return "unknown";
    case NodeStateModel::Stateless:
        return "stateless";
    case NodeStateModel::SessionStateful:
        return "sessionStateful";
    }
    return "unknown";
}

std::string_view latencyKindName(LatencyKind kind)
{
    switch (kind)
    {
    case LatencyKind::None:
        return "none";
    case LatencyKind::Fixed:
        return "fixed";
    case LatencyKind::PreparationTime:
        return "preparationTime";
    case LatencyKind::VariableOrUnknown:
        return "variableOrUnknown";
    }
    return "variableOrUnknown";
}

std::string_view preparedLatencyRuleName(PreparedLatencyRule rule)
{
    switch (rule)
    {
    case PreparedLatencyRule::None:
        return "none";
    case PreparedLatencyRule::ParameterFrames:
        return "parameterFrames";
    case PreparedLatencyRule::ParameterFramesMinusOne:
        return "parameterFramesMinusOne";
    case PreparedLatencyRule::SampleRateOverParameterNextPowerOfTwoMinusOne:
        return "sampleRateOverParameterNextPowerOfTwoMinusOne";
    case PreparedLatencyRule::SimplePitchShifterInterval:
        return "simplePitchShifterInterval";
    case PreparedLatencyRule::FrequencyScaleProfile:
        return "frequencyScaleProfile";
    case PreparedLatencyRule::MonotonizerProfile:
        return "monotonizerProfile";
    case PreparedLatencyRule::TimeFrequencyScaleProfile:
        return "timeFrequencyScaleProfile";
    case PreparedLatencyRule::CdpPhaseVocoderProfile:
        return "cdpPhaseVocoderProfile";
    case PreparedLatencyRule::CdpPortReported:
        return "cdpPortReported";
    }
    return "none";
}

std::string_view resourceAccessModeName(ResourceAccessMode mode)
{
    switch (mode)
    {
    case ResourceAccessMode::Unclassified:
        return "unclassified";
    case ResourceAccessMode::None:
        return "none";
    case ResourceAccessMode::ImmutableRead:
        return "immutableRead";
    case ResourceAccessMode::StreamRead:
        return "streamRead";
    case ResourceAccessMode::StreamWrite:
        return "streamWrite";
    case ResourceAccessMode::VariableOrUnknown:
        return "variableOrUnknown";
    }
    return "unclassified";
}

std::string_view fanInLatencyPolicyName(FanInLatencyPolicy policy)
{
    switch (policy)
    {
    case FanInLatencyPolicy::Unclassified:
        return "unclassified";
    case FanInLatencyPolicy::NotApplicable:
        return "notApplicable";
    case FanInLatencyPolicy::Align:
        return "align";
    case FanInLatencyPolicy::PreserveRelative:
        return "preserveRelative";
    case FanInLatencyPolicy::VariableOrUnknown:
        return "variableOrUnknown";
    }
    return "unclassified";
}

ParameterDescriptor choiceParameter(std::string id, std::string defaultChoice, std::vector<std::string> choices)
{
    return {
        std::move(id),
        {},
        talas::expr::ValueDomain::Scalar,
        ParameterKind::Choice,
        std::move(defaultChoice),
        std::move(choices),
        std::nullopt,
        {},
        {},
    };
}

ParameterDescriptor booleanParameter(std::string id, bool defaultValue)
{
    return {
        std::move(id),
        {},
        talas::expr::ValueDomain::Scalar,
        ParameterKind::Boolean,
        {},
        {},
        defaultValue,
        {},
        {},
    };
}

ParameterDescriptor resourceParameter(std::string id, std::string defaultResource)
{
    return {
        std::move(id),
        {},
        talas::expr::ValueDomain::Scalar,
        ParameterKind::Resource,
        {},
        {},
        std::nullopt,
        std::move(defaultResource),
        {},
    };
}

ParameterDescriptor textParameter(std::string id, std::string defaultText)
{
    return {
        std::move(id),
        {},
        talas::expr::ValueDomain::Scalar,
        ParameterKind::Text,
        {},
        {},
        std::nullopt,
        {},
        std::move(defaultText),
    };
}

ParameterDescriptor staticResourceListParameter(
    std::string id,
    std::string defaultText)
{
    auto result = textParameter(
        std::move(id), std::move(defaultText));
    result.resourceAuthority =
        ParameterResourceAuthority::StaticResourceList;
    return result;
}

void validateParameterDescriptor(const ParameterDescriptor& parameter)
{
    if (parameter.id.empty())
        throw std::invalid_argument("Parameter id cannot be empty");
    if (parameter.resourceAuthority
            == ParameterResourceAuthority::StaticResourceList
        && parameter.kind != ParameterKind::Text)
    {
        throw std::invalid_argument(
            "Static resource-list authority requires a text parameter");
    }

    if (parameter.kind == ParameterKind::Expression)
    {
        if (!parameter.defaultChoice.empty() || !parameter.choices.empty())
            throw std::invalid_argument("Expression parameter cannot declare choices");
        if (parameter.defaultBoolean.has_value())
            throw std::invalid_argument("Expression parameter cannot declare a boolean default");
        if (!parameter.defaultResource.empty())
            throw std::invalid_argument("Expression parameter cannot declare a resource default");
        if (!parameter.defaultText.empty())
            throw std::invalid_argument("Expression parameter cannot declare a text default");
        return;
    }

    if (parameter.kind == ParameterKind::Boolean)
    {
        if (!parameter.defaultExpression.empty())
            throw std::invalid_argument("Boolean parameter cannot declare a default expression");
        if (!parameter.defaultChoice.empty() || !parameter.choices.empty())
            throw std::invalid_argument("Boolean parameter cannot declare choices");
        if (!parameter.defaultBoolean.has_value())
            throw std::invalid_argument("Boolean parameter default cannot be empty");
        if (!parameter.defaultResource.empty())
            throw std::invalid_argument("Boolean parameter cannot declare a resource default");
        if (!parameter.defaultText.empty())
            throw std::invalid_argument("Boolean parameter cannot declare a text default");
        return;
    }

    if (parameter.kind == ParameterKind::Resource)
    {
        if (!parameter.defaultExpression.empty())
            throw std::invalid_argument("Resource parameter cannot declare a default expression");
        if (!parameter.defaultChoice.empty() || !parameter.choices.empty())
            throw std::invalid_argument("Resource parameter cannot declare choices");
        if (parameter.defaultBoolean.has_value())
            throw std::invalid_argument("Resource parameter cannot declare a boolean default");
        if (!parameter.defaultText.empty())
            throw std::invalid_argument("Resource parameter cannot declare a text default");
        return;
    }

    if (parameter.kind == ParameterKind::Text)
    {
        if (!parameter.defaultExpression.empty())
            throw std::invalid_argument("Text parameter cannot declare a default expression");
        if (!parameter.defaultChoice.empty() || !parameter.choices.empty())
            throw std::invalid_argument("Text parameter cannot declare choices");
        if (parameter.defaultBoolean.has_value())
            throw std::invalid_argument("Text parameter cannot declare a boolean default");
        if (!parameter.defaultResource.empty())
            throw std::invalid_argument("Text parameter cannot declare a resource default");
        return;
    }

    if (!parameter.defaultExpression.empty())
        throw std::invalid_argument("Choice parameter cannot declare a default expression");
    if (parameter.defaultBoolean.has_value())
        throw std::invalid_argument("Choice parameter cannot declare a boolean default");
    if (!parameter.defaultResource.empty())
        throw std::invalid_argument("Choice parameter cannot declare a resource default");
    if (!parameter.defaultText.empty())
        throw std::invalid_argument("Choice parameter cannot declare a text default");
    if (parameter.defaultChoice.empty())
        throw std::invalid_argument("Choice parameter default cannot be empty");
    if (parameter.choices.empty())
        throw std::invalid_argument("Choice parameter choices cannot be empty");

    bool defaultChoiceFound = false;
    for (std::size_t index = 0; index < parameter.choices.size(); ++index)
    {
        if (parameter.choices[index].empty())
            throw std::invalid_argument("Choice parameter choice cannot be empty");
        if (parameter.choices[index] == parameter.defaultChoice)
            defaultChoiceFound = true;
        if (std::ranges::find(parameter.choices.begin() + static_cast<std::ptrdiff_t>(index + 1),
                              parameter.choices.end(),
                              parameter.choices[index]) != parameter.choices.end())
        {
            throw std::invalid_argument("Choice parameter choices must be unique");
        }
    }

    if (!defaultChoiceFound)
        throw std::invalid_argument("Choice parameter default must be in choices");
}

void validatePortDescriptor(const PortDescriptor& port)
{
    if (port.id.empty())
        throw std::invalid_argument("Port id cannot be empty");

    const auto requireNoSpectralFormat = [&] {
        if (port.signal.spectralFormat
            != SpectralFrameFormat::NotApplicable)
        {
            throw std::invalid_argument(
                "Non-spectral port cannot declare a spectral frame format");
        }
    };
    const auto requireNoAudioLayout = [&] {
        if (port.signal.audioLayout
            != AudioBusLayoutKind::NotApplicable)
        {
            throw std::invalid_argument(
                "Non-audio port cannot declare an audio bus layout");
        }
    };
    const auto requireFrameLayout = [&] {
        if (port.signal.audioLayout == AudioBusLayoutKind::NotApplicable)
        {
            throw std::invalid_argument(
                "Spectral frame port must declare a channel layout");
        }
    };

    switch (port.type)
    {
    case PortType::Audio:
        requireNoSpectralFormat();
        if (port.signal.rate != PortSignalRate::Sample
            || port.signal.audioLayout
                == AudioBusLayoutKind::NotApplicable
            || port.signal.edge != PortEdgeSemantics::Continuous
            || !port.signal.timelineBearing)
        {
            throw std::invalid_argument(
                "Audio port must be a continuous sample-rate timeline with a channel layout");
        }
        return;
    case PortType::Control:
        requireNoSpectralFormat();
        requireNoAudioLayout();
        if (port.signal.rate == PortSignalRate::Frame
            || port.signal.edge == PortEdgeSemantics::NotApplicable)
        {
            throw std::invalid_argument(
                "Control port must declare a continuous, gate, or trigger edge contract");
        }
        return;
    case PortType::Event:
    case PortType::NoteStream:
        requireNoSpectralFormat();
        requireNoAudioLayout();
        if (port.signal.rate != PortSignalRate::Event
            || port.signal.edge != PortEdgeSemantics::NotApplicable
            || port.signal.timelineBearing)
        {
            throw std::invalid_argument(
                "Event port must use the event-rate non-buffer contract");
        }
        return;
    case PortType::SampleBuffer:
    case PortType::Wavetable:
        requireNoSpectralFormat();
        requireNoAudioLayout();
        if (port.signal.rate != PortSignalRate::Constant
            || port.signal.edge != PortEdgeSemantics::NotApplicable
            || port.signal.timelineBearing)
        {
            throw std::invalid_argument(
                "Prepared data port must use the constant non-timeline contract");
        }
        return;
    case PortType::Spectrum:
        requireFrameLayout();
        if (port.signal.rate != PortSignalRate::Frame
            || port.signal.edge != PortEdgeSemantics::Continuous
            || !port.signal.timelineBearing
            || (port.signal.spectralFormat
                    != SpectralFrameFormat::LegacyPartialTriples
                && port.signal.spectralFormat
                    != SpectralFrameFormat::PartialSpectrumV1))
        {
            throw std::invalid_argument(
                "Spectrum port must declare a partial frame timeline contract");
        }
        return;
    case PortType::ComplexSpectrum:
        requireFrameLayout();
        if (port.signal.rate != PortSignalRate::Frame
            || port.signal.edge != PortEdgeSemantics::Continuous
            || !port.signal.timelineBearing
            || port.signal.spectralFormat
                != SpectralFrameFormat::ComplexSpectrumV1)
        {
            throw std::invalid_argument(
                "ComplexSpectrum port must declare the versioned complex frame timeline contract");
        }
        return;
    }
    throw std::invalid_argument("Unknown port type");
}

void normalizeNodeTypeDescriptor(NodeTypeDescriptor& descriptor)
{
    if (descriptor.capabilities.resourceAccess
        == ResourceAccessMode::Unclassified)
    {
        const auto hasResourceAuthorityParameter = std::ranges::any_of(
            descriptor.parameters,
            [](const auto& parameter) {
                return parameter.kind == ParameterKind::Resource
                    || parameter.resourceAuthority
                        == ParameterResourceAuthority::StaticResourceList;
            });
        descriptor.capabilities.resourceAccess =
            hasResourceAuthorityParameter
            ? ResourceAccessMode::VariableOrUnknown
            : ResourceAccessMode::None;
    }

    if (descriptor.capabilities.fanInLatencyPolicy
        == FanInLatencyPolicy::Unclassified)
    {
        const auto timelineInputCount = std::ranges::count_if(
            descriptor.inputs,
            [](const auto& port) {
                return port.signal.timelineBearing;
            });
        descriptor.capabilities.fanInLatencyPolicy =
            timelineInputCount >= 2
            ? FanInLatencyPolicy::Align
            : FanInLatencyPolicy::NotApplicable;
    }
}

void validateNodeTypeDescriptor(const NodeTypeDescriptor& descriptor)
{
    constexpr std::size_t maximumProvenanceVendorBytes = 512;
    if (descriptor.provenance.vendor.empty()
        || descriptor.provenance.vendor.size() > maximumProvenanceVendorBytes)
    {
        throw std::invalid_argument(
            "Node provenance vendor must be non-empty and bounded");
    }
    switch (descriptor.provenance.origin)
    {
    case NodeOrigin::BuiltIn:
    case NodeOrigin::Project:
    case NodeOrigin::PreservedComponent:
    case NodeOrigin::IsolatedComponent:
    case NodeOrigin::External:
        break;
    default:
        throw std::invalid_argument("Unknown node provenance origin");
    }
    for (const auto& port : descriptor.inputs)
        validatePortDescriptor(port);
    for (const auto& port : descriptor.outputs)
        validatePortDescriptor(port);
    for (const auto& parameter : descriptor.parameters)
        validateParameterDescriptor(parameter);

    const auto& latency = descriptor.capabilities.latency;
    if (latency.kind == LatencyKind::Fixed)
    {
        if (latency.fixedFrames == 0)
            throw std::invalid_argument("Fixed node latency must be positive");
        if (!latency.parameterId.empty()
            || latency.preparedRule != PreparedLatencyRule::None)
        {
            throw std::invalid_argument(
                "Fixed node latency cannot declare a preparation-time rule");
        }
    }
    else if (latency.kind == LatencyKind::PreparationTime)
    {
        if (latency.fixedFrames != 0 || latency.parameterId.empty()
            || latency.preparedRule == PreparedLatencyRule::None)
        {
            throw std::invalid_argument(
                "Preparation-time latency requires a parameter rule and no fixed frames");
        }
        const auto parameter = std::ranges::find(
            descriptor.parameters,
            latency.parameterId,
            &ParameterDescriptor::id);
        // A port-reported latency may follow a resource, as fast
        // convolution's follows its impulse sample's length.
        if (parameter == descriptor.parameters.end()
            || (parameter->kind != ParameterKind::Expression
                && !(latency.preparedRule == PreparedLatencyRule::CdpPortReported
                    && parameter->kind == ParameterKind::Resource)))
        {
            throw std::invalid_argument(
                "Preparation-time latency parameter must name an expression parameter");
        }
    }
    else if (latency.fixedFrames != 0 || !latency.parameterId.empty()
             || latency.preparedRule != PreparedLatencyRule::None)
    {
        throw std::invalid_argument(
            "Non-fixed node latency cannot declare fixed or prepared fields");
    }

    if (descriptor.capabilities.resourceAccess
        == ResourceAccessMode::Unclassified)
    {
        throw std::invalid_argument(
            "Node resource access must be classified before publication");
    }
    const auto hasResourceParameter = std::ranges::any_of(
        descriptor.parameters,
        [](const auto& parameter) {
            return parameter.kind == ParameterKind::Resource;
        });
    const auto hasStaticResourceListAuthority =
        std::ranges::any_of(
            descriptor.parameters,
            [](const auto& parameter) {
                return parameter.resourceAuthority
                    == ParameterResourceAuthority::StaticResourceList;
            });
    const auto hasResourceAuthorityParameter =
        hasResourceParameter || hasStaticResourceListAuthority;
    if (hasResourceAuthorityParameter
        && descriptor.capabilities.resourceAccess
            == ResourceAccessMode::None)
    {
        throw std::invalid_argument(
            "Resource authority parameters require an explicit resource access contract");
    }
    if (!hasResourceAuthorityParameter
        && descriptor.capabilities.resourceAccess
            != ResourceAccessMode::None)
    {
        throw std::invalid_argument(
            "Resource access requires an explicit typed parameter authority");
    }
    if (descriptor.capabilities.fanInLatencyPolicy
        == FanInLatencyPolicy::Unclassified)
    {
        throw std::invalid_argument(
            "Node fan-in latency policy must be classified before publication");
    }
    const auto timelineInputCount = std::ranges::count_if(
        descriptor.inputs,
        [](const auto& port) {
            return port.signal.timelineBearing;
        });
    if (timelineInputCount >= 2
        && descriptor.capabilities.fanInLatencyPolicy
            == FanInLatencyPolicy::NotApplicable)
    {
        throw std::invalid_argument(
            "Timeline-bearing fan-in requires an explicit latency policy");
    }
    for (std::size_t index = 0;
         index < descriptor.capabilities.outputBusLayouts.size();
         ++index)
    {
        const auto layout =
            descriptor.capabilities.outputBusLayouts[index];
        if (layout == AudioBusLayoutKind::NotApplicable)
        {
            throw std::invalid_argument(
                "Output bus layout cannot be not-applicable");
        }
        if (std::ranges::find(
                descriptor.capabilities.outputBusLayouts.begin()
                    + static_cast<std::ptrdiff_t>(index + 1),
                descriptor.capabilities.outputBusLayouts.end(),
                layout)
            != descriptor.capabilities.outputBusLayouts.end())
        {
            throw std::invalid_argument(
                "Output bus layouts must be unique");
        }
    }
    if (descriptor.capabilities.supportsReset
        && descriptor.capabilities.state != NodeStateModel::SessionStateful)
    {
        throw std::invalid_argument(
            "Only session-stateful nodes can declare reset support");
    }
    if (descriptor.capabilities.supportsReset
        && (descriptor.capabilities.preparedRealtime
                == ExecutionSupport::Unsupported
            || descriptor.capabilities.preparedRealtimeBinding
                == ExecutionBinding::None))
    {
        throw std::invalid_argument(
            "Reset support requires a prepared realtime binding");
    }

    const auto validateBinding = [&descriptor](
        std::string_view context,
        ExecutionSupport support,
        ExecutionBinding binding,
        bool preparedRealtime) {
        const auto claimsExecution =
            support != ExecutionSupport::Unsupported;
        const auto hasBinding = binding != ExecutionBinding::None;
        if (claimsExecution != hasBinding)
        {
            throw std::invalid_argument(
                std::string(context)
                + " execution support and binding must agree");
        }
        if (!claimsExecution)
            return;

        const auto bindingExists = [&] {
            switch (binding)
            {
            case ExecutionBinding::None:
                return false;
            case ExecutionBinding::GraphExpansion:
                return descriptor.typeId.starts_with(
                    "talas.project.sound.");
            case ExecutionBinding::LegacyAdapter:
                return preparedRealtime
                    ? hasBuiltinPreparedRealtimeLegacyBinding(
                          descriptor.typeId)
                    : hasBuiltinOfflineLegacyBinding(
                          descriptor.typeId);
            case ExecutionBinding::Kernel:
                return preparedRealtime
                    ? hasBuiltinPreparedRealtimeKernelBinding(
                          descriptor.typeId)
                    : hasBuiltinOfflineKernelBinding(
                          descriptor.typeId);
            case ExecutionBinding::IsolatedProcess:
                return preparedRealtime
                    && descriptor.capabilities.offline
                        == ExecutionSupport::Unsupported
                    && descriptor.capabilities.offlineBinding
                        == ExecutionBinding::None;
            }
            return false;
        };
        if (!bindingExists())
        {
            throw std::invalid_argument(
                std::string(context)
                + " execution support has no registered implementation binding");
        }
    };
    validateBinding(
        "Offline",
        descriptor.capabilities.offline,
        descriptor.capabilities.offlineBinding,
        false);
    validateBinding(
        "Prepared realtime",
        descriptor.capabilities.preparedRealtime,
        descriptor.capabilities.preparedRealtimeBinding,
        true);

    for (std::size_t index = 0;
         index < descriptor.capabilities.limitations.size();
         ++index)
    {
        const auto& limitation =
            descriptor.capabilities.limitations[index];
        if (limitation.empty())
            throw std::invalid_argument("Node limitation id cannot be empty");
        if (std::ranges::find(
                descriptor.capabilities.limitations.begin()
                    + static_cast<std::ptrdiff_t>(index + 1),
                descriptor.capabilities.limitations.end(),
                limitation)
            != descriptor.capabilities.limitations.end())
        {
            throw std::invalid_argument("Node limitation ids must be unique");
        }
    }
}

void replaceExecutionLimitation(
    std::vector<std::string>& limitations,
    std::string_view prefix,
    std::string replacement)
{
    std::erase_if(limitations, [&](const auto& limitation) {
        return limitation.starts_with(prefix);
    });
    if (!replacement.empty())
        limitations.push_back(std::move(replacement));
}

void NodeRegistry::addNodeType(NodeTypeDescriptor descriptor)
{
    if (descriptor.typeId.empty())
        throw std::invalid_argument("Node type id cannot be empty");
    normalizeNodeTypeDescriptor(descriptor);
    validateNodeTypeDescriptor(descriptor);

    const auto [_, inserted] = nodeTypesById_.emplace(descriptor.typeId, std::move(descriptor));
    if (!inserted)
        throw std::invalid_argument("Duplicate node type id");
}

void NodeRegistry::setNodeTypeSupport(
    std::string_view typeId,
    NodeSupportLevel level,
    std::string summary)
{
    if (summary.empty())
        throw std::invalid_argument("Node support summary cannot be empty");

    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
        throw std::invalid_argument("Unknown node type id for support metadata");

    found->second.supportLevel = level;
    found->second.supportSummary = std::move(summary);
    validateNodeTypeDescriptor(found->second);
}

void NodeRegistry::setNodeTypeOfflineSupport(
    std::string_view typeId,
    ExecutionSupport support,
    ExecutionBinding binding)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
    {
        throw std::invalid_argument(
            "Unknown node type id for offline metadata");
    }

    auto updated = found->second;
    updated.capabilities.offline = support;
    updated.capabilities.offlineBinding = binding;
    replaceExecutionLimitation(
        updated.capabilities.limitations,
        "offline-",
        support == ExecutionSupport::Unsupported
            ? "offline-unsupported"
            : support == ExecutionSupport::Subset
            ? updated.supportLevel == NodeSupportLevel::Placeholder
                ? "offline-compatibility-placeholder"
                : "offline-functional-subset"
            : std::string{});
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

void NodeRegistry::setNodeTypePreparedRealtimeSupport(
    std::string_view typeId,
    ExecutionSupport support,
    ExecutionBinding binding,
    std::string limitationId)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
    {
        throw std::invalid_argument(
            "Unknown node type id for prepared realtime metadata");
    }

    auto updated = found->second;
    updated.capabilities.preparedRealtime = support;
    updated.capabilities.preparedRealtimeBinding = binding;
    if (!limitationId.empty()
        && (support != ExecutionSupport::Subset
            || !limitationId.starts_with("prepared-realtime-")))
    {
        throw std::invalid_argument(
            "Custom prepared realtime limitation requires subset support and a prepared-realtime- id");
    }
    replaceExecutionLimitation(
        updated.capabilities.limitations,
        "prepared-realtime-",
        support == ExecutionSupport::Unsupported
            ? "prepared-realtime-unsupported"
            : support == ExecutionSupport::Subset
            ? limitationId.empty()
                ? "prepared-realtime-subset"
                : std::move(limitationId)
            : std::string{});
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

void NodeRegistry::addNodeTypeLimitation(
    std::string_view typeId,
    std::string limitationId)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
    {
        throw std::invalid_argument(
            "Unknown node type id for limitation metadata");
    }

    auto updated = found->second;
    updated.capabilities.limitations.push_back(
        std::move(limitationId));
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

void NodeRegistry::setNodeTypeReplacement(
    std::string_view typeId,
    std::string replacementTypeId)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end()
        || replacementTypeId.empty()
        || nodeTypesById_.find(replacementTypeId) == nodeTypesById_.end())
    {
        throw std::invalid_argument(
            "Unknown node type id for replacement metadata");
    }
    found->second.replacedBy = std::move(replacementTypeId);
}

void NodeRegistry::setNodeTypeNestedSoundSupport(
    std::string_view typeId,
    NestedSoundSupport support)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
    {
        throw std::invalid_argument(
            "Unknown node type id for nested-Sound metadata");
    }
    found->second.capabilities.nestedSounds = support;
    validateNodeTypeDescriptor(found->second);
}

void NodeRegistry::setNodeTypeProcessingContract(
    std::string_view typeId,
    NodeStateModel state,
    bool supportsReset,
    LatencyDescriptor latency)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
    {
        throw std::invalid_argument(
            "Unknown node type id for processing contract");
    }

    auto updated = found->second;
    updated.capabilities.state = state;
    updated.capabilities.supportsReset = supportsReset;
    updated.capabilities.latency = latency;
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

void NodeRegistry::setNodeTypeEventTransportContract(
    std::string_view typeId,
    bool typedEvents,
    bool transportAware)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
    {
        throw std::invalid_argument(
            "Unknown node type id for event/transport contract");
    }

    auto updated = found->second;
    updated.capabilities.typedEvents = typedEvents;
    updated.capabilities.transportAware = transportAware;
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

void NodeRegistry::setNodeTypeResourceAccess(
    std::string_view typeId,
    ResourceAccessMode mode)
{
    if (mode == ResourceAccessMode::Unclassified)
        throw std::invalid_argument("Resource access cannot be unclassified");
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
        throw std::invalid_argument("Unknown node type id for resource access");

    auto updated = found->second;
    updated.capabilities.resourceAccess = mode;
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

void NodeRegistry::setNodeTypeFanInLatencyPolicy(
    std::string_view typeId,
    FanInLatencyPolicy policy)
{
    if (policy == FanInLatencyPolicy::Unclassified)
        throw std::invalid_argument("Fan-in latency policy cannot be unclassified");
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
        throw std::invalid_argument("Unknown node type id for fan-in latency policy");

    auto updated = found->second;
    updated.capabilities.fanInLatencyPolicy = policy;
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

void NodeRegistry::setNodeTypeOutputBusLayouts(
    std::string_view typeId,
    std::vector<AudioBusLayoutKind> layouts)
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
        throw std::invalid_argument("Unknown node type id for output bus layouts");

    auto updated = found->second;
    updated.capabilities.outputBusLayouts = std::move(layouts);
    validateNodeTypeDescriptor(updated);
    found->second = std::move(updated);
}

const NodeTypeDescriptor* NodeRegistry::findNodeType(std::string_view typeId) const
{
    const auto found = nodeTypesById_.find(std::string(typeId));
    if (found == nodeTypesById_.end())
        return nullptr;
    return &found->second;
}

std::vector<NodeTypeDescriptor> NodeRegistry::nodeTypes() const
{
    std::vector<NodeTypeDescriptor> result;
    result.reserve(nodeTypesById_.size());

    for (const auto& [_, descriptor] : nodeTypesById_)
        result.push_back(descriptor);

    std::ranges::sort(result, {}, &NodeTypeDescriptor::typeId);
    return result;
}
}
