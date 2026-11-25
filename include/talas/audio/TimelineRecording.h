#pragma once

#include "talas/audio/SampleFileFormats.h"
#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Recording a Timeline to the hard disk: the mix of
// all its Sounds after their level and panning controls, each chosen output
// channel of the Master into a mono file of its own, or two channels into
// one interleaved stereo file, in the file format and bit depth chosen, at
// the current sample rate, from the start of the Timeline to its end. The
// channels are the Master's, so a Timeline records more channels than the
// hardware has.
namespace talas::audio
{
enum class TimelineRecordingFormat
{
    Wav,
    Aiff,
};

struct TimelineRecordingOptions
{
    // The file's path for a stereo file; a channel's file adds its number
    // to the name, "haiku 1.wav".
    std::filesystem::path destination;
    std::optional<std::filesystem::path> projectFilePath;
    double sampleRate = 48000.0;
    std::size_t maxBlockFrames = 4096;
    // The Master's output channels to record, from 1.
    std::vector<std::uint32_t> channels;
    // Two channels into one interleaved stereo file instead of two.
    bool interleavedStereo = false;
    TimelineRecordingFormat format = TimelineRecordingFormat::Wav;
    // 8, 16 or 24.
    std::uint16_t bits = 16;
    std::function<bool()> shouldCancel;
    // The recording's progress, for its thermometer:
    // the fraction recorded after each block, 1 at the end.
    std::function<void(double)> progress;
    // The Timeline played live and recorded as it is
    // heard (LiveTimelineRecording), so a WaitUntil waits for its MIDI key
    // down and MIDI faders and presets reach the recording; false renders it
    // offline from start to end, as recordTimelineToDisk does.
    bool live = false;
};

struct TimelineRecordingResult
{
    std::vector<std::filesystem::path> files;
    std::size_t frameCount = 0;
    std::string error;
    bool cancelled = false;

    [[nodiscard]] bool ok() const noexcept { return error.empty() && !cancelled; }
};

// The files a recording writes, in the order of its channels.
[[nodiscard]] std::vector<std::filesystem::path> timelineRecordingFiles(const TimelineRecordingOptions& options);

// The Timeline's output channels, its Master's; 0 when it has none.
[[nodiscard]] std::uint32_t timelineRecordingChannelCount(const talas::project::ProjectDocument& document);

// Channels of samples as a PCM AIFF file of 8, 16 or 24 bits, replacing
// what is at `destination`, as the Tape Recorder saves a recording in the
// AIFF format: empty, or what went wrong.
[[nodiscard]] std::string writeAiffFile(
    const std::filesystem::path& destination,
    double sampleRate,
    const std::vector<std::vector<double>>& channels,
    std::uint16_t bits);

// A recording written as WAV saved again as AIFF at the bits given and the
// WAV removed, as Record to disk saves a take in the file format chosen
//: empty, or what went wrong, the WAV kept then.
[[nodiscard]] std::string convertRecordingToAiff(
    const std::filesystem::path& wav,
    const std::filesystem::path& aiff,
    std::uint16_t bits);

// Channels of samples as a file of any of the sample file formats, WAVE, AIFF,
// IRCAM, SD-I or SD-II, at 8, 16 or 24 bits, replacing what is at
// `destination`: empty, or what went wrong.
[[nodiscard]] std::string writeSampleFile(
    const std::filesystem::path& destination,
    double sampleRate,
    const std::vector<std::vector<double>>& channels,
    std::uint16_t bits,
    SampleFileFormat format);

// A recording written as WAV saved again in the format chosen at the bits
// given and the WAV removed, as a DiskRecorder's take is saved in its
// Format: empty, or what went wrong, the WAV
// kept then.
[[nodiscard]] std::string convertRecordingToSampleFile(
    const std::filesystem::path& wav,
    const std::filesystem::path& destination,
    SampleFileFormat format,
    std::uint16_t bits);

[[nodiscard]] TimelineRecordingResult recordTimelineToDisk(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    TimelineRecordingOptions options);
}
