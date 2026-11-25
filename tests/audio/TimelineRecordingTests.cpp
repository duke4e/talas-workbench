#include "talas/audio/TimelineRecording.h"
#include "talas/audio/WavReader.h"
#include "talas/core/BuiltinNodes.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Recording a Timeline to the hard disk, each output
// channel into a mono file of its own or two into an interleaved stereo
// file, in the file format and bit depth chosen.
namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const auto& registry()
{
    static const auto r = talas::core::makeBuiltinNodeRegistry();
    return r;
}

talas::project::ScoreTime seconds(double value)
{
    return {talas::project::ScoreTimeDomain::Seconds, value, 0};
}

// A Sound of `outputs` channels, 0.5 on the first, -0.25 on the third and
// silence on the others.
talas::project::ProjectSoundDefinition levels(std::uint32_t outputs)
{
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = "levels";
    sound.displayName = "Levels";
    for (std::uint32_t channel = 0; channel < outputs; ++channel)
    {
        talas::project::ProjectParameterExpression parameter;
        parameter.parameterId = "value";
        parameter.expression = channel == 0 ? "0.5" : channel == 2 ? "-0.25" : "0";
        const auto id = "level" + std::to_string(channel + 1);
        sound.graph.nodes.push_back({id, "talas.valToSound", {parameter}});
        sound.outputs.push_back({channel == 0 ? std::string{"audioOut"} : "out" + std::to_string(channel + 1),
            talas::core::PortType::Audio, id, "audioOut"});
    }
    return sound;
}

// One track of the Master's `outputs` channels with that Sound, 16 ms long.
talas::project::ProjectDocument haiku(std::uint32_t outputs = 4)
{
    talas::project::ProjectDocument document;
    document.soundDefinitions = {levels(outputs)};
    document.timeline.score.tracks = {
        {"trk1", "Trk 1", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", outputs,
            talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, outputs,
            talas::project::ScoreOverlapMode::Layer}};
    document.timeline.score.clips = {
        {"a", "trk1", "levels", seconds(0), seconds(0.016), seconds(0), seconds(0), seconds(0), 1},
    };
    return document;
}

std::filesystem::path directory()
{
    const auto path = std::filesystem::temp_directory_path()
        / ("talas-timeline-recording-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

talas::audio::TimelineRecordingOptions options(const std::filesystem::path& folder, std::string name)
{
    talas::audio::TimelineRecordingOptions result;
    result.destination = folder / std::move(name);
    result.sampleRate = 1000.0;
    result.maxBlockFrames = 8;
    return result;
}

void requireLevel(const std::vector<double>& samples, double expected, double tolerance, const std::string& what)
{
    require(samples.size() == 16, what + ": the recording lasts as long as the Timeline");
    for (const auto sample : samples)
        require(std::abs(sample - expected) < tolerance, what + ": " + std::to_string(sample));
}

void testEachChannelIntoItsOwnFile()
{
    const auto folder = directory();
    auto request = options(folder, "haiku.wav");
    request.channels = {1, 3};
    request.bits = 24;
    const auto recorded = talas::audio::recordTimelineToDisk(haiku(), registry(), request);
    require(recorded.ok() && recorded.files.size() == 2 && recorded.frameCount == 16,
        "record the Timeline's channels: " + recorded.error);
    require(recorded.files[0].filename() == "haiku 1.wav" && recorded.files[1].filename() == "haiku 3.wav",
        "each output channel into a separate audio file");
    const auto one = talas::audio::readSampleFile(recorded.files[0]);
    const auto three = talas::audio::readSampleFile(recorded.files[1]);
    require(one.ok() && three.ok() && one.channels.size() == 1 && three.channels.size() == 1 && one.sampleRate == 1000,
        "mono files at the current sample rate");
    requireLevel(one.channels[0], 0.5, 1e-6, "channel 1 is what the Timeline sends to output 1");
    requireLevel(three.channels[0], -0.25, 1e-6, "channel 3 is what it sends to output 3");
    require(!std::filesystem::exists(folder / "haiku 2.wav"), "only the channels checked are recorded");

    // A thermometer shows the recording in progress.
    std::vector<double> progress;
    request.maxBlockFrames = 4;
    request.progress = [&progress](double fraction) { progress.push_back(fraction); };
    require(talas::audio::recordTimelineToDisk(haiku(), registry(), request).ok()
            && progress == std::vector<double>{0.25, 0.5, 0.75, 1.0},
        "the recording should report its progress up to the end");
    request.maxBlockFrames = 4096;
    request.progress = {};

    request.channels = {2};
    request.bits = 8;
    const auto again = talas::audio::recordTimelineToDisk(haiku(), registry(), request);
    const auto two = talas::audio::readSampleFile(folder / "haiku 2.wav");
    require(again.ok() && two.ok() && std::filesystem::exists(folder / "haiku 1.wav"),
        "several passes over the Timeline, a different set of channels each time");
    requireLevel(two.channels[0], 0.0, 1e-9, "an 8-bit recording of the silent channel 2");
    std::filesystem::remove_all(folder);
}

void testTwoChannelsAsInterleavedStereo()
{
    const auto folder = directory();
    auto request = options(folder, "haiku.aiff");
    request.channels = {1, 3};
    request.interleavedStereo = true;
    request.format = talas::audio::TimelineRecordingFormat::Aiff;
    const auto recorded = talas::audio::recordTimelineToDisk(haiku(), registry(), request);
    require(recorded.ok() && recorded.files.size() == 1 && recorded.files[0].filename() == "haiku.aiff",
        "two channels as an interleaved stereo file: " + recorded.error);
    const auto stereo = talas::audio::readSampleFile(recorded.files[0]);
    require(stereo.ok() && stereo.channels.size() == 2 && stereo.sampleRate == 1000,
        "an AIFF file, the file format chosen: " + stereo.error);
    requireLevel(stereo.channels[0], 0.5, 1e-4, "the left channel is output 1");
    requireLevel(stereo.channels[1], -0.25, 1e-4, "the right channel is output 3");
    request.interleavedStereo = false;
    request.bits = 24;
    const auto mono = talas::audio::recordTimelineToDisk(haiku(), registry(), request);
    require(mono.ok() && mono.files.size() == 2 && talas::audio::readSampleFile(mono.files[1]).ok(),
        "or as two independent files: " + mono.error);
    std::filesystem::remove_all(folder);
}

void testMoreChannelsThanTheHardware()
{
    const auto folder = directory();
    auto request = options(folder, "surround.wav");
    request.channels = {1, 2, 3, 4, 5, 6};
    request.sampleRate = 2000.0;
    require(talas::audio::timelineRecordingChannelCount(haiku(6)) == 6, "a 5.1 Timeline has six output channels");
    const auto recorded = talas::audio::recordTimelineToDisk(haiku(6), registry(), request);
    require(recorded.ok() && recorded.files.size() == 6 && recorded.frameCount == 32,
        "6 independent mono files for 5.1 surround, more channels than you have: " + recorded.error);
    const auto third = talas::audio::readSampleFile(recorded.files[2]);
    require(third.ok() && third.sampleRate == 2000 && third.channels[0].size() == 32,
        "the recording's sample rate is the one selected");
    request.channels = {1, 2, 3, 4, 5, 6, 7, 8};
    const auto eight = talas::audio::recordTimelineToDisk(haiku(8), registry(), request);
    require(eight.ok() && eight.files.size() == 8,
        "as many as 8 files, one for each output channel, even if you don't have 8: " + eight.error);
    std::filesystem::remove_all(folder);
}

// Record to disk records each output channel after the level and
// panning controls, where a cache is recorded before them.
void testTheRecordingIsPostFader()
{
    const auto folder = directory();
    auto levelled = haiku();
    talas::project::ScoreAutomationLane level;
    level.laneId = "level";
    level.ownerId = "trk1";
    level.eventName = "Level";
    level.points = {{seconds(0.0), 0.5, talas::core::AutomationInterpolation::Step}};
    levelled.timeline.score.automation = {level};
    auto request = options(folder, "mix.wav");
    request.channels = {1};
    request.bits = 24;
    const auto recorded = talas::audio::recordTimelineToDisk(levelled, registry(), request);
    const auto one = talas::audio::readSampleFile(folder / "mix 1.wav");
    require(recorded.ok() && one.ok(), "record the levelled Timeline: " + recorded.error);
    requireLevel(one.channels[0], 0.25, 1e-6, "the recording is post-fader, after Trk 1's level");
    std::filesystem::remove_all(folder);
}

void testTheChoicesAreChecked()
{
    const auto folder = directory();
    auto request = options(folder, "haiku.wav");
    request.channels = {5};
    require(!talas::audio::recordTimelineToDisk(haiku(), registry(), request).ok(), "a channel the Master has");
    request.channels = {1, 1};
    require(!talas::audio::recordTimelineToDisk(haiku(), registry(), request).ok(), "each channel once");
    request.channels = {1};
    request.bits = 12;
    require(!talas::audio::recordTimelineToDisk(haiku(), registry(), request).ok(), "8, 16 or 24 bits");
    request.bits = 16;
    auto empty = haiku();
    empty.timeline.score.clips.clear();
    require(!talas::audio::recordTimelineToDisk(empty, registry(), request).ok(), "a Timeline with Sounds to record");
    std::filesystem::remove_all(folder);
}
}

int main()
{
    try
    {
        testEachChannelIntoItsOwnFile();
        testTwoChannelsAsInterleavedStereo();
        testMoreChannelsThanTheHardware();
        testTheChoicesAreChecked();
        testTheRecordingIsPostFader();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
