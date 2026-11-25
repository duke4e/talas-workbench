#include "talas/dsp/LinearVocoderBank.h"
#include "talas/dsp/ScaleVocoderIntervals.h"
#include "talas/dsp/VocoderBandShaping.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

namespace
{
constexpr double sampleRate = 48000.0;

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

[[nodiscard]] std::vector<double> tone(
    double frequency,
    std::size_t frames,
    double amplitude = 1.0)
{
    std::vector<double> result(frames);
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        result[frame] = amplitude * std::sin(
            2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate);
    }
    return result;
}

[[nodiscard]] double rms(
    std::span<const double> samples,
    std::size_t firstFrame)
{
    double sum = 0.0;
    for (std::size_t frame = firstFrame; frame < samples.size(); ++frame)
        sum += samples[frame] * samples[frame];
    return std::sqrt(sum / static_cast<double>(samples.size() - firstFrame));
}

[[nodiscard]] double projectedAmplitude(
    std::span<const double> samples,
    std::size_t firstFrame,
    double frequency)
{
    double sine = 0.0;
    double cosine = 0.0;
    const auto count = samples.size() - firstFrame;
    for (std::size_t frame = firstFrame; frame < samples.size(); ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate;
        sine += samples[frame] * std::sin(phase);
        cosine += samples[frame] * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine) / static_cast<double>(count);
}

[[nodiscard]] talas::dsp::LinearVocoderBankConfiguration configuration(
    double carrierFrequency = 1000.0,
    double analysisFrequency = 1000.0,
    std::size_t bandCount = 1,
    double timeConstant = 0.005,
    double attackTime = talas::dsp::inheritVocoderEnvelopeTimeSeconds,
    double releaseTime = talas::dsp::inheritVocoderEnvelopeTimeSeconds)
{
    return {
        sampleRate,
        bandCount,
        carrierFrequency,
        carrierFrequency + (bandCount > 1 ? 2000.0 : 0.0),
        analysisFrequency,
        analysisFrequency + (bandCount > 1 ? 2000.0 : 0.0),
        0.35,
        0.35,
        timeConstant,
        talas::dsp::VocoderBankFrequencySpacing::Linear,
        attackTime,
        releaseTime,
    };
}

[[nodiscard]] std::array<talas::dsp::MappedVocoderBandConfiguration, 2>
mappedConfiguration(double carrierGain = 1.0)
{
    return {{
        {1000.0, 300.0, carrierGain, 1000.0, 250.0, 1.0},
        {4000.0, 600.0, 0.5, 4000.0, 500.0, 0.75},
    }};
}

void testRejectsInvalidPreparation()
{
    const auto rejects = [](auto config) {
        try
        {
            talas::dsp::PreparedLinearVocoderBank bank{config};
            (void) bank;
            return false;
        }
        catch (const std::invalid_argument&)
        {
            return true;
        }
    };
    auto invalid = configuration();
    invalid.bandCount = 0;
    require(rejects(invalid), "zero-band configuration should reject");
    invalid = configuration();
    invalid.bandCount = talas::dsp::maximumLinearVocoderBandCount + 1;
    require(rejects(invalid), "oversized band configuration should reject");
    invalid = configuration();
    invalid.analysisHighFrequency = sampleRate * 0.5;
    require(rejects(invalid), "Nyquist analysis range should reject");
    invalid = configuration();
    invalid.carrierBandwidthRatio = 0.0;
    require(rejects(invalid), "zero bandwidth should reject");
    invalid = configuration();
    invalid.analysisBandwidthRatio = 0.0;
    require(rejects(invalid), "zero analysis bandwidth should reject");
    invalid = configuration();
    invalid.envelopeTimeConstantSeconds = -0.1;
    require(rejects(invalid), "negative time constant should reject");
    invalid = configuration();
    invalid.envelopeAttackTimeSeconds = -0.5;
    require(rejects(invalid), "negative attack override should reject");
    invalid = configuration();
    invalid.envelopeReleaseTimeSeconds =
        std::numeric_limits<double>::infinity();
    require(rejects(invalid), "non-finite release override should reject");
    invalid = configuration();
    invalid.envelopeAttackTimeSeconds =
        std::numeric_limits<double>::max();
    require(rejects(invalid),
        "unrepresentable attack coefficient should reject");
}

void testAttackReleaseResolutionAndLegacyIdentity()
{
    const auto inherited = talas::dsp::resolveVocoderEnvelopeTimes(
        0.005,
        talas::dsp::inheritVocoderEnvelopeTimeSeconds,
        talas::dsp::inheritVocoderEnvelopeTimeSeconds);
    const auto mixed = talas::dsp::resolveVocoderEnvelopeTimes(
        0.005, 0.0, talas::dsp::inheritVocoderEnvelopeTimeSeconds);
    require(inherited.has_value()
            && inherited->attackSeconds == 0.005
            && inherited->releaseSeconds == 0.005,
        "both inheritance sentinels should resolve to legacy timeConstant");
    require(mixed.has_value() && mixed->attackSeconds == 0.0
            && mixed->releaseSeconds == 0.005,
        "attack and release should resolve independently");
    require(!talas::dsp::resolveVocoderEnvelopeTimes(0.005, -0.5, 0.1)
                .has_value(),
        "non-sentinel negative override should reject");

    constexpr std::size_t frames = 4096;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1000.0, frames, 0.75);
    std::vector<double> inheritedOutput(frames);
    std::vector<double> explicitOutput(frames);
    talas::dsp::PreparedLinearVocoderBank inheritedBank{
        configuration(1000.0, 1000.0, 1, 0.005)};
    talas::dsp::PreparedLinearVocoderBank explicitBank{
        configuration(1000.0, 1000.0, 1, 0.005, 0.005, 0.005)};
    require(inheritedBank.process(carrier, analysis, inheritedOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && explicitBank.process(carrier, analysis, explicitOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "legacy identity fixtures should process");
    require(inheritedOutput == explicitOutput,
        "inherited equal attack/release should be bit-identical to explicit legacy time");
}

void testAsymmetricAttackAndRelease()
{
    constexpr std::size_t activeFrames = 16384;
    constexpr std::size_t releaseFrames = 512;
    const auto carrier = tone(1000.0, activeFrames);
    const auto analysis = tone(1000.0, activeFrames);
    std::vector<double> output(activeFrames);
    talas::dsp::PreparedLinearVocoderBank fastAttackSlowRelease{
        configuration(1000.0, 1000.0, 1, 0.01, 0.0, 0.1)};
    talas::dsp::PreparedLinearVocoderBank slowAttackFastRelease{
        configuration(1000.0, 1000.0, 1, 0.01, 0.1, 0.0)};

    require(fastAttackSlowRelease.process(
                std::span<const double>{carrier}.first(releaseFrames),
                std::span<const double>{analysis}.first(releaseFrames),
                std::span<double>{output}.first(releaseFrames))
                == talas::dsp::LinearVocoderBankStatus::Ok
            && slowAttackFastRelease.process(
                std::span<const double>{carrier}.first(releaseFrames),
                std::span<const double>{analysis}.first(releaseFrames),
                std::span<double>{output}.first(releaseFrames))
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "asymmetric attack fixtures should process");
    require(fastAttackSlowRelease.envelope(0)
            > slowAttackFastRelease.envelope(0),
        "instant attack should rise faster than slow attack");

    fastAttackSlowRelease.reset();
    slowAttackFastRelease.reset();
    require(fastAttackSlowRelease.process(carrier, analysis, output)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && slowAttackFastRelease.process(carrier, analysis, output)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "asymmetric release fixtures should prime");
    std::array<double, releaseFrames> silence{};
    std::array<double, releaseFrames> releaseOutput{};
    require(fastAttackSlowRelease.process(
                silence, silence, releaseOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && slowAttackFastRelease.process(
                silence, silence, releaseOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "asymmetric release fixtures should process silence");
    require(fastAttackSlowRelease.envelope(0)
            > slowAttackFastRelease.envelope(0)
            && fastAttackSlowRelease.envelope(0) >= 0.0
            && slowAttackFastRelease.envelope(0) >= 0.0,
        "slow release should retain a larger non-negative envelope");
}

void testRelativeLinearAndLogarithmicLayouts()
{
    const auto linearRange =
        talas::dsp::deriveRelativeVocoderBankFrequencyRange(
            100.0, 1.0, 4,
            talas::dsp::VocoderBankFrequencySpacing::Linear);
    const auto logarithmicRange =
        talas::dsp::deriveRelativeVocoderBankFrequencyRange(
            100.0, 1.0, 4,
            talas::dsp::VocoderBankFrequencySpacing::Logarithmic);
    require(linearRange.has_value() && linearRange->highFrequency == 400.0,
        "relative linear range should end at base*(1+spacing*i)");
    require(
        logarithmicRange.has_value()
            && logarithmicRange->highFrequency == 800.0,
        "relative logarithmic range should end at base*2^(spacing*i)");
    require(
        !talas::dsp::deriveRelativeVocoderBankFrequencyRange(
            100.0, 1024.0, 64,
            talas::dsp::VocoderBankFrequencySpacing::Logarithmic).has_value(),
        "overflowing logarithmic range should reject");

    auto linear = configuration(100.0, 100.0, 4);
    linear.carrierHighFrequency = 400.0;
    linear.analysisHighFrequency = 400.0;
    linear.carrierBandwidthRatio = 0.25;
    linear.analysisBandwidthRatio = 0.75;
    talas::dsp::PreparedLinearVocoderBank linearBank{linear};
    require(linearBank.carrierCenterFrequency(0) == 100.0
            && linearBank.carrierCenterFrequency(1) == 200.0
            && linearBank.carrierCenterFrequency(2) == 300.0
            && linearBank.carrierCenterFrequency(3) == 400.0,
        "linear bank should expose arithmetic centers");
    require(linearBank.configuration().carrierBandwidthRatio == 0.25
            && linearBank.configuration().analysisBandwidthRatio == 0.75,
        "bank should retain separate carrier and analysis bandwidths");

    auto logarithmic = linear;
    logarithmic.carrierHighFrequency = 800.0;
    logarithmic.analysisHighFrequency = 800.0;
    logarithmic.frequencySpacing =
        talas::dsp::VocoderBankFrequencySpacing::Logarithmic;
    talas::dsp::PreparedLinearVocoderBank logarithmicBank{logarithmic};
    require(std::abs(logarithmicBank.analysisCenterFrequency(0) - 100.0) < 1.0e-12
            && std::abs(logarithmicBank.analysisCenterFrequency(1) - 200.0) < 1.0e-12
            && std::abs(logarithmicBank.analysisCenterFrequency(2) - 400.0) < 1.0e-12
            && std::abs(logarithmicBank.analysisCenterFrequency(3) - 800.0) < 1.0e-12,
        "logarithmic bank should expose geometric centers");
}

// Spacing is in hz between linear centers and
// in nn (half steps) between logarithmic ones.
void testTunableSpacingInHertzOrHalfSteps()
{
    const auto linearRange = talas::dsp::deriveVocoderBankFrequencyRange(
        100.0, 50.0, 4, talas::dsp::VocoderBankFrequencySpacing::Linear);
    require(linearRange.has_value() && linearRange->lowFrequency == 100.0
            && linearRange->highFrequency == 250.0,
        "linear spacing should add its hertz between centers");
    const auto logarithmicRange = talas::dsp::deriveVocoderBankFrequencyRange(
        100.0, 4.0, 4, talas::dsp::VocoderBankFrequencySpacing::Logarithmic);
    require(logarithmicRange.has_value()
            && std::abs(logarithmicRange->highFrequency - 200.0) < 1.0e-12,
        "three steps of four half steps should span an octave");
    require(!talas::dsp::deriveVocoderBankFrequencyRange(
                100.0, 0.0, 4, talas::dsp::VocoderBankFrequencySpacing::Linear)
                .has_value()
            && !talas::dsp::deriveVocoderBankFrequencyRange(
                100.0, 12.0 * 1024.0, 64,
                talas::dsp::VocoderBankFrequencySpacing::Logarithmic)
                .has_value(),
        "zero and overflowing spacing should reject");
}

void testScaleIntervalParsingAndExplicitCenters()
{
    const auto layout = talas::dsp::deriveScaleVocoderIntervalLayout(
        "  -12\t0  7.5\n12 ", 69.0, 4, sampleRate);
    require(layout.status == talas::dsp::ScaleVocoderIntervalStatus::Ok,
        "valid signed fractional interval layout should parse");
    require(layout.count == 4
            && std::abs(layout.centerFrequencies[0] - 220.0) < 1.0e-12
            && std::abs(layout.centerFrequencies[1] - 440.0) < 1.0e-12
            && std::abs(layout.centerFrequencies[3] - 880.0) < 1.0e-12,
        "known tonic/interval centers should derive exactly");
    const auto explicitPlus = talas::dsp::deriveScaleVocoderIntervalLayout(
        "+0 +12", 69.0, 2, sampleRate);
    require(explicitPlus.status == talas::dsp::ScaleVocoderIntervalStatus::Ok,
        "explicit positive interval signs should parse");

    const auto status = [](std::string_view text, std::size_t count = 2) {
        return talas::dsp::deriveScaleVocoderIntervalLayout(
            text, 69.0, count, sampleRate).status;
    };
    require(status("0 x") == talas::dsp::ScaleVocoderIntervalStatus::InvalidToken,
        "non-decimal interval should reject");
    require(status("0 0") == talas::dsp::ScaleVocoderIntervalStatus::IntervalsNotIncreasing,
        "duplicate interval should reject");
    require(status("12 0") == talas::dsp::ScaleVocoderIntervalStatus::IntervalsNotIncreasing,
        "descending interval should reject");
    require(status("-129 0") == talas::dsp::ScaleVocoderIntervalStatus::IntervalOutOfRange,
        "out-of-range interval should reject");
    require(status("") == talas::dsp::ScaleVocoderIntervalStatus::InvalidCount,
        "a scale without intervals should reject");
    // The scale may have any number of steps; the bands take its
    // first ones.
    const auto truncated = talas::dsp::deriveScaleVocoderIntervalLayout(
        "0 4 7", 69.0, 2, sampleRate);
    require(truncated.status == talas::dsp::ScaleVocoderIntervalStatus::Ok
            && truncated.count == 2
            && std::abs(truncated.centerFrequencies[1] - 440.0 * std::exp2(4.0 / 12.0)) < 1.0e-9,
        "a scale with more steps than bands should take its first steps");
    // The scale repeats each octave for as many bands as there are.
    const auto repeated = talas::dsp::deriveScaleVocoderIntervalLayout(
        "0 7", 69.0, 5, sampleRate);
    require(repeated.status == talas::dsp::ScaleVocoderIntervalStatus::Ok
            && repeated.count == 5
            && std::abs(repeated.centerFrequencies[0] - 440.0) < 1.0e-9
            && std::abs(repeated.centerFrequencies[2] - 880.0) < 1.0e-9
            && std::abs(repeated.centerFrequencies[3]
                    - 880.0 * std::exp2(7.0 / 12.0)) < 1.0e-9
            && std::abs(repeated.centerFrequencies[4] - 1760.0) < 1.0e-9,
        "a two-step scale should repeat each octave across five bands");
    require(
        talas::dsp::deriveScaleVocoderIntervalLayout(
            std::string(talas::dsp::maximumScaleVocoderIntervalTextBytes + 1, '0'),
            69.0, 1, sampleRate).status
            == talas::dsp::ScaleVocoderIntervalStatus::TextTooLong,
        "oversized interval text should reject before parsing");
    require(
        talas::dsp::deriveScaleVocoderIntervalLayout(
            "0", 10000.0, 1, sampleRate).status
            == talas::dsp::ScaleVocoderIntervalStatus::InvalidDerivedFrequency,
        "out-of-Nyquist tonic should reject");

    auto config = configuration(220.0, 220.0, layout.count);
    config.carrierHighFrequency = 880.0;
    config.analysisHighFrequency = 880.0;
    talas::dsp::PreparedLinearVocoderBank bank{
        config, layout.centers(), layout.centers()};
    require(std::abs(bank.carrierCenterFrequency(2)
                - layout.centerFrequencies[2]) < 1.0e-12,
        "explicit prepared center should retain nonuniform interval frequency");
}

void testTransfersOnlyMatchingAnalysisBand()
{
    constexpr std::size_t frames = 16384;
    const auto carrier = tone(1000.0, frames);
    const auto matchingAnalysis = tone(1000.0, frames);
    const auto remoteAnalysis = tone(4000.0, frames);
    std::vector<double> matching(frames);
    std::vector<double> remote(frames);
    talas::dsp::PreparedLinearVocoderBank matchingBank{configuration()};
    talas::dsp::PreparedLinearVocoderBank remoteBank{configuration()};
    require(
        matchingBank.process(carrier, matchingAnalysis, matching)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "matching analysis band should process");
    require(
        remoteBank.process(carrier, remoteAnalysis, remote)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "remote analysis band should process");
    require(
        rms(matching, frames / 2) > rms(remote, frames / 2) * 4.0,
        "matching analysis band should dominate remote analysis energy");
}

void testIsolatedEnvelopeSelectsMatchingCarrierBand()
{
    constexpr std::size_t frames = 32768;
    auto carrier = tone(1000.0, frames, 0.5);
    const auto highCarrier = tone(4000.0, frames, 0.5);
    for (std::size_t frame = 0; frame < carrier.size(); ++frame)
        carrier[frame] += highCarrier[frame];
    const auto analysis = tone(1000.0, frames);
    std::vector<double> output(frames);
    auto config = configuration(1000.0, 1000.0, 2);
    config.carrierHighFrequency = 4000.0;
    config.analysisHighFrequency = 4000.0;
    // Bands a fifth of their 3 kHz spacing wide.
    config.carrierBandwidthRatio = 0.2;
    config.analysisBandwidthRatio = 0.2;
    talas::dsp::PreparedLinearVocoderBank bank{config};
    require(
        bank.process(carrier, analysis, output)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "two-band isolation fixture should process");
    const auto matching = projectedAmplitude(output, frames / 2, 1000.0);
    const auto nonmatching = projectedAmplitude(output, frames / 2, 4000.0);
    require(matching > nonmatching * 4.0,
        "one analysis band should select its matching carrier band");
}

void testEnvelopeControlsWetEnergy()
{
    constexpr std::size_t frames = 8192;
    const auto carrier = tone(1000.0, frames);
    const auto loudAnalysis = tone(1000.0, frames, 1.0);
    const auto quietAnalysis = tone(1000.0, frames, 0.25);
    std::vector<double> loud(frames);
    std::vector<double> quiet(frames);
    talas::dsp::PreparedLinearVocoderBank loudBank{configuration()};
    talas::dsp::PreparedLinearVocoderBank quietBank{configuration()};
    require(
        loudBank.process(carrier, loudAnalysis, loud)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "loud analysis envelope should process");
    require(
        quietBank.process(carrier, quietAnalysis, quiet)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "quiet analysis envelope should process");
    const auto ratio = rms(loud, frames / 2) / rms(quiet, frames / 2);
    require(ratio > 3.9 && ratio < 4.1,
        "analysis amplitude should scale steady-state wet energy");
}

void testPartitionAndResetAreExact()
{
    constexpr std::size_t frames = 4096;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1000.0, frames, 0.75);
    std::vector<double> whole(frames);
    std::vector<double> partitioned(frames);
    std::vector<double> resetOutput(frames);
    talas::dsp::PreparedLinearVocoderBank wholeBank{configuration()};
    talas::dsp::PreparedLinearVocoderBank partitionedBank{configuration()};
    require(
        wholeBank.process(carrier, analysis, whole)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "whole buffer should process");
    constexpr std::array<std::size_t, 7> partitions{
        1, 17, 256, 3, 511, 64, 29};
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < frames)
    {
        const auto count = std::min(
            partitions[partition++ % partitions.size()], frames - offset);
        require(
            partitionedBank.process(
                std::span<const double>{carrier}.subspan(offset, count),
                std::span<const double>{analysis}.subspan(offset, count),
                std::span<double>{partitioned}.subspan(offset, count))
                == talas::dsp::LinearVocoderBankStatus::Ok,
            "partitioned buffer should process");
        offset += count;
    }
    require(whole == partitioned,
        "whole and irregular partitions should be bit-identical");
    partitionedBank.reset();
    require(
        partitionedBank.process(carrier, analysis, resetOutput)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "reset bank should process");
    require(whole == resetOutput,
        "reset should reproduce first-run output exactly");
}

void testStateSnapshotRestoresExactBankWithoutAllocation()
{
    constexpr std::size_t warmFrames = 1024;
    constexpr std::size_t retryFrames = 2048;
    constexpr std::size_t diversionFrames = 333;
    auto config = configuration(500.0, 500.0, 4, 0.015, 0.002, 0.04);
    config.carrierHighFrequency = 3500.0;
    config.analysisHighFrequency = 3500.0;
    config.frequencySpacing =
        talas::dsp::VocoderBankFrequencySpacing::Logarithmic;

    const auto carrier = tone(1000.0, warmFrames + retryFrames);
    const auto analysis = tone(
        1500.0, warmFrames + retryFrames, 0.65);
    std::vector<double> warmOutput(warmFrames);
    std::vector<double> expected(retryFrames);
    std::vector<double> actual(retryFrames);
    std::array<double, diversionFrames> diversionCarrier{};
    std::array<double, diversionFrames> diversionAnalysis{};
    std::array<double, diversionFrames> diversionOutput{};
    for (std::size_t frame = 0; frame < diversionFrames; ++frame)
    {
        diversionCarrier[frame] = frame % 3 == 0 ? -0.75 : 0.25;
        diversionAnalysis[frame] = frame % 5 == 0 ? 1.0 : -0.5;
    }

    talas::dsp::PreparedLinearVocoderBank bank{config};
    require(bank.process(
                std::span<const double>{carrier}.first(warmFrames),
                std::span<const double>{analysis}.first(warmFrames),
                warmOutput)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "snapshot fixture should warm every bank state field");

    talas::dsp::LinearVocoderBankState checkpoint{};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto captureStatus = bank.captureState(checkpoint);
    countAllocations.store(false, std::memory_order_release);
    require(captureStatus == talas::dsp::LinearVocoderBankStatus::Ok,
        "warmed bank state should capture");
    require(checkpoint.bandCount == config.bandCount,
        "snapshot should authenticate its exact active band count");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "bank state capture should not allocate");

    require(bank.process(
                std::span<const double>{carrier}.subspan(
                    warmFrames, retryFrames),
                std::span<const double>{analysis}.subspan(
                    warmFrames, retryFrames),
                expected)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "snapshot reference tail should process");
    require(bank.process(
                diversionCarrier, diversionAnalysis, diversionOutput)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "snapshot fixture should diverge all mutable state");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto restoreStatus = bank.restoreState(checkpoint);
    countAllocations.store(false, std::memory_order_release);
    require(restoreStatus == talas::dsp::LinearVocoderBankStatus::Ok,
        "valid bank snapshot should restore");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "bank state restore should not allocate");
    require(bank.process(
                std::span<const double>{carrier}.subspan(
                    warmFrames, retryFrames),
                std::span<const double>{analysis}.subspan(
                    warmFrames, retryFrames),
                actual)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "restored bank retry should process");
    require(expected == actual,
        "restoring every carrier/analysis/envelope field should reproduce the exact tail");

    talas::dsp::LinearVocoderBankState beforeInvalid{};
    require(bank.captureState(beforeInvalid)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "post-retry bank state should capture");
    auto malformed = beforeInvalid;
    malformed.bands[malformed.bandCount - 1].analysisSecondIntegrator =
        std::numeric_limits<double>::quiet_NaN();
    require(bank.restoreState(malformed)
            == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "non-finite late-band snapshot should reject");
    talas::dsp::LinearVocoderBankState afterInvalid{};
    require(bank.captureState(afterInvalid)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && afterInvalid == beforeInvalid,
        "invalid late-band snapshot should not partially mutate the bank");

    malformed = beforeInvalid;
    malformed.bandCount -= 1;
    require(bank.restoreState(malformed)
            == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "snapshot from a different bank shape should reject");
    malformed = beforeInvalid;
    malformed.bands.front().envelope = -1.0;
    require(bank.restoreState(malformed)
            == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "negative envelope authority should reject");
    require(bank.captureState(afterInvalid)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && afterInvalid == beforeInvalid,
        "every malformed snapshot should leave entry state unchanged");

    auto foreignConfig = config;
    foreignConfig.carrierLowFrequency = 600.0;
    foreignConfig.carrierHighFrequency = 3600.0;
    talas::dsp::PreparedLinearVocoderBank foreignBank{foreignConfig};
    talas::dsp::LinearVocoderBankState foreignState{};
    require(foreignBank.captureState(foreignState)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.restoreState(foreignState)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "same-sized snapshot from a different filter configuration should reject");

    talas::dsp::PreparedLinearVocoderBank shapedBank{config};
    constexpr std::array<double, 4> shapedGains{0.5, 0.75, 1.25, 2.0};
    talas::dsp::LinearVocoderBankState shapedState{};
    require(shapedBank.configurePreparedCarrierGains(shapedGains)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && shapedBank.captureState(shapedState)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.restoreState(shapedState)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "same-config snapshot with different prepared band authority should reject");
    require(bank.captureState(afterInvalid)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && afterInvalid == beforeInvalid,
        "foreign snapshot rejection should preserve the complete entry bank");

    bank.reset();
    talas::dsp::PreparedLinearVocoderBank fresh{config};
    talas::dsp::LinearVocoderBankState resetState{};
    talas::dsp::LinearVocoderBankState freshState{};
    require(bank.captureState(resetState)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && fresh.captureState(freshState)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && resetState == freshState,
        "reset snapshot should equal a freshly prepared bank exactly");
}

void testMappedStateSnapshotRestoresCompleteConfigurationAtomically()
{
    constexpr std::size_t warmFrames = 1024;
    constexpr std::size_t retryFrames = 2048;
    const auto carrier = tone(1000.0, warmFrames + retryFrames, 0.75);
    const auto analysis = tone(1000.0, warmFrames + retryFrames, 0.65);
    const auto mapping = mappedConfiguration();
    auto changedMapping = mappedConfiguration(0.25);
    changedMapping[0].analysisGain = 0.5;

    talas::dsp::PreparedLinearVocoderBank bank{sampleRate, 2, 0.005};
    talas::dsp::LinearVocoderBankState initial{};
    require(bank.captureState(initial)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && initial.mappedConfigurationMode
            && !initial.mappedConfigurationReady,
        "fresh mapped-bank snapshot should preserve unready authority");

    std::vector<double> warmOutput(warmFrames);
    require(bank.configureMappedBands(mapping)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(
                std::span<const double>{carrier}.first(warmFrames),
                std::span<const double>{analysis}.first(warmFrames),
                warmOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "mapped snapshot fixture should warm mapping A");
    require(bank.restoreState(initial)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "mapped bank should restore its exact initial unready snapshot");
    talas::dsp::LinearVocoderBankState restoredInitial{};
    require(bank.captureState(restoredInitial)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && restoredInitial == initial,
        "initial mapped restore should recover all authority and history exactly");
    std::array<double, 16> rejected{};
    require(bank.process(
                std::span<const double>{carrier}.first(rejected.size()),
                std::span<const double>{analysis}.first(rejected.size()),
                rejected)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "restored unready mapped bank should remain fail closed");

    require(bank.configureMappedBands(mapping)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(
                std::span<const double>{carrier}.first(warmFrames),
                std::span<const double>{analysis}.first(warmFrames),
                warmOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "mapped snapshot fixture should re-warm mapping A");
    talas::dsp::LinearVocoderBankState checkpoint{};
    require(bank.captureState(checkpoint)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && checkpoint.mappedConfigurationReady,
        "warmed mapping A should capture complete ready authority");

    std::vector<double> expected(retryFrames);
    std::vector<double> actual(retryFrames);
    require(bank.process(
                std::span<const double>{carrier}.subspan(
                    warmFrames, retryFrames),
                std::span<const double>{analysis}.subspan(
                    warmFrames, retryFrames),
                expected)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "mapping A reference tail should process");
    require(bank.configureMappedBands(changedMapping)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(
                std::span<const double>{carrier}.first(warmFrames),
                std::span<const double>{analysis}.first(warmFrames),
                warmOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "mapping B should replace and diverge all mutable mapped state");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto restoreStatus = bank.restoreState(checkpoint);
    countAllocations.store(false, std::memory_order_release);
    require(restoreStatus == talas::dsp::LinearVocoderBankStatus::Ok
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "complete mapping A restore should succeed without allocation");
    talas::dsp::LinearVocoderBankState restoredCheckpoint{};
    require(bank.captureState(restoredCheckpoint)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && restoredCheckpoint == checkpoint,
        "mapped restore should recover coefficients, gains, readiness, and history exactly");
    require(bank.process(
                std::span<const double>{carrier}.subspan(
                    warmFrames, retryFrames),
                std::span<const double>{analysis}.subspan(
                    warmFrames, retryFrames),
                actual)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && actual == expected,
        "restored mapping A should reproduce its exact reference tail");

    talas::dsp::LinearVocoderBankState beforeInvalid{};
    require(bank.captureState(beforeInvalid)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "post-retry mapped state should capture");
    auto malformed = beforeInvalid;
    malformed.authority[1].carrierA1 =
        std::numeric_limits<double>::quiet_NaN();
    require(bank.restoreState(malformed)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "non-finite mapped coefficient authority should reject");
    malformed = beforeInvalid;
    malformed.authority[1].carrierCenterFrequency += 1.0;
    require(bank.restoreState(malformed)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "mapped coefficient authority inconsistent with its center should reject");
    malformed = initial;
    malformed.mappedConfigurationReady = true;
    require(bank.restoreState(malformed)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "unconfigured mapped authority must not authenticate as ready");
    malformed = initial;
    malformed.bands[0].envelope = 0.5;
    require(bank.restoreState(malformed)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "unready mapped authority must not authenticate warmed history");
    malformed = checkpoint;
    malformed.mappedConfigurationReady = false;
    for (std::size_t index = 0; index < malformed.bandCount; ++index)
        malformed.bands[index] = {};
    require(bank.restoreState(malformed)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "unready mapped snapshot must not retain configured coefficient authority");
    talas::dsp::LinearVocoderBankState afterInvalid{};
    require(bank.captureState(afterInvalid)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && afterInvalid == beforeInvalid,
        "every invalid mapped snapshot should preserve complete entry state");

    bank.reset();
    talas::dsp::LinearVocoderBankState resetState{};
    require(bank.captureState(resetState)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && resetState == initial,
        "mapped reset should restore pristine unready authority and history exactly");
}

void testFailuresClearOutput()
{
    std::array<double, 4> carrier{1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> analysis{1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> output{1.0, 1.0, 1.0, 1.0};
    talas::dsp::PreparedLinearVocoderBank bank{configuration()};
    require(
        bank.process(
            carrier,
            std::span<const double>{analysis}.first(3),
            output)
            == talas::dsp::LinearVocoderBankStatus::InvalidBufferShape,
        "shape mismatch should reject");
    require(std::ranges::all_of(output, [](double value) { return value == 0.0; }),
        "shape rejection should clear output");
    output.fill(1.0);
    carrier[2] = std::numeric_limits<double>::quiet_NaN();
    require(
        bank.process(carrier, analysis, output)
            == talas::dsp::LinearVocoderBankStatus::NonFiniteInput,
        "non-finite input should reject");
    require(std::ranges::all_of(output, [](double value) { return value == 0.0; }),
        "input rejection should clear output");
    carrier[2] = 1.0;
    std::array<double, 4> recovered{};
    std::array<double, 4> freshOutput{};
    talas::dsp::PreparedLinearVocoderBank fresh{configuration()};
    require(
        bank.process(carrier, analysis, recovered)
            == talas::dsp::LinearVocoderBankStatus::Ok
        && fresh.process(carrier, analysis, freshOutput)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "bank should recover after fail-closed reset");
    require(recovered == freshOutput,
        "failure should reset partial filter and envelope state");
}

void testMappedConfigurationIsFailureAtomicAndStateCoherent()
{
    constexpr std::size_t frames = 4096;
    auto carrier = tone(1000.0, frames, 0.75);
    const auto upperCarrier = tone(4000.0, frames, 0.25);
    for (std::size_t frame = 0; frame < frames; ++frame)
        carrier[frame] += upperCarrier[frame];
    const auto analysis = tone(1000.0, frames);
    const auto mapping = mappedConfiguration();

    talas::dsp::PreparedLinearVocoderBank ordinary{
        configuration(1000.0, 1000.0, 2)};
    talas::dsp::LinearVocoderBankState ordinaryBefore{};
    talas::dsp::LinearVocoderBankState ordinaryAfter{};
    require(ordinary.captureState(ordinaryBefore)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && ordinary.configureMappedBands(mapping)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration
            && ordinary.captureState(ordinaryAfter)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && ordinaryAfter == ordinaryBefore,
        "wrong-mode mapped configuration must preserve immutable ordinary-bank authority");
    talas::dsp::PreparedLinearVocoderBank ordinaryReference{
        configuration(1000.0, 1000.0, 2)};
    std::vector<double> ordinaryOutput(frames);
    std::vector<double> ordinaryExpected(frames);
    require(ordinary.process(carrier, analysis, ordinaryOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && ordinaryReference.process(
                carrier, analysis, ordinaryExpected)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && ordinaryOutput == ordinaryExpected,
        "wrong-mode mapped rejection must preserve ordinary-bank output identity");

    talas::dsp::PreparedLinearVocoderBank whole{sampleRate, 2, 0.005};
    talas::dsp::PreparedLinearVocoderBank partitioned{sampleRate, 2, 0.005};
    require(
        whole.configureMappedBands(mapping)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "valid mapped configuration should prepare");
    std::vector<double> expected(frames);
    std::vector<double> actual(frames);
    require(
        whole.process(carrier, analysis, expected)
            == talas::dsp::LinearVocoderBankStatus::Ok,
        "whole mapped configuration should process");

    constexpr std::array<std::size_t, 5> partitions{31, 256, 7, 509, 43};
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < frames)
    {
        require(
            partitioned.configureMappedBands(mapping)
                == talas::dsp::LinearVocoderBankStatus::Ok,
            "unchanged mapped configuration should remain prepared");
        const auto count = std::min(
            partitions[partition++ % partitions.size()], frames - offset);
        require(
            partitioned.process(
                std::span<const double>{carrier}.subspan(offset, count),
                std::span<const double>{analysis}.subspan(offset, count),
                std::span<double>{actual}.subspan(offset, count))
                == talas::dsp::LinearVocoderBankStatus::Ok,
            "partitioned mapped configuration should process");
        offset += count;
    }
    require(expected == actual,
        "unchanged mapped configuration should retain exact filter state");

    auto invalid = mapping;
    invalid[1].carrierCenterFrequency = mapping[0].carrierCenterFrequency;
    require(
        partitioned.configureMappedBands(invalid)
            == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "non-increasing mapped centers should reject");
    std::array<double, 16> rejected{};
    require(
        partitioned.process(
            std::span<const double>{carrier}.first(rejected.size()),
            std::span<const double>{analysis}.first(rejected.size()),
            rejected)
            == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "failed mapped configuration should make processing fail closed");

    const auto changed = mappedConfiguration(0.25);
    talas::dsp::PreparedLinearVocoderBank fresh{sampleRate, 2, 0.005};
    require(
        partitioned.configureMappedBands(changed)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && fresh.configureMappedBands(changed)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "valid changed mapping should recover after rejection");
    std::vector<double> recovered(frames);
    std::vector<double> freshOutput(frames);
    require(
        partitioned.process(carrier, analysis, recovered)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && fresh.process(carrier, analysis, freshOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "changed mapping should process after a state reset");
    require(recovered == freshOutput,
        "changed mapping should restart from the same state as a fresh bank");

    auto muted = mapping;
    for (auto& band : muted)
        band.analysisGain = 0.0;
    talas::dsp::PreparedLinearVocoderBank mutedBank{sampleRate, 2, 0.005};
    std::vector<double> mutedOutput(frames, 1.0);
    require(
        mutedBank.configureMappedBands(muted)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && mutedBank.process(carrier, analysis, mutedOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "zero mapped analysis gains should process");
    require(std::ranges::all_of(
                mutedOutput, [](double sample) { return sample == 0.0; }),
        "zero mapped analysis gains should produce exact wet silence");
}

void testMappedConfigureAndProcessDoNotAllocate()
{
    constexpr std::size_t frames = 256;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1000.0, frames);
    const auto mapping = mappedConfiguration();
    std::array<double, frames> output{};
    talas::dsp::PreparedLinearVocoderBank bank{sampleRate, 2, 0.005};

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto configureStatus = bank.configureMappedBands(mapping);
    const auto processStatus = bank.process(carrier, analysis, output);
    countAllocations.store(false, std::memory_order_release);
    require(
        configureStatus == talas::dsp::LinearVocoderBankStatus::Ok
            && processStatus == talas::dsp::LinearVocoderBankStatus::Ok,
        "prepared mapped configure and process should succeed");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared mapped configure and process should not allocate");
}

void testVocoderBandShapingLawAndBoundaries()
{
    constexpr std::array<double, 3> centers{250.0, 1000.0, 4000.0};
    const auto neutral = talas::dsp::deriveVocoderBandShaping(
        centers, 0.0, 0.0);
    require(neutral.status == talas::dsp::VocoderBandShapingStatus::Ok,
        "neutral vocoder shaping should derive");
    require(std::ranges::all_of(
                neutral.gains(), [](double gain) { return gain == 1.0; }),
        "neutral vocoder shaping should produce exact unity gains");

    const auto bright = talas::dsp::deriveVocoderBandShaping(
        centers, 1.0, 0.0);
    const auto dark = talas::dsp::deriveVocoderBandShaping(
        centers, -1.0, 0.0);
    const auto rolled = talas::dsp::deriveVocoderBandShaping(
        centers, 0.0, 4.0);
    require(bright.status == talas::dsp::VocoderBandShapingStatus::Ok
            && dark.status == talas::dsp::VocoderBandShapingStatus::Ok
            && rolled.status == talas::dsp::VocoderBandShapingStatus::Ok,
        "bounded tone and rolloff endpoints should derive");
    require(bright.carrierGains[0] == 0.5
            && bright.carrierGains[1] == 1.0
            && bright.carrierGains[2] == 2.0,
        "positive tone should tilt exact octave-spaced endpoints upward");
    require(dark.carrierGains[0] == 2.0
            && dark.carrierGains[1] == 1.0
            && dark.carrierGains[2] == 0.5,
        "negative tone should tilt exact octave-spaced endpoints downward");
    require(rolled.carrierGains[0] == 1.0
            && rolled.carrierGains[1] == 0.25
            && rolled.carrierGains[2] == 0.0625,
        "maximum rolloff should monotonically attenuate higher bands");

    constexpr std::array<double, 1> single{1000.0};
    const auto oneBand = talas::dsp::deriveVocoderBandShaping(single, 1.0, 4.0);
    require(oneBand.status == talas::dsp::VocoderBandShapingStatus::Ok
            && oneBand.carrierGains[0] == 0.25,
        "one-band shaping should use the fixed midpoint position");
    require(talas::dsp::deriveVocoderBandShaping(centers, 1.01, 0.0).status
                == talas::dsp::VocoderBandShapingStatus::InvalidTone,
        "out-of-range tone should reject");
    require(talas::dsp::deriveVocoderBandShaping(centers, 0.0, 4.01).status
                == talas::dsp::VocoderBandShapingStatus::InvalidRolloff,
        "out-of-range rolloff should reject");
    constexpr std::array<double, 2> unordered{1000.0, 500.0};
    require(talas::dsp::deriveVocoderBandShaping(unordered, 0.0, 0.0).status
                == talas::dsp::VocoderBandShapingStatus::InvalidCenterFrequency,
        "unordered shaping centers should reject");
    require(talas::dsp::deriveVocoderBandShaping(
                centers, std::numeric_limits<double>::quiet_NaN(), 0.0).status
                == talas::dsp::VocoderBandShapingStatus::InvalidTone,
        "non-finite tone should reject");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto allocationFixture = talas::dsp::deriveVocoderBandShaping(
        centers, 0.5, 2.0);
    countAllocations.store(false, std::memory_order_release);
    require(allocationFixture.status == talas::dsp::VocoderBandShapingStatus::Ok
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "bounded vocoder shaping derivation should not allocate");
}

void testPreparedCarrierGainPublicationIsFailureAtomic()
{
    constexpr std::size_t frames = 2048;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1000.0, frames);
    const auto config = configuration(1000.0, 1000.0, 2);
    constexpr std::array<double, 2> unity{1.0, 1.0};
    constexpr std::array<double, 2> shaped{0.5, 2.0};
    talas::dsp::PreparedLinearVocoderBank partitioned{config};
    talas::dsp::PreparedLinearVocoderBank whole{config};
    require(partitioned.configurePreparedCarrierGains(unity)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && whole.configurePreparedCarrierGains(unity)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "unity prepared carrier gains should publish");
    std::vector<double> expected(frames);
    std::vector<double> actual(frames);
    require(whole.process(carrier, analysis, expected)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "whole unity-gain bank should process");
    constexpr std::array<std::size_t, 4> partitions{17, 256, 3, 511};
    std::size_t offset = 0;
    std::size_t index = 0;
    while (offset < frames)
    {
        require(partitioned.configurePreparedCarrierGains(unity)
                    == talas::dsp::LinearVocoderBankStatus::Ok,
            "unchanged prepared gains should retain state");
        const auto count = std::min(
            partitions[index++ % partitions.size()], frames - offset);
        require(partitioned.process(
                    std::span<const double>{carrier}.subspan(offset, count),
                    std::span<const double>{analysis}.subspan(offset, count),
                    std::span<double>{actual}.subspan(offset, count))
                    == talas::dsp::LinearVocoderBankStatus::Ok,
            "partitioned prepared-gain bank should process");
        offset += count;
    }
    require(expected == actual,
        "unchanged prepared gains should preserve exact continuity");

    constexpr std::array<double, 2> invalid{
        1.0, std::numeric_limits<double>::quiet_NaN()};
    require(partitioned.configurePreparedCarrierGains(invalid)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "invalid prepared gain vector should reject");
    std::array<double, 8> rejected{};
    require(partitioned.process(
                std::span<const double>{carrier}.first(rejected.size()),
                std::span<const double>{analysis}.first(rejected.size()),
                rejected)
                == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "rejected prepared gains should make the bank fail closed");

    talas::dsp::PreparedLinearVocoderBank fresh{config};
    require(partitioned.configurePreparedCarrierGains(shaped)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && fresh.configurePreparedCarrierGains(shaped)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "valid changed prepared gains should recover");
    std::vector<double> recovered(frames);
    std::vector<double> freshOutput(frames);
    require(partitioned.process(carrier, analysis, recovered)
                == talas::dsp::LinearVocoderBankStatus::Ok
            && fresh.process(carrier, analysis, freshOutput)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "changed prepared gains should process from reset state");
    require(recovered == freshOutput,
        "changed prepared gains should match a fresh bank exactly");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto status = fresh.configurePreparedCarrierGains(shaped);
    countAllocations.store(false, std::memory_order_release);
    require(status == talas::dsp::LinearVocoderBankStatus::Ok
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared carrier-gain publication should not allocate");
}

void testChangingGainsKeepFilterAndEnvelopeState()
{
    constexpr std::size_t frames = 4096;
    constexpr std::size_t block = 64;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1000.0, frames);
    // Process in blocks while a gain changes by one part in 10^9 before
    // every block; the output must follow the steady reference.
    const auto requireContinuous = [&](
                                       talas::dsp::PreparedLinearVocoderBank& steadyBank,
                                       talas::dsp::PreparedLinearVocoderBank& movingBank,
                                       const auto& configureStep,
                                       const std::string& label) {
        std::vector<double> steady(frames);
        std::vector<double> moving(frames);
        require(steadyBank.process(carrier, analysis, steady)
                    == talas::dsp::LinearVocoderBankStatus::Ok,
            (label + ": steady bank should process").c_str());
        for (std::size_t offset = 0; offset < frames; offset += block)
        {
            configureStep(offset / block);
            require(movingBank.process(
                        std::span<const double>{carrier}.subspan(offset, block),
                        std::span<const double>{analysis}.subspan(offset, block),
                        std::span<double>{moving}.subspan(offset, block))
                        == talas::dsp::LinearVocoderBankStatus::Ok,
                (label + ": changing bank should process").c_str());
        }
        double peak = 0.0;
        double difference = 0.0;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            peak = std::max(peak, std::abs(steady[frame]));
            difference = std::max(
                difference, std::abs(steady[frame] - moving[frame]));
        }
        require(peak > 0.0 && difference < peak * 1.0e-6,
            (label + ": a tiny gain change must not reset filters or envelopes")
                .c_str());
    };
    const auto nudged = [](std::size_t step) {
        return step % 2 == 0 ? 1.0 : 1.0 + 1.0e-9;
    };

    const auto config = configuration(1000.0, 1000.0, 2);
    talas::dsp::PreparedLinearVocoderBank steadyPrepared{config};
    talas::dsp::PreparedLinearVocoderBank movingPrepared{config};
    constexpr std::array<double, 2> unity{1.0, 1.0};
    require(steadyPrepared.configurePreparedCarrierGains(unity)
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "prepared gains should publish");
    requireContinuous(steadyPrepared, movingPrepared,
        [&](std::size_t step) {
            const std::array<double, 2> gains{nudged(step), 1.0};
            require(movingPrepared.configurePreparedCarrierGains(gains)
                        == talas::dsp::LinearVocoderBankStatus::Ok,
                "changed prepared gains should publish");
        },
        "prepared carrier gains");

    talas::dsp::PreparedLinearVocoderBank steadyMapped{sampleRate, 2, 0.005};
    talas::dsp::PreparedLinearVocoderBank movingMapped{sampleRate, 2, 0.005};
    require(steadyMapped.configureMappedBands(mappedConfiguration())
                == talas::dsp::LinearVocoderBankStatus::Ok,
        "mapped bands should publish");
    requireContinuous(steadyMapped, movingMapped,
        [&](std::size_t step) {
            require(movingMapped.configureMappedBands(
                        mappedConfiguration(nudged(step)))
                        == talas::dsp::LinearVocoderBankStatus::Ok,
                "changed mapped gains should publish");
        },
        "mapped band gains");
}

// A retuned bank plays on with its filters, envelopes and carrier gains, as
// a bank built with the new tuning would from the same state.
void testRetuneKeepsFiltersEnvelopesAndGains()
{
    constexpr std::size_t frames = 2048;
    const auto carrier = tone(1100.0, frames);
    const auto analysis = tone(900.0, frames);
    const auto first = std::span<const double>{carrier}.first(frames / 2);
    const auto firstAnalysis = std::span<const double>{analysis}.first(frames / 2);
    const auto second = std::span<const double>{carrier}.subspan(frames / 2);
    const auto secondAnalysis = std::span<const double>{analysis}.subspan(frames / 2);
    const auto before = configuration(400.0, 300.0, 4, 0.02);
    auto after = configuration(700.0, 500.0, 4, 0.01);
    after.carrierBandwidthRatio = 0.5;
    constexpr std::array<double, 4> gains{1.0, 0.75, 0.5, 0.25};
    talas::dsp::PreparedLinearVocoderBank bank{before};
    std::vector<double> output(frames / 2);
    require(bank.configurePreparedCarrierGains(gains) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(first, firstAnalysis, output) == talas::dsp::LinearVocoderBankStatus::Ok,
        "the bank should play before it retunes");
    talas::dsp::LinearVocoderBankState played{};
    require(bank.captureState(played) == talas::dsp::LinearVocoderBankStatus::Ok,
        "the played bank should capture");

    auto invalid = after;
    invalid.carrierHighFrequency = sampleRate;
    // More bands than the bank reserves room for.
    auto otherCount = after;
    otherCount.bandCount = talas::dsp::maximumLinearVocoderBandCount + 1;
    talas::dsp::LinearVocoderBankState unchanged{};
    require(bank.retune(invalid) == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration
            && bank.retune(otherCount) == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration
            && bank.captureState(unchanged) == talas::dsp::LinearVocoderBankStatus::Ok
            && unchanged == played,
        "a refused retune should leave the bank unchanged");

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto retuned = bank.retune(after);
    countAllocations.store(false, std::memory_order_release);
    require(retuned == talas::dsp::LinearVocoderBankStatus::Ok
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "a retune should succeed without allocating");
    talas::dsp::LinearVocoderBankState state{};
    require(bank.captureState(state) == talas::dsp::LinearVocoderBankStatus::Ok
            && state.configuration == after && state.bands == played.bands,
        "a retune should keep the filters and envelopes");
    for (std::size_t index = 0; index < 4; ++index)
    {
        require(state.authority[index].carrierGain == gains[index],
            "a retune should keep the carrier gains");
    }

    talas::dsp::PreparedLinearVocoderBank reference{after};
    std::vector<double> expected(frames / 2);
    require(reference.configurePreparedCarrierGains(gains) == talas::dsp::LinearVocoderBankStatus::Ok
            && reference.restoreState(state) == talas::dsp::LinearVocoderBankStatus::Ok
            && reference.process(second, secondAnalysis, expected) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(second, secondAnalysis, output) == talas::dsp::LinearVocoderBankStatus::Ok
            && output == expected,
        "a retuned bank should play as one built with its tuning from the same state");

    talas::dsp::PreparedLinearVocoderBank mapped{sampleRate, 2, 0.005};
    require(mapped.retune(configuration(1000.0, 1000.0, 2)) == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "a mapped bank should refuse a retune");
}

// A bank takes back a state of an earlier tuning once it retunes to it.
void testRetuneToTakesBackAnEarlierTuning()
{
    constexpr std::size_t frames = 1024;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1300.0, frames);
    const auto before = configuration(500.0, 400.0, 3, 0.01);
    const auto after = configuration(800.0, 900.0, 3, 0.03);
    talas::dsp::PreparedLinearVocoderBank bank{before};
    talas::dsp::PreparedLinearVocoderBank twin{before};
    constexpr std::array<double, 3> gains{0.5, 1.0, 1.5};
    std::vector<double> output(frames);
    std::vector<double> twinOutput(frames);
    require(bank.configurePreparedCarrierGains(gains) == talas::dsp::LinearVocoderBankStatus::Ok
            && twin.configurePreparedCarrierGains(gains) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(carrier, analysis, output) == talas::dsp::LinearVocoderBankStatus::Ok
            && twin.process(carrier, analysis, twinOutput) == talas::dsp::LinearVocoderBankStatus::Ok,
        "the banks should play");
    talas::dsp::LinearVocoderBankState earlier{};
    require(bank.captureState(earlier) == talas::dsp::LinearVocoderBankStatus::Ok,
        "the bank should capture its earlier tuning");
    constexpr std::array<double, 3> unity{1.0, 1.0, 1.0};
    require(bank.retune(after) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.configurePreparedCarrierGains(unity) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(carrier, analysis, output) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.restoreState(earlier) == talas::dsp::LinearVocoderBankStatus::InvalidConfiguration,
        "a state of an earlier tuning should not restore into a retuned bank");
    talas::dsp::LinearVocoderBankState restored{};
    require(bank.retuneTo(earlier) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.restoreState(earlier) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.captureState(restored) == talas::dsp::LinearVocoderBankStatus::Ok
            && restored == earlier,
        "retuning to an earlier state should let it restore");
    require(bank.process(carrier, analysis, output) == talas::dsp::LinearVocoderBankStatus::Ok
            && twin.process(carrier, analysis, twinOutput) == talas::dsp::LinearVocoderBankStatus::Ok
            && output == twinOutput,
        "the restored bank should play on as its twin");
}

// A voice that reads NbrBands as it starts retunes the bank to its band
// count (2026-10-13): within the 64 bands the bank reserves, without
// allocating, the bank then playing as one built with that many bands, and
// a state of the earlier count still taken back.
void testRetuneChangesTheBandCountWithoutAllocating()
{
    constexpr std::size_t frames = 1024;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1300.0, frames);
    const auto three = configuration(500.0, 400.0, 3, 0.01);
    auto five = three;
    five.bandCount = 5;
    five.carrierHighFrequency = 4000.0;
    five.analysisHighFrequency = 4000.0;
    talas::dsp::PreparedLinearVocoderBank bank{three};
    talas::dsp::PreparedLinearVocoderBank twin{three};
    std::vector<double> output(frames);
    std::vector<double> twinOutput(frames);
    require(bank.process(carrier, analysis, output) == talas::dsp::LinearVocoderBankStatus::Ok
            && twin.process(carrier, analysis, twinOutput) == talas::dsp::LinearVocoderBankStatus::Ok,
        "the three-band banks should play");
    talas::dsp::LinearVocoderBankState earlier{};
    require(bank.captureState(earlier) == talas::dsp::LinearVocoderBankStatus::Ok,
        "the three-band bank should capture");
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto grown = bank.retune(five);
    countAllocations.store(false, std::memory_order_release);
    require(grown == talas::dsp::LinearVocoderBankStatus::Ok && bank.bandCount() == 5
            && allocationCount.load(std::memory_order_relaxed) == 0,
        "a retune should give the bank five bands without allocating");
    talas::dsp::PreparedLinearVocoderBank reference{five};
    std::vector<double> expected(frames);
    bank.reset();
    require(reference.process(carrier, analysis, expected) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.process(carrier, analysis, output) == talas::dsp::LinearVocoderBankStatus::Ok
            && output == expected,
        "a bank retuned to five bands should play as one built with five");
    talas::dsp::LinearVocoderBankState restored{};
    require(bank.retuneTo(earlier) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.restoreState(earlier) == talas::dsp::LinearVocoderBankStatus::Ok
            && bank.captureState(restored) == talas::dsp::LinearVocoderBankStatus::Ok
            && restored == earlier,
        "a state of three bands should restore into the five-band bank");
    require(bank.process(carrier, analysis, output) == talas::dsp::LinearVocoderBankStatus::Ok
            && twin.process(carrier, analysis, twinOutput) == talas::dsp::LinearVocoderBankStatus::Ok
            && output == twinOutput,
        "the restored bank should play on as its three-band twin");
}

void testProcessAndResetDoNotAllocate()
{
    constexpr std::size_t frames = 256;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1000.0, frames);
    std::array<double, frames> output{};
    auto config = configuration(300.0, 300.0, 16);
    config.carrierHighFrequency = 6000.0;
    config.analysisHighFrequency = 6000.0;
    talas::dsp::PreparedLinearVocoderBank bank{config};
    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto status = bank.process(carrier, analysis, output);
    bank.reset();
    countAllocations.store(false, std::memory_order_release);
    require(status == talas::dsp::LinearVocoderBankStatus::Ok,
        "allocation fixture should process");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "prepared process and reset should not allocate");
}

void testLocalTimingBudget()
{
    constexpr std::size_t frames = 256;
    constexpr std::size_t iterations = 400;
    const auto carrier = tone(1000.0, frames);
    const auto analysis = tone(1000.0, frames);
    std::array<double, frames> output{};
    auto config = configuration(300.0, 300.0, 16);
    config.carrierHighFrequency = 6000.0;
    config.analysisHighFrequency = 6000.0;
    talas::dsp::PreparedLinearVocoderBank bank{config};
    std::array<std::chrono::nanoseconds, iterations> timings{};
    for (std::size_t iteration = 0; iteration < iterations; ++iteration)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto status = bank.process(carrier, analysis, output);
        const auto end = std::chrono::steady_clock::now();
        require(status == talas::dsp::LinearVocoderBankStatus::Ok,
            "timing fixture should process");
        timings[iteration] = end - start;
    }
    std::ranges::sort(timings);
    const auto p99 = timings[(iterations * 99) / 100].count();
    std::cout << "linear-vocoder 16-band 256-frame p99 " << p99
              << " ns\n";
    if (!sanitizerInstrumented)
    {
        require(p99 < 5'333'333,
            "local linear-vocoder p99 should stay below one callback period");
    }
}
}

int main()
{
    testRejectsInvalidPreparation();
    testAttackReleaseResolutionAndLegacyIdentity();
    testAsymmetricAttackAndRelease();
    testRelativeLinearAndLogarithmicLayouts();
    testTunableSpacingInHertzOrHalfSteps();
    testScaleIntervalParsingAndExplicitCenters();
    testTransfersOnlyMatchingAnalysisBand();
    testIsolatedEnvelopeSelectsMatchingCarrierBand();
    testEnvelopeControlsWetEnergy();
    testPartitionAndResetAreExact();
    testStateSnapshotRestoresExactBankWithoutAllocation();
    testMappedStateSnapshotRestoresCompleteConfigurationAtomically();
    testFailuresClearOutput();
    testMappedConfigurationIsFailureAtomicAndStateCoherent();
    testMappedConfigureAndProcessDoNotAllocate();
    testVocoderBandShapingLawAndBoundaries();
    testPreparedCarrierGainPublicationIsFailureAtomic();
    testChangingGainsKeepFilterAndEnvelopeState();
    testRetuneKeepsFiltersEnvelopesAndGains();
    testRetuneToTakesBackAnEarlierTuning();
    testRetuneChangesTheBandCountWithoutAllocating();
    testProcessAndResetDoNotAllocate();
    testLocalTimingBudget();
    std::cout << "Linear vocoder bank tests passed\n";
    return 0;
}
