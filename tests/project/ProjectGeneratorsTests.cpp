#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectGenerators.h"
#include "talas/project/ProjectJson.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

void eraseEmptyLiveSetLibrary(std::string& source)
{
    constexpr std::string_view field =
        "  \"liveSetLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"liveSets\": [],\n"
        "    \"tools\": []\n"
        "  },\n";
    const auto position = source.find(field);
    require(position != std::string::npos,
        "current fixture should expose the live-set boundary");
    source.erase(position, field.size());
}

void eraseTrackMemorySharingOption(std::string& source)
{
    constexpr std::string_view field =
        ",\n          \"sharingMemoryWriters\": false";
    for (auto position = source.find(field);
         position != std::string::npos;
         position = source.find(field))
        source.erase(position, field.size());
}

talas::project::ProjectGeneratorProgram generator(
    std::string id = "phrase")
{
    talas::project::ProjectGeneratorProgram program;
    program.generatorId = std::move(id);
    program.displayName = "Phrase Generator";
    program.source.terminals = {"note", "rest"};
    program.source.grammar =
        "start:phrase\n"
        "phrase ->note phrase|rest phrase|note";
    program.source.seed = std::numeric_limits<std::uint64_t>::max();
    program.source.maximumRewritePasses = 8;
    program.source.maximumOutputSymbols = 64;
    return program;
}

talas::project::ProjectGeneratorProgram generatorWithOutput(
    std::string id = "phrase")
{
    auto program = generator(std::move(id));
    talas::project::ProjectGeneratorTerminalBinding output;
    output.terminal = "note";
    output.actions = {
        talas::project::ProjectGeneratorTimelineEventAction{
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            60.0,
            talas::core::AutomationInterpolation::Step,
        },
        talas::project::ProjectGeneratorScoreClipAction{
            "voice",
            "voiceDefinition",
            960,
            0,
            120,
            120,
            1,
        },
        talas::project::ProjectGeneratorScoreAutomationAction{
            "gainLane",
            talas::expr::ValueDomain::Scalar,
            0.5,
            talas::core::AutomationInterpolation::Linear,
        },
        talas::project::ProjectGeneratorScoreMarkerAction{
            "Generated",
            std::nullopt,
        },
    };
    program.output.timelineStepMicroseconds = 250000;
    program.output.scoreStepTicks = 480;
    program.output.bindings.push_back(std::move(output));
    return program;
}

void addGeneratorOutputTargets(talas::project::ProjectDocument& document)
{
    document.virtualControls.push_back({
        "Pitch",
        talas::expr::ValueDomain::NoteNumber,
        talas::project::VirtualControlKind::Fader,
        60.0,
        0.0,
        127.0,
    });
    document.soundDefinitions.push_back({"voiceDefinition", "Voice"});
    document.timeline.score.tracks = {
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
    document.timeline.score.automation.push_back({
        "gainLane",
        talas::project::ScoreAutomationScope::Track,
        "voice",
        "Gain",
        talas::expr::ValueDomain::Scalar,
        {},
    });
}

void testValidationAndExactRanges()
{
    talas::project::ProjectGeneratorLibrary library;
    library.programs.push_back(generator());
    require(
        talas::project::validateProjectGeneratorLibrary(library).ok(),
        "valid bounded generator library should pass");

    library.programs.push_back(generator());
    const auto duplicate =
        talas::project::validateProjectGeneratorLibrary(library);
    require(
        !duplicate.ok()
            && duplicate.diagnostics.size() == 1
            && duplicate.diagnostics.front().code
                == talas::project::ProjectGeneratorValidationCode::DuplicateProgram
            && duplicate.diagnostics.front().path
                == "$.generatorLibrary.programs[1].generatorId",
        "duplicate generator identities should fail at the exact declaration");

    library.programs.resize(1);
    library.programs.front().source.grammar =
        "start: missing\n"
        "phrase -> note\n";
    const auto invalid =
        talas::project::validateProjectGeneratorLibrary(library);
    require(
        !invalid.ok()
            && !invalid.diagnostics.empty()
            && invalid.diagnostics.front().path
                == "$.generatorLibrary.programs[0].source"
            && invalid.diagnostics.front().sourceRange.end
                > invalid.diagnostics.front().sourceRange.begin,
        "grammar diagnostics should retain an exact source range");

    library.programs.clear();
    for (std::size_t programIndex = 0;
         programIndex < talas::generative::maximumContextFreePrograms;
         ++programIndex)
    {
        auto large = generator("large" + std::to_string(programIndex));
        large.output.bindings.clear();
        large.source.terminals.clear();
        for (std::size_t terminalIndex = 0;
             terminalIndex < talas::generative::maximumContextFreeSymbols;
             ++terminalIndex)
        {
            auto id = "t" + std::to_string(terminalIndex) + "_";
            id.append(
                talas::generative::maximumContextFreeSymbolBytes - id.size(),
                'x');
            large.source.terminals.push_back(std::move(id));
        }
        large.source.grammar =
            "start: " + large.source.terminals.front() + "\n";
        library.programs.push_back(std::move(large));
    }
    const auto aggregate =
        talas::project::validateProjectGeneratorLibrary(library);
    require(
        !aggregate.ok()
            && aggregate.diagnostics.back().code
                == talas::project::ProjectGeneratorValidationCode::TooMuchSource,
        "terminal declarations should participate in the aggregate source budget");
}

void testCurrentRoundTripAndFormatThirtyTwoGraphAdmission()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    addGeneratorOutputTargets(document);
    document.generatorLibrary.programs.push_back(generatorWithOutput());
    const auto canonical = talas::project::serializeProjectDocument(document);
    require(
        canonical.find("\"version\": 156") != std::string::npos
            && canonical.find("\"generatorLibrary\": {")
                != std::string::npos
            && canonical.find("\"version\": 2")
                != std::string::npos
            && canonical.find("\"kind\": \"scoreAutomationPoint\"")
                != std::string::npos
            && canonical.find("\"seed\": 18446744073709551615")
                != std::string::npos,
        "current format should persist exact generator and output authority");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.generatorLibrary
                == document.generatorLibrary
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "current generator bytes should reopen canonically");

    auto formatThirtyTwo = canonical;
    const auto formatThirtyTwoVersion =
        formatThirtyTwo.find("\"version\": 156");
    require(
        formatThirtyTwoVersion != std::string::npos,
        "current generator fixture should expose its format version");
    formatThirtyTwo.replace(
        formatThirtyTwoVersion,
        std::string_view{"\"version\": 156"}.size(),
        "\"version\":  32");
    eraseEmptyLiveSetLibrary(formatThirtyTwo);
    eraseTrackMemorySharingOption(formatThirtyTwo);
    const auto migratedThirtyTwo =
        talas::project::parseProjectDocument(formatThirtyTwo);
    require(
        migratedThirtyTwo.ok()
            && migratedThirtyTwo.document.formatVersion == talas::project::currentProjectFormatVersion
            && migratedThirtyTwo.document.generatorLibrary
                == document.generatorLibrary,
        "format 32 graph/output authority should migrate losslessly into format 33");

    auto formatThirtyOne = canonical;
    const auto priorVersion = formatThirtyOne.find("\"version\": 156");
    require(
        priorVersion != std::string::npos,
        "current generator fixture should expose its format version");
    formatThirtyOne.replace(
        priorVersion,
        std::string_view{"\"version\": 156"}.size(),
        "\"version\":  31");
    eraseEmptyLiveSetLibrary(formatThirtyOne);
    eraseTrackMemorySharingOption(formatThirtyOne);
    const auto migratedThirtyOne =
        talas::project::parseProjectDocument(formatThirtyOne);
    require(
        migratedThirtyOne.ok()
            && migratedThirtyOne.document.formatVersion == talas::project::currentProjectFormatVersion
            && migratedThirtyOne.document.generatorLibrary
                == document.generatorLibrary,
        "format 31 output profiles should migrate losslessly without inventing graph actions");

    talas::project::ProjectDocument graphDocument;
    graphDocument.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    auto graphProgram = generator("chain");
    graphProgram.output.bindings.push_back({
        "note",
        {
            talas::project::ProjectGeneratorGraphNodeAction{"talas.oscillator"},
            talas::project::ProjectGeneratorGraphNodeAction{"talas.filter"},
        },
    });
    graphDocument.generatorLibrary.programs.push_back(graphProgram);
    const auto graphCanonical =
        talas::project::serializeProjectDocument(graphDocument);
    const auto graphReopened =
        talas::project::parseProjectDocument(graphCanonical);
    require(
        graphCanonical.find("\"kind\": \"graphNode\"")
                != std::string::npos
            && graphCanonical.find("\"nodeTypeId\": \"talas.oscillator\"")
                != std::string::npos
            && graphReopened.ok()
            && graphReopened.document.generatorLibrary
                == graphDocument.generatorLibrary
            && talas::project::serializeProjectDocument(
                graphReopened.document) == graphCanonical,
        "current format should round-trip closed graph-node actions canonically");

    auto prematureGraph = graphCanonical;
    const auto graphVersion = prematureGraph.find("\"version\": 156");
    prematureGraph.replace(
        graphVersion,
        std::string_view{"\"version\": 156"}.size(),
        "\"version\":  31");
    const auto rejectedPrematureGraph =
        talas::project::parseProjectDocument(prematureGraph);
    require(
        !rejectedPrematureGraph.ok()
            && rejectedPrematureGraph.errors.front().path.find(
                ".actions[0].kind") != std::string::npos,
        "format 31 must reject a graph-node action before migration");

    auto unknownGraphField = graphCanonical;
    const auto graphKind = unknownGraphField.find(
        "\"kind\": \"graphNode\",");
    require(graphKind != std::string::npos,
        "graph action fixture should expose its closed kind field");
    unknownGraphField.insert(
        graphKind + std::string_view{"\"kind\": \"graphNode\","}.size(),
        "\n            \"extra\": true,");
    require(
        !talas::project::parseProjectDocument(unknownGraphField).ok(),
        "closed graph actions must reject unknown fields");

    auto unknownGraphType = graphCanonical;
    const auto oscillatorType = unknownGraphType.find("talas.oscillator");
    require(oscillatorType != std::string::npos,
        "graph action fixture should expose its catalog identity");
    unknownGraphType.replace(
        oscillatorType,
        std::string_view{"talas.oscillator"}.size(),
        "talas.unavailable");
    const auto rejectedGraphType =
        talas::project::parseProjectDocument(unknownGraphType);
    require(
        !rejectedGraphType.ok()
            && rejectedGraphType.errors.front().path.find(
                ".actions[0].nodeTypeId") != std::string::npos,
        "format 32 should reject unavailable graph node types at their exact path");

    auto spellingVariant = document;
    spellingVariant.generatorLibrary.programs.front().source.grammar =
        "\r\n\tstart : phrase\r\n\r\n"
        "phrase\t->\tnote phrase\t| rest phrase | note\r\n\r\n";
    require(
        talas::project::serializeProjectDocument(spellingVariant) == canonical,
        "equivalent whitespace and line endings should serialize to one structural form");

    talas::project::ProjectDocument nearLimit;
    auto dense = generator("dense");
    dense.output.bindings.clear();
    const auto shortTerminal = "s" + std::string(61, 'x');
    const auto longTerminal = "l" + std::string(62, 'x');
    dense.source.terminals = {shortTerminal, longTerminal};
    dense.source.grammar = "start:r\nr ->";
    std::size_t emittedSymbols = 0;
    for (std::size_t alternativeIndex = 0;
         alternativeIndex
             < talas::generative::maximumContextFreeAlternativesPerRule;
         ++alternativeIndex)
    {
        if (alternativeIndex != 0U)
            dense.source.grammar.push_back('|');
        for (std::size_t symbolIndex = 0;
             symbolIndex
                 < talas::generative::maximumContextFreeSymbolsPerAlternative;
             ++symbolIndex)
        {
            if (symbolIndex != 0U)
                dense.source.grammar.push_back(' ');
            dense.source.grammar += emittedSymbols < 11U
                ? shortTerminal
                : longTerminal;
            ++emittedSymbols;
        }
    }
    require(
        dense.source.grammar.size()
            == talas::generative::maximumContextFreeSourceBytes,
        "dense canonical fixture should reach the exact source-byte boundary");
    nearLimit.generatorLibrary.programs.push_back(std::move(dense));
    const auto nearLimitBytes =
        talas::project::serializeProjectDocument(nearLimit);
    const auto reopenedNearLimit =
        talas::project::parseProjectDocument(nearLimitBytes);
    require(
        reopenedNearLimit.ok()
            && reopenedNearLimit.document.generatorLibrary
                == nearLimit.generatorLibrary
            && talas::project::serializeProjectDocument(
                   reopenedNearLimit.document)
                == nearLimitBytes,
        "a structurally maximal compact grammar should remain readable after canonical serialization");

    auto fractionalSeed = canonical;
    const auto seed = fractionalSeed.find(
        "\"seed\": 18446744073709551615");
    require(seed != std::string::npos, "fixture should contain exact seed");
    fractionalSeed.replace(
        seed,
        std::string_view{"\"seed\": 18446744073709551615"}.size(),
        "\"seed\": 1.5");
    const auto rejected =
        talas::project::parseProjectDocument(fractionalSeed);
    require(
        !rejected.ok()
            && !rejected.errors.empty(),
        "fractional seed authority should fail closed");

    auto fractionalTimelineStep = canonical;
    const auto timelineStep = fractionalTimelineStep.find(
        "\"timelineStepMicroseconds\": 250000");
    require(timelineStep != std::string::npos,
        "fixture should contain the exact Timeline step");
    fractionalTimelineStep.replace(
        timelineStep,
        std::string_view{"\"timelineStepMicroseconds\": 250000"}.size(),
        "\"timelineStepMicroseconds\": 1.5");
    require(
        !talas::project::parseProjectDocument(fractionalTimelineStep).ok(),
        "fractional output-grid authority should fail before retention");

    auto unknownActionKind = canonical;
    const auto markerKind = unknownActionKind.find(
        "\"kind\": \"scoreMarker\"");
    require(markerKind != std::string::npos,
        "fixture should contain one closed marker action kind");
    unknownActionKind.replace(
        markerKind,
        std::string_view{"\"kind\": \"scoreMarker\""}.size(),
        "\"kind\": \"futureAction\"");
    require(
        !talas::project::parseProjectDocument(unknownActionKind).ok(),
        "unknown generator output action kinds should fail closed");

    auto missingTarget = canonical;
    const auto laneId = missingTarget.find("\"laneId\": \"gainLane\"");
    require(laneId != std::string::npos,
        "fixture should contain one exact automation lane target");
    missingTarget.replace(
        laneId,
        std::string_view{"\"laneId\": \"gainLane\""}.size(),
        "\"laneId\": \"missingLane\"");
    const auto rejectedTarget =
        talas::project::parseProjectDocument(missingTarget);
    require(
        !rejectedTarget.ok()
            && std::ranges::any_of(
                rejectedTarget.errors,
                [](const auto& error) {
                    return error.path
                        == "$.generatorLibrary.programs[0].output.bindings[0].actions[2].laneId";
                }),
        "unavailable output targets should fail at their exact persisted path");

    talas::project::ProjectDocument singleLarge;
    singleLarge.generatorLibrary.programs.push_back(generator());
    const auto singleLargeBytes =
        talas::project::serializeProjectDocument(singleLarge);
    const auto objectBegin = singleLargeBytes.find("      {\n");
    const auto objectEnd = singleLargeBytes.find(
        "\n    ]\n  },\n  \"liveSetLibrary\"", objectBegin);
    require(
        objectBegin != std::string::npos
            && objectEnd != std::string::npos,
        "large generator fixture should expose one serialized object");
    const auto object = singleLargeBytes.substr(
        objectBegin, objectEnd - objectBegin);
    talas::project::ProjectDocument emptyCurrent;
    auto aggregateBytes =
        talas::project::serializeProjectDocument(emptyCurrent);
    std::string objects;
    for (std::size_t index = 0; index < 18; ++index)
    {
        auto candidate = object;
        const auto id = candidate.find("\"generatorId\": \"phrase\"");
        require(id != std::string::npos,
            "large generator object should retain its ID field");
        candidate.replace(
            id,
            std::string_view{"\"generatorId\": \"phrase\""}.size(),
            "\"generatorId\": \"phrase" + std::to_string(index) + "\"");
        const auto grammarEnd = candidate.find(
            "\",\n        \"seed\"");
        require(grammarEnd != std::string::npos,
            "large generator object should retain its grammar field");
        candidate.insert(grammarEnd, 60U * 1024U, ' ');
        if (!objects.empty())
            objects += ",\n";
        objects += candidate;
    }
    const auto emptyPrograms = aggregateBytes.find("\"programs\": []");
    require(emptyPrograms != std::string::npos,
        "empty format-32 fixture should expose generator array");
    aggregateBytes.replace(
        emptyPrograms,
        std::string_view{"\"programs\": []"}.size(),
        "\"programs\": [\n" + objects + "\n    ]");
    const auto aggregateRejected =
        talas::project::parseProjectDocument(aggregateBytes);
    const auto aggregateError = std::ranges::any_of(
        aggregateRejected.errors,
        [](const auto& error) {
            return error.message.find(
                "aggregate object member string byte limit")
                != std::string::npos;
        });
    require(
        !aggregateRejected.ok()
            && aggregateError,
        aggregateRejected.errors.empty()
            ? "aggregate generator source unexpectedly parsed"
            : aggregateRejected.errors.front().message);

    auto formatThirty = canonical;
    const auto currentVersion = formatThirty.find("\"version\": 156");
    const auto libraryBegin = formatThirty.find("\"generatorLibrary\": {");
    const auto libraryVersion = formatThirty.find(
        "\"version\": 2",
        libraryBegin);
    const auto outputBegin = formatThirty.find(
        ",\n        \"output\": {",
        libraryBegin);
    const auto outputEnd = formatThirty.find("\n      }", outputBegin);
    require(
        currentVersion != std::string::npos
            && libraryBegin != std::string::npos
            && libraryVersion != std::string::npos
            && outputBegin != std::string::npos
            && outputEnd != std::string::npos,
        "current generator fixture should expose its format-32 output profile");
    formatThirty.replace(
        currentVersion,
        std::string_view{"\"version\": 156"}.size(),
        "\"version\":  30");
    formatThirty.replace(
        libraryVersion,
        std::string_view{"\"version\": 2"}.size(),
        "\"version\": 1");
    formatThirty.erase(outputBegin, outputEnd - outputBegin);
    eraseEmptyLiveSetLibrary(formatThirty);
    eraseTrackMemorySharingOption(formatThirty);
    const auto migratedThirty =
        talas::project::parseProjectDocument(formatThirty);
    require(
        migratedThirty.ok()
            && migratedThirty.document.formatVersion == talas::project::currentProjectFormatVersion
            && migratedThirty.document.generatorLibrary.version == 2
            && migratedThirty.document.generatorLibrary.programs.size() == 1
            && migratedThirty.document.generatorLibrary.programs.front()
                .source == document.generatorLibrary.programs.front().source
            && migratedThirty.document.generatorLibrary.programs.front()
                .output.bindings.empty(),
        "format 30 should preserve generator source and migrate it as preview-only");

    talas::project::ProjectDocument empty;
    empty.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV1;
    auto legacy = talas::project::serializeProjectDocument(empty);
    const auto version = legacy.find("\"version\": 156");
    const std::string generatorField =
        "  \"generatorLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"programs\": []\n"
        "  },\n";
    const auto field = legacy.find(generatorField);
    require(
        version != std::string::npos && field != std::string::npos,
        "current fixture should contain format and generator authority");
    legacy.replace(version, std::string_view{"\"version\": 156"}.size(),
                   "\"version\":  29");
    legacy.erase(field, generatorField.size());
    eraseEmptyLiveSetLibrary(legacy);
    const auto migrated = talas::project::parseProjectDocument(legacy);
    require(
        migrated.ok()
            && migrated.document.formatVersion == talas::project::currentProjectFormatVersion
            && migrated.document.generatorLibrary.programs.empty(),
        "format 29 should migrate with an empty generator library");
}

talas::project::ScoreTime ticks(std::uint64_t value)
{
    return {
        talas::project::ScoreTimeDomain::MusicalTicks,
        0.0,
        value,
    };
}

talas::project::ProjectGeneratorEditAuthority authorityFor(
    const talas::project::ProjectEditor& editor)
{
    return {
        editor.documentAuthorityGeneration(),
        talas::project::serializeProjectDocument(editor.document()),
    };
}

void testGeneratedEditsUseExactAuthorityAndOneUndo()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument source;
    addGeneratorOutputTargets(source);
    source.generatorLibrary.programs.push_back(generatorWithOutput());
    talas::project::ProjectEditor editor{source, registry};
    const auto originalBytes =
        talas::project::serializeProjectDocument(editor.document());

    auto timelineAuthority = authorityFor(editor);
    std::vector<talas::project::ProjectGeneratorTimelineEvent> events{
        {
            1'500'000,
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            64.0,
            talas::core::AutomationInterpolation::Step,
        },
    };
    require(
        editor.applyGeneratedTimelineEvents(
            std::move(events), timelineAuthority).ok()
            && editor.document().timeline.eventValues.size() == 1
            && editor.document().timeline.eventValues.front().timeSeconds
                == 1.5,
        "generated Timeline events should commit through exact integer-microsecond authority");
    require(
        editor.undo().ok()
            && talas::project::serializeProjectDocument(editor.document())
                == originalBytes,
        "one generated Timeline batch should consume exactly one Undo step");

    auto scoreAuthority = authorityFor(editor);
    talas::project::ProjectScoreEditBatch score;
    score.clips.push_back({
        "generatedClip",
        "voice",
        "voiceDefinition",
        ticks(0),
        ticks(480),
        ticks(0),
        ticks(60),
        ticks(60),
        1,
    });
    auto lane = editor.document().timeline.score.automation.front();
    lane.points.push_back({
        ticks(0),
        0.5,
        talas::core::AutomationInterpolation::Linear,
    });
    score.automation.push_back(std::move(lane));
    score.markers.push_back({
        "generatedMarker",
        "Generated",
        ticks(0),
        ticks(240),
    });
    require(
        editor.applyGeneratedScoreEdits(
            std::move(score), scoreAuthority).ok()
            && editor.document().timeline.score.clips.size() == 1
            && editor.document().timeline.score.markers.size() == 1
            && editor.document().timeline.score.automation.front()
                .points.size() == 1,
        "generated clip, automation, and marker edits should publish as one Score candidate");
    require(
        editor.undo().ok()
            && talas::project::serializeProjectDocument(editor.document())
                == originalBytes,
        "one generated Score batch should consume exactly one Undo step");

    const auto staleAuthority = authorityFor(editor);
    require(
        editor.addTimelineEventValue({
            2.0,
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            65.0,
            talas::core::AutomationInterpolation::Step,
        }).ok()
            && editor.undo().ok()
            && talas::project::serializeProjectDocument(editor.document())
                == staleAuthority.canonicalDocumentBytes,
        "ABA fixture should restore exact bytes while advancing authority");
    const auto stale = editor.applyGeneratedTimelineEvents(
        {{
            3'000'000,
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            67.0,
            talas::core::AutomationInterpolation::Step,
        }},
        staleAuthority);
    require(
        !stale.ok()
            && stale.error
                == talas::project::EditError::StaleProjectGeneratorOutput
            && editor.document().timeline.eventValues.empty(),
        "edit-undo ABA must reject a generated candidate despite equal canonical bytes");

    talas::project::ProjectEditor isolated{source, registry};
    isolated.beginHistoryTransaction();
    require(
        isolated.addTimelineEventValue({
            0.25,
            "Pitch",
            talas::expr::ValueDomain::NoteNumber,
            61.0,
            talas::core::AutomationInterpolation::Step,
        }).ok(),
        "transaction isolation fixture should author one prior edit");
    const auto isolatedAuthority = authorityFor(isolated);
    require(
        isolated.applyGeneratedTimelineEvents(
            {{
                500'000,
                "Pitch",
                talas::expr::ValueDomain::NoteNumber,
                62.0,
                talas::core::AutomationInterpolation::Step,
            }},
            isolatedAuthority).ok(),
        "generated edit should close an unrelated open history transaction");
    require(
        isolated.undo().ok()
            && isolated.document().timeline.eventValues.size() == 1
            && isolated.document().timeline.eventValues.front().timeSeconds
                == 0.25,
        "first Undo should remove only the isolated generated edit");
    require(
        isolated.undo().ok()
            && isolated.document().timeline.eventValues.empty(),
        "second Undo should independently remove the prior transaction");
}

void testGeneratorTargetLifecycleRejectsDanglingEdits()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectDocument source;
    addGeneratorOutputTargets(source);
    source.virtualControls.push_back({
        "CustomTarget",
        talas::expr::ValueDomain::Scalar,
        talas::project::VirtualControlKind::Fader,
        0.0,
        0.0,
        1.0,
    });
    auto program = generatorWithOutput();
    auto& event = std::get<talas::project::ProjectGeneratorTimelineEventAction>(
        program.output.bindings.front().actions.front());
    event.eventName = "CustomTarget";
    event.domain = talas::expr::ValueDomain::Scalar;
    program.output.bindings.front().actions.back() =
        talas::project::ProjectGeneratorTimelineEventAction{
            "KeyVelocity",
            talas::expr::ValueDomain::Scalar,
            0.5,
            talas::core::AutomationInterpolation::Step,
        };
    talas::project::ProjectEditor editor{source, registry};
    require(
        editor.upsertProjectGenerator(program).ok(),
        "a generator should author only when every exact target is available");
    const auto acceptedBytes =
        talas::project::serializeProjectDocument(editor.document());

    const auto conflictingTimeline = editor.addTimelineEventValue({
        0.0,
        "CustomTarget",
        talas::expr::ValueDomain::Hertz,
        440.0,
        talas::core::AutomationInterpolation::Step,
    });
    require(
        !conflictingTimeline.ok()
            && conflictingTimeline.error
                == talas::project::EditError::InvalidProjectGenerator
            && talas::project::serializeProjectDocument(editor.document())
                == acceptedBytes,
        "adding a conflicting Timeline Event Value must not invalidate a generator target");

    talas::project::VirtualControl conflictingBuiltin;
    conflictingBuiltin.eventName = "KeyVelocity";
    conflictingBuiltin.domain = talas::expr::ValueDomain::NoteNumber;
    const auto conflictingControl =
        editor.addVirtualControl(std::move(conflictingBuiltin));
    require(
        !conflictingControl.ok()
            && conflictingControl.error
                == talas::project::EditError::InvalidProjectGenerator
            && talas::project::serializeProjectDocument(editor.document())
                == acceptedBytes,
        "adding a conflicting virtual control must not invalidate a builtin generator target");

    const auto controlRemoval =
        editor.removeVirtualControl("CustomTarget");
    require(
        !controlRemoval.ok()
            && controlRemoval.error
                == talas::project::EditError::InvalidProjectGenerator
            && controlRemoval.message.find("actions[0].eventName")
                != std::string::npos
            && talas::project::serializeProjectDocument(editor.document())
                == acceptedBytes,
        "an Event Value target removal should reject at the exact generator path");

    const auto laneRemoval = editor.removeScoreAutomationLane("gainLane");
    require(
        !laneRemoval.ok()
            && laneRemoval.error
                == talas::project::EditError::InvalidProjectGenerator
            && laneRemoval.message.find("actions[2].laneId")
                != std::string::npos,
        "an automation target removal should not leave a dangling generator");

    const auto definitionRemoval =
        editor.removeSoundDefinition("voiceDefinition");
    require(
        !definitionRemoval.ok()
            && definitionRemoval.error
                == talas::project::EditError::InvalidProjectGenerator
            && definitionRemoval.message.find("actions[1].soundDefinitionId")
                != std::string::npos,
        "a Sound target removal should not leave a dangling generator");

    auto wrongDomain = editor.document().timeline.score.tracks.front();
    wrongDomain.timeDomain = talas::project::ScoreTimeDomain::Seconds;
    const auto trackUpdate = editor.updateScoreTrack(std::move(wrongDomain));
    require(
        !trackUpdate.ok()
            && trackUpdate.error
                == talas::project::EditError::InvalidProjectGenerator
            && trackUpdate.message.find("actions[1].trackId")
                != std::string::npos
            && talas::project::serializeProjectDocument(editor.document())
                == acceptedBytes,
        "a Score track authority change should reject without partial mutation");
}

void testEditorEditsAreAtomicAndUndoable()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor(
        talas::project::ProjectDocument{}, registry);
    auto original = generator();
    require(
        editor.upsertProjectGenerator(original).ok()
            && editor.document().generatorLibrary.programs.size() == 1,
        "editor should add one valid generator");

    auto malformed = original;
    malformed.source.grammar = "start: missing\n";
    require(
        !editor.replaceProjectGenerator(
             "phrase",
             editor.documentAuthorityGeneration(),
             malformed).ok()
            && editor.document().generatorLibrary.programs.front()
                == original,
        "invalid replacement should leave generator authority unchanged");

    auto renamed = original;
    renamed.generatorId = "renamed";
    renamed.displayName = "Renamed";
    const auto staleGeneration = editor.documentAuthorityGeneration();
    require(editor.upsertProjectGenerator(generator("other")).ok(),
        "stale authority fixture should advance the document generation");
    require(
        !editor.replaceProjectGenerator(
             "phrase", staleGeneration, renamed).ok()
            && editor.document().generatorLibrary.programs.front()
                == original,
        "stale same-ID replacement authority should reject");
    require(editor.undo().ok(),
        "stale authority fixture cleanup should undo the unrelated edit");
    require(
        editor.replaceProjectGenerator(
            "phrase",
            editor.documentAuthorityGeneration(),
            renamed).ok()
            && editor.document().generatorLibrary.programs.front()
                == renamed,
        "replacement should require and replace the exact prior identity");
    require(
        editor.undo().ok()
            && editor.document().generatorLibrary.programs.front()
                == original,
        "one replacement should consume exactly one undo step");
    require(
        editor.removeProjectGenerator("phrase").ok()
            && editor.document().generatorLibrary.programs.empty(),
        "exact generator removal should succeed");
    require(
        editor.undo().ok()
            && editor.document().generatorLibrary.programs.front()
                == original,
        "generator removal should be undoable in one step");
}
}

int main()
{
    testValidationAndExactRanges();
    testCurrentRoundTripAndFormatThirtyTwoGraphAdmission();
    testGeneratedEditsUseExactAuthorityAndOneUndo();
    testGeneratorTargetLifecycleRejectsDanglingEdits();
    testEditorEditsAreAtomicAndUndoable();
    return 0;
}
