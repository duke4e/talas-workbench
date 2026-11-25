#include "talas/app/DescriptorPresentationModel.h"
#include "talas/core/BuiltinNodes.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testWirePresentationUsesRateEdgeAndSelectionMetadata()
{
    talas::core::PortDescriptor audio{
        "audioOut",
        talas::core::PortType::Audio,
        false,
    };
    const auto ordinary = talas::app::descriptorWirePresentation(audio);
    const auto selected = talas::app::descriptorWirePresentation(
        audio,
        talas::app::DescriptorConnectionEmphasis::SelectedIncident);
    const auto dimmed = talas::app::descriptorWirePresentation(
        audio,
        talas::app::DescriptorConnectionEmphasis::ContextDimmed);
    require(
        ordinary.colour == talas::app::DescriptorWireColourRole::Audio
            && ordinary.pattern == talas::app::DescriptorWirePattern::Solid
            && ordinary.terminalShape
                == talas::app::DescriptorPortShape::Circle
            && selected.width > ordinary.width
            && selected.outlineWidth > ordinary.outlineWidth
            && selected.opacity > ordinary.opacity
            && dimmed.colour == ordinary.colour
            && dimmed.pattern == ordinary.pattern
            && dimmed.terminalShape == ordinary.terminalShape
            && dimmed.width < ordinary.width
            && dimmed.opacity < ordinary.opacity,
        "selection should add a colour-independent outline while context dimming preserves semantic geometry and lowers emphasis");

    talas::core::PortDescriptor gate{
        "gate",
        talas::core::PortType::Control,
        false,
        {
            talas::core::PortSignalRate::Block,
            talas::core::AudioBusLayoutKind::NotApplicable,
            talas::core::PortEdgeSemantics::Gate,
            talas::core::SpectralFrameFormat::NotApplicable,
            false,
        },
    };
    const auto gateStyle = talas::app::descriptorWirePresentation(gate);
    require(
        gateStyle.colour == talas::app::DescriptorWireColourRole::Control
            && gateStyle.pattern
                == talas::app::DescriptorWirePattern::ShortDash
            && gateStyle.marker == talas::app::DescriptorWireMarker::Gate,
        "block-rate gate metadata should produce redundant colour, dash, and marker semantics");
}

void testEverySemanticWireClassHasAClosedProjection()
{
    using Colour = talas::app::DescriptorWireColourRole;
    const std::array typeCases{
        std::pair{talas::core::PortType::Audio, Colour::Audio},
        std::pair{talas::core::PortType::Control, Colour::Control},
        std::pair{talas::core::PortType::Event, Colour::Event},
        std::pair{talas::core::PortType::NoteStream, Colour::Event},
        std::pair{talas::core::PortType::SampleBuffer, Colour::Data},
        std::pair{talas::core::PortType::Wavetable, Colour::Data},
        std::pair{talas::core::PortType::Spectrum, Colour::Data},
        std::pair{talas::core::PortType::ComplexSpectrum, Colour::Data},
    };
    for (const auto& [type, expected] : typeCases)
    {
        const talas::core::PortDescriptor port{"port", type, false};
        require(
            talas::app::descriptorWirePresentation(port).colour == expected,
            "every port type should map to one semantic colour role");
    }

    using Pattern = talas::app::DescriptorWirePattern;
    const std::array rateCases{
        std::pair{talas::core::PortSignalRate::Constant, Pattern::LongDash},
        std::pair{talas::core::PortSignalRate::Block, Pattern::ShortDash},
        std::pair{talas::core::PortSignalRate::Sample, Pattern::Solid},
        std::pair{talas::core::PortSignalRate::Event, Pattern::Dotted},
        std::pair{talas::core::PortSignalRate::Frame, Pattern::DashDot},
        std::pair{
            talas::core::PortSignalRate::VariableOrUnknown,
            Pattern::SparseDotted},
    };
    for (const auto& [rate, expected] : rateCases)
    {
        talas::core::PortDescriptor port{
            "port",
            talas::core::PortType::Control,
            false,
        };
        port.signal.rate = rate;
        require(
            talas::app::descriptorWirePresentation(port).pattern == expected,
            "every signal rate should have a distinct closed wire pattern");
    }

    using Marker = talas::app::DescriptorWireMarker;
    const std::array edgeCases{
        std::pair{talas::core::PortEdgeSemantics::NotApplicable, Marker::None},
        std::pair{talas::core::PortEdgeSemantics::Continuous, Marker::None},
        std::pair{talas::core::PortEdgeSemantics::Gate, Marker::Gate},
        std::pair{talas::core::PortEdgeSemantics::Trigger, Marker::Trigger},
    };
    for (const auto& [edge, expected] : edgeCases)
    {
        talas::core::PortDescriptor port{
            "port",
            talas::core::PortType::Control,
            false,
        };
        port.signal.edge = edge;
        require(
            talas::app::descriptorWirePresentation(port).marker == expected,
            "every edge semantic should have a closed marker projection");
    }
}

void testEveryPortFamilyHasDistinctGeometryAndCode()
{
    using Shape = talas::app::DescriptorPortShape;
    const std::array cases{
        std::tuple{
            talas::core::PortType::Audio,
            Shape::Circle,
            std::string_view{"A"}},
        std::tuple{
            talas::core::PortType::Control,
            Shape::Square,
            std::string_view{"C"}},
        std::tuple{
            talas::core::PortType::Event,
            Shape::Diamond,
            std::string_view{"E"}},
        std::tuple{
            talas::core::PortType::NoteStream,
            Shape::Diamond,
            std::string_view{"E"}},
        std::tuple{
            talas::core::PortType::SampleBuffer,
            Shape::Hexagon,
            std::string_view{"D"}},
        std::tuple{
            talas::core::PortType::Wavetable,
            Shape::Hexagon,
            std::string_view{"D"}},
        std::tuple{
            talas::core::PortType::Spectrum,
            Shape::Hexagon,
            std::string_view{"D"}},
        std::tuple{
            talas::core::PortType::ComplexSpectrum,
            Shape::Hexagon,
            std::string_view{"D"}},
    };
    for (const auto& [type, shape, code] : cases)
    {
        const talas::core::PortDescriptor port{"port", type, false};
        const auto presentation =
            talas::app::descriptorPortPresentation(port);
        const auto wire = talas::app::descriptorWirePresentation(port);
        require(
            presentation.shape == shape
                && presentation.code == code
                && wire.terminalShape == shape,
            "each semantic port family should retain matching port geometry, short text code, and wire terminal geometry");
    }
}

void testEveryParameterDomainAndOriginHasAClosedLabel()
{
    require(
        talas::app::descriptorParameterKindName(
            talas::core::ParameterKind::Expression) == "expression"
            && talas::app::descriptorParameterKindName(
                   talas::core::ParameterKind::Choice) == "choice"
            && talas::app::descriptorParameterKindName(
                   talas::core::ParameterKind::Boolean) == "boolean"
            && talas::app::descriptorParameterKindName(
                   talas::core::ParameterKind::Resource) == "resource"
            && talas::app::descriptorParameterKindName(
                   talas::core::ParameterKind::Text) == "text",
        "every parameter kind should have a stable label");
    require(
        talas::app::descriptorValueDomainName(talas::expr::ValueDomain::Scalar)
                == "scalar"
            && talas::app::descriptorValueDomainName(
                   talas::expr::ValueDomain::Hertz) == "hertz"
            && talas::app::descriptorValueDomainName(
                   talas::expr::ValueDomain::Seconds) == "seconds"
            && talas::app::descriptorValueDomainName(
                   talas::expr::ValueDomain::NoteNumber) == "note number"
            && talas::app::descriptorValueDomainName(
                   talas::expr::ValueDomain::Samples) == "samples",
        "every value domain should have a stable unit/domain label");
    require(
        talas::app::descriptorOriginLabel(talas::core::NodeOrigin::BuiltIn)
                == "Built-in"
            && talas::app::descriptorOriginLabel(talas::core::NodeOrigin::Project)
                == "Project"
            && talas::app::descriptorOriginLabel(
                   talas::core::NodeOrigin::PreservedComponent)
                == "Preserved component"
            && talas::app::descriptorOriginLabel(
                   talas::core::NodeOrigin::IsolatedComponent)
                == "Isolated component"
            && talas::app::descriptorOriginLabel(talas::core::NodeOrigin::External)
                == "External",
        "every descriptor origin should have a stable product label");
}

void testInfoHintsExposeCompleteDescriptorFacts()
{
    const talas::core::PortDescriptor spectrum{
        "partials",
        talas::core::PortType::Spectrum,
        true,
        {
            talas::core::PortSignalRate::Frame,
            talas::core::AudioBusLayoutKind::Mono,
            talas::core::PortEdgeSemantics::Continuous,
            talas::core::SpectralFrameFormat::PartialSpectrumV1,
            true,
        },
    };
    const auto port = talas::app::descriptorPortInfoHint(spectrum, false);
    require(
        port.find("Input partials: spectrum, required") != std::string::npos
            && port.find("rate frame") != std::string::npos
            && port.find("layout mono") != std::string::npos
            && port.find("edge continuous") != std::string::npos
            && port.find("format partialSpectrumV1") != std::string::npos
            && port.find("timeline-bearing yes") != std::string::npos,
        "port Info Hint should expose direction, type, requiredness, rate, layout, edge, format, and timeline semantics");

    const auto parameter = talas::core::choiceParameter(
        "window",
        "hann",
        {"hann", "blackman"});
    const auto hint = talas::app::descriptorParameterInfoHint(parameter);
    require(
        hint.find("choice") != std::string::npos
            && hint.find("domain scalar") != std::string::npos
            && hint.find("default hann") != std::string::npos
            && hint.find("blackman") != std::string::npos,
        "parameter Info Hint should expose kind, domain, default, and choices");

    talas::core::ParameterDescriptor frequency{
        "frequency",
        "440 hz",
        talas::expr::ValueDomain::Hertz,
    };
    require(
        talas::app::descriptorParameterInfoHint(frequency).find(
            "domain hertz, default 440 hz") != std::string::npos
            && talas::app::descriptorParameterInfoHint(
                   talas::core::booleanParameter("enabled", true))
                   .find("default enabled") != std::string::npos
            && talas::app::descriptorParameterInfoHint(
                   talas::core::resourceParameter("sample", "asset-1"))
                   .find("project resource reference, default asset-1")
                != std::string::npos
            && talas::app::descriptorParameterInfoHint(
                   talas::core::textParameter("label", "hello"))
                   .find("literal text, default hello") != std::string::npos,
        "expression, boolean, resource, and text Info Hints should preserve their descriptor-specific semantics");
    // The help names the values a field
    // takes, a numbered Scale1 a scale's.
    const talas::core::ParameterDescriptor scale1{"scale1", "1.0", talas::expr::ValueDomain::Scalar};
    require(talas::app::descriptorParameterInfoHint(scale1).find("takes -1.0 to 1.0") != std::string::npos
            && talas::app::descriptorParameterInfoHint(frequency).find("takes -25000.0 hz to 25000.0 hz") != std::string::npos
            && talas::app::descriptorParameterInfoHint(parameter).find("takes") == std::string::npos,
        "a parameter's help should give the range of values its field expects");
    // The help over a Noise's Type explains white, pink and
    // hot pink noise.
    const auto noise = talas::app::prototypeParameterHelp("talas.noise", "type");
    require(noise.find("white:") != std::string::npos && noise.find("pink: the same energy in every octave") != std::string::npos
            && noise.find("hotPink:") != std::string::npos && talas::app::prototypeParameterHelp("talas.noise", "scale").empty(),
        "the fly-by help should explain the differences between white, pink and hot pink noise");
    // A resonator's Damping of 1 is the open loop, which its name does not
    // say (2026-11-25).
    const auto damping = talas::app::prototypeParameterHelp("talas.stereoDispersiveResonator", "damping");
    require(damping.find("not an amount of damping") != std::string::npos
            && damping.find("1 leaves the loop open") != std::string::npos
            && damping.find("0, a 20 Hz cut-off") != std::string::npos,
        "the fly-by help over a resonator's Damping should say that 1 is the open loop and 0 the darkest");
}

void testCompatibilityAndAdaptersUseRegisteredDescriptors()
{
    const talas::core::PortDescriptor audio{
        "audio",
        talas::core::PortType::Audio,
        false,
    };
    const talas::core::PortDescriptor complex{
        "complex",
        talas::core::PortType::ComplexSpectrum,
        true,
    };
    require(
        !talas::app::descriptorPortsCompatible(audio, complex)
            && talas::app::descriptorConnectionCompatibilitySummary(
                   audio,
                   complex)
                   .find("audio to complex spectrum")
                != std::string::npos,
        "incompatible descriptor types should produce an exact readable reason");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto adapters = talas::app::descriptorAdapterSuggestions(
        audio,
        complex,
        registry,
        8);
    require(
        !adapters.empty() && adapters.front().typeId == "talas.forwardFftV1"
            && adapters.front().inputPortId == "audioIn"
            && adapters.front().outputPortId == "spectrumOut",
        "adapter discovery should find and describe the registered forward FFT without a node-type table");
    require(
        talas::app::descriptorAdapterSuggestions(audio, audio, registry).empty()
            && talas::app::descriptorAdapterSuggestions(
                   audio,
                   complex,
                   registry,
                   0).empty(),
        "compatible ports and a zero result limit should never invent adapter guidance");

    auto fixtureRegistry = talas::core::NodeRegistry{};
    const auto* forward = registry.findNodeType("talas.forwardFftV1");
    require(forward != nullptr, "fixture adapter source should exist");
    auto fixtureAdapter = *forward;
    fixtureAdapter.typeId = "talas.project.sound.descriptorDrivenAdapter";
    fixtureAdapter.displayName = "Descriptor-driven adapter";
    fixtureAdapter.provenance.vendor = "Example Vendor";
    fixtureAdapter.provenance.origin = talas::core::NodeOrigin::External;
    fixtureAdapter.capabilities.offline =
        talas::core::ExecutionSupport::Supported;
    fixtureAdapter.capabilities.offlineBinding =
        talas::core::ExecutionBinding::GraphExpansion;
    fixtureAdapter.capabilities.preparedRealtime =
        talas::core::ExecutionSupport::Supported;
    fixtureAdapter.capabilities.preparedRealtimeBinding =
        talas::core::ExecutionBinding::GraphExpansion;
    fixtureAdapter.capabilities.limitations.clear();
    fixtureRegistry.addNodeType(std::move(fixtureAdapter));
    const auto fixtureSuggestions = talas::app::descriptorAdapterSuggestions(
        audio,
        complex,
        fixtureRegistry);
    require(
        fixtureSuggestions.size() == 1
            && fixtureSuggestions.front().typeId
                == "talas.project.sound.descriptorDrivenAdapter",
        "adding a compatible fixture descriptor should change adapter guidance without a UI node table");

    auto legacy = complex;
    legacy.type = talas::core::PortType::Spectrum;
    legacy.signal = talas::core::defaultPortSignalContract(
        talas::core::PortType::Spectrum);
    auto native = legacy;
    native.signal.spectralFormat =
        talas::core::SpectralFrameFormat::PartialSpectrumV1;
    require(
        !talas::app::descriptorPortsCompatible(legacy, native)
            && talas::app::descriptorConnectionCompatibilitySummary(
                   legacy,
                   native)
                   .find("legacyPartialTriples")
                != std::string::npos,
        "same-type spectral ports should still require exact frame-format compatibility");
}

void testCapabilitySummaryKeepsProvenanceAndLimitsVisible()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* oscillator = registry.findNodeType("talas.oscillator");
    require(oscillator != nullptr, "capability fixture should find oscillator");
    const auto summary =
        talas::app::descriptorNodeCapabilitySummary(*oscillator);
    require(
        summary.find("Vendor: Talas") != std::string::npos
            && summary.find("Origin: Built-in") != std::string::npos
            && summary.find("Maturity: fullRuntime")
                != std::string::npos
            && summary.find("Prepared realtime: Supported")
                != std::string::npos
            && summary.find("Nested Sounds:") != std::string::npos
            && summary.find("State:") != std::string::npos
            && summary.find("Typed events:") != std::string::npos
            && summary.find("Limits:") != std::string::npos,
        "capability summary should keep maturity, provenance, every execution contract, and limitations visible together");
}

// Describe Sound: a help
// window describes the Sound's class and each of its parameters, as each
// field's help tells the nature and the values it expects.
void testDescribeSoundListsTheClassAndEveryParameter()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* noise = registry.findNodeType("talas.noise");
    require(noise != nullptr, "the help fixture should find Noise");
    const auto help = talas::app::descriptorSoundClassHelp(*noise);
    require(help.starts_with(noise->displayName + " (" + noise->category + ")\n" + noise->supportSummary),
        "the help should open with the class's name, category and what it does");
    for (const auto& parameter : noise->parameters)
        require(help.find(talas::app::descriptorParameterInfoHint(parameter)) != std::string::npos,
            "the help should give every parameter's field help: " + parameter.id);
    for (const auto& port : noise->outputs)
        require(help.find(talas::app::descriptorPortInfoHint(port, true)) != std::string::npos,
            "the help should list the outputs");
    const auto* filter = registry.findNodeType("talas.filter");
    require(filter != nullptr && talas::app::descriptorSoundClassHelp(*filter).find("\n\nInputs\nInput audioIn")
            != std::string::npos,
        "a filter's help should list its input");
    // The names of the parameters the virtual
    // control surface can control red, the others black.
    const auto spans = talas::app::descriptorSoundClassHelpSpans(*noise, [](std::string_view id) { return id == "initialState"; });
    std::string joined;
    std::vector<std::string> red;
    for (const auto& span : spans)
    {
        joined += span.text;
        if (span.red)
            red.push_back(span.text);
    }
    require(joined == help, "the spans should make up Describe Sound's text");
    require(std::ranges::find(red, std::string{"frequency"}) != red.end() && std::ranges::find(red, std::string{"scale"}) != red.end()
            && std::ranges::find(red, std::string{"type"}) == red.end()
            && std::ranges::find(red, std::string{"initialState"}) == red.end(),
        "the hot fields' names should be red, a choice's and a constant field's black");
}
}

int main()
{
    testWirePresentationUsesRateEdgeAndSelectionMetadata();
    testEverySemanticWireClassHasAClosedProjection();
    testEveryPortFamilyHasDistinctGeometryAndCode();
    testEveryParameterDomainAndOriginHasAClosedLabel();
    testInfoHintsExposeCompleteDescriptorFacts();
    testCompatibilityAndAdaptersUseRegisteredDescriptors();
    testCapabilitySummaryKeepsProvenanceAndLimitsVisible();
    testDescribeSoundListsTheClassAndEveryParameter();
    return 0;
}
