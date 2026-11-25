#pragma once

#include "talas/runtime/ComponentIsolationProtocol.h"
#include "talas/runtime/ComponentTrustPolicy.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace talas::runtime
{
class ComponentIsolationAudioDataPlane;

namespace detail
{
struct ComponentIsolationLiveEvidenceStateV1
{
    explicit ComponentIsolationLiveEvidenceStateV1(
        const ComponentIsolationSessionContractV1& contract,
        std::weak_ptr<const ComponentIsolationAudioDataPlane> plane) noexcept
        : sessionGeneration(contract.sessionGeneration),
          sharedStorageIdentity(contract.sharedStorageIdentity),
          validationNonce(contract.validationNonce),
          audioPlane(std::move(plane))
    {
        static_assert(
            componentIsolationEvidenceNonceBytesV1
            == componentIsolationValidationNonceBytesV1);
    }

    std::atomic<bool> ready{false};
    std::atomic<std::uint64_t> audioPlaneRevision{0};
    const std::uint64_t sessionGeneration = 0;
    const std::uint64_t sharedStorageIdentity = 0;
    const std::array<std::uint8_t, componentIsolationEvidenceNonceBytesV1>
        validationNonce{};
    const std::weak_ptr<const ComponentIsolationAudioDataPlane> audioPlane;
};
}
}
