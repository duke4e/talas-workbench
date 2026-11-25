#include "talas/runtime/MtcTransportFollower.h"

#include <cmath>
#include <limits>
#include <span>

namespace talas::runtime
{
namespace
{
void addSaturated(std::uint64_t& value) noexcept
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}
}

bool MtcTransportIdentity::valid() const noexcept
{
    return authorityGeneration != 0 && sourceGeneration != 0
        && stageId != 0 && sourceSlot > 0 && sourceSlot <= 16;
}

void MtcTransportFollower::clearPartialSet() noexcept
{
    pieces_.fill({});
    nextExpectedPiece_ = 0;
}

void MtcTransportFollower::revokeRunningEvidence(
    MtcDiscontinuityReason reason) noexcept
{
    const auto wasRunning = runningEvidence_;
    runningEvidence_ = false;
    if (state_ == MtcTransportLockState::Locked)
    {
        state_ = hasTimeCode_
            ? MtcTransportLockState::Acquiring
            : MtcTransportLockState::Waiting;
    }
    if (state_ != MtcTransportLockState::Stale)
        discontinuityReason_ = reason;
    if (wasRunning)
        addSaturated(discontinuities_);
}

void MtcTransportFollower::configure(MtcTransportIdentity identity) noexcept
{
    if (!identity.valid())
        identity = {};
    if (identity == identity_)
        return;
    identity_ = identity;
    clearPartialSet();
    timeCode_ = {};
    lastRecordTimeSeconds_ = 0.0;
    lastCompleteTimeSeconds_ = 0.0;
    lastIngressSequence_ = 0;
    completeSetSequence_ = 0;
    state_ = MtcTransportLockState::Waiting;
    discontinuityReason_ = identity_.valid()
        ? MtcDiscontinuityReason::IdentityChange
        : MtcDiscontinuityReason::None;
    hasTimeCode_ = false;
    runningEvidence_ = false;
}

void MtcTransportFollower::reset() noexcept
{
    identity_ = {};
    clearPartialSet();
    timeCode_ = {};
    lastRecordTimeSeconds_ = 0.0;
    lastCompleteTimeSeconds_ = 0.0;
    lastIngressSequence_ = 0;
    completeSetSequence_ = 0;
    state_ = MtcTransportLockState::Waiting;
    discontinuityReason_ = MtcDiscontinuityReason::None;
    hasTimeCode_ = false;
    runningEvidence_ = false;
}

MtcTransportResult MtcTransportFollower::observe(
    const MtcTransportRecord& record) noexcept
{
    if (!identity_.valid() || record.identity != identity_)
    {
        clearPartialSet();
        revokeRunningEvidence(MtcDiscontinuityReason::IdentityChange);
        addSaturated(identityRejections_);
        return MtcTransportResult::IdentityRejected;
    }
    if (!std::isfinite(record.arrivalTimeSeconds)
        || record.arrivalTimeSeconds <= 0.0
        || record.arrivalTimeSeconds <= lastRecordTimeSeconds_
        || record.ingressSequence <= lastIngressSequence_)
    {
        clearPartialSet();
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(timestampRejections_);
        return MtcTransportResult::TimestampRejected;
    }
    if (record.byteCount != 2 && record.byteCount != 10)
    {
        clearPartialSet();
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(invalidMessages_);
        return MtcTransportResult::InvalidMessage;
    }
    lastRecordTimeSeconds_ = record.arrivalTimeSeconds;
    lastIngressSequence_ = record.ingressSequence;
    return record.byteCount == 2
        ? observeQuarterFrame(record)
        : observeFullFrame(record);
}

MtcTransportResult MtcTransportFollower::observeQuarterFrame(
    const MtcTransportRecord& record) noexcept
{
    MtcQuarterFramePiece piece;
    const auto decoded = decodeMtcQuarterFramePiece(
        std::span<const std::uint8_t>{record.bytes.data(), record.byteCount},
        piece);
    if (decoded != MtcDecodeResult::Accepted)
    {
        clearPartialSet();
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(invalidMessages_);
        return MtcTransportResult::InvalidMessage;
    }
    if (piece.type != nextExpectedPiece_)
    {
        clearPartialSet();
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(reorderedOrSkippedPieces_);
        return MtcTransportResult::Reordered;
    }
    pieces_[piece.type] = piece;
    nextExpectedPiece_ = static_cast<std::uint8_t>(piece.type + 1);
    addSaturated(acceptedQuarterFrames_);
    if (nextExpectedPiece_ < mtcQuarterFramePieceCount)
        return MtcTransportResult::PieceAccepted;

    SmpteTimeCode complete;
    bool wrapped = false;
    const auto completeResult = decodeMtcQuarterFrameSet(
        pieces_,
        SmpteDirection::Forward,
        true,
        complete,
        wrapped);
    clearPartialSet();
    switch (completeResult)
    {
    case MtcDecodeResult::Accepted:
        return acceptCompleteSet(
            complete, wrapped, record.arrivalTimeSeconds);
    case MtcDecodeResult::InvalidLabel:
    case MtcDecodeResult::InvalidRate:
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(invalidLabelsOrRates_);
        return MtcTransportResult::InvalidLabel;
    case MtcDecodeResult::UnsupportedDirection:
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(unsupportedDirections_);
        return MtcTransportResult::UnsupportedDirection;
    case MtcDecodeResult::ArithmeticOverflow:
        revokeRunningEvidence(MtcDiscontinuityReason::ArithmeticOverflow);
        addSaturated(arithmeticOverflows_);
        return MtcTransportResult::ArithmeticOverflow;
    case MtcDecodeResult::InvalidShape:
    case MtcDecodeResult::InvalidPiece:
    default:
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(invalidMessages_);
        return MtcTransportResult::InvalidMessage;
    }
}

MtcTransportResult MtcTransportFollower::acceptCompleteSet(
    const SmpteTimeCode& decoded,
    bool wrapped,
    double arrivalTimeSeconds) noexcept
{
    addSaturated(acceptedCompleteSets_);
    addSaturated(completeSetSequence_);
    lastCompleteTimeSeconds_ = arrivalTimeSeconds;

    if (wrapped)
    {
        timeCode_ = decoded;
        hasTimeCode_ = true;
        state_ = MtcTransportLockState::Acquiring;
        discontinuityReason_ = MtcDiscontinuityReason::DayWrap;
        runningEvidence_ = false;
        addSaturated(discontinuities_);
        return MtcTransportResult::Discontinuity;
    }

    if (!hasTimeCode_)
    {
        timeCode_ = decoded;
        hasTimeCode_ = true;
        state_ = MtcTransportLockState::Locked;
        discontinuityReason_ = MtcDiscontinuityReason::None;
        runningEvidence_ = true;
        addSaturated(acquisitions_);
        return MtcTransportResult::Acquired;
    }

    const auto sameRate = decoded.rate == timeCode_.rate
        && decoded.dropFrame == timeCode_.dropFrame;
    const auto limit = smpteFramesPer24Hours(decoded.rate);
    const auto forwardDelta = sameRate && limit != 0
        ? (decoded.absoluteFrame + limit - timeCode_.absoluteFrame) % limit
        : 0;
    const auto continuous = sameRate && forwardDelta == 2
        && decoded.absoluteFrame > timeCode_.absoluteFrame;
    const auto priorState = state_;
    const auto priorAbsoluteFrame = timeCode_.absoluteFrame;
    timeCode_ = decoded;
    if (!continuous)
    {
        state_ = MtcTransportLockState::Acquiring;
        discontinuityReason_ = !sameRate
            ? MtcDiscontinuityReason::RateChange
            : decoded.absoluteFrame <= priorAbsoluteFrame
                ? MtcDiscontinuityReason::BackwardMovement
                : MtcDiscontinuityReason::ForwardJump;
        runningEvidence_ = false;
        addSaturated(discontinuities_);
        return MtcTransportResult::Discontinuity;
    }
    state_ = MtcTransportLockState::Locked;
    discontinuityReason_ = MtcDiscontinuityReason::None;
    runningEvidence_ = true;
    if (priorState == MtcTransportLockState::Locked)
        return MtcTransportResult::Advanced;
    addSaturated(reacquisitions_);
    return MtcTransportResult::Reacquired;
}

MtcTransportResult MtcTransportFollower::observeFullFrame(
    const MtcTransportRecord& record) noexcept
{
    MtcFullFrameMessage decoded;
    const auto result = decodeMtcFullFrame(
        std::span<const std::uint8_t>{record.bytes.data(), record.byteCount},
        decoded);
    clearPartialSet();
    switch (result)
    {
    case MtcDecodeResult::Accepted:
        timeCode_ = decoded.timeCode;
        hasTimeCode_ = true;
        runningEvidence_ = false;
        state_ = MtcTransportLockState::Located;
        discontinuityReason_ = MtcDiscontinuityReason::FullFrameLocate;
        lastCompleteTimeSeconds_ = record.arrivalTimeSeconds;
        addSaturated(acceptedFullFrameLocates_);
        addSaturated(discontinuities_);
        return MtcTransportResult::Located;
    case MtcDecodeResult::InvalidLabel:
    case MtcDecodeResult::InvalidRate:
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(invalidLabelsOrRates_);
        return MtcTransportResult::InvalidLabel;
    case MtcDecodeResult::UnsupportedDirection:
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(unsupportedDirections_);
        return MtcTransportResult::UnsupportedDirection;
    case MtcDecodeResult::ArithmeticOverflow:
        revokeRunningEvidence(MtcDiscontinuityReason::ArithmeticOverflow);
        addSaturated(arithmeticOverflows_);
        return MtcTransportResult::ArithmeticOverflow;
    case MtcDecodeResult::InvalidShape:
    case MtcDecodeResult::InvalidPiece:
    default:
        revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
        addSaturated(invalidMessages_);
        return MtcTransportResult::InvalidMessage;
    }
}

MtcTransportResult MtcTransportFollower::advanceTime(
    double callbackTimeSeconds) noexcept
{
    if (!identity_.valid() || state_ == MtcTransportLockState::Waiting
        || state_ == MtcTransportLockState::Stale
        || lastCompleteTimeSeconds_ <= 0.0
        || !std::isfinite(callbackTimeSeconds)
        || callbackTimeSeconds <= lastCompleteTimeSeconds_)
    {
        return MtcTransportResult::PieceAccepted;
    }
    constexpr auto timeoutSeconds =
        static_cast<double>(mtcSignalTimeoutNanoseconds) / 1.0e9;
    if (callbackTimeSeconds - lastCompleteTimeSeconds_ <= timeoutSeconds)
        return MtcTransportResult::PieceAccepted;
    clearPartialSet();
    state_ = MtcTransportLockState::Stale;
    discontinuityReason_ = MtcDiscontinuityReason::SignalTimeout;
    runningEvidence_ = false;
    addSaturated(staleTimeouts_);
    return MtcTransportResult::TimedOut;
}

void MtcTransportFollower::rejectEvidence() noexcept
{
    clearPartialSet();
    revokeRunningEvidence(MtcDiscontinuityReason::EvidenceRejected);
}

void MtcTransportFollower::markDiscontinuity(
    MtcDiscontinuityReason reason) noexcept
{
    clearPartialSet();
    state_ = hasTimeCode_
        ? MtcTransportLockState::Acquiring
        : MtcTransportLockState::Waiting;
    runningEvidence_ = false;
    discontinuityReason_ = reason;
    addSaturated(discontinuities_);
    if (reason == MtcDiscontinuityReason::ArithmeticOverflow)
        addSaturated(arithmeticOverflows_);
}

MtcTransportFollowerSnapshot MtcTransportFollower::snapshot() const noexcept
{
    return {
        .state = state_,
        .identity = identity_,
        .timeCode = timeCode_,
        .lastCompleteTimeSeconds = lastCompleteTimeSeconds_,
        .lastIngressSequence = lastIngressSequence_,
        .completeSetSequence = completeSetSequence_,
        .acceptedQuarterFrames = acceptedQuarterFrames_,
        .acceptedCompleteSets = acceptedCompleteSets_,
        .acceptedFullFrameLocates = acceptedFullFrameLocates_,
        .acquisitions = acquisitions_,
        .reacquisitions = reacquisitions_,
        .staleTimeouts = staleTimeouts_,
        .reorderedOrSkippedPieces = reorderedOrSkippedPieces_,
        .identityRejections = identityRejections_,
        .timestampRejections = timestampRejections_,
        .invalidMessages = invalidMessages_,
        .invalidLabelsOrRates = invalidLabelsOrRates_,
        .unsupportedDirections = unsupportedDirections_,
        .arithmeticOverflows = arithmeticOverflows_,
        .discontinuities = discontinuities_,
        .nextExpectedPiece = nextExpectedPiece_,
        .discontinuityReason = discontinuityReason_,
        .hasTimeCode = hasTimeCode_,
        .runningEvidence = runningEvidence_,
    };
}
}
