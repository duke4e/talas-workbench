#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Audio signals as controls: a Sound dropped
// into the controller editor gives its amplitude envelope, or its frequency
// envelope within a range of frequencies to expect, for as long as asked;
// the envelope is rendered offline from the Sound through an amplitude
// follower or a frequency tracker (in hertz), one value a frame.
namespace talas::runtime
{
enum class EnvelopeKind
{
    Amplitude,
    Frequency,
};

struct EnvelopeExtractionRequest
{
    std::string soundDefinitionId;
    EnvelopeKind kind = EnvelopeKind::Amplitude;
    double durationSeconds = 1.0;
    double minimumHz = 50.0;
    double maximumHz = 2000.0;
    double sampleRate = 48000.0;
    std::size_t maximumBlockFrames = 512;
};

struct EnvelopeExtractionResult
{
    std::vector<double> values;
    double sampleRate = 0.0;
    std::string error;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && !values.empty(); }
};

// The Sound's own duration in seconds when it has one to give: the length
// of the longest sample it plays, else none.
[[nodiscard]] std::optional<double> envelopeSoundDuration(
    const talas::project::ProjectDocument& document,
    std::string_view soundDefinitionId);

[[nodiscard]] EnvelopeExtractionResult extractEnvelope(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    const std::optional<std::filesystem::path>& projectFilePath,
    const EnvelopeExtractionRequest& request);
}
