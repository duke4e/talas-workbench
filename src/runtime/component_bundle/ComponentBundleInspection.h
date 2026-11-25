#pragma once

#include "runtime/component_bundle/ComponentBundleManifest.h"

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace talas::runtime::component_bundle
{
inline constexpr std::size_t maximumComponentBundleDirectoryEntries = 512;
inline constexpr std::size_t maximumComponentBundleDirectoryDepth = 16;
inline constexpr std::uint64_t maximumComponentBundleSignatureBytes =
    256U * 1024U;

using ComponentBundleInspectionCancellationCheck = std::function<bool()>;

struct ComponentBundleFrozenPayload
{
    ComponentBundleFilePurpose purpose =
        ComponentBundleFilePurpose::Resource;
    std::string path;
    std::array<std::uint8_t, componentTrustDigestBytes> sha256{};
    std::vector<std::uint8_t> bytes;
    bool isEntry = false;

    bool operator==(const ComponentBundleFrozenPayload&) const = default;
};

struct ComponentBundleSnapshot
{
    ComponentBundleManifest manifest;
    std::string canonicalManifest;
    std::array<std::uint8_t, componentTrustDigestBytes> contentDigest{};
    std::vector<ComponentBundleFrozenPayload> payloads;
    std::optional<std::vector<std::uint8_t>> detachedSignatureCms;

    bool operator==(const ComponentBundleSnapshot&) const = default;
};

enum class ComponentBundleInspectionErrorCode : std::uint8_t
{
    InvalidRoot,
    ManifestUnavailable,
    ManifestInvalid,
    SymlinkDenied,
    HardlinkDenied,
    NonRegularEntry,
    UndeclaredEntry,
    SizeMismatch,
    DigestMismatch,
    ConcurrentMutation,
    BudgetExceeded,
    ReadFailed,
    Cancelled,
};

struct ComponentBundleInspectionError
{
    ComponentBundleInspectionErrorCode code =
        ComponentBundleInspectionErrorCode::InvalidRoot;
    std::string path;
    std::string summary;

    bool operator==(const ComponentBundleInspectionError&) const = default;
};

struct ComponentBundleInspectionResult
{
    std::optional<ComponentBundleSnapshot> snapshot;
    std::vector<ComponentBundleInspectionError> errors;

    [[nodiscard]] bool ok() const noexcept
    {
        return snapshot.has_value() && errors.empty();
    }
};

[[nodiscard]] std::array<std::uint8_t, componentTrustDigestBytes>
calculateComponentBundleSnapshotContentDigest(
    const ComponentBundleSnapshot& snapshot);

[[nodiscard]] bool validateComponentBundleSnapshotIdentity(
    const ComponentBundleSnapshot& snapshot);

[[nodiscard]] ComponentBundleInspectionResult inspectComponentBundle(
    const std::filesystem::path& root,
    ComponentBundleInspectionCancellationCheck shouldCancel = {});
}
