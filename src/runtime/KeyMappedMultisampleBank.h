#pragma once

#include "CheckedKeyMappedMultisampleState.h"
#include "CheckedSampleState.h"
#include "SampleReferencePitch.h"

#include "talas/runtime/Runtime.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// A KeyMappedMultisample's bank: every
// sample file in FirstSample's folder, in the order of their names. The
// loader gives each a table naming the bank's first sample; a first sample
// without such tables is a bank of one.
namespace talas::runtime::key_mapped_multisample_bank
{
// The most samples a bank maps.
inline constexpr std::size_t maximumSamples = 128;

// The bank `firstSample` starts, in the order of its file names, or nothing
// when that table is missing or the bank holds too many.
[[nodiscard]] inline std::optional<std::vector<const TableResource*>> bankTables(
    std::span<const TableResource> tables,
    std::string_view firstSample)
{
    std::vector<const TableResource*> bank;
    for (const auto& table : tables)
    {
        if (table.bank == firstSample)
            bank.push_back(&table);
    }
    if (bank.empty())
    {
        const auto first = std::ranges::find(tables, firstSample, &TableResource::id);
        if (first == tables.end())
            return std::nullopt;
        bank.push_back(&*first);
    }
    if (bank.size() > maximumSamples)
        return std::nullopt;
    std::ranges::stable_sort(bank, {}, [](const TableResource* table) -> const std::string& {
        return table->bankName;
    });
    return bank;
}

// How each sample plays and where its header places it.
struct Bank
{
    std::vector<checked_key_mapped_multisample::Sample> samples;
    std::vector<checked_key_mapped_multisample::SampleKeys> keys;
};

[[nodiscard]] inline Bank describe(
    std::span<const TableResource* const> tables,
    double sampleRate,
    bool headerLoops)
{
    Bank bank;
    bank.samples.reserve(tables.size());
    bank.keys.reserve(tables.size());
    for (const auto* table : tables)
    {
        const auto recorded = sample_reference_pitch::recordedFrequency(table->samplePitch);
        checked_key_mapped_multisample::Sample sample{
            table->samples,
            sample_reference_pitch::unityFrequency(recorded, table->sourceSampleRate, sampleRate),
            0.0,
            1.0,
            sample_reference_pitch::originalRateStep(table->sourceSampleRate, sampleRate),
        };
        // Loop loops at the loop in each file's header.
        if (headerLoops)
        {
            if (const auto header = checked_sample::headerLoop(table->forwardLoop, table->samples.size()))
            {
                sample.loopStart = header->start;
                sample.loopEnd = header->end;
            }
        }
        bank.samples.push_back(sample);
        checked_key_mapped_multisample::SampleKeys keys;
        keys.basePitch = checked_key_mapped_multisample::noteNumber(recorded);
        if (table->keyRange.has_value())
        {
            keys.lowNote = table->keyRange->lowNote;
            keys.highNote = table->keyRange->highNote;
            keys.lowVelocity = table->keyRange->lowVelocity;
            keys.highVelocity = table->keyRange->highVelocity;
        }
        bank.keys.push_back(keys);
    }
    return bank;
}

[[nodiscard]] inline std::optional<checked_key_mapped_multisample::Mapping> mapping(
    std::string_view choice) noexcept
{
    using checked_key_mapped_multisample::Mapping;
    if (choice == "onePerHalfStep")
        return Mapping::OnePerHalfStep;
    if (choice == "equallySpaced")
        return Mapping::EquallySpaced;
    if (choice == "byBasePitch")
        return Mapping::ByBasePitch;
    if (choice == "byPitchRange")
        return Mapping::ByPitchRange;
    return std::nullopt;
}

// The mapping with the halfStepsFrom choice: keyOne counts OnePerHalfStep
// from MIDI key 1, loFreq (or none, as in older
// projects) from the key LoFreq gives.
[[nodiscard]] inline std::optional<checked_key_mapped_multisample::Mapping> mapping(
    std::string_view choice, std::string_view halfStepsFrom) noexcept
{
    using checked_key_mapped_multisample::Mapping;
    const auto chosen = mapping(choice);
    if (chosen == Mapping::OnePerHalfStep && halfStepsFrom == "keyOne")
        return Mapping::OnePerHalfStepFromKeyOne;
    return chosen;
}
}
