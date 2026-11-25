#pragma once

#include "talas/core/NodeRegistry.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
enum class DescriptorWireColourRole
{
    Audio,
    Control,
    Event,
    Data
};

enum class DescriptorWirePattern
{
    Solid,
    LongDash,
    ShortDash,
    Dotted,
    DashDot,
    SparseDotted
};

enum class DescriptorWireMarker
{
    None,
    Gate,
    Trigger
};

enum class DescriptorPortShape
{
    Circle,
    Square,
    Diamond,
    Hexagon
};

enum class DescriptorConnectionEmphasis
{
    Normal,
    SelectedIncident,
    ContextDimmed
};

struct DescriptorWirePresentation
{
    DescriptorWireColourRole colour = DescriptorWireColourRole::Control;
    DescriptorWirePattern pattern = DescriptorWirePattern::Dotted;
    DescriptorWireMarker marker = DescriptorWireMarker::None;
    DescriptorPortShape terminalShape = DescriptorPortShape::Square;
    float width = 2.0F;
    float outlineWidth = 0.0F;
    float opacity = 0.9F;
};

struct DescriptorPortPresentation
{
    DescriptorWireColourRole colour = DescriptorWireColourRole::Control;
    DescriptorPortShape shape = DescriptorPortShape::Square;
    std::string_view code = "C";
};

struct DescriptorAdapterSuggestion
{
    std::string typeId;
    std::string displayName;
    std::string inputPortId;
    std::string outputPortId;
};

[[nodiscard]] std::string descriptorPortTypeName(talas::core::PortType type);
[[nodiscard]] std::string descriptorParameterKindName(
    talas::core::ParameterKind kind);
[[nodiscard]] std::string descriptorValueDomainName(
    talas::expr::ValueDomain domain);
[[nodiscard]] std::string descriptorOriginLabel(
    talas::core::NodeOrigin origin);

[[nodiscard]] DescriptorWirePresentation descriptorWirePresentation(
    const talas::core::PortDescriptor& source,
    DescriptorConnectionEmphasis emphasis =
        DescriptorConnectionEmphasis::Normal);
[[nodiscard]] DescriptorPortPresentation descriptorPortPresentation(
    const talas::core::PortDescriptor& port) noexcept;

[[nodiscard]] std::string descriptorPortInfoHint(
    const talas::core::PortDescriptor& port,
    bool output);
[[nodiscard]] std::string descriptorParameterInfoHint(
    const talas::core::ParameterDescriptor& parameter);
// What a prototype's field does, beyond its kind
// and range, for the fly-by help over its name; empty where there is
// nothing more to say. A Noise's Type explains white, pink and hot pink.
[[nodiscard]] std::string prototypeParameterHelp(std::string_view typeId, std::string_view parameterId);
[[nodiscard]] std::string descriptorNodeCapabilitySummary(
    const talas::core::NodeTypeDescriptor& descriptor);
// Describe Sound: what a
// kind of Sound is and does, its inputs and outputs, and each of its
// parameters as its field's help gives it, for a help window.
[[nodiscard]] std::string descriptorSoundClassHelp(
    const talas::core::NodeTypeDescriptor& descriptor);
// The same text in spans: the names of the
// parameters the virtual control surface can control, expression fields
// that are hot (not `constantField`), red; the rest black.
struct SoundClassHelpSpan
{
    std::string text;
    bool red = false;

    bool operator==(const SoundClassHelpSpan&) const = default;
};
[[nodiscard]] std::vector<SoundClassHelpSpan> descriptorSoundClassHelpSpans(
    const talas::core::NodeTypeDescriptor& descriptor, const std::function<bool(std::string_view)>& constantField = {});

[[nodiscard]] bool descriptorPortsCompatible(
    const talas::core::PortDescriptor& source,
    const talas::core::PortDescriptor& destination) noexcept;
[[nodiscard]] std::string descriptorConnectionCompatibilitySummary(
    const talas::core::PortDescriptor& source,
    const talas::core::PortDescriptor& destination);
[[nodiscard]] std::vector<DescriptorAdapterSuggestion>
descriptorAdapterSuggestions(
    const talas::core::PortDescriptor& source,
    const talas::core::PortDescriptor& destination,
    const talas::core::NodeRegistry& registry,
    std::size_t limit = 4);
}
