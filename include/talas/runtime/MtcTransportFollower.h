#pragma once

#include "talas/runtime/MtcSmpteProtocol.h"

#include <array>
#include <cstdint>

namespace talas::runtime
{
enum class MtcTransportLockState : std::uint8_t
{
    Waiting,
    Acquiring,
    Locked,
    Located,
    Stale,
};

enum class MtcTransportResult : std::uint8_t
{
    PieceAccepted,
    Acquired,
    Reacquired,
    Advanced,
    Located,
    Discontinuity,
    TimedOut,
    IdentityRejected,
    TimestampRejected,
    Reordered,
    InvalidMessage,
    InvalidLabel,
    UnsupportedDirection,
    ArithmeticOverflow,
};

enum class MtcDiscontinuityReason : std::uint8_t
{
    None,
    FullFrameLocate,
    ForwardJump,
    BackwardMovement,
    RateChange,
    DayWrap,
    SignalTimeout,
    IdentityChange,
    EvidenceRejected,
    ArithmeticOverflow,
};

struct MtcTransportIdentity
{
    std::uint64_t authorityGeneration = 0;
    std::uint64_t sourceGeneration = 0;
    std::uint64_t stageId = 0;
    std::uint8_t sourceSlot = 0;

    [[nodiscard]] bool valid() const noexcept;
    bool operator==(const MtcTransportIdentity&) const = default;
};

struct MtcTransportRecord
{
    std::array<std::uint8_t, 10> bytes{};
    double arrivalTimeSeconds = 0.0;
    std::uint64_t ingressSequence = 0;
    std::uint64_t evidenceGeneration = 0;
    MtcTransportIdentity identity;
    std::uint8_t byteCount = 0;

    bool operator==(const MtcTransportRecord&) const = default;
};

struct MtcTransportFollowerSnapshot
{
    MtcTransportLockState state = MtcTransportLockState::Waiting;
    MtcTransportIdentity identity;
    SmpteTimeCode timeCode;
    double lastCompleteTimeSeconds = 0.0;
    std::uint64_t lastIngressSequence = 0;
    std::uint64_t completeSetSequence = 0;
    std::uint64_t acceptedQuarterFrames = 0;
    std::uint64_t acceptedCompleteSets = 0;
    std::uint64_t acceptedFullFrameLocates = 0;
    std::uint64_t acquisitions = 0;
    std::uint64_t reacquisitions = 0;
    std::uint64_t staleTimeouts = 0;
    std::uint64_t reorderedOrSkippedPieces = 0;
    std::uint64_t identityRejections = 0;
    std::uint64_t timestampRejections = 0;
    std::uint64_t invalidMessages = 0;
    std::uint64_t invalidLabelsOrRates = 0;
    std::uint64_t unsupportedDirections = 0;
    std::uint64_t arithmeticOverflows = 0;
    std::uint64_t discontinuities = 0;
    std::uint8_t nextExpectedPiece = 0;
    MtcDiscontinuityReason discontinuityReason =
        MtcDiscontinuityReason::None;
    bool hasTimeCode = false;
    bool runningEvidence = false;
};

class MtcTransportFollower
{
public:
    void configure(MtcTransportIdentity identity) noexcept;
    void reset() noexcept;

    [[nodiscard]] MtcTransportResult observe(
        const MtcTransportRecord& record) noexcept;
    [[nodiscard]] MtcTransportResult advanceTime(
        double callbackTimeSeconds) noexcept;
    void rejectEvidence() noexcept;
    void markDiscontinuity(MtcDiscontinuityReason reason) noexcept;
    [[nodiscard]] MtcTransportFollowerSnapshot snapshot() const noexcept;

private:
    void clearPartialSet() noexcept;
    void revokeRunningEvidence(
        MtcDiscontinuityReason reason) noexcept;
    [[nodiscard]] MtcTransportResult observeQuarterFrame(
        const MtcTransportRecord& record) noexcept;
    [[nodiscard]] MtcTransportResult observeFullFrame(
        const MtcTransportRecord& record) noexcept;
    [[nodiscard]] MtcTransportResult acceptCompleteSet(
        const SmpteTimeCode& decoded,
        bool wrapped,
        double arrivalTimeSeconds) noexcept;

    MtcTransportIdentity identity_;
    std::array<MtcQuarterFramePiece, mtcQuarterFramePieceCount> pieces_{};
    SmpteTimeCode timeCode_;
    double lastRecordTimeSeconds_ = 0.0;
    double lastCompleteTimeSeconds_ = 0.0;
    std::uint64_t lastIngressSequence_ = 0;
    std::uint64_t completeSetSequence_ = 0;
    std::uint64_t acceptedQuarterFrames_ = 0;
    std::uint64_t acceptedCompleteSets_ = 0;
    std::uint64_t acceptedFullFrameLocates_ = 0;
    std::uint64_t acquisitions_ = 0;
    std::uint64_t reacquisitions_ = 0;
    std::uint64_t staleTimeouts_ = 0;
    std::uint64_t reorderedOrSkippedPieces_ = 0;
    std::uint64_t identityRejections_ = 0;
    std::uint64_t timestampRejections_ = 0;
    std::uint64_t invalidMessages_ = 0;
    std::uint64_t invalidLabelsOrRates_ = 0;
    std::uint64_t unsupportedDirections_ = 0;
    std::uint64_t arithmeticOverflows_ = 0;
    std::uint64_t discontinuities_ = 0;
    std::uint8_t nextExpectedPiece_ = 0;
    MtcTransportLockState state_ = MtcTransportLockState::Waiting;
    MtcDiscontinuityReason discontinuityReason_ =
        MtcDiscontinuityReason::None;
    bool hasTimeCode_ = false;
    bool runningEvidence_ = false;
};
}
