#include "talas/core/BuiltinNodes.h"
#include "talas/project/PreparedProjectGenerators.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectGeneratorOutputs.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectScore.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

talas::project::ProjectGeneratorProgram program()
{
    talas::project::ProjectGeneratorProgram result;
    result.generatorId = "phrase";
    result.displayName = "Phrase";
    result.source.terminals = {"note", "rest"};
    result.source.grammar = "start:note rest note";
    result.source.seed = 7;
    result.source.maximumRewritePasses = 1;
    result.source.maximumOutputSymbols = 8;

    talas::project::ProjectGeneratorTerminalBinding note;
    note.terminal = "note";
    note.actions.push_back(
        talas::project::ProjectGeneratorTimelineEventAction{
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            60.0,
            talas::core::AutomationInterpolation::Step,
        });
    note.actions.push_back(
        talas::project::ProjectGeneratorScoreClipAction{
            "voice",
            "voiceDefinition",
            480,
            0,
            60,
            60,
            1,
        });
    note.actions.push_back(
        talas::project::ProjectGeneratorScoreAutomationAction{
            "gainLane",
            talas::expr::ValueDomain::Scalar,
            0.5,
            talas::core::AutomationInterpolation::Linear,
        });
    note.actions.push_back(
        talas::project::ProjectGeneratorScoreMarkerAction{
            "Generated note",
            240,
        });
    result.output.timelineStepMicroseconds = 250000;
    result.output.scoreStepTicks = 960;
    result.output.bindings.push_back(std::move(note));
    return result;
}

talas::project::ProjectGeneratorProgram maximumProgram(
    talas::project::ProjectGeneratorOutputAction action)
{
    talas::project::ProjectGeneratorProgram result;
    result.generatorId = "maximum";
    result.displayName = "Maximum";
    result.source.terminals = {"note"};
    result.source.grammar = "start:A\nA ->";
    for (std::size_t index = 0;
         index < talas::generative::maximumContextFreeSymbolsPerAlternative;
         ++index)
    {
        result.source.grammar += " B";
    }
    result.source.grammar += "\nB ->";
    for (std::size_t index = 0;
         index < talas::generative::maximumContextFreeSymbolsPerAlternative;
         ++index)
    {
        result.source.grammar += " note";
    }
    result.source.seed = 1;
    result.source.maximumRewritePasses = 2;
    result.source.maximumOutputSymbols =
        talas::generative::maximumContextFreeOutputSymbols;
    result.output.timelineStepMicroseconds = 1;
    result.output.scoreStepTicks = 1;
    result.output.bindings.push_back({"note", {std::move(action)}});
    return result;
}

talas::project::ProjectGeneratorProgram graphProgram(
    std::string grammar,
    std::vector<talas::project::ProjectGeneratorTerminalBinding> bindings,
    std::size_t maximumOutputSymbols = 8,
    std::size_t maximumRewritePasses = 1)
{
    talas::project::ProjectGeneratorProgram result;
    result.generatorId = "soundChain";
    result.displayName = "Sound Chain";
    result.source.terminals = {"source", "processor"};
    result.source.grammar = std::move(grammar);
    result.source.seed = 11;
    result.source.maximumRewritePasses = maximumRewritePasses;
    result.source.maximumOutputSymbols = maximumOutputSymbols;
    result.output.timelineStepMicroseconds = 1;
    result.output.scoreStepTicks = 1;
    result.output.bindings = std::move(bindings);
    return result;
}

talas::project::ProjectGeneratorProgram graphProgram(
    std::string grammar = "start:source processor")
{
    return graphProgram(
        std::move(grammar),
        {
            {
                "source",
                {talas::project::ProjectGeneratorGraphNodeAction{
                    "talas.oscillator"}},
            },
            {
                "processor",
                {talas::project::ProjectGeneratorGraphNodeAction{
                    "talas.filter"}},
            },
        });
}

talas::project::ProjectGeneratorProgram graphCapacityProgram(
    bool overflow)
{
    std::string grammar = "start:source";
    if (overflow)
    {
        grammar += " N8";
    }
    else
    {
        for (int exponent = 7; exponent >= 0; --exponent)
            grammar += " N" + std::to_string(exponent);
    }
    grammar += "\nN0 -> processor";
    for (int exponent = 1; exponent <= 8; ++exponent)
    {
        grammar += "\nN" + std::to_string(exponent)
            + " -> N" + std::to_string(exponent - 1)
            + " N" + std::to_string(exponent - 1);
    }
    return graphProgram(
        std::move(grammar),
        {
            {
                "source",
                {talas::project::ProjectGeneratorGraphNodeAction{
                    "talas.oscillator"}},
            },
            {
                "processor",
                {talas::project::ProjectGeneratorGraphNodeAction{
                    "talas.filter"}},
            },
        },
        overflow ? 257U : 256U,
        9U);
}

talas::project::ProjectDocument document()
{
    talas::project::ProjectDocument result;
    result.soundDefinitions.push_back({
        "voiceDefinition",
        "Voice",
    });
    result.timeline.score.tracks = {
        {
            "voice",
            "Voice",
            talas::project::ScoreTrackKind::Track,
            talas::project::ScoreTimeDomain::MusicalTicks,
            "master",
            2,
            talas::project::ScoreOverlapMode::Layer,
        },
        {
            "master",
            "Master",
            talas::project::ScoreTrackKind::Master,
            talas::project::ScoreTimeDomain::Seconds,
            {},
            2,
            talas::project::ScoreOverlapMode::Layer,
        },
    };
    result.timeline.score.automation.push_back({
        "gainLane",
        talas::project::ScoreAutomationScope::Track,
        "voice",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        {},
    });
    return result;
}

struct PreparedFixture
{
    talas::project::PreparedProjectGenerator generator;
    std::array<std::uint16_t,
               talas::generative::maximumContextFreeOutputSymbols> current{};
    std::array<std::uint16_t,
               talas::generative::maximumContextFreeOutputSymbols> next{};
    std::array<std::uint16_t,
               talas::generative::maximumContextFreeOutputSymbols> output{};

    talas::generative::ContextFreeGeneratorScratch scratch()
    {
        return {current, next, output};
    }
};

PreparedFixture prepare(const talas::project::ProjectGeneratorProgram& source)
{
    talas::project::ProjectGeneratorLibrary library;
    library.programs.push_back(source);
    auto prepared = talas::project::prepareProjectGenerators(library);
    require(prepared.ok() && prepared.prepared.programs.size() == 1,
        "fixture generator and output profile should prepare");
    PreparedFixture result;
    result.generator = std::move(prepared.prepared.programs.front());
    return result;
}

void testStructuralValidationAndPreparedAuthority()
{
    auto source = program();
    require(
        !talas::project::projectGeneratorOutputValidationError(
            source.output,
            source.source.terminals),
        "closed terminal output profile should validate");

    source.output.bindings.front().terminal = "missing";
    auto error = talas::project::projectGeneratorOutputValidationError(
        source.output,
        source.source.terminals);
    require(
        error.has_value()
            && error->code
                == talas::project::ProjectGeneratorOutputValidationCode::UnknownTerminal,
        "binding must target one exact declared terminal");

    source = program();
    source.output.bindings.front().actions.push_back(
        talas::project::ProjectGeneratorScoreMarkerAction{"overflow", {}});
    error = talas::project::projectGeneratorOutputValidationError(
        source.output,
        source.source.terminals);
    require(
        error.has_value()
            && error->code
                == talas::project::ProjectGeneratorOutputValidationCode::TooManyActions,
        "one terminal must not exceed four ordered output actions");

    source = program();
    source.output.bindings.front().actions[0] =
        talas::project::ProjectGeneratorTimelineEventAction{
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            60.0,
            talas::core::AutomationInterpolation::Step,
        };
    source.output.bindings.front().actions[1] =
        talas::project::ProjectGeneratorTimelineEventAction{
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            61.0,
            talas::core::AutomationInterpolation::Step,
        };
    error = talas::project::projectGeneratorOutputValidationError(
        source.output,
        source.source.terminals);
    require(
        error.has_value()
            && error->code
                == talas::project::ProjectGeneratorOutputValidationCode::DuplicateTarget,
        "one terminal occurrence must not assign one Event Value twice");

    auto prepared = prepare(program());
    prepared.generator.output.bindings.front().actions.push_back(
        talas::project::ProjectGeneratorScoreMarkerAction{"mutated", {}});
    const auto mutated = talas::project::generateProjectTimelineEvents(
        prepared.generator.program,
        prepared.generator.output,
        prepared.scratch(),
        {0});
    require(
        !mutated.ok()
            && mutated.status
                == talas::project::ProjectGeneratorProjectionStatus::InvalidPreparedAuthority
            && mutated.events.empty(),
        "a public prepared profile mutated past its action bound must fail closed");
}

void testGraphNodeCatalogAuthorityIsClosed()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto oscillator =
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.oscillator",
            registry);
    require(
        oscillator.ok()
            && oscillator.descriptor->isSource()
            && oscillator.descriptor->audioOutputPortId == "audioOut",
        "the builtin default resource-empty oscillator should remain one approved generated source");

    talas::core::NodeRegistry resourceDefaultRegistry;
    auto resourceDefault = *registry.findNodeType("talas.oscillator");
    const auto wavetable = std::ranges::find(
        resourceDefault.parameters, std::string_view{"wavetable"},
        &talas::core::ParameterDescriptor::id);
    require(wavetable != resourceDefault.parameters.end(),
        "generator resource-safety fixture should expose Oscillator wavetable");
    wavetable->defaultResource = "Tables/UnsafeGeneratedDefault";
    resourceDefaultRegistry.addNodeType(std::move(resourceDefault));
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.oscillator", resourceDefaultRegistry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
        "generated immutable-read sources must reject a nonempty resource default");

    const auto filter =
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.filter",
            registry);
    require(
        filter.ok()
            && !filter.descriptor->isSource()
            && filter.descriptor->audioInputPortId
                == std::optional<std::string>{"audioIn"}
            && filter.descriptor->audioOutputPortId == "audioOut",
        "the builtin default filter should expose one canonical generated processor shape");

    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.iteratedWaveshaper", registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
        "generated processors must reject immutable resource authority");

    for (const auto nodeTypeId : {
             std::string_view{"talas.sample"},
             std::string_view{"talas.sumOfSines"},
             std::string_view{"talas.timeFrequencyScale"},
         })
    {
        const auto validation =
            talas::project::validateProjectGeneratorGraphNodeType(
                nodeTypeId, registry);
        // A Sample's stereo output pair (format 95) is refused as a port
        // shape before its resource is.
        require(
            !validation.ok()
                && (validation.diagnostic->code
                        == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability
                    || (nodeTypeId == "talas.sample"
                        && validation.diagnostic->code
                            == talas::project::ProjectGeneratorGraphNodeValidationCode::InvalidAudioPortShape)),
            "generated source defaults must reject resource-dependent type "
                + std::string{nodeTypeId});
    }

    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "../talas.oscillator",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::InvalidTypeId,
        "noncanonical node type text should reject before catalog lookup");
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.unavailable",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnknownType,
        "an unavailable node type should reject exactly");
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.parameterTransformer",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
        "executable-text transformer nodes must stay outside generated graphs");
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.contextFreeGrammar",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
        "a ContextFreeGrammar, which compiling expands, must stay outside generated graphs");
    for (const auto typeId : {
             std::string_view{"talas.limeInterpreter"},
             std::string_view{"talas.script"},
         })
    {
        require(
            talas::project::validateProjectGeneratorGraphNodeType(
                typeId,
                registry).diagnostic->code
                == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedExecution,
            std::string(typeId)
                + " compatibility placeholder must stay outside generated graphs");
    }
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.audioOutput",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedExecution,
        "device/output terminals must not be generated as reusable-Sound nodes");
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.feedbackLoopOutput",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedCapability,
        "structural feedback outputs must not become generated Sound sources");

    talas::core::NodeRegistry externalRegistry;
    talas::core::NodeTypeDescriptor external{
        "org.example.externalSource",
        "External Source",
        "External",
        {},
        {{"audioOut", talas::core::PortType::Audio, false}},
        {},
    };
    external.provenance.vendor = "Example";
    external.provenance.origin = talas::core::NodeOrigin::External;
    externalRegistry.addNodeType(std::move(external));
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "org.example.externalSource",
            externalRegistry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::UnsupportedOrigin,
        "external catalog entries must reject even when otherwise executable");

    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.crossfade",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::InvalidAudioPortShape,
        "ambiguous required Audio inputs must reject before planning");
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.filterBank",
            registry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::RequiredSideInput,
        "required non-Audio side inputs must reject before planning");

    talas::core::NodeRegistry oversizedRegistry;
    auto oversizedDescriptor = *registry.findNodeType("talas.oscillator");
    oversizedDescriptor.displayName.assign(1024U * 1024U + 1U, 'x');
    oversizedRegistry.addNodeType(std::move(oversizedDescriptor));
    require(
        talas::project::validateProjectGeneratorGraphNodeType(
            "talas.oscillator",
            oversizedRegistry).diagnostic->code
            == talas::project::ProjectGeneratorGraphNodeValidationCode::DescriptorLimitExceeded,
        "catalog descriptors must reject before unbounded field traversal or projection copying");

    auto source = document();
    auto generated = program();
    generated.output.bindings.front().actions = {
        talas::project::ProjectGeneratorGraphNodeAction{"talas.oscillator"}};
    source.generatorLibrary.programs.push_back(std::move(generated));
    require(
        talas::project::validateProjectGeneratorOutputTargets(
            source,
            registry).ok(),
        "shared document target authority should accept approved graph actions");
    std::get<talas::project::ProjectGeneratorGraphNodeAction>(
        source.generatorLibrary.programs.front()
            .output.bindings.front().actions.front()).nodeTypeId =
        "talas.parameterTransformer";
    const auto rejected =
        talas::project::validateProjectGeneratorOutputTargets(
            source,
            registry);
    require(
        !rejected.ok()
            && rejected.diagnostics.front().code
                == talas::project::ProjectGeneratorTargetValidationCode::InvalidGraphNodeType
            && rejected.diagnostics.front().path
                == "$.generatorLibrary.programs[0].output.bindings[0].actions[0].nodeTypeId",
        "shared document target authority should reject disallowed graph actions at their exact path");
}

void testTimelineProjectionIsDeterministicAndFailureAtomic()
{
    auto fixture = prepare(program());
    auto timeline = talas::project::generateProjectTimelineEvents(
        fixture.generator.program,
        fixture.generator.output,
        fixture.scratch(),
        {1000000});
    require(
        timeline.ok()
            && timeline.generatedSymbols == 3
            && timeline.events.size() == 2
            && timeline.events[0].timeMicroseconds == 1000000
            && timeline.events[1].timeMicroseconds == 1500000
            && timeline.events[0].eventName == "Pitch"
            && timeline.events[0].domain
                == talas::expr::ValueDomain::NoteNumber,
        "terminal order should map to exact integer-microsecond Timeline events while rests advance the grid");

    auto replayFixture = prepare(program());
    const auto replay = talas::project::generateProjectTimelineEvents(
        replayFixture.generator.program,
        replayFixture.generator.output,
        replayFixture.scratch(),
        {1000000});
    require(
        replay.ok() && replay.events == timeline.events,
        "same generator, output profile, and base must replay exact Timeline events");

    fixture.generator.output.timelineStepMicroseconds =
        talas::project::maximumProjectGeneratorTimelineMicroseconds;
    const auto overflow = talas::project::generateProjectTimelineEvents(
        fixture.generator.program,
        fixture.generator.output,
        fixture.scratch(),
        {talas::project::maximumProjectGeneratorTimelineMicroseconds});
    require(
        !overflow.ok()
            && overflow.status
                == talas::project::ProjectGeneratorProjectionStatus::TimeOverflow
            && overflow.events.empty(),
        "Timeline horizon overflow must publish no partial event prefix");

    auto stoppedFixture = prepare(program());
    std::stop_source stopped;
    stopped.request_stop();
    const auto cancelled = talas::project::generateProjectTimelineEvents(
        stoppedFixture.generator.program,
        stoppedFixture.generator.output,
        stoppedFixture.scratch(),
        {0},
        stopped.get_token());
    require(
        !cancelled.ok()
            && cancelled.status
                == talas::project::ProjectGeneratorProjectionStatus::Cancelled
            && cancelled.events.empty(),
        "cancelled Timeline generation must retain no partial output");
}

void testScoreProjectionUsesStableTargetsAndOneCandidate()
{
    const auto sourceDocument = document();
    auto fixture = prepare(program());
    const auto score = talas::project::generateProjectScoreEdits(
        fixture.generator.program,
        fixture.generator.output,
        fixture.scratch(),
        sourceDocument,
        {100, "take"});
    require(
        score.ok()
            && score.generatedSymbols == 3
            && score.edits.clips.size() == 2
            && score.edits.clips[0].clipId == "take_c0_1"
            && score.edits.clips[1].clipId == "take_c2_1"
            && score.edits.clips[0].start.tick == 100
            && score.edits.clips[1].start.tick == 2020
            && score.edits.markers.size() == 2
            && score.edits.automation.size() == 1
            && score.edits.automation.front().points.size() == 2
            && sourceDocument.timeline.score.clips.empty(),
        "Score projection should derive deterministic IDs and musical ticks without mutating its source document");

    std::vector<std::string_view> definitions{"voiceDefinition"};
    const auto applied = talas::project::projectScoreWithEdits(
        sourceDocument.timeline.score,
        score.edits,
        definitions);
    require(applied.ok(),
        "generated Score edit batch should pass the sole Score candidate authority");

    auto occupied = sourceDocument;
    occupied.timeline.score = *applied.candidate;
    auto collisionFixture = prepare(program());
    const auto collision = talas::project::generateProjectScoreEdits(
        collisionFixture.generator.program,
        collisionFixture.generator.output,
        collisionFixture.scratch(),
        occupied,
        {100, "take"});
    require(
        !collision.ok()
            && collision.status
                == talas::project::ProjectGeneratorProjectionStatus::IdentityCollision
            && collision.edits.clips.empty()
            && collision.edits.markers.empty()
            && collision.edits.automation.empty(),
        "reusing a deterministic output namespace must reject the whole colliding batch");

    auto missingTarget = sourceDocument;
    missingTarget.timeline.score.automation.clear();
    auto missingFixture = prepare(program());
    const auto rejected = talas::project::generateProjectScoreEdits(
        missingFixture.generator.program,
        missingFixture.generator.output,
        missingFixture.scratch(),
        missingTarget,
        {0, "missing"});
    require(
        !rejected.ok()
            && rejected.status
                == talas::project::ProjectGeneratorProjectionStatus::InvalidTarget
            && rejected.edits.clips.empty(),
        "one missing exact target must discard every generated Score edit");
}

void testCrossDocumentTargetAuthorityIsExact()
{
    auto source = document();
    source.generatorLibrary.programs.push_back(program());
    require(
        talas::project::validateProjectGeneratorOutputTargets(source).ok(),
        "all generator outputs should resolve through exact document targets");

    source.timeline.score.automation.clear();
    const auto missingLane =
        talas::project::validateProjectGeneratorOutputTargets(source);
    require(
        !missingLane.ok()
            && missingLane.diagnostics.size() == 1
            && missingLane.diagnostics.front().code
                == talas::project::ProjectGeneratorTargetValidationCode::UnknownAutomationLane
            && missingLane.diagnostics.front().path
                == "$.generatorLibrary.programs[0].output.bindings[0].actions[2].laneId",
        "a missing exact Score lane should fail at the persisted action path");

    source = document();
    source.generatorLibrary.programs.push_back(program());
    source.timeline.score.tracks.front().timeDomain =
        talas::project::ScoreTimeDomain::Seconds;
    const auto wrongTrackDomain =
        talas::project::validateProjectGeneratorOutputTargets(source);
    require(
        !wrongTrackDomain.ok()
            && wrongTrackDomain.diagnostics.front().code
                == talas::project::ProjectGeneratorTargetValidationCode::InvalidScoreTrackDomain,
        "generated clips should require an exact musical-tick Score track");

    source = document();
    source.generatorLibrary.programs.push_back(program());
    source.timeline.score.tracks.front().timeDomain =
        talas::project::ScoreTimeDomain::Seconds;
    auto& clipAction = std::get<
        talas::project::ProjectGeneratorScoreClipAction>(
        source.generatorLibrary.programs.front()
            .output.bindings.front().actions[1]);
    clipAction.trackId = "missingTrack";
    const auto wrongAutomationDomain =
        talas::project::validateProjectGeneratorOutputTargets(source);
    require(
        std::ranges::any_of(
            wrongAutomationDomain.diagnostics,
            [](const auto& diagnostic) {
                return diagnostic.code
                    == talas::project::ProjectGeneratorTargetValidationCode::InvalidAutomationLaneDomain;
            }),
        "generated automation should require a musical-tick owner lane");

    source = document();
    source.generatorLibrary.programs.push_back(program());
    source.timeline.score.tracks.resize(
        talas::project::maximumScoreTracks + 1U);
    const auto oversized =
        talas::project::validateProjectGeneratorOutputTargets(source);
    require(
        !oversized.ok()
            && oversized.diagnostics.size() == 1
            && oversized.diagnostics.front().code
                == talas::project::ProjectGeneratorTargetValidationCode::TargetModelLimitExceeded,
        "raw oversized target models should reject before target-map allocation");

    talas::project::ProjectDocument aggregateBoundary;
    aggregateBoundary.virtualControls.resize(
        talas::project::maximumProjectGeneratorTargetRecords / 2U);
    aggregateBoundary.timeline.eventValues.resize(
        talas::project::maximumProjectGeneratorTargetRecords / 2U);
    require(
        talas::project::validateProjectGeneratorOutputTargets(
            aggregateBoundary).ok(),
        "the exact aggregate 65,536-record inspection boundary should remain accepted");
    aggregateBoundary.soundDefinitions.resize(1U);
    const auto aggregateOversized =
        talas::project::validateProjectGeneratorOutputTargets(
            aggregateBoundary);
    require(
        !aggregateOversized.ok()
            && aggregateOversized.diagnostics.size() == 1
            && aggregateOversized.diagnostics.front().code
                == talas::project::ProjectGeneratorTargetValidationCode::TargetModelLimitExceeded,
        "individually bounded target collections must share one aggregate inspection budget");

    auto oversizedFixture = prepare(program());
    const auto oversizedProjection =
        talas::project::generateProjectScoreEdits(
            oversizedFixture.generator.program,
            oversizedFixture.generator.output,
            oversizedFixture.scratch(),
            source,
            {0, "oversized"});
    require(
        !oversizedProjection.ok()
            && oversizedProjection.status
                == talas::project::ProjectGeneratorProjectionStatus::CapacityExceeded,
        "Score projection should reject raw oversized documents before lookup allocation");
}


void testExactProjectionBoundaries()
{
    auto timelineSource = maximumProgram(
        talas::project::ProjectGeneratorTimelineEventAction{
            "Value",
            talas::expr::ValueDomain::Scalar,
            1.0,
            talas::core::AutomationInterpolation::Step,
        });
    auto timelineFixture = prepare(timelineSource);
    const auto capacity = talas::project::projectGeneratorProjectionCapacity(
        timelineFixture.generator.program,
        timelineFixture.generator.output);
    require(
        capacity.has_value()
            && capacity->scratchSymbols == 4096
            && capacity->maximumTimelineActions == 4096
            && capacity->maximumScoreActions == 0
            && capacity->maximumGraphActions == 0,
        "capacity query should expose the checked worst-case scratch and action bounds");
    const auto timeline = talas::project::generateProjectTimelineEvents(
        timelineFixture.generator.program,
        timelineFixture.generator.output,
        timelineFixture.scratch(),
        {0});
    require(
        timeline.ok()
            && timeline.events.size()
                == talas::project::maximumProjectGeneratorTimelineEvents
            && timeline.events.back().timeMicroseconds == 4095,
        "the exact 4,096-event Timeline boundary should remain accepted");

    timelineSource.output.bindings.front().actions.push_back(
        talas::project::ProjectGeneratorTimelineEventAction{
            "Other",
            talas::expr::ValueDomain::Scalar,
            2.0,
            talas::core::AutomationInterpolation::Step,
        });
    auto overflowFixture = prepare(timelineSource);
    const auto overflow = talas::project::generateProjectTimelineEvents(
        overflowFixture.generator.program,
        overflowFixture.generator.output,
        overflowFixture.scratch(),
        {0});
    require(
        !overflow.ok()
            && overflow.status
                == talas::project::ProjectGeneratorProjectionStatus::CapacityExceeded
            && overflow.events.empty(),
        "one event beyond the fixed projection capacity must discard the whole candidate");

    auto scoreFixture = prepare(maximumProgram(
        talas::project::ProjectGeneratorScoreMarkerAction{"Boundary", {}}));
    const auto score = talas::project::generateProjectScoreEdits(
        scoreFixture.generator.program,
        scoreFixture.generator.output,
        scoreFixture.scratch(),
        document(),
        {0, "limit"});
    require(
        score.ok()
            && score.edits.markers.size()
                == talas::project::maximumProjectGeneratorScoreEdits
            && score.edits.markers.back().start.tick == 4095,
        "the exact 4,096-entity Score boundary should remain accepted");
}


void testSoundProjectionBuildsOneDeterministicValidatedChain()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto fixture = prepare(graphProgram());
    const auto sourceDocument = document();
    const talas::project::ProjectGeneratorSoundRequest request{
        "generatedVoice",
        "Generated Voice",
        "generated",
    };
    const auto projected = talas::project::generateProjectSoundDefinition(
        fixture.generator.program,
        fixture.generator.output,
        fixture.scratch(),
        sourceDocument,
        registry,
        request);
    require(
        projected.ok() && projected.definition != nullptr,
        "source plus processor should produce one immutable reusable Sound candidate");
    const auto& definition = *projected.definition;
    require(
        definition.definitionId == "generatedVoice"
            && definition.displayName == "Generated Voice"
            && definition.inputs.empty()
            && definition.outputs.size() == 1
            && definition.outputs.front()
                == talas::project::ProjectSoundPort{
                    "audioOut",
                    talas::core::PortType::Audio,
                    "generated_n1",
                    "audioOut",
                },
        "generated Sound should expose only the final canonical Audio output");
    require(
        definition.graph.nodes.size() == 2
            && definition.graph.nodes[0].instanceId == "generated_n0"
            && definition.graph.nodes[0].typeId == "talas.oscillator"
            && definition.graph.nodes[0].parameters.empty()
            && definition.graph.nodes[1].instanceId == "generated_n1"
            && definition.graph.nodes[1].typeId == "talas.filter"
            && definition.graph.nodes[1].parameters.empty(),
        "graph action order should derive stable node identities and catalog defaults");
    require(
        definition.graph.connections
            == std::vector<talas::core::Connection>{
                {
                    "generated_n0",
                    "audioOut",
                    "generated_n1",
                    "audioIn",
                },
            },
        "each processor should connect only to its immediate predecessor");

    auto replayFixture = prepare(graphProgram());
    const auto replay = talas::project::generateProjectSoundDefinition(
        replayFixture.generator.program,
        replayFixture.generator.output,
        replayFixture.scratch(),
        sourceDocument,
        registry,
        request);
    require(
        replay.ok() && *replay.definition == definition
            && replay.catalogAuthorityFingerprint
                == projected.catalogAuthorityFingerprint
            && replay.catalogAuthorityFingerprint.has_value(),
        "exact generator, seed, request, document, and catalog authority should replay byte-equivalent structure");

    talas::core::NodeRegistry changedRegistry;
    for (auto descriptor : registry.nodeTypes())
    {
        if (descriptor.typeId == "talas.oscillator")
            descriptor.parameters.front().defaultExpression = "441 hz";
        changedRegistry.addNodeType(std::move(descriptor));
    }
    auto changedFixture = prepare(graphProgram());
    const auto changedCatalog =
        talas::project::generateProjectSoundDefinition(
            changedFixture.generator.program,
            changedFixture.generator.output,
            changedFixture.scratch(),
            sourceDocument,
            changedRegistry,
            request);
    require(
        changedCatalog.ok()
            && *changedCatalog.definition == definition
            && changedCatalog.catalogAuthorityFingerprint
                != projected.catalogAuthorityFingerprint,
        "catalog authority must detect changed defaults even when the generated node IDs and type IDs are unchanged");

    auto singleFixture = prepare(graphProgram("start:source"));
    const auto single = talas::project::generateProjectSoundDefinition(
        singleFixture.generator.program,
        singleFixture.generator.output,
        singleFixture.scratch(),
        sourceDocument,
        registry,
        {"singleSource", "Single Source", "single"});
    require(
        single.ok()
            && single.definition->graph.nodes.size() == 1
            && single.definition->graph.connections.empty()
            && single.definition->outputs.front().nodeId == "single_n0",
        "one approved source should form the exact one-node Sound boundary");
}

void testSoundProjectionRejectsInvalidSequenceAndAuthorityAtomically()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto sourceDocument = document();
    const auto project = [&](talas::project::ProjectGeneratorProgram source) {
        auto fixture = prepare(source);
        return talas::project::generateProjectSoundDefinition(
            fixture.generator.program,
            fixture.generator.output,
            fixture.scratch(),
            sourceDocument,
            registry,
            {"candidate", "Candidate", "candidate"});
    };

    const auto empty = project(graphProgram("start:"));
    require(
        !empty.ok() && empty.definition == nullptr
            && empty.status
                == talas::project::ProjectGeneratorProjectionStatus::InvalidGraphSequence,
        "empty generated graph output must publish no partial Sound");
    const auto missingSource = project(graphProgram("start:processor"));
    require(
        !missingSource.ok() && missingSource.definition == nullptr
            && missingSource.status
                == talas::project::ProjectGeneratorProjectionStatus::InvalidGraphSequence,
        "a generated processor cannot become an implicit source");
    const auto secondSource = project(graphProgram("start:source source"));
    require(
        !secondSource.ok() && secondSource.definition == nullptr
            && secondSource.status
                == talas::project::ProjectGeneratorProjectionStatus::InvalidGraphSequence,
        "a source after the first graph action must reject the whole chain");

    auto unknown = graphProgram("start:source");
    unknown.output.bindings[0].actions[0] =
        talas::project::ProjectGeneratorGraphNodeAction{"talas.unknown"};
    const auto invalidTarget = project(std::move(unknown));
    require(
        !invalidTarget.ok() && invalidTarget.definition == nullptr
            && invalidTarget.status
                == talas::project::ProjectGeneratorProjectionStatus::InvalidTarget,
        "planner must revalidate exact catalog authority before constructing nodes");

    auto collisionDocument = sourceDocument;
    collisionDocument.soundDefinitions.push_back({
        "candidate",
        "Existing",
    });
    auto collisionFixture = prepare(graphProgram("start:source"));
    const auto collision = talas::project::generateProjectSoundDefinition(
        collisionFixture.generator.program,
        collisionFixture.generator.output,
        collisionFixture.scratch(),
        collisionDocument,
        registry,
        {"candidate", "Candidate", "candidate"});
    require(
        !collision.ok() && collision.definition == nullptr
            && collision.status
                == talas::project::ProjectGeneratorProjectionStatus::IdentityCollision,
        "definition collisions must reject before publishing a candidate");

    talas::project::ProjectDocument fullSoundLibrary;
    fullSoundLibrary.soundDefinitions.resize(
        talas::project::maximumProjectGeneratorTargetRecords);
    auto fullLibraryFixture = prepare(graphProgram("start:source"));
    const auto fullLibrary =
        talas::project::generateProjectSoundDefinition(
            fullLibraryFixture.generator.program,
            fullLibraryFixture.generator.output,
            fullLibraryFixture.scratch(),
            fullSoundLibrary,
            registry,
            {"capacity", "Capacity", "capacity"});
    require(
        !fullLibrary.ok() && fullLibrary.definition == nullptr
            && fullLibrary.status
                == talas::project::ProjectGeneratorProjectionStatus::CapacityExceeded,
        "an exact-cap Sound library must reject a 65,537th definition before generation");

    std::stop_source stop;
    stop.request_stop();
    auto cancelledFixture = prepare(graphProgram());
    const auto cancelled = talas::project::generateProjectSoundDefinition(
        cancelledFixture.generator.program,
        cancelledFixture.generator.output,
        cancelledFixture.scratch(),
        sourceDocument,
        registry,
        {"cancelled", "Cancelled", "cancelled"},
        stop.get_token());
    require(
        !cancelled.ok() && cancelled.definition == nullptr
            && cancelled.status
                == talas::project::ProjectGeneratorProjectionStatus::Cancelled,
        "cancellation must leave no reusable Sound candidate");
}

void testSoundProjectionEnforcesExactGraphCapacity()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto exactFixture = prepare(graphCapacityProgram(false));
    const auto exactCapacity = talas::project::projectGeneratorProjectionCapacity(
        exactFixture.generator.program,
        exactFixture.generator.output);
    require(
        exactCapacity.has_value()
            && exactCapacity->maximumTimelineActions == 0
            && exactCapacity->maximumScoreActions == 0
            && exactCapacity->maximumGraphActions == 256,
        "shared capacity authority should count graph actions independently");
    const auto exact = talas::project::generateProjectSoundDefinition(
        exactFixture.generator.program,
        exactFixture.generator.output,
        exactFixture.scratch(),
        document(),
        registry,
        {"maximumChain", "Maximum Chain", "maximum"});
    require(
        exact.ok()
            && exact.definition->graph.nodes.size()
                == talas::project::maximumProjectGeneratorGraphNodes
            && exact.definition->graph.connections.size() == 255,
        "the exact 256-node and 255-connection Sound boundary should remain accepted");

    auto overflowFixture = prepare(graphCapacityProgram(true));
    const auto overflow = talas::project::generateProjectSoundDefinition(
        overflowFixture.generator.program,
        overflowFixture.generator.output,
        overflowFixture.scratch(),
        document(),
        registry,
        {"overflowChain", "Overflow Chain", "overflow"});
    require(
        !overflow.ok() && overflow.definition == nullptr
            && overflow.status
                == talas::project::ProjectGeneratorProjectionStatus::CapacityExceeded,
        "the 257th generated node must discard the entire candidate");
}

void testGeneratedSoundCommitUsesExactDocumentCatalogAndOneUndo()
{
    auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto source = document();
    talas::project::ProjectEditor editor{source, registry};
    const auto originalBytes =
        talas::project::serializeProjectDocument(editor.document());
    const auto authority = talas::project::ProjectGeneratorEditAuthority{
        editor.documentAuthorityGeneration(),
        originalBytes,
    };
    auto fixture = prepare(graphProgram());
    auto planned = talas::project::generateProjectSoundDefinition(
        fixture.generator.program,
        fixture.generator.output,
        fixture.scratch(),
        editor.document(),
        registry,
        {"generatedVoice", "Generated Voice", "generated"});
    require(
        planned.ok()
            && editor.applyGeneratedSoundDefinition(
                planned,
                authority).ok()
            && editor.document().soundDefinitions.size()
                == source.soundDefinitions.size() + 1U
            && editor.document().soundDefinitions.back()
                == *planned.definition,
        "exact generated Sound candidate should append through ProjectEditor once");
    const auto generatedBytes =
        talas::project::serializeProjectDocument(editor.document());
    require(
        editor.undo().ok()
            && talas::project::serializeProjectDocument(editor.document())
                == originalBytes
            && !editor.canUndo(),
        "generated Sound append should consume exactly one isolated Undo step");
    require(
        editor.redo().ok()
            && talas::project::serializeProjectDocument(editor.document())
                == generatedBytes,
        "generated Sound append should redo as the same complete candidate");

    talas::project::ProjectDocument aggregateDocument;
    aggregateDocument.timeline.eventValues.assign(
        talas::project::maximumProjectGeneratorTargetRecords,
        talas::project::TimelineEventValue{
            0.0,
            "Control",
            talas::expr::ValueDomain::Scalar,
            0.0,
            talas::core::AutomationInterpolation::Step,
        });
    talas::project::ProjectEditor aggregateEditor{
        aggregateDocument,
        registry};
    const auto aggregateAuthority =
        talas::project::ProjectGeneratorEditAuthority{
            aggregateEditor.documentAuthorityGeneration(),
            talas::project::serializeProjectDocument(
                aggregateEditor.document()),
        };
    auto aggregateFixture = prepare(graphProgram());
    auto aggregatePlan = talas::project::generateProjectSoundDefinition(
        aggregateFixture.generator.program,
        aggregateFixture.generator.output,
        aggregateFixture.scratch(),
        talas::project::ProjectDocument{},
        registry,
        {"aggregateVoice", "Aggregate Voice", "aggregate"});
    const auto aggregateRejected =
        aggregateEditor.applyGeneratedSoundDefinition(
            std::move(aggregatePlan),
            aggregateAuthority);
    require(
        !aggregateRejected.ok()
            && aggregateRejected.error
                == talas::project::EditError::InvalidProjectGenerator
            && aggregateEditor.document().soundDefinitions.empty()
            && !aggregateEditor.canUndo(),
        "ProjectEditor must reject an append that would exceed the aggregate record authority");

    talas::project::ProjectEditor staleEditor{source, registry};
    const auto staleAuthority = talas::project::ProjectGeneratorEditAuthority{
        staleEditor.documentAuthorityGeneration(),
        talas::project::serializeProjectDocument(staleEditor.document()),
    };
    auto staleFixture = prepare(graphProgram());
    auto stalePlan = talas::project::generateProjectSoundDefinition(
        staleFixture.generator.program,
        staleFixture.generator.output,
        staleFixture.scratch(),
        staleEditor.document(),
        registry,
        {"staleVoice", "Stale Voice", "stale"});
    require(
        staleEditor.renameSoundDefinition(
            "voiceDefinition",
            "Temporary Voice").ok()
            && staleEditor.undo().ok()
            && talas::project::serializeProjectDocument(
                staleEditor.document())
                == staleAuthority.canonicalDocumentBytes,
        "generated Sound ABA fixture should restore exact project bytes");
    const auto staleGeneration =
        staleEditor.documentAuthorityGeneration();
    const auto stale = staleEditor.applyGeneratedSoundDefinition(
        std::move(stalePlan),
        staleAuthority);
    require(
        !stale.ok()
            && stale.error
                == talas::project::EditError::StaleProjectGeneratorOutput
            && staleEditor.document().soundDefinitions.size()
                == source.soundDefinitions.size()
            && staleEditor.documentAuthorityGeneration()
                == staleGeneration,
        "same-bytes edit-undo ABA must reject without changing document, history, or authority");

    auto catalogRegistry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor catalogEditor{source, catalogRegistry};
    const auto catalogAuthority = talas::project::ProjectGeneratorEditAuthority{
        catalogEditor.documentAuthorityGeneration(),
        talas::project::serializeProjectDocument(catalogEditor.document()),
    };
    auto catalogFixture = prepare(graphProgram());
    auto catalogPlan = talas::project::generateProjectSoundDefinition(
        catalogFixture.generator.program,
        catalogFixture.generator.output,
        catalogFixture.scratch(),
        catalogEditor.document(),
        catalogRegistry,
        {"catalogVoice", "Catalog Voice", "catalog"});
    catalogRegistry.setNodeTypeSupport(
        "talas.filter",
        talas::core::NodeSupportLevel::FullRuntime,
        "Changed catalog authority");
    const auto beforeCatalogReject =
        talas::project::serializeProjectDocument(catalogEditor.document());
    const auto catalogRejected =
        catalogEditor.applyGeneratedSoundDefinition(
            std::move(catalogPlan),
            catalogAuthority);
    require(
        !catalogRejected.ok()
            && catalogRejected.error
                == talas::project::EditError::StaleProjectGeneratorOutput
            && talas::project::serializeProjectDocument(
                catalogEditor.document()) == beforeCatalogReject
            && !catalogEditor.canUndo(),
        "changed selected catalog descriptor must reject without resolving the old type ID anew");
}

void testGeneratedSoundCommitRevalidatesCompleteCandidate()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto source = document();
    talas::project::ProjectEditor editor{source, registry};
    const auto authority = talas::project::ProjectGeneratorEditAuthority{
        editor.documentAuthorityGeneration(),
        talas::project::serializeProjectDocument(editor.document()),
    };
    auto fixture = prepare(graphProgram());
    auto planned = talas::project::generateProjectSoundDefinition(
        fixture.generator.program,
        fixture.generator.output,
        fixture.scratch(),
        editor.document(),
        registry,
        {"invalidVoice", "Invalid Voice", "invalid"});
    auto invalidDefinition = *planned.definition;
    invalidDefinition.graph.nodes[1].instanceId =
        invalidDefinition.graph.nodes[0].instanceId;
    planned.definition =
        std::make_shared<const talas::project::ProjectSoundDefinition>(
            std::move(invalidDefinition));
    const auto before =
        talas::project::serializeProjectDocument(editor.document());
    const auto generation = editor.documentAuthorityGeneration();
    const auto rejected = editor.applyGeneratedSoundDefinition(
        std::move(planned),
        authority);
    require(
        !rejected.ok()
            && rejected.error
                == talas::project::EditError::InvalidProjectGenerator
            && talas::project::serializeProjectDocument(editor.document())
                == before
            && editor.documentAuthorityGeneration() == generation
            && !editor.canUndo(),
        "complete candidate node collisions must reject atomically before history publication");
}
}

int main()
{
    testStructuralValidationAndPreparedAuthority();
    testGraphNodeCatalogAuthorityIsClosed();
    testTimelineProjectionIsDeterministicAndFailureAtomic();
    testScoreProjectionUsesStableTargetsAndOneCandidate();
    testCrossDocumentTargetAuthorityIsExact();
    testExactProjectionBoundaries();
    testSoundProjectionBuildsOneDeterministicValidatedChain();
    testSoundProjectionRejectsInvalidSequenceAndAuthorityAtomically();
    testSoundProjectionEnforcesExactGraphCapacity();
    testGeneratedSoundCommitUsesExactDocumentCatalogAndOneUndo();
    testGeneratedSoundCommitRevalidatesCompleteCandidate();
    return 0;
}
