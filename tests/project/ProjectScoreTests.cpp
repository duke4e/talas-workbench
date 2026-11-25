#include "talas/project/ProjectScore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string(message));
}

talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}

talas::project::ScoreTime ticks(std::uint64_t value)
{
    return {talas::project::ScoreTimeDomain::MusicalTicks, 0.0, value};
}

talas::project::ProjectScore
baseScore(talas::project::ScoreOverlapMode overlap = talas::project::ScoreOverlapMode::Reject)
{
    talas::project::ProjectScore score;
    score.tracks = {
        {"voice", "Voice", talas::project::ScoreTrackKind::Track,
         talas::project::ScoreTimeDomain::MusicalTicks, "music", 2, overlap},
        {"music", "Music", talas::project::ScoreTrackKind::Submix,
         talas::project::ScoreTimeDomain::MusicalTicks, "master", 2,
         talas::project::ScoreOverlapMode::Layer},
        {"master",
         "Master",
         talas::project::ScoreTrackKind::Master,
         talas::project::ScoreTimeDomain::Seconds,
         {},
         2,
         talas::project::ScoreOverlapMode::Layer},
    };
    score.clips.push_back({"intro", "voice", "voiceDefinition", ticks(0), ticks(960), ticks(0),
                           ticks(120), ticks(120), 2});
    score.automation.push_back({"gainLane",
                                talas::project::ScoreAutomationScope::Track,
                                "voice",
                                "Gain",
                                talas::expr::ValueDomain::Scalar,
                                {{ticks(0), 0.25, talas::core::AutomationInterpolation::Linear},
                                 {ticks(1920), 1.0, talas::core::AutomationInterpolation::Step}}});
    score.markers.push_back({"verse", "Verse", ticks(960), ticks(1920)});
    return score;
}

constexpr std::string_view definitions[]{"voiceDefinition"};

void testValidScoreResolvesTimeRoutingAndClipOrder()
{
    auto score = baseScore(talas::project::ScoreOverlapMode::Layer);
    score.clips.push_back({"layer", "voice", "voiceDefinition", ticks(480), ticks(960), ticks(0),
                           ticks(0), ticks(0), 1});
    require(!talas::project::projectScoreValidationError(score, definitions),
            "bounded score fixture should validate");

    const talas::project::ProjectMusicalTime musicalTime;
    require(talas::project::scoreTimeToSampleFrame(ticks(960), musicalTime, 48000) ==
                    std::optional<std::uint64_t>{24000} &&
                talas::project::scoreTimeToSampleFrame(seconds(0.5), musicalTime, 48000) ==
                    std::optional<std::uint64_t>{24000},
            "seconds and musical intent should convert through one checked frame "
            "boundary");
    require(talas::project::activeScoreClipIds(score, "voice", ticks(600)) ==
                std::vector<std::string>({"intro", "layer"}),
            "layer overlap should preserve deterministic document order");
    score.tracks.front().overlapMode = talas::project::ScoreOverlapMode::Replace;
    require(talas::project::activeScoreClipIds(score, "voice", ticks(600)) ==
                std::vector<std::string>({"layer"}),
            "replace overlap should choose the last exact document-order clip");

    score.tracks.front().solo = true;
    const auto resolved = talas::project::resolveScoreTrackAudibility(score);
    require(resolved == std::vector<talas::project::ResolvedScoreTrack>({
                            {"voice", true},
                            {"music", true},
                            {"master", true},
                        }),
            "solo should retain the exact track and its routing ancestors");
    score.tracks[1].muted = true;
    require(!talas::project::resolveScoreTrackAudibility(score).front().audible,
            "explicit mute on a routing ancestor should fail the child closed");
}

// Tracks muted one at a time or several together go
// silent; soloing one track or several silences the rest; a soloed track
// that is muted is silent too, and a muted track sounds in a solo only
// once it is unmuted.
void testTimelineTracksMuteAndSolo()
{
    talas::project::ProjectScore score;
    for (const auto* id : {"one", "two", "three"})
        score.tracks.push_back({id, id, talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", 2,
            talas::project::ScoreOverlapMode::Layer});
    score.tracks.push_back({"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, 2,
        talas::project::ScoreOverlapMode::Layer});
    const auto heard = [&] {
        std::vector<bool> audible;
        for (const auto& track : talas::project::resolveScoreTrackAudibility(score))
            if (track.trackId != "master")
                audible.push_back(track.audible);
        return audible;
    };
    require(heard() == std::vector<bool>{true, true, true}, "every track should sound with none muted or soloed");
    score.tracks[0].muted = true;
    require(heard() == std::vector<bool>{false, true, true}, "a muted track should go silent");
    score.tracks[1].muted = true;
    require(heard() == std::vector<bool>{false, false, true}, "muting should extend to more tracks");
    score.tracks[0].muted = false;
    score.tracks[1].muted = false;
    score.tracks[0].solo = true;
    score.tracks[1].solo = true;
    require(heard() == std::vector<bool>{true, true, false}, "soloing should extend to more tracks, silencing the rest");
    score.tracks[0].muted = true;
    require(heard() == std::vector<bool>{false, true, false}, "a soloed track that is muted should be silent");
    score.tracks[0].muted = false;
    require(heard() == std::vector<bool>{true, true, false}, "unmuted, the soloed track should sound again");
}

void testValidationRejectsAmbiguousOrUnboundAuthority()
{
    auto score = baseScore();
    score.clips.push_back({"overlap", "voice", "voiceDefinition", ticks(480), ticks(960), ticks(0),
                           ticks(0), ticks(0), 1});
    auto validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::OverlapRejected,
            "Reject overlap mode should reject ambiguous overlapping clips");

    score = baseScore();
    score.tracks[1].outputTrackId = "voice";
    validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                (validation->code == talas::project::ScoreValidationCode::InvalidRoute ||
                 validation->code == talas::project::ScoreValidationCode::RouteCycle),
            "routing into a Sound track or cycle should reject");

    score = baseScore();
    score.clips.front().soundDefinitionId = "renamedDefinition";
    validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::UnknownSoundDefinition,
            "clip must never rebind by display order or a stale definition name");

    score = baseScore();
    score.clips.front().duration = seconds(1.0);
    validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::InvalidClip,
            "clip fields must use the exact owning track time domain");

    score = baseScore();
    score.automation.push_back(score.automation.front());
    score.automation.back().laneId = "secondGain";
    validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::DuplicateAutomationTarget,
            "one exact owner/Event Value/domain target should have one lane "
            "authority");

    score = baseScore();
    score.automation.push_back({"clipGain",
                                talas::project::ScoreAutomationScope::Clip,
                                "intro",
                                "Gain",
                                talas::expr::ValueDomain::Scalar,
                                {{ticks(1921), 0.5, talas::core::AutomationInterpolation::Step}}});
    validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::InvalidAutomation,
            "clip-local automation must remain inside the repeated clip duration");

    score = baseScore();
    score.automation.push_back({"masterTrackScope",
                                talas::project::ScoreAutomationScope::Track,
                                "master",
                                "Gain",
                                talas::expr::ValueDomain::Scalar,
                                {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step}}});
    validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::InvalidAutomation,
            "master automation must use the sole exact Master scope authority");

    score = baseScore();
    score.tracks.front().overlapMode = static_cast<talas::project::ScoreOverlapMode>(99);
    validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::InvalidTrack,
            "unknown closed enum values should reject before persistence or "
            "projection");
}

void testCandidateEditsAreAtomicAndViewIndependent()
{
    const auto original = baseScore();
    auto invalid = original.clips.front();
    invalid.clipId = "badClip";
    invalid.soundDefinitionId = "missingDefinition";
    const auto rejected =
        talas::project::projectScoreWithClip(original, std::move(invalid), definitions);
    require(!rejected.ok() && original == baseScore(),
            "rejected candidate edit should not mutate the prior score");

    auto marker = talas::project::ScoreMarker{"chorus", "Chorus", ticks(2880), ticks(3840)};
    const auto added = talas::project::projectScoreWithMarker(original, marker, definitions);
    require(added.ok() && added.candidate->markers.size() == 2 && original.markers.size() == 1,
            "accepted candidate edit should publish one new immutable score value");
    marker.title = "Chorus A";
    const auto updated = talas::project::projectScoreWithMarker(*added.candidate, marker, definitions);
    require(updated.ok() && updated.candidate->markers.size() == 2 &&
                updated.candidate->markers.back().title == "Chorus A",
            "same stable identity should update in document order without "
            "duplication");

    const auto removal = talas::project::projectScoreWithoutTrack(original, "music", definitions);
    require(!removal.ok() && removal.error->code == talas::project::ScoreValidationCode::UnknownRoute,
            "removing a referenced route should reject the whole candidate");

    auto movedIntro = original.clips.front();
    movedIntro.start = ticks(1920);
    talas::project::ProjectScoreEditBatch batch;
    batch.clips.push_back(std::move(movedIntro));
    batch.markers.push_back({"outro", "Outro", ticks(3840), std::nullopt});
    const auto bulk = talas::project::projectScoreWithEdits(original, std::move(batch), definitions);
    require(bulk.ok() && bulk.candidate->clips.front().start == ticks(1920) &&
                bulk.candidate->markers.size() == 2 && original.clips.front().start == ticks(0),
            "bulk edit should publish every accepted entity in one immutable candidate");

    talas::project::ProjectScoreEditBatch invalidBatch;
    invalidBatch.removeClipIds = {"missing", "missing"};
    const auto rejectedBatch =
        talas::project::projectScoreWithEdits(original, std::move(invalidBatch), definitions);
    require(!rejectedBatch.ok() && original == baseScore(),
            "invalid bulk edit should preserve the complete prior score");

    talas::project::ProjectScoreEditBatch duplicateUpsertBatch;
    duplicateUpsertBatch.clips = {original.clips.front(), original.clips.front()};
    const auto duplicateUpsert =
        talas::project::projectScoreWithEdits(original, std::move(duplicateUpsertBatch), definitions);
    require(!duplicateUpsert.ok() && original == baseScore(),
            "bulk edit must reject duplicate upsert identities");

    talas::project::ProjectScoreEditBatch removeUpsertBatch;
    removeUpsertBatch.removeClipIds.push_back("intro");
    removeUpsertBatch.clips.push_back(original.clips.front());
    const auto removeUpsert =
        talas::project::projectScoreWithEdits(original, std::move(removeUpsertBatch), definitions);
    require(!removeUpsert.ok() && original == baseScore(),
            "bulk edit must reject remove-plus-upsert identity reuse without reordering");

    talas::project::ProjectScoreEditBatch crossKindBatch;
    crossKindBatch.removeMarkerIds.push_back("verse");
    crossKindBatch.tracks.push_back({"verse", "Verse", talas::project::ScoreTrackKind::Track,
                                     talas::project::ScoreTimeDomain::MusicalTicks, "music", 2,
                                     talas::project::ScoreOverlapMode::Reject});
    const auto crossKind =
        talas::project::projectScoreWithEdits(original, std::move(crossKindBatch), definitions);
    require(!crossKind.ok() && original == baseScore(),
            "one global score identity must not change entity kind inside a batch");

    auto overBound = original;
    overBound.tracks.resize(talas::project::maximumScoreTracks + 1);
    const auto rejectedSource = talas::project::projectScoreWithMarker(
        overBound, {"bounded", "Bounded", ticks(1), std::nullopt}, definitions);
    require(!rejectedSource.ok() &&
                rejectedSource.error->code == talas::project::ScoreValidationCode::TrackLimit,
            "candidate helpers must reject an over-bound source before copying it");
    require(talas::project::resolveScoreTrackAudibility(overBound).empty() &&
                talas::project::activeScoreClipIds(overBound, "voice", ticks(0)).empty(),
            "public score resolvers must reject over-bound vectors before allocation or scans");
    auto overBoundClips = original;
    overBoundClips.clips.resize(talas::project::maximumScoreClips + 1);
    require(talas::project::activeScoreClipIds(overBoundClips, "voice", ticks(0)).empty(),
            "active clip resolution must independently reject the clip-count bound");

    const std::string overlongId(talas::project::maximumScoreIdentifierBytes * 1024U, 'x');
    auto invalidIdentity = original;
    invalidIdentity.tracks.front().trackId = overlongId;
    const auto identityError =
        talas::project::projectScoreValidationError(invalidIdentity, definitions);
    const auto removalError =
        talas::project::projectScoreWithoutMarker(original, overlongId, definitions);
    require(identityError.has_value() && identityError->entityId.empty() && !removalError.ok() &&
                removalError.error->entityId.empty() &&
                talas::project::resolveScoreTrackAudibility(invalidIdentity).empty() &&
                talas::project::activeScoreClipIds(invalidIdentity, "voice", ticks(0)).empty(),
            "invalid overlong identities must not be copied into bounded diagnostics");
    auto invalidClipIdentity = original;
    invalidClipIdentity.clips.front().clipId = overlongId;
    require(talas::project::activeScoreClipIds(invalidClipIdentity, "voice", ticks(0)).empty(),
            "active clip resolution must reject an overlong clip identity before copying it");
}

void testBoundsAndCanonicalTimeFailClosed()
{
    require(!talas::project::isCanonicalScoreTime({talas::project::ScoreTimeDomain::Seconds, 1.0, 1}) &&
                !talas::project::isCanonicalScoreTime(
                    {talas::project::ScoreTimeDomain::MusicalTicks, 0.25, 1}),
            "inactive time-domain payload must be canonical zero");
    auto score = baseScore();
    score.markers.front().title.assign(talas::project::maximumScoreMarkerTitleBytes + 1, 'x');
    const auto validation = talas::project::projectScoreValidationError(score, definitions);
    require(validation.has_value() &&
                validation->code == talas::project::ScoreValidationCode::InvalidMarker,
            "over-bound marker presentation must reject before projection");

    score = baseScore();
    score.clips.front().start = ticks(std::numeric_limits<std::uint64_t>::max() - 100U);
    score.clips.front().duration = ticks(101U);
    score.clips.front().repeatCount = 1;
    const auto overflow = talas::project::projectScoreValidationError(score, definitions);
    require(overflow.has_value() &&
                overflow->code == talas::project::ScoreValidationCode::TimeOverflow,
            "musical clip arithmetic must reject exact uint64 overflow without "
            "floating-point conversion");

    score = baseScore();
    constexpr auto firstExactTick = std::uint64_t{1} << 53U;
    score.automation.front().points = {
        {ticks(firstExactTick), 0.25, talas::core::AutomationInterpolation::Linear},
        {ticks(firstExactTick + 1U), 0.5, talas::core::AutomationInterpolation::Step},
    };
    require(!talas::project::projectScoreValidationError(score, definitions),
            "adjacent uint64 musical ticks above binary64 precision must remain distinct");

    const talas::project::ProjectMusicalTime musicalTime;
    const auto exclusiveFrameMaximum = std::ldexp(1.0, 64);
    require(!talas::project::scoreTimeToSampleFrame(seconds(exclusiveFrameMaximum), musicalTime, 1) &&
                talas::project::scoreTimeToSampleFrame(
                    seconds(std::nextafter(exclusiveFrameMaximum, 0.0)), musicalTime, 1) ==
                    std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max() - 2047U},
            "seconds frame conversion must use one portable binary64 uint64 boundary");
    require(
        talas::project::scoreTimeToSampleFrame(seconds(0x1.5d867c3ece2a5p+48), musicalTime, 48000) ==
            std::optional<std::uint64_t>{std::numeric_limits<std::uint64_t>::max() - 615U},
        "seconds frame conversion must retain the exact valid product below uint64 max");

    score = baseScore();
    score.tracks.front().timeDomain = talas::project::ScoreTimeDomain::Seconds;
    score.clips.front().start = seconds(1.0e300);
    score.clips.front().duration = seconds(1.0);
    score.clips.front().contentOffset = seconds(0.0);
    score.clips.front().fadeIn = seconds(0.0);
    score.clips.front().fadeOut = seconds(0.0);
    score.automation.front().points.clear();
    const auto stagnantEnd = talas::project::projectScoreValidationError(score, definitions);
    require(stagnantEnd.has_value() &&
                stagnantEnd->code == talas::project::ScoreValidationCode::TimeOverflow,
            "a positive seconds duration must advance the clip end at binary64 precision");
}

void testClipRenderArtifactShapeIsClosedAndBounded()
{
    auto score = baseScore();
    score.clips.front().renderArtifact =
        talas::project::ScoreClipRenderArtifact{
            talas::project::ScoreClipRenderMode::Cached,
            std::string(talas::project::scoreClipRenderFingerprintBytes, 'a'),
            48000,
            96000,
            {"Caches/intro-left", "Caches/intro-right"},
        };
    require(
        !talas::project::projectScoreValidationError(score, definitions),
        "a clip artifact should accept one bounded distinct Project Asset ID per exact track channel");

    auto invalid = score;
    invalid.clips.front().renderArtifact->channelAssetIds.pop_back();
    auto error = talas::project::projectScoreValidationError(
        invalid,
        definitions);
    require(
        error.has_value()
            && error->code
                == talas::project::ScoreValidationCode::InvalidRenderArtifact,
        "clip artifact channel count must equal the owning track contract");

    invalid = score;
    invalid.clips.front().renderArtifact->channelAssetIds.back() =
        invalid.clips.front().renderArtifact->channelAssetIds.front();
    error = talas::project::projectScoreValidationError(invalid, definitions);
    require(
        error.has_value()
            && error->code
                == talas::project::ScoreValidationCode::InvalidRenderArtifact,
        "clip artifact channel asset identities must be distinct");

    invalid = score;
    invalid.clips.front().renderArtifact->sourceFingerprint.front() = 'A';
    error = talas::project::projectScoreValidationError(invalid, definitions);
    require(
        error.has_value()
            && error->code
                == talas::project::ScoreValidationCode::InvalidRenderArtifact,
        "clip artifact fingerprint must be canonical lowercase SHA-256");

    invalid = score;
    invalid.clips.front().renderArtifact->sampleRate = 0;
    error = talas::project::projectScoreValidationError(invalid, definitions);
    require(
        error.has_value()
            && error->code
                == talas::project::ScoreValidationCode::InvalidRenderArtifact,
        "clip artifact sample rate must be exact and nonzero");

    invalid = score;
    invalid.clips.front().renderArtifact->mode =
        static_cast<talas::project::ScoreClipRenderMode>(99);
    error = talas::project::projectScoreValidationError(invalid, definitions);
    require(
        error.has_value()
            && error->code
                == talas::project::ScoreValidationCode::InvalidRenderArtifact,
        "clip artifact mode must remain a closed enum");
}
} // namespace

int main()
{
    testValidScoreResolvesTimeRoutingAndClipOrder();
    testTimelineTracksMuteAndSolo();
    testValidationRejectsAmbiguousOrUnboundAuthority();
    testCandidateEditsAreAtomicAndViewIndependent();
    testBoundsAndCanonicalTimeFailClosed();
    testClipRenderArtifactShapeIsClosedAndBounded();
}
