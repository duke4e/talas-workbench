#include "talas/app/GeneratorPreview.h"

#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace talas::app
{
namespace
{
[[nodiscard]] std::string previewFailureMessage(
    talas::generative::ContextFreeGenerateStatus status)
{
    using Status = talas::generative::ContextFreeGenerateStatus;
    switch (status)
    {
    case Status::Succeeded: return {};
    case Status::InvalidPreparedProgram:
        return "Prepared generator authority is invalid";
    case Status::InsufficientScratch:
        return "Generator scratch capacity is insufficient";
    case Status::AliasedScratch:
        return "Generator scratch buffers overlap";
    case Status::RewriteLimitReached:
        return "Rewrite pass limit reached before terminal output";
    case Status::OutputLimitExceeded:
        return "Generated output exceeded its fixed symbol limit";
    case Status::OperationLimitExceeded:
        return "Generation exceeded its fixed operation limit";
    case Status::Cancelled: return "Generation cancelled";
    }
    return "Unknown generator failure";
}
}

std::optional<GeneratorPreviewRequest> GeneratorPreviewCoordinator::begin(
    std::shared_ptr<const talas::project::PreparedProjectGeneratorSet> preparedSet,
    std::string_view generatorId,
    std::uint64_t documentAuthorityGeneration,
    std::shared_ptr<const std::string> canonicalDocumentBytes)
{
    if (preparedSet == nullptr || canonicalDocumentBytes == nullptr
        || generatorId.empty() || documentAuthorityGeneration == 0)
    {
        return std::nullopt;
    }
    const auto found = [&]() -> const talas::project::PreparedProjectGenerator* {
        for (const auto& candidate : preparedSet->programs)
        {
            if (candidate.generatorId == generatorId)
                return &candidate;
        }
        return nullptr;
    }();
    if (found == nullptr)
        return std::nullopt;

    std::scoped_lock lock(gate_);
    if (nextRequestGeneration_ == 0
        || nextRequestGeneration_ == std::numeric_limits<std::uint64_t>::max())
    {
        return std::nullopt;
    }
    GeneratorPreviewRequest request;
    request.requestGeneration = nextRequestGeneration_++;
    currentRequestGeneration_ = request.requestGeneration;
    request.documentAuthorityGeneration = documentAuthorityGeneration;
    request.programChecksum = found->canonicalChecksum;
    request.generatorId = std::string{generatorId};
    request.canonicalDocumentBytes = std::move(canonicalDocumentBytes);
    request.preparedSet = std::move(preparedSet);
    return request;
}

GeneratorPreviewCandidate GeneratorPreviewCoordinator::execute(
    const GeneratorPreviewRequest& request,
    std::stop_token stopToken) const
{
    GeneratorPreviewCandidate candidate;
    if (!request.valid())
    {
        candidate.message = "Generator preview request is invalid";
        return candidate;
    }
    const auto* prepared = talas::project::findPreparedProjectGenerator(
        *request.preparedSet,
        request.generatorId,
        request.programChecksum);
    if (prepared == nullptr)
    {
        candidate.message = "Prepared generator identity is stale";
        return candidate;
    }

    std::array<std::uint16_t,
               talas::generative::maximumContextFreeOutputSymbols> current{};
    std::array<std::uint16_t,
               talas::generative::maximumContextFreeOutputSymbols> next{};
    std::array<std::uint16_t,
               talas::generative::maximumContextFreeOutputSymbols> output{};
    talas::generative::ContextFreeGeneratorScratch scratch{
        current, next, output};
    const auto generated = talas::generative::generateContextFreeSymbols(
        prepared->program,
        scratch,
        stopToken);
    candidate.status = generated.status;
    candidate.rewritePasses = generated.rewritePasses;
    candidate.operations = generated.operations;
    if (!generated.ok())
    {
        candidate.message = previewFailureMessage(generated.status);
        return candidate;
    }

    const auto indices =
        talas::generative::generatedContextFreeSymbolIndices(scratch, generated);
    candidate.symbols.reserve(indices.size());
    for (const auto index : indices)
    {
        if (index >= prepared->program.symbols.size())
        {
            candidate.status = talas::generative::
                ContextFreeGenerateStatus::InvalidPreparedProgram;
            candidate.symbols.clear();
            candidate.message = "Generated symbol identity is invalid";
            return candidate;
        }
        candidate.symbols.push_back(prepared->program.symbols[index]);
    }
    return candidate;
}

bool GeneratorPreviewCoordinator::publish(
    const GeneratorPreviewRequest& request,
    GeneratorPreviewCandidate candidate,
    std::uint64_t currentDocumentAuthorityGeneration,
    std::string_view currentCanonicalDocumentBytes)
{
    if (!request.valid() || !candidate.ok()
        || request.documentAuthorityGeneration
            != currentDocumentAuthorityGeneration
        || *request.canonicalDocumentBytes != currentCanonicalDocumentBytes)
    {
        return false;
    }

    GeneratorPreviewSnapshot next;
    try
    {
        next = {
            request.requestGeneration,
            request.documentAuthorityGeneration,
            request.programChecksum,
            request.generatorId,
            std::move(candidate.symbols),
            candidate.rewritePasses,
            candidate.operations,
        };
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    catch (const std::length_error&)
    {
        return false;
    }

    std::scoped_lock lock(gate_);
    if (currentRequestGeneration_ != request.requestGeneration)
        return false;
    lastGood_ = std::move(next);
    return true;
}

void GeneratorPreviewCoordinator::invalidate() noexcept
{
    std::scoped_lock lock(gate_);
    currentRequestGeneration_ = 0;
}

bool GeneratorPreviewCoordinator::isCurrent(
    const GeneratorPreviewRequest& request) const noexcept
{
    std::scoped_lock lock(gate_);
    return request.valid()
        && currentRequestGeneration_ == request.requestGeneration;
}

GeneratorPreviewSnapshot GeneratorPreviewCoordinator::snapshot() const
{
    std::scoped_lock lock(gate_);
    return lastGood_;
}
}
