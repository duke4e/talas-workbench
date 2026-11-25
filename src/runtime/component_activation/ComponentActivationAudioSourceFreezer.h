#pragma once

#include "talas/core/ResourceSnapshot.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/runtime/ComponentTrustPolicy.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace talas::runtime::detail
{
struct ComponentActivationPcm16WaveShapeV1
{
    std::uint16_t channelCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t frameCount = 0;
};

struct ComponentActivationFrozenAudioSourceDataV1
{
    std::string id;
    std::uint64_t generation = 0;
    std::array<std::uint8_t, componentTrustDigestBytes> sourceContentDigest{};
    std::vector<std::uint8_t> bytes;
    std::uint16_t channelCount = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t frameCount = 0;
};
}

namespace talas::runtime::component_activation
{
class ComponentActivationAuthorizationFactory;
struct ComponentActivationAuthorizationTestPeer;

[[nodiscard]] std::optional<detail::ComponentActivationPcm16WaveShapeV1>
parseComponentActivationPcm16WaveV1(
    std::span<const std::uint8_t> bytes) noexcept;

enum class ComponentActivationAudioSourceFreezeCodeV1 : std::uint8_t
{
    Ready,
    SnapshotInvalid,
    Unavailable,
    NotRegularFile,
    ReadFailed,
    Cancelled,
    ContentMismatch,
    MediaInvalid,
    SourceChanged,
    AllocationFailed,
};

class ComponentActivationFrozenAudioSourceV1
{
public:
    ComponentActivationFrozenAudioSourceV1() noexcept = default;
    ~ComponentActivationFrozenAudioSourceV1();

    ComponentActivationFrozenAudioSourceV1(
        ComponentActivationFrozenAudioSourceV1&&) noexcept;
    ComponentActivationFrozenAudioSourceV1& operator=(
        ComponentActivationFrozenAudioSourceV1&&) noexcept;

    ComponentActivationFrozenAudioSourceV1(
        const ComponentActivationFrozenAudioSourceV1&) = delete;
    ComponentActivationFrozenAudioSourceV1& operator=(
        const ComponentActivationFrozenAudioSourceV1&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::uint64_t sourceByteCount() const noexcept;

private:
    explicit ComponentActivationFrozenAudioSourceV1(
        std::shared_ptr<
            const detail::ComponentActivationFrozenAudioSourceDataV1> data)
        noexcept;

    std::shared_ptr<
        const detail::ComponentActivationFrozenAudioSourceDataV1> data_;

    friend class ComponentActivationAudioSourceFreezerV1;
    friend class ComponentActivationAuthorizationFactory;
    friend struct ComponentActivationAuthorizationTestPeer;
};

struct ComponentActivationAudioSourceFreezeResultV1
{
    std::optional<ComponentActivationFrozenAudioSourceV1> source;
    ComponentActivationAudioSourceFreezeCodeV1 code =
        ComponentActivationAudioSourceFreezeCodeV1::SnapshotInvalid;

    [[nodiscard]] bool ok() const noexcept
    {
        return code == ComponentActivationAudioSourceFreezeCodeV1::Ready
            && source.has_value() && source->valid();
    }
};

class ComponentActivationAudioSourceFreezerV1
{
public:
    [[nodiscard]] static ComponentActivationAudioSourceFreezeResultV1 freeze(
        const talas::core::ResourceSnapshot& snapshot,
        talas::project::ProjectAssetInspectionCancellationCheck shouldCancel = {})
        noexcept;
};
}
