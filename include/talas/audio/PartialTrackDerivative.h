#pragma once

#include "talas/project/PartialTrackAsset.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectPartialTrackEdits.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace talas::audio
{
enum class PartialTrackDerivativeKind : std::uint8_t
{
    Audio,
    EditedPartial,
};

enum class PartialTrackDerivativeStatus : std::uint8_t
{
    Succeeded,
    InvalidRequest,
    Cancelled,
    AuthorityChanged,
    DestinationRejected,
    PreparationFailed,
    RenderFailed,
    PublicationFailed,
    AllocationFailed,
};

struct PartialTrackDerivativeRequest
{
    std::filesystem::path destination;
    std::filesystem::path resolvedSourcePath;
    talas::project::ProjectAsset sourceManifest;
    std::shared_ptr<const talas::project::PartialTrackAsset> source;
    std::optional<talas::project::ProjectPartialTrackEditSet> edits;
    PartialTrackDerivativeKind kind = PartialTrackDerivativeKind::Audio;
    std::size_t blockFrames = 4096;
};

struct PartialTrackDerivativeResult
{
    PartialTrackDerivativeStatus status =
        PartialTrackDerivativeStatus::InvalidRequest;
    std::filesystem::path destination;
    std::string contentHash;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::string encoding;
    std::string message;

    [[nodiscard]] bool ok() const noexcept
    {
        return status == PartialTrackDerivativeStatus::Succeeded;
    }
};

using PartialTrackDerivativeCancellationCheck = std::function<bool()>;
using PartialTrackDerivativeAuthorityCheck = std::function<bool()>;
using PartialTrackDerivativeProgress = std::function<void(std::uint16_t)>;

// Renders from one immutable source/edit snapshot into private sibling
// staging. The destination is installed with no-replace semantics only after
// the caller's terminal authority check succeeds. Cancellation, validation,
// allocation, render, and publication failures leave the destination absent
// and never modify the source asset.
[[nodiscard]] PartialTrackDerivativeResult renderPartialTrackDerivative(
    const PartialTrackDerivativeRequest& request,
    PartialTrackDerivativeCancellationCheck shouldCancel = {},
    PartialTrackDerivativeAuthorityCheck terminalAuthorityCheck = {},
    PartialTrackDerivativeProgress reportProgress = {});
}
