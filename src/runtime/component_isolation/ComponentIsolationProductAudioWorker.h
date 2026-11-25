#pragma once

#include "talas/runtime/ComponentIsolationAudioDataPlane.h"
#include "talas/runtime/ComponentIsolationControlSession.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime
{
inline constexpr float componentIsolationProductGainV1 = 0.5F;

enum class ComponentIsolationProductAudioWorkerPreparationDiagnosticV1 :
    std::uint8_t
{
    Ready,
    ServiceUnsupported,
    ContractInvalid,
    RegionMismatch,
    AtomicsNotLockFree,
};

enum class ComponentIsolationProductAudioWorkerOperationV1 : std::uint8_t
{
    Published,
    Missing,
    InvalidState,
    Quarantined,
};

class ComponentIsolationProductAudioWorkerV1
{
public:
    [[nodiscard]]
    ComponentIsolationProductAudioWorkerPreparationDiagnosticV1 prepare(
        ComponentIsolationProductServiceV1 service,
        const ComponentIsolationSessionContractV1& contract,
        std::span<const std::byte> requestRegion,
        std::span<std::byte> responseRegion) noexcept;

    [[nodiscard]] ComponentIsolationProductAudioWorkerOperationV1 pump()
        noexcept;

private:
    std::array<
        const ComponentIsolationAudioSharedRequestSlotV1*,
        componentIsolationAudioSlotCountV1>
        requestSlots_{};
    std::array<
        ComponentIsolationAudioSharedResponseSlotV1*,
        componentIsolationAudioSlotCountV1>
        responseSlots_{};
    std::array<
        const float*,
        componentIsolationAudioSlotCountV1>
        inputSamples_{};
    std::array<float*, componentIsolationAudioSlotCountV1> outputSamples_{};
    std::array<std::uint64_t, componentIsolationAudioSlotCountV1>
        processedSequences_{};
    ComponentIsolationSessionContractV1 contract_;
    std::uint64_t nextSequence_ = 1;
    bool ready_ = false;
    bool quarantined_ = false;
    bool sequenceExhausted_ = false;
};
}
