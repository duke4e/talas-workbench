#include "talas/app/GeneratorPreview.h"

#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"

#include <memory>
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

talas::project::ProjectGeneratorProgram program(
    std::string id,
    std::string grammar,
    std::uint64_t seed = 7)
{
    talas::project::ProjectGeneratorProgram result;
    result.generatorId = std::move(id);
    result.displayName = "Generator";
    result.source.terminals = {"note", "rest"};
    result.source.grammar = std::move(grammar);
    result.source.seed = seed;
    result.source.maximumRewritePasses = 8;
    result.source.maximumOutputSymbols = 64;
    return result;
}

std::shared_ptr<const talas::project::PreparedProjectGeneratorSet> prepare(
    const talas::project::ProjectGeneratorLibrary& library)
{
    auto result = talas::project::prepareProjectGenerators(library);
    require(result.ok(), "fixture generator set should prepare");
    return std::make_shared<const talas::project::PreparedProjectGeneratorSet>(
        std::move(result.prepared));
}

void testPreparationIsAllOrNothingAndCancelled()
{
    talas::project::ProjectGeneratorLibrary library;
    library.programs = {
        program("one", "start: note\n"),
        program("two", "start: missing\n"),
    };
    const auto invalid = talas::project::prepareProjectGenerators(library);
    require(
        !invalid.ok() && invalid.prepared.programs.empty(),
        "one invalid program should publish no partial prepared set");

    library.programs.pop_back();
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto stopped = talas::project::prepareProjectGenerators(
        library, cancelled.get_token());
    require(
        stopped.cancelled && stopped.prepared.programs.empty(),
        "cancelled preparation should publish no candidate set");
}

void testExactPublicationAndLastGoodFailureAtomicity()
{
    talas::project::ProjectDocument document;
    document.generatorLibrary.programs = {
        program("one", "start: note rest\n"),
        program("two", "start: phrase\nphrase -> rest\n"),
    };
    auto prepared = prepare(document.generatorLibrary);
    auto bytes = std::make_shared<const std::string>(
        talas::project::serializeProjectDocument(document));

    talas::app::GeneratorPreviewCoordinator coordinator;
    auto request = coordinator.begin(prepared, "one", 11, bytes);
    require(request.has_value(), "exact prepared preview should begin");
    auto candidate = coordinator.execute(*request);
    require(
        candidate.ok()
            && candidate.symbols
                == std::vector<std::string>{"note", "rest"},
        "preview should expose the exact terminal sequence");
    require(
        coordinator.publish(*request, std::move(candidate), 11, *bytes),
        "matching document and program authority should publish");
    const auto lastGood = coordinator.snapshot();
    require(
        lastGood.available()
            && lastGood.generatorId == "one"
            && lastGood.symbols
                == std::vector<std::string>{"note", "rest"},
        "successful publication should replace last-good exactly");

    auto second = coordinator.begin(prepared, "two", 11, bytes);
    require(second.has_value(), "second independent program should begin");
    auto secondCandidate = coordinator.execute(*second);
    require(
        secondCandidate.ok()
            && secondCandidate.symbols
                == std::vector<std::string>{"rest"},
        "same terminal identities should not alias cross-program state");

    require(
        !coordinator.publish(
            *request,
            coordinator.execute(*request),
            11,
            *bytes),
        "a newer request should invalidate older publication");
    require(
        !coordinator.publish(
            *second,
            std::move(secondCandidate),
            12,
            *bytes)
            && coordinator.snapshot().symbols == lastGood.symbols,
        "authority mismatch should preserve last-good preview");
}

void testBytesGenerationChecksumAndCancellationAreAllRequired()
{
    talas::project::ProjectDocument document;
    document.generatorLibrary.programs = {
        program("one", "start: note\n"),
    };
    auto prepared = prepare(document.generatorLibrary);
    auto bytes = std::make_shared<const std::string>(
        talas::project::serializeProjectDocument(document));

    talas::app::GeneratorPreviewCoordinator coordinator;
    auto request = coordinator.begin(prepared, "one", 21, bytes);
    require(request.has_value(), "authority fixture should begin");
    auto candidate = coordinator.execute(*request);
    require(
        !coordinator.publish(
            *request, std::move(candidate), 22, *bytes),
        "same bytes after edit-undo must not bypass generation authority");

    request = coordinator.begin(prepared, "one", 23, bytes);
    require(request.has_value(), "byte mismatch fixture should begin");
    candidate = coordinator.execute(*request);
    require(
        !coordinator.publish(
            *request, std::move(candidate), 23, *bytes + " "),
        "document byte mismatch should reject publication");

    auto editedDocument = document;
    editedDocument.generatorLibrary.programs.front().source.seed = 99;
    const auto editedBytes =
        talas::project::serializeProjectDocument(editedDocument);
    request = coordinator.begin(prepared, "one", 24, bytes);
    require(request.has_value(), "checksum mismatch fixture should begin");
    candidate = coordinator.execute(*request);
    require(
        !coordinator.publish(
            *request, std::move(candidate), 24, editedBytes),
        "program checksum mismatch should reject silent rebinding");

    request = coordinator.begin(prepared, "one", 25, bytes);
    require(request.has_value(), "cancel fixture should begin");
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledCandidate =
        coordinator.execute(*request, cancelled.get_token());
    require(
        !cancelledCandidate.ok()
            && cancelledCandidate.status
                == talas::generative::ContextFreeGenerateStatus::Cancelled,
        "cancelled execution should return one typed failure");
    coordinator.invalidate();
    require(
        !coordinator.isCurrent(*request),
        "invalidation should revoke the exact pending request");
}
}

int main()
{
    testPreparationIsAllOrNothingAndCancelled();
    testExactPublicationAndLastGoodFailureAtomicity();
    testBytesGenerationChecksumAndCancellationAreAllRequired();
    return 0;
}
