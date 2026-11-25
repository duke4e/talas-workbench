#include "talas/app/SoundLibraryAuditionModel.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/runtime/RealtimeRenderSession.h"
#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::ProjectSoundDefinition voiceDefinition()
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "voice";
    definition.displayName = "Audition Voice";
    definition.inputs = {
        {"signal", talas::core::PortType::Audio, "gain", "audioIn"},
        {"level", talas::core::PortType::Control, "gain", "gain"},
    };
    definition.outputs = {
        {"audioOut", talas::core::PortType::Audio, "gain", "audioOut"},
    };
    definition.graph.nodes.push_back({"gain", "talas.vca", {}, {}});
    return definition;
}

talas::app::SoundLibraryEntry projectEntry()
{
    talas::app::SoundLibraryEntry entry;
    entry.stableId = "project:voice";
    entry.sourceKind = talas::app::SoundLibrarySourceKind::ProjectDefinition;
    entry.displayName = "Audition Voice";
    entry.compatibility = talas::app::SoundLibraryCompatibility::Ready;
    entry.actions = talas::app::soundLibraryActionMask(
        talas::app::SoundLibraryAction::Audition);
    entry.definitionId = "voice";
    return entry;
}

void testPrivateAuditionUsesExactClosedSubstitutions()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument source;
    source.metadata.title = "Editable Project";
    source.soundDefinitions.push_back(voiceDefinition());
    const auto before = source;
    std::vector<talas::project::WorkspaceSoundLibraryInputSubstitution> plan{
        {"signal",
         talas::project::WorkspaceSoundLibraryAuditionSource::Sine,
         330.0,
         std::nullopt},
        {"level",
         talas::project::WorkspaceSoundLibraryAuditionSource::Constant,
         0.25,
         std::nullopt},
    };
    const auto prepared = talas::app::prepareSoundLibraryAudition(
        source, projectEntry(), registry, plan);
    require(prepared.ok() && prepared.outputChannelCount == 1
            && prepared.inputs.size() == 2,
        "exact Audio/Control substitution plan should prepare privately");
    require(source.metadata.title == before.metadata.title
            && source.graph.nodes.empty()
            && source.soundDefinitions == before.soundDefinitions,
        "audition preparation must not mutate source topology or metadata");
    require(std::ranges::any_of(
                prepared.document->graph.nodes,
                [](const auto& node) { return node.typeId == "talas.oscillator"; })
            && std::ranges::any_of(
                prepared.document->graph.nodes,
                [](const auto& node) { return node.typeId == "talas.constant"; }),
        "private audition should author exact bounded test-source nodes");
}

void testFrozenAssetAndUnsupportedBoundariesFailClosed()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument source;
    source.soundDefinitions.push_back(voiceDefinition());
    talas::project::ProjectAsset asset;
    asset.id = "frozen";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "frozen.wav";
    asset.contentHash.assign(64, 'a');
    asset.generation = 1;
    source.assets.push_back(asset);
    const auto frozen = talas::app::prepareSoundLibraryAudition(
        source,
        projectEntry(),
        registry,
        {{"signal",
          talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset,
          0.0,
          std::optional<std::string>{"frozen"}}});
    require(frozen.ok()
            && std::ranges::any_of(
                frozen.document->graph.nodes,
                [](const auto& node) { return node.typeId == "talas.diskPlayer"; })
            && frozen.document->assets.size() == 1
            && frozen.document->assets.front() == asset,
        "verified frozen Audio asset should remain declared in the private disk-source closure");

    auto unsupported = voiceDefinition();
    unsupported.definitionId = "eventVoice";
    unsupported.inputs.front().type = talas::core::PortType::Event;
    source.soundDefinitions.push_back(std::move(unsupported));
    auto unsupportedEntry = projectEntry();
    unsupportedEntry.stableId = "project:eventVoice";
    unsupportedEntry.definitionId = "eventVoice";
    require(!talas::app::prepareSoundLibraryAudition(
                 source, unsupportedEntry, registry, {})
                 .ok(),
        "Event substitution should remain visibly unsupported");
    require(!talas::app::prepareSoundLibraryAudition(
                 source,
                 projectEntry(),
                 registry,
                 {{"missing",
                   talas::project::WorkspaceSoundLibraryAuditionSource::Constant,
                   0.0,
                   std::nullopt}})
                 .ok(),
        "substitution for an absent input must not silently rebind");
    require(!talas::app::prepareSoundLibraryAudition(
                 source,
                 projectEntry(),
                 registry,
                 {{"signal",
                   talas::project::WorkspaceSoundLibraryAuditionSource::Silence,
                   440.0,
                   std::optional<std::string>{"frozen"}}})
                 .ok(),
        "contradictory source value and asset evidence must fail closed");
}

void testWholeExampleAndComponentAuthorityAreClosed()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument example;
    example.graph.nodes = {
        {"osc", "talas.oscillator", {}, {}},
        {"out", "talas.audioOutput", {}, {}},
    };
    example.graph.connections = {{"osc", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(example);
    talas::app::SoundLibraryEntry entry;
    entry.stableId = "example:osc";
    entry.sourceKind = talas::app::SoundLibrarySourceKind::OriginalExample;
    entry.displayName = "Original Oscillator";
    entry.compatibility = talas::app::SoundLibraryCompatibility::Ready;
    entry.actions = talas::app::soundLibraryActionMask(
        talas::app::SoundLibraryAction::Audition);
    const auto prepared = talas::app::prepareSoundLibraryAudition(
        example, entry, registry, {});
    require(prepared.ok() && prepared.outputChannelCount == 1,
        "renderable original example should prepare as an immutable private graph");

    talas::project::ProjectDocument multichannel;
    multichannel.graph.nodes = {
        {"osc1", "talas.oscillator", {}, {}},
        {"osc2", "talas.oscillator", {}, {}},
        {"osc3", "talas.oscillator", {}, {}},
        {"osc4", "talas.oscillator", {}, {}},
        {"out", "talas.output4", {}, {}},
    };
    multichannel.graph.connections = {
        {"osc1", "audioOut", "out", "out1"},
        {"osc2", "audioOut", "out", "out2"},
        {"osc3", "audioOut", "out", "out3"},
        {"osc4", "audioOut", "out", "out4"},
    };
    talas::project::reconcileProjectPresentation(multichannel);
    const auto preparedMultichannel =
        talas::app::prepareSoundLibraryAudition(
            multichannel, entry, registry, {});
    require(preparedMultichannel.ok()
            && preparedMultichannel.outputChannelCount == 4,
        "whole-example audition should retain the exact bounded multichannel output profile");

    entry.sourceKind = talas::app::SoundLibrarySourceKind::Component;
    require(!talas::app::prepareSoundLibraryAudition(
                 example, entry, registry, {})
                 .ok(),
        "component preview must not bypass private component-host authorization");
}

void requireAuditionMatchesRuntime(
    const talas::app::SoundLibraryAuditionPreparation& preparation,
    std::size_t expectedChannels)
{
    const auto compiled = talas::runtime::compileProject(
        *preparation.document, talas::core::makeBuiltinNodeRegistry(), {8000.0, 64});
    require(compiled.ok(), "audition output fixture must compile");
    talas::runtime::RealtimeRenderSession session(*compiled.graph);
    require(session.ready() && session.outputChannelCount() == expectedChannels
            && preparation.outputChannelCount == expectedChannels,
        "audition admission and prepared runtime must agree on channel count");
    const auto offline = talas::runtime::renderOffline(*compiled.graph, 64);
    require(offline.ok() && offline.channels.size() == expectedChannels,
        "offline terminal projection must retain the same ordered channels");
    std::array<std::array<double, 64>, 8> samples{};
    std::array<double*, 8> channels{};
    for (std::size_t index = 0; index < expectedChannels; ++index)
        channels[index] = samples[index].data();
    require(session.render({}, channels.data(), expectedChannels, 64),
        "all admitted audition channels must render");
    for (std::size_t channel = 0; channel < expectedChannels; ++channel)
    {
        require(std::ranges::any_of(samples[channel], [](double sample) {
            return std::abs(sample) > 1.0e-6;
        }), "each admitted channel must contain audible output");
        for (std::size_t frame = 0; frame < 64; ++frame)
            require(std::abs(samples[channel][frame] - offline.channels[channel][frame]) < 1.0e-12,
                "admitted channel order must agree with offline terminal rendering");
    }
}

void testWholeExampleStereoOutputBounds()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto entry = projectEntry();
    entry.sourceKind = talas::app::SoundLibrarySourceKind::OriginalExample;
    entry.definitionId.reset();
    talas::project::ProjectDocument document;
    document.graph.nodes = {{"osc", "talas.oscillator", {}}};
    for (std::size_t index = 0; index < 5; ++index)
    {
        const auto id = "out" + std::to_string(index);
        document.graph.nodes.push_back({id, "talas.audioOutput", {}});
        document.graph.connections.push_back({"osc", "audioOut", id, "audioIn"});
        document.graph.connections.push_back({"osc", "audioOut", id, "rightIn"});
        talas::project::reconcileProjectPresentation(document);
        const auto prepared = talas::app::prepareSoundLibraryAudition(
            document, entry, registry, {});
        if (index < 4)
        {
            require(prepared.ok() && prepared.outputChannelCount == (index + 1) * 2,
                "each connected stereo terminal must contribute two audition channels");
            if (index == 0)
                requireAuditionMatchesRuntime(prepared, 2);
            else
            {
                const auto compiled = talas::runtime::compileProject(document, registry, {});
                require(!compiled.ok() && std::ranges::any_of(
                        compiled.diagnostics, [](const auto& diagnostic) {
                            return diagnostic.code == talas::runtime::DiagnosticCode::MultipleAudioOutputs;
                        }),
                    "channel accounting must not bypass the runtime's single-terminal boundary");
            }
        }
        else
        {
            require(!prepared.ok()
                    && talas::app::soundLibraryProjectOutputChannelCount(document)
                        > talas::app::maximumSoundLibraryAuditionOutputs,
                "five stereo terminals must be rejected before private runtime preparation");
        }
    }
    // Only the right input of the retained terminal is disconnected.
    document.graph.nodes.resize(2);
    document.graph.connections.resize(1);
    talas::project::reconcileProjectPresentation(document);
    const auto mono = talas::app::prepareSoundLibraryAudition(document, entry, registry, {});
    require(mono.ok() && mono.outputChannelCount == 1,
        "a disconnected optional right input must restore mono audition accounting");
    requireAuditionMatchesRuntime(mono, 1);

    for (const auto count : {4, 8})
    {
        auto multichannel = document;
        multichannel.graph.nodes.back().typeId = "talas.output" + std::to_string(count);
        multichannel.graph.connections.clear();
        for (int channel = 1; channel <= count; ++channel)
            multichannel.graph.connections.push_back(
                {"osc", "audioOut", "out0", "out" + std::to_string(channel)});
        talas::project::reconcileProjectPresentation(multichannel);
        const auto prepared = talas::app::prepareSoundLibraryAudition(multichannel, entry, registry, {});
        require(prepared.ok(), "one multichannel terminal must retain audition admission");
        requireAuditionMatchesRuntime(prepared, count);
    }

    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "stereo";
    definition.displayName = "Stereo";
    definition.graph.nodes = {
        {"left", "talas.oscillator", {{"frequency", "110 hz"}}},
        {"right", "talas.oscillator", {{"frequency", "220 hz"}}},
    };
    definition.outputs = {
        {"left", talas::core::PortType::Audio, "left", "audioOut"},
        {"right", talas::core::PortType::Audio, "right", "audioOut"},
    };
    talas::project::ProjectSoundDefinition wrapper;
    wrapper.definitionId = "wrapper";
    wrapper.displayName = "Wrapper";
    wrapper.graph.nodes = {{"inner", {}, {}, "stereo"}};
    wrapper.outputs = {
        {"left", talas::core::PortType::Audio, "inner", "left"},
        {"right", talas::core::PortType::Audio, "inner", "right"},
    };
    document.soundDefinitions = {definition, wrapper};
    document.graph.nodes = {
        {"voice", {}, {}, "wrapper"}, {"out", "talas.audioOutput", {}}};
    document.graph.connections = {
        {"voice", "left", "out", "audioIn"},
        {"voice", "right", "out", "rightIn"},
    };
    talas::project::reconcileProjectPresentation(document);
    const auto nested = talas::app::prepareSoundLibraryAudition(document, entry, registry, {});
    require(nested.ok() && nested.outputChannelCount == 2,
        "nested Sound outputs must retain stereo whole-example audition accounting");
    requireAuditionMatchesRuntime(nested, 2);
}

void testWholeExampleAcceptsExactBoundedRawMidiTerminal()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument example;
    talas::project::ProjectNode terminal;
    terminal.instanceId = "terminal";
    terminal.typeId = "talas.midiOutputEventInBytes";
    terminal.parameters.push_back({
        "bytes", {}, {}, {}, {}, "16r90 64 16r7f"});
    example.graph.nodes.push_back(std::move(terminal));
    talas::project::reconcileProjectPresentation(example);
    talas::app::SoundLibraryEntry entry;
    entry.stableId = "example:bounded-raw-midi-output";
    entry.sourceKind = talas::app::SoundLibrarySourceKind::OriginalExample;
    entry.displayName = "Bounded Raw MIDI Output";
    entry.compatibility = talas::app::SoundLibraryCompatibility::Ready;
    entry.actions = talas::app::soundLibraryActionMask(
        talas::app::SoundLibraryAction::Audition);
    const auto prepared = talas::app::prepareSoundLibraryAudition(
        example, entry, registry, {});
    require(
        prepared.ok() && prepared.outputChannelCount == 0
            && prepared.document.has_value()
            && prepared.document->graph.nodes.size() == 1
            && prepared.document->graph.nodes.front().typeId
                == "talas.midiOutputEventInBytes",
        "exact bounded Raw MIDI terminal should retain zero-audio private audition authority");

    example.graph.nodes.front().typeId =
        "talas.midiOutputController";
    example.graph.nodes.front().parameters = {
        {"channel", "3"},
        {"controllerNumber", "10"},
        {"value", {}, {}, {}, {}, "127"},
    };
    entry.stableId = "example:bounded-midi-controller-output";
    entry.displayName = "Bounded MIDI Controller Output";
    const auto preparedController =
        talas::app::prepareSoundLibraryAudition(
            example, entry, registry, {});
    require(
        preparedController.ok()
            && preparedController.outputChannelCount == 0
            && preparedController.document.has_value()
            && preparedController.document->graph.nodes.size() == 1
            && preparedController.document->graph.nodes.front().typeId
                == "talas.midiOutputController",
        "exact bounded C40 controller terminal should retain zero-audio private audition authority");

    example.graph.nodes.front().typeId = "talas.midiOutputEvent";
    example.graph.nodes.front().parameters = {
        {"frequency", "440 hz"},
        {"amplitude", "1"},
        {"channel", "2"},
        {"gate", "1"},
    };
    entry.stableId = "example:bounded-midi-note-output";
    entry.displayName = "Bounded MIDI Note Output";
    const auto preparedNote = talas::app::prepareSoundLibraryAudition(
        example, entry, registry, {});
    require(
        preparedNote.ok()
            && preparedNote.outputChannelCount == 0
            && preparedNote.document.has_value()
            && preparedNote.document->graph.nodes.size() == 1
            && preparedNote.document->graph.nodes.front().typeId
                == "talas.midiOutputEvent",
        "exact bounded C41 note terminal should retain zero-audio private audition authority");

    auto noTerminal = example;
    noTerminal.graph.nodes.clear();
    talas::project::reconcileProjectPresentation(noTerminal);
    require(
        !talas::app::prepareSoundLibraryAudition(
             noTerminal, entry, registry, {}).ok(),
        "zero-audio graph without the exact bounded terminal must remain closed");
}

void testClosedAuditionProfileBounds()
{
    auto tooManyInputs = voiceDefinition();
    tooManyInputs.inputs.clear();
    for (std::size_t index = 0;
         index <= talas::app::maximumSoundLibraryAuditionInputs;
         ++index)
    {
        tooManyInputs.inputs.push_back({
            "input" + std::to_string(index),
            talas::core::PortType::Control,
            "gain",
            "gain",
        });
    }
    require(
        !talas::app::soundLibraryDefinitionAuditionProfile(tooManyInputs).ok(),
        "more than the closed input bound must be rejected before preparation");

    auto tooManyOutputs = voiceDefinition();
    tooManyOutputs.outputs.clear();
    for (std::size_t index = 0;
         index <= talas::app::maximumSoundLibraryAuditionOutputs;
         ++index)
    {
        tooManyOutputs.outputs.push_back({
            "output" + std::to_string(index),
            talas::core::PortType::Audio,
            "gain",
            "audioOut",
        });
    }
    require(
        !talas::app::soundLibraryDefinitionAuditionProfile(tooManyOutputs).ok(),
        "more than the closed output bound must be rejected before preparation");

    talas::project::ProjectDocument wholeExample;
    wholeExample.graph.nodes = {
        {"outA", "talas.output8", {}, {}},
        {"outB", "talas.audioOutput", {}, {}},
    };
    require(
        talas::app::soundLibraryProjectOutputChannelCount(wholeExample)
            > talas::app::maximumSoundLibraryAuditionOutputs,
        "whole-example output accounting must expose an over-capacity graph");

    const auto textParameter = [](
        std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.text = std::move(value);
        return parameter;
    };
    const auto choiceParameter = [](
        std::string id, std::string value) {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = std::move(id);
        parameter.choice = std::move(value);
        return parameter;
    };
    talas::project::ProjectDocument writableRecorder;
    writableRecorder.graph.nodes = {{
        "recorder",
        "talas.diskRecorder",
        {
            textParameter("format", "WAV"),
            textParameter("wordSize", "16"),
            choiceParameter("channels", "mono"),
        },
    }};
    require(
        talas::app::soundLibraryProjectOutputChannelCount(writableRecorder)
            == 1,
        "whole-example output accounting should admit exact writable Disk Recorder profile");

    // A DiskRecorder writes 8- and 24-bit WAV as well as 16-bit
    //.
    for (const auto* wordSize : {"8", "24"})
    {
        auto wider = writableRecorder;
        std::ranges::find(wider.graph.nodes.front().parameters, std::string_view{"wordSize"},
            &talas::project::ProjectParameterExpression::parameterId)->text = wordSize;
        require(talas::app::soundLibraryProjectOutputChannelCount(wider) == 1,
            "whole-example output accounting should admit 8- and 24-bit Disk Recorder WAV");
    }
    // A stereo DiskRecorder records two channels.
    auto stereoRecorder = writableRecorder;
    std::ranges::find(stereoRecorder.graph.nodes.front().parameters, std::string_view{"channels"},
        &talas::project::ProjectParameterExpression::parameterId)->choice = "stereo";
    require(talas::app::soundLibraryProjectOutputChannelCount(stereoRecorder) == 2,
        "whole-example output accounting should admit a stereo Disk Recorder's two channels");

    for (const auto unsupported : {
             std::pair{"format", "MP3"},
             std::pair{"wordSize", "32"},
             std::pair{"channels", "surround"},
         })
    {
        auto unsupportedRecorder = writableRecorder;
        auto& parameters =
            unsupportedRecorder.graph.nodes.front().parameters;
        const auto parameter = std::ranges::find(
            parameters,
            std::string_view{unsupported.first},
            &talas::project::ProjectParameterExpression::parameterId);
        if (unsupported.first == std::string_view{"channels"})
            parameter->choice = unsupported.second;
        else
            parameter->text = unsupported.second;
        require(
            talas::app::soundLibraryProjectOutputChannelCount(
                unsupportedRecorder)
                == 0,
            "unsupported Disk Recorder profile must not inherit whole-example audition output authority");
    }
}

void testReusableAuditionRetainsOnlySelectedClosure()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument source;
    source.soundDefinitions.push_back(voiceDefinition());

    auto unavailable = voiceDefinition();
    unavailable.definitionId = "unrelatedUnavailable";
    unavailable.graph.nodes.front().typeId = "missing.node.type";
    source.soundDefinitions.push_back(std::move(unavailable));

    const auto prepared = talas::app::prepareSoundLibraryAudition(
        source, projectEntry(), registry, {});
    require(prepared.ok(),
        "unrelated unavailable definitions must not disable an otherwise exact audition closure");
    require(prepared.document->soundDefinitions.size() == 1
            && prepared.document->soundDefinitions.front().definitionId
                == "voice",
        "private audition must retain only the selected reusable definition closure");
}

void testInPlaceProfileEvolutionDropsUnreachableSubstitutions()
{
    auto entry = projectEntry();
    entry.auditionInputs = voiceDefinition().inputs;
    entry.auditionAudioAssetIds = {"retainedAudio"};
    talas::project::WorkspaceSoundLibraryState state;
    state.selectedEntryId = entry.stableId;
    state.inputSubstitutions = {
        {"signal",
         talas::project::WorkspaceSoundLibraryAuditionSource::FrozenAsset,
         0.0,
         std::optional<std::string>{"removedAudio"}},
        {"level",
         talas::project::WorkspaceSoundLibraryAuditionSource::Constant,
         0.25,
         std::nullopt},
        {"removedPort",
         talas::project::WorkspaceSoundLibraryAuditionSource::Constant,
         0.5,
         std::nullopt},
    };
    require(talas::app::reconcileSoundLibraryAuditionSubstitutions(
                state, {entry})
            && state.inputSubstitutions.size() == 1
            && state.inputSubstitutions.front().portId == "level",
        "same-ID profile evolution must remove unreachable port and asset substitutions");
}
}

int main()
{
    testPrivateAuditionUsesExactClosedSubstitutions();
    testFrozenAssetAndUnsupportedBoundariesFailClosed();
    testWholeExampleAndComponentAuthorityAreClosed();
    testWholeExampleStereoOutputBounds();
    testWholeExampleAcceptsExactBoundedRawMidiTerminal();
    testClosedAuditionProfileBounds();
    testReusableAuditionRetainsOnlySelectedClosure();
    testInPlaceProfileEvolutionDropsUnreachableSubstitutions();
}
