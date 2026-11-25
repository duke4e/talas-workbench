#pragma once

#include "talas/generative/ContextFreeGenerator.h"
#include "talas/project/PreparedProjectGenerators.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
struct GeneratorPreviewRequest
{
    std::uint64_t requestGeneration = 0;
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t programChecksum = 0;
    std::string generatorId;
    std::shared_ptr<const std::string> canonicalDocumentBytes;
    std::shared_ptr<const talas::project::PreparedProjectGeneratorSet>
        preparedSet;

    [[nodiscard]] bool valid() const noexcept
    {
        return requestGeneration != 0
            && documentAuthorityGeneration != 0
            && !generatorId.empty()
            && canonicalDocumentBytes != nullptr
            && preparedSet != nullptr;
    }
};

struct GeneratorPreviewCandidate
{
    talas::generative::ContextFreeGenerateStatus status =
        talas::generative::ContextFreeGenerateStatus::InvalidPreparedProgram;
    std::vector<std::string> symbols;
    std::size_t rewritePasses = 0;
    std::size_t operations = 0;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status
            == talas::generative::ContextFreeGenerateStatus::Succeeded;
    }
};

struct GeneratorPreviewSnapshot
{
    std::uint64_t requestGeneration = 0;
    std::uint64_t documentAuthorityGeneration = 0;
    std::uint64_t programChecksum = 0;
    std::string generatorId;
    std::vector<std::string> symbols;
    std::size_t rewritePasses = 0;
    std::size_t operations = 0;

    [[nodiscard]] bool available() const noexcept
    {
        return requestGeneration != 0;
    }
};

class GeneratorPreviewCoordinator
{
public:
    [[nodiscard]] std::optional<GeneratorPreviewRequest> begin(
        std::shared_ptr<const talas::project::PreparedProjectGeneratorSet>
            preparedSet,
        std::string_view generatorId,
        std::uint64_t documentAuthorityGeneration,
        std::shared_ptr<const std::string> canonicalDocumentBytes);

    [[nodiscard]] GeneratorPreviewCandidate execute(
        const GeneratorPreviewRequest& request,
        std::stop_token stopToken = {}) const;

    [[nodiscard]] bool publish(
        const GeneratorPreviewRequest& request,
        GeneratorPreviewCandidate candidate,
        std::uint64_t currentDocumentAuthorityGeneration,
        std::string_view currentCanonicalDocumentBytes);

    void invalidate() noexcept;
    [[nodiscard]] bool isCurrent(
        const GeneratorPreviewRequest& request) const noexcept;
    [[nodiscard]] GeneratorPreviewSnapshot snapshot() const;

private:
    mutable std::mutex gate_;
    std::uint64_t nextRequestGeneration_ = 1;
    std::uint64_t currentRequestGeneration_ = 0;
    GeneratorPreviewSnapshot lastGood_;
};
}
