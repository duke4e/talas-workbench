#include "talas/runtime/MtcTransportFollower.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

constexpr talas::runtime::MtcTransportIdentity identity{7, 11, 13, 2};

talas::runtime::MtcTransportRecord quarter(
    std::uint8_t type,
    std::uint8_t value,
    std::uint64_t sequence,
    double time,
    talas::runtime::MtcTransportIdentity recordIdentity = identity)
{
    talas::runtime::MtcTransportRecord record;
    record.bytes[0] = 0xF1;
    record.bytes[1] = static_cast<std::uint8_t>((type << 4) | value);
    record.arrivalTimeSeconds = time;
    record.ingressSequence = sequence;
    record.identity = recordIdentity;
    record.byteCount = 2;
    return record;
}

std::array<std::uint8_t, 8> nibblesFor(
    std::uint8_t hour,
    std::uint8_t minute,
    std::uint8_t second,
    std::uint8_t frame,
    std::uint8_t rateCode)
{
    return {
        static_cast<std::uint8_t>(frame & 0x0F),
        static_cast<std::uint8_t>((frame >> 4) & 0x01),
        static_cast<std::uint8_t>(second & 0x0F),
        static_cast<std::uint8_t>((second >> 4) & 0x03),
        static_cast<std::uint8_t>(minute & 0x0F),
        static_cast<std::uint8_t>((minute >> 4) & 0x03),
        static_cast<std::uint8_t>(hour & 0x0F),
        static_cast<std::uint8_t>(((hour >> 4) & 0x01)
            | ((rateCode & 0x03) << 1)),
    };
}

talas::runtime::MtcTransportResult sendSet(
    talas::runtime::MtcTransportFollower& follower,
    std::uint8_t hour,
    std::uint8_t minute,
    std::uint8_t second,
    std::uint8_t frame,
    std::uint8_t rateCode,
    std::uint64_t& sequence,
    double& time)
{
    const auto nibbles = nibblesFor(
        hour, minute, second, frame, rateCode);
    auto result = talas::runtime::MtcTransportResult::InvalidMessage;
    for (std::uint8_t type = 0; type < 8; ++type)
    {
        time += 0.001;
        result = follower.observe(
            quarter(type, nibbles[type], ++sequence, time));
        require(type == 7
                || result == talas::runtime::MtcTransportResult::PieceAccepted,
            "partial forward sets should accept each exact piece");
    }
    return result;
}

talas::runtime::MtcTransportRecord fullFrame(
    std::uint8_t hourByte,
    std::uint8_t minute,
    std::uint8_t second,
    std::uint8_t frame,
    std::uint64_t sequence,
    double time)
{
    talas::runtime::MtcTransportRecord record;
    record.bytes = {0xF0, 0x7F, 0x7F, 0x01, 0x01,
        hourByte, minute, second, frame, 0xF7};
    record.arrivalTimeSeconds = time;
    record.ingressSequence = sequence;
    record.identity = identity;
    record.byteCount = 10;
    return record;
}

void testIdentityAndForwardAcquisition()
{
    using namespace talas::runtime;
    MtcTransportFollower follower;
    follower.configure(identity);
    std::uint64_t sequence = 0;
    double time = 1.0;
    require(sendSet(follower, 1, 2, 3, 4, 0, sequence, time)
                == MtcTransportResult::Acquired,
        "the first complete forward set should acquire transport evidence");
    auto snapshot = follower.snapshot();
    require(snapshot.state == MtcTransportLockState::Locked
            && snapshot.runningEvidence
            && snapshot.timeCode.hour == 1
            && snapshot.timeCode.frame == 6
            && snapshot.acceptedQuarterFrames == 8
            && snapshot.acceptedCompleteSets == 1
            && snapshot.acquisitions == 1,
        "acquisition should retain compensated exact evidence and counters");

    require(sendSet(follower, 1, 2, 3, 6, 0, sequence, time)
                == MtcTransportResult::Advanced,
        "the next exact two-frame set should advance without relocation");

    auto wrongIdentity = identity;
    wrongIdentity.sourceGeneration = 12;
    require(follower.observe(
                quarter(0, 0, ++sequence, time + 0.01, wrongIdentity))
                == MtcTransportResult::IdentityRejected,
        "mixed source generations should reject before partial-set reuse");
    snapshot = follower.snapshot();
    require(snapshot.nextExpectedPiece == 0
            && snapshot.identityRejections == 1
            && snapshot.state == MtcTransportLockState::Acquiring
            && !snapshot.runningEvidence
            && snapshot.discontinuityReason
                == MtcDiscontinuityReason::IdentityChange,
        "identity mismatch should clear the partial set, revoke running evidence, and remain observable");
}

void testReorderDiscontinuityAndReacquisition()
{
    using namespace talas::runtime;
    MtcTransportFollower follower;
    follower.configure(identity);
    std::uint64_t sequence = 0;
    double time = 2.0;
    require(follower.observe(quarter(1, 0, ++sequence, time += 0.001))
                == MtcTransportResult::Reordered,
        "a set that does not begin at piece zero should reject");
    require(follower.observe(quarter(0, 4, ++sequence, time += 0.001))
                == MtcTransportResult::PieceAccepted
            && follower.observe(
                   quarter(0, 4, ++sequence, time += 0.001))
                == MtcTransportResult::Reordered,
        "a duplicate piece should invalidate the in-progress set");

    require(sendSet(follower, 0, 0, 10, 0, 3, sequence, time)
                == MtcTransportResult::Acquired,
        "a fresh forward set should recover after reorder rejection");
    require(sendSet(follower, 0, 0, 10, 10, 3, sequence, time)
                == MtcTransportResult::Discontinuity,
        "a forward jump larger than two frames should stop and reacquire");
    require(!follower.snapshot().runningEvidence
            && follower.snapshot().state
                == MtcTransportLockState::Acquiring,
        "discontinuity should preserve position but revoke running evidence");
    require(sendSet(follower, 0, 0, 10, 12, 3, sequence, time)
                == MtcTransportResult::Reacquired,
        "one fresh continuous set should reacquire after relocation");
    require(follower.observe(
                quarter(1, 0, ++sequence, time += 0.001))
                == MtcTransportResult::Reordered,
        "reordered evidence should reject after reacquisition");
    const auto rejected = follower.snapshot();
    require(rejected.state == MtcTransportLockState::Acquiring
            && !rejected.runningEvidence
            && rejected.discontinuityReason
                == MtcDiscontinuityReason::EvidenceRejected,
        "reordered evidence must immediately hold a previously locked follower");
}

void testFullFrameLocateAndPartialSetTimeout()
{
    using namespace talas::runtime;
    MtcTransportFollower follower;
    follower.configure(identity);
    std::uint64_t sequence = 1;
    double time = 3.0;
    require(follower.observe(fullFrame(0, 0, 20, 0, sequence, time))
                == MtcTransportResult::Located,
        "full-frame MTC should establish one held locate");
    auto snapshot = follower.snapshot();
    require(snapshot.state == MtcTransportLockState::Located
            && snapshot.hasTimeCode && !snapshot.runningEvidence
            && snapshot.acceptedFullFrameLocates == 1,
        "full-frame locate should never imply play");

    require(follower.observe(
                quarter(0, 2, ++sequence, time += 0.4))
                == MtcTransportResult::PieceAccepted,
        "one partial set may begin after a locate");
    require(follower.advanceTime(3.51)
                == MtcTransportResult::TimedOut,
        "one partial set must not extend the complete-evidence timeout");
    snapshot = follower.snapshot();
    require(snapshot.state == MtcTransportLockState::Stale
            && !snapshot.runningEvidence && snapshot.staleTimeouts == 1
            && snapshot.nextExpectedPiece == 0,
        "timeout should remain terminal and discard partial assembly");

    time = 3.6;
    require(sendSet(follower, 0, 0, 20, 0, 0, sequence, time)
                == MtcTransportResult::Reacquired,
        "a fresh complete moving set should reacquire stale held position");
}

void testRateChangeBackwardAndWrapDiscontinuities()
{
    using namespace talas::runtime;
    MtcTransportFollower follower;
    follower.configure(identity);
    std::uint64_t sequence = 0;
    double time = 4.0;
    require(sendSet(follower, 0, 0, 30, 0, 0, sequence, time)
                == MtcTransportResult::Acquired,
        "discontinuity fixture should acquire 24 fps");
    require(sendSet(follower, 0, 0, 30, 2, 1, sequence, time)
                == MtcTransportResult::Discontinuity,
        "rate changes should be explicit discontinuities");
    require(sendSet(follower, 0, 0, 29, 0, 1, sequence, time)
                == MtcTransportResult::Discontinuity,
        "backward movement should remain held and fail closed");

    MtcTransportFollower wrapFollower;
    wrapFollower.configure(identity);
    sequence = 0;
    time = 5.0;
    require(sendSet(wrapFollower, 23, 59, 59, 23, 0, sequence, time)
                == MtcTransportResult::Discontinuity,
        "two-frame compensation across 24 hours should be a discontinuity");
    require(wrapFollower.snapshot().timeCode.hour == 0
            && !wrapFollower.snapshot().runningEvidence,
        "24-hour wrap should preserve the wrapped held position without running");
}
}

int main()
{
    static_assert(noexcept(std::declval<talas::runtime::MtcTransportFollower&>()
        .observe(std::declval<const talas::runtime::MtcTransportRecord&>())));
    testIdentityAndForwardAcquisition();
    testReorderDiscontinuityAndReacquisition();
    testFullFrameLocateAndPartialSetTimeout();
    testRateChangeBackwardAndWrapDiscontinuities();
    std::cout << "MTC transport follower tests passed\n";
    return 0;
}
