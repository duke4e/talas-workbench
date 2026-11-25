#pragma once

#include "talas/core/PortType.h"
#include "talas/expr/Expression.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace talas::core
{
enum class PortSignalRate
{
    Constant,
    Block,
    Sample,
    Event,
    Frame,
    VariableOrUnknown
};

[[nodiscard]] std::string_view portSignalRateName(PortSignalRate rate);

enum class AudioBusLayoutKind
{
    NotApplicable,
    Mono,
    Stereo,
    Discrete4,
    Discrete8,
    VariableOrUnknown,
    Discrete6
};

[[nodiscard]] std::string_view audioBusLayoutKindName(
    AudioBusLayoutKind layout);

enum class PortEdgeSemantics
{
    NotApplicable,
    Continuous,
    Gate,
    Trigger
};

[[nodiscard]] std::string_view portEdgeSemanticsName(
    PortEdgeSemantics semantics);

enum class SpectralFrameFormat
{
    NotApplicable,
    LegacyPartialTriples,
    PartialSpectrumV1,
    ComplexSpectrumV1,
    VariableOrUnknown
};

[[nodiscard]] std::string_view spectralFrameFormatName(
    SpectralFrameFormat format);

struct PortSignalContract
{
    PortSignalRate rate = PortSignalRate::VariableOrUnknown;
    AudioBusLayoutKind audioLayout =
        AudioBusLayoutKind::NotApplicable;
    PortEdgeSemantics edge = PortEdgeSemantics::NotApplicable;
    SpectralFrameFormat spectralFormat =
        SpectralFrameFormat::NotApplicable;
    bool timelineBearing = false;
};

[[nodiscard]] PortSignalContract defaultPortSignalContract(PortType type);

struct PortDescriptor
{
    std::string id;
    PortType type = PortType::Audio;
    bool required = false;
    PortSignalContract signal;
    bool compilerPopulated = false;
    // Collection fields, such as a Mixer's Inputs, take any number of Sounds:
    // such an input accepts many connections and sums them.
    bool acceptsMany = false;

    PortDescriptor() = default;
    PortDescriptor(std::string idValue, PortType typeValue, bool isRequired);
    PortDescriptor(
        std::string idValue,
        PortType typeValue,
        bool isRequired,
        PortSignalContract signalContract);
};

[[nodiscard]] bool portDescriptorsCompatible(
    const PortDescriptor& source,
    const PortDescriptor& destination) noexcept;

enum class ParameterKind
{
    Expression,
    Choice,
    Boolean,
    Resource,
    Text
};

enum class ParameterResourceAuthority
{
    None,
    StaticResourceList
};

enum class NodeSupportLevel
{
    FullRuntime,
    FunctionalSubset,
    Placeholder,
    Utility
};

[[nodiscard]] std::string_view nodeSupportLevelName(NodeSupportLevel level);

enum class NodeOrigin
{
    BuiltIn,
    Project,
    PreservedComponent,
    IsolatedComponent,
    External
};

[[nodiscard]] std::string_view nodeOriginName(NodeOrigin origin);

struct NodeProvenanceDescriptor
{
    std::string vendor = "Talas";
    NodeOrigin origin = NodeOrigin::BuiltIn;
};

enum class ExecutionSupport
{
    Unsupported,
    Subset,
    Supported
};

[[nodiscard]] std::string_view executionSupportName(
    ExecutionSupport support);

enum class ExecutionBinding
{
    None,
    GraphExpansion,
    LegacyAdapter,
    Kernel,
    IsolatedProcess
};

[[nodiscard]] std::string_view executionBindingName(
    ExecutionBinding binding);

enum class NestedSoundSupport
{
    Unsupported,
    Expanded,
    Native
};

[[nodiscard]] std::string_view nestedSoundSupportName(
    NestedSoundSupport support);

enum class NodeStateModel
{
    Unknown,
    Stateless,
    SessionStateful
};

[[nodiscard]] std::string_view nodeStateModelName(
    NodeStateModel model);

enum class LatencyKind
{
    None,
    Fixed,
    PreparationTime,
    VariableOrUnknown
};

[[nodiscard]] std::string_view latencyKindName(LatencyKind kind);

enum class PreparedLatencyRule
{
    None,
    ParameterFrames,
    ParameterFramesMinusOne,
    SampleRateOverParameterNextPowerOfTwoMinusOne,
    SimplePitchShifterInterval,
    FrequencyScaleProfile,
    MonotonizerProfile,
    TimeFrequencyScaleProfile,
    CdpPhaseVocoderProfile,
    // The latency a CDP port reports once the node's static fields (and
    // resources) prepare it; the parameter names the field it follows most.
    CdpPortReported
};

[[nodiscard]] std::string_view preparedLatencyRuleName(
    PreparedLatencyRule rule);

struct LatencyDescriptor
{
    LatencyKind kind = LatencyKind::VariableOrUnknown;
    std::size_t fixedFrames = 0;
    std::string parameterId;
    PreparedLatencyRule preparedRule = PreparedLatencyRule::None;
};

enum class ResourceAccessMode
{
    Unclassified,
    None,
    ImmutableRead,
    StreamRead,
    StreamWrite,
    VariableOrUnknown
};

[[nodiscard]] std::string_view resourceAccessModeName(
    ResourceAccessMode mode);

enum class FanInLatencyPolicy
{
    Unclassified,
    NotApplicable,
    Align,
    PreserveRelative,
    VariableOrUnknown
};

[[nodiscard]] std::string_view fanInLatencyPolicyName(
    FanInLatencyPolicy policy);

struct NodeCapabilityDescriptor
{
    ExecutionSupport offline = ExecutionSupport::Unsupported;
    ExecutionSupport preparedRealtime = ExecutionSupport::Unsupported;
    ExecutionBinding offlineBinding = ExecutionBinding::None;
    ExecutionBinding preparedRealtimeBinding = ExecutionBinding::None;
    NestedSoundSupport nestedSounds = NestedSoundSupport::Unsupported;
    NodeStateModel state = NodeStateModel::Unknown;
    bool supportsReset = false;
    bool typedEvents = false;
    bool transportAware = false;
    LatencyDescriptor latency;
    ResourceAccessMode resourceAccess =
        ResourceAccessMode::Unclassified;
    FanInLatencyPolicy fanInLatencyPolicy =
        FanInLatencyPolicy::Unclassified;
    std::vector<AudioBusLayoutKind> outputBusLayouts;
    std::vector<std::string> limitations;
};

struct ParameterDescriptor
{
    std::string id;
    std::string defaultExpression;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    ParameterKind kind = ParameterKind::Expression;
    std::string defaultChoice;
    std::vector<std::string> choices;
    std::optional<bool> defaultBoolean;
    std::string defaultResource;
    std::string defaultText;
    ParameterResourceAuthority resourceAuthority =
        ParameterResourceAuthority::None;
    // The value the `default` keyword names in this field, in its domain;
    // without one, `default` is an unknown identifier.
    std::optional<double> defaultKeywordValue;
};

[[nodiscard]] ParameterDescriptor choiceParameter(
    std::string id,
    std::string defaultChoice,
    std::vector<std::string> choices);

[[nodiscard]] ParameterDescriptor booleanParameter(
    std::string id,
    bool defaultValue);

[[nodiscard]] ParameterDescriptor resourceParameter(
    std::string id,
    std::string defaultResource);

[[nodiscard]] ParameterDescriptor textParameter(
    std::string id,
    std::string defaultText);

[[nodiscard]] ParameterDescriptor staticResourceListParameter(
    std::string id,
    std::string defaultText);

struct NodeTypeDescriptor
{
    std::string typeId;
    std::string displayName;
    std::string category;
    std::vector<PortDescriptor> inputs;
    std::vector<PortDescriptor> outputs;
    std::vector<ParameterDescriptor> parameters;
    NodeSupportLevel supportLevel = NodeSupportLevel::Utility;
    std::string supportSummary = "Unclassified utility node.";
    NodeCapabilityDescriptor capabilities;
    std::string typeVersion = "1.0.0";
    std::uint32_t stateSchemaVersion = 1;
    NodeProvenanceDescriptor provenance;
    // A legacy node kept so older graphs still compile names the node that
    // replaces it; the palette offers only the replacement.
    std::string replacedBy;
};

class NodeRegistry
{
public:
    void addNodeType(NodeTypeDescriptor descriptor);
    void setNodeTypeSupport(
        std::string_view typeId,
        NodeSupportLevel level,
        std::string summary);
    void setNodeTypeOfflineSupport(
        std::string_view typeId,
        ExecutionSupport support,
        ExecutionBinding binding);
    void setNodeTypePreparedRealtimeSupport(
        std::string_view typeId,
        ExecutionSupport support,
        ExecutionBinding binding,
        std::string limitationId = {});
    void addNodeTypeLimitation(
        std::string_view typeId,
        std::string limitationId);
    void setNodeTypeReplacement(
        std::string_view typeId,
        std::string replacementTypeId);
    void setNodeTypeNestedSoundSupport(
        std::string_view typeId,
        NestedSoundSupport support);
    void setNodeTypeProcessingContract(
        std::string_view typeId,
        NodeStateModel state,
        bool supportsReset,
        LatencyDescriptor latency);
    void setNodeTypeEventTransportContract(
        std::string_view typeId,
        bool typedEvents,
        bool transportAware);
    void setNodeTypeResourceAccess(
        std::string_view typeId,
        ResourceAccessMode mode);
    void setNodeTypeFanInLatencyPolicy(
        std::string_view typeId,
        FanInLatencyPolicy policy);
    void setNodeTypeOutputBusLayouts(
        std::string_view typeId,
        std::vector<AudioBusLayoutKind> layouts);
    [[nodiscard]] const NodeTypeDescriptor* findNodeType(std::string_view typeId) const;
    [[nodiscard]] std::vector<NodeTypeDescriptor> nodeTypes() const;

private:
    std::unordered_map<std::string, NodeTypeDescriptor> nodeTypesById_;
};
}
