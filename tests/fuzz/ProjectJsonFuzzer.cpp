#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectParameterTransformer.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/PreparedProjectGenerators.h"
#include "talas/project/ProjectGeneratorOutputs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace
{
constexpr std::size_t maximumProjectJsonBytes = 1024U * 1024U;

[[noreturn]] void invariantFailure() noexcept
{
    __builtin_trap();
}

void exerciseGeneratorOutputs(const talas::project::ProjectDocument& document)
{
    auto fallback = talas::project::ProjectDocument{};
    const auto* source = &document;
    if (document.generatorLibrary.programs.empty())
    {
        fallback = document;
        talas::project::ProjectGeneratorProgram program;
        program.generatorId = "fuzzProgram";
        program.displayName = "Fuzz Program";
        program.source.terminals = {"note"};
        program.source.grammar = "start:note";
        program.source.maximumRewritePasses = 1;
        program.source.maximumOutputSymbols = 8;
        program.output.bindings.push_back({
            "note",
            {
                talas::project::ProjectGeneratorTimelineEventAction{
                    "Pitch",
                    talas::expr::ValueDomain::NoteNumber,
                    60.0,
                    talas::core::AutomationInterpolation::Step,
                },
                talas::project::ProjectGeneratorScoreMarkerAction{
                    "Fuzz marker",
                    std::nullopt,
                },
            },
        });
        fallback.generatorLibrary.programs.push_back(std::move(program));
        source = &fallback;
    }
    const auto targets =
        talas::project::validateProjectGeneratorOutputTargets(*source);
    const auto prepared =
        talas::project::prepareProjectGenerators(source->generatorLibrary);
    if (!prepared.ok())
        invariantFailure();
    for (const auto& program : prepared.prepared.programs)
    {
        std::array<std::uint16_t,
            talas::generative::maximumContextFreeOutputSymbols> firstCurrent{};
        std::array<std::uint16_t,
            talas::generative::maximumContextFreeOutputSymbols> firstNext{};
        std::array<std::uint16_t,
            talas::generative::maximumContextFreeOutputSymbols> firstOutput{};
        std::array<std::uint16_t,
            talas::generative::maximumContextFreeOutputSymbols> secondCurrent{};
        std::array<std::uint16_t,
            talas::generative::maximumContextFreeOutputSymbols> secondNext{};
        std::array<std::uint16_t,
            talas::generative::maximumContextFreeOutputSymbols> secondOutput{};
        const auto firstTimeline =
            talas::project::generateProjectTimelineEvents(
                program.program,
                program.output,
                {firstCurrent, firstNext, firstOutput},
                {0});
        const auto secondTimeline =
            talas::project::generateProjectTimelineEvents(
                program.program,
                program.output,
                {secondCurrent, secondNext, secondOutput},
                {0});
        if (firstTimeline.status != secondTimeline.status
            || firstTimeline.events != secondTimeline.events
            || firstTimeline.generatedSymbols
                != secondTimeline.generatedSymbols)
        {
            invariantFailure();
        }

        firstCurrent.fill(0);
        firstNext.fill(0);
        firstOutput.fill(0);
        secondCurrent.fill(0);
        secondNext.fill(0);
        secondOutput.fill(0);
        const auto firstScore = talas::project::generateProjectScoreEdits(
            program.program,
            program.output,
            {firstCurrent, firstNext, firstOutput},
            *source,
            {0, "fuzz"});
        const auto secondScore = talas::project::generateProjectScoreEdits(
            program.program,
            program.output,
            {secondCurrent, secondNext, secondOutput},
            *source,
            {0, "fuzz"});
        if (firstScore.status != secondScore.status
            || firstScore.generatedSymbols != secondScore.generatedSymbols
            || firstScore.edits.clips != secondScore.edits.clips
            || firstScore.edits.automation != secondScore.edits.automation
            || firstScore.edits.markers != secondScore.edits.markers
            || firstScore.edits.tracks != secondScore.edits.tracks)
        {
            invariantFailure();
        }
        if (targets.ok()
            && firstTimeline.status
                == talas::project::ProjectGeneratorProjectionStatus::InvalidTarget)
        {
            invariantFailure();
        }
    }
}

void exerciseParameterTransformer(
    const talas::project::ProjectDocument& document,
    std::string_view fuzzSource)
{
    static const auto registry = talas::core::makeBuiltinNodeRegistry();
    static_cast<void>(
        talas::project::parseProjectParameterTransformation(fuzzSource));
    static_cast<void>(
        talas::project::prepareProjectParameterTransformers(
            document,
            registry));

    auto fallback = talas::project::ProjectDocument{};
    fallback.graph.nodes = {
        {"osc", "talas.oscillator", {
            {"frequency", std::string{"100 hz"}},
        }},
        {"transform", "talas.parameterTransformer", {
            {"transformation", std::nullopt, std::nullopt,
                std::nullopt, std::nullopt,
                std::string{"scale frequency by 2"}},
        }},
        {"out", "talas.audioOutput"},
    };
    fallback.graph.connections = {
        {"osc", "audioOut", "transform", "audioIn"},
        {"transform", "audioOut", "out", "audioIn"},
    };
    const auto prepared =
        talas::project::prepareProjectParameterTransformers(
            fallback,
            registry);
    if (!prepared.ok() || prepared.transformedParameters != 1)
        invariantFailure();
}
}

extern "C" int LLVMFuzzerTestOneInput(
    const std::uint8_t* data,
    std::size_t size)
{
    if (size > maximumProjectJsonBytes)
        return 0;

    const auto source = std::string_view{
        reinterpret_cast<const char*>(data), size};
    static_cast<void>(
        talas::project::parseProjectParameterTransformation(source));
    const auto parsed = talas::project::parseProjectDocument(source);
    if (!parsed.ok())
        return 0;

    const auto canonical =
        talas::project::serializeProjectDocument(parsed.document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    if (!reopened.ok())
        invariantFailure();
    if (talas::project::serializeProjectDocument(reopened.document)
        != canonical)
    {
        invariantFailure();
    }
    exerciseGeneratorOutputs(reopened.document);
    exerciseParameterTransformer(reopened.document, source);
    return 0;
}
