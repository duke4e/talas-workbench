#include "talas/audio/ResourceLoader.h"
#include "talas/audio/SampleFileFormats.h"
#include "talas/audio/TapeRecorder.h"
#include "talas/audio/TimelineRecording.h"
#include "talas/audio/WavReader.h"
#include "talas/audio/WavWriter.h"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// The Tape Recorder tool: its transport, the input heard while monitoring,
// the largest levels seen, the faders, the channels chosen, the recording
// and its playback, and the AIFF format.
namespace
{
using talas::audio::TapeRecorderEngine;
using talas::audio::TapeRecorderState;
using talas::audio::TapeRecorderTransport;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance = 1.0e-6)
{
    return std::abs(a - b) <= tolerance;
}

std::filesystem::path directory()
{
    const auto path = std::filesystem::temp_directory_path()
        / ("talas-tape-recorder-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

// One callback of `frames` with the left input at `left` and the right at
// `right`, the outputs starting silent; the outputs after it.
std::array<std::vector<float>, 2> callback(TapeRecorderEngine& engine, std::size_t frames, float left, float right)
{
    std::vector<float> inLeft(frames, left);
    std::vector<float> inRight(frames, right);
    std::array<std::vector<float>, 2> outputs{std::vector<float>(frames, 0.0F), std::vector<float>(frames, 0.0F)};
    const std::array<const float*, 2> inputs{inLeft.data(), inRight.data()};
    const std::array<float*, 2> outs{outputs[0].data(), outputs[1].data()};
    engine.process(inputs, outs, frames);
    return outputs;
}

// Record puts the recorder into monitor mode with the pause button down;
// play, or pause, starts recording; pause pauses and resumes; stop ends it;
// play then plays the recording back.
void testTheTransport()
{
    TapeRecorderTransport transport;
    require(transport.state() == TapeRecorderState::Stopped && !transport.monitors(), "a recorder starts stopped");
    transport.play(false);
    require(transport.state() == TapeRecorderState::Stopped, "play does nothing without a recording");

    transport.record();
    require(transport.state() == TapeRecorderState::Monitoring && transport.pauseDown() && transport.recordDown()
            && transport.monitors() && !transport.records(),
        "record monitors the input with the pause button down");
    transport.play(false);
    require(transport.records() && !transport.pauseDown() && transport.monitors(), "play starts the recording");
    transport.pause();
    require(transport.state() == TapeRecorderState::RecordingPaused && transport.pauseDown() && !transport.records(),
        "pause pauses the recording");
    transport.pause();
    require(transport.records(), "pause again resumes the recording");
    transport.stop();
    require(transport.state() == TapeRecorderState::Stopped && !transport.monitors(), "stop ends the recording");

    transport.record();
    transport.pause();
    require(transport.records(), "releasing the pause button also starts the recording");
    transport.stop();

    transport.play(true);
    require(transport.plays() && !transport.monitors(), "play plays the recording back");
    transport.pause();
    require(transport.state() == TapeRecorderState::PlayingPaused && transport.pauseDown(), "pause pauses the playback");
    transport.play(true);
    require(transport.plays(), "play resumes the playback");
    transport.record();
    require(transport.state() == TapeRecorderState::Monitoring, "record from playback monitors again");
}

// The input is heard while monitoring, through the faders; the Max levels
// hold the largest input level seen until Reset.
void testMonitoringLevelsAndFaders()
{
    TapeRecorderEngine engine{64};
    auto silent = callback(engine, 32, 0.5F, -0.25F);
    require(silent[0][0] == 0.0F && silent[1][0] == 0.0F, "the input is not heard unless monitoring");
    require(near(engine.maxLevel(0), 0.5) && near(engine.maxLevel(1), 0.25), "the Max levels show the inputs' peaks");
    require(near(engine.currentLevel(0), 0.5) && near(engine.currentLevel(1), 0.25), "the meters show the input levels");

    engine.setMonitoring(true);
    engine.setInputLevel(0, 2.0F);
    auto heard = callback(engine, 100, 0.2F, -0.25F);
    require(near(heard[0][99], 0.4) && near(heard[1][0], -0.25), "monitoring plays the input through its fader");

    callback(engine, 16, 0.1F, 0.1F);
    require(near(engine.maxLevel(0), 0.5) && near(engine.maxLevel(1), 0.25) && near(engine.currentLevel(1), 0.1),
        "the Max levels keep the largest level, the meters the current one");
    engine.resetMaxLevels();
    require(engine.maxLevel(0) == 0.0F && engine.maxLevel(1) == 0.0F, "Reset clears the Max levels");
    callback(engine, 16, 0.2F, 0.1F);
    require(near(engine.maxLevel(0), 0.4) && near(engine.maxLevel(1), 0.1),
        "the Max levels measure the input after its fader");

    // A level too low is boosted with the Input fader,
    // which cannot attenuate.
    engine.setInputLevel(1, 0.5F);
    require(engine.inputLevel(1) == 1.0F, "an Input fader cannot attenuate");
    engine.setInputLevel(1, 40.0F);
    require(engine.inputLevel(1) == talas::audio::tapeRecorderMaximumInputGain, "an Input fader boosts up to its most");
    engine.setChannelEnabled(1, false);
    auto left = callback(engine, 8, 0.2F, 0.2F);
    require(engine.enabledChannelCount() == 1 && left[1][0] == 0.0F && near(left[0][0], 0.4),
        "a channel left out is neither heard nor recorded");
}

void testTheRecording()
{
    const auto folder = directory();
    const auto destination = folder / "tape.wav";
    TapeRecorderEngine engine{64};
    engine.setInputLevel(1, 2.0F);
    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.sampleRate = 48000.0;
    options.channelCount = engine.enabledChannelCount();
    options.maxBlockFrames = 64;
    options.sourceStageId = TapeRecorderEngine::takeStageId;
    options.wordSize = talas::audio::WavWordSize{24};
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(options, error);
    require(take != nullptr, "the take starts: " + error);

    engine.setMonitoring(true);
    callback(engine, 50, 0.9F, 0.9F);
    engine.setRecordingTake(take.get());
    callback(engine, 100, 0.5F, -0.25F);
    engine.setRecordingPaused(true);
    callback(engine, 40, 0.9F, 0.9F);
    engine.setRecordingPaused(false);
    callback(engine, 30, -0.25F, 0.125F);
    require(engine.recordedFrames() == 130, "the frames recorded leave out the pause");
    engine.setRecordingTake(nullptr);
    take->requestStop();
    take->join();
    const auto snapshot = take->snapshot();
    require(snapshot.state == talas::audio::RealtimeOutputTakeState::Completed && snapshot.writtenFrames == 130,
        "the take records what was played: " + snapshot.diagnostic);

    const auto file = talas::audio::readSampleFile(destination);
    require(file.ok() && file.sampleRate == 48000 && file.channels.size() == 2 && file.channels[0].size() == 130,
        "the recording is a stereo file: " + file.error);
    require(near(file.channels[0][0], 0.5, 1.0e-5) && near(file.channels[1][99], -0.5, 1.0e-5)
            && near(file.channels[0][100], -0.25, 1.0e-5) && near(file.channels[1][129], 0.25, 1.0e-5),
        "the recording is the input through the faders");

    // Recording again replaces what was recorded before.
    options.existingPolicy = talas::audio::ExistingAudioFilePolicy::Replace;
    auto again = talas::audio::RealtimeOutputTake::start(options, error);
    require(again != nullptr, "a second take starts over the first: " + error);
    engine.setRecordingTake(again.get());
    require(engine.recordedFrames() == 0, "a new take counts from zero");
    callback(engine, 20, 0.1F, 0.1F);
    engine.setRecordingTake(nullptr);
    again->requestStop();
    again->join();
    const auto replaced = talas::audio::readSampleFile(destination);
    require(replaced.ok() && replaced.channels[0].size() == 20 && near(replaced.channels[1][0], 0.2, 1.0e-5),
        "the new recording replaces the old one: " + replaced.error);
    std::filesystem::remove_all(folder);
}

void testOneChannelRecordsAMonoFile()
{
    const auto folder = directory();
    const auto destination = folder / "right.wav";
    // The engine's own block size, a take's largest.
    TapeRecorderEngine engine;
    engine.setChannelEnabled(0, false);
    talas::audio::RealtimeOutputTakeOptions options;
    options.destination = destination;
    options.sampleRate = 44100.0;
    options.channelCount = engine.enabledChannelCount();
    options.maxBlockFrames = engine.maximumBlockFrames();
    options.sourceStageId = TapeRecorderEngine::takeStageId;
    std::string error;
    auto take = talas::audio::RealtimeOutputTake::start(options, error);
    require(take != nullptr, "the mono take starts: " + error);
    engine.setRecordingTake(take.get());
    callback(engine, 70, 0.75F, -0.5F);
    // A callback longer than a block goes to the take in blocks.
    callback(engine, engine.maximumBlockFrames() + 30, 0.75F, -0.5F);
    engine.setRecordingTake(nullptr);
    take->requestStop();
    take->join();
    const auto file = talas::audio::readSampleFile(destination);
    require(file.ok() && file.channels.size() == 1 && file.channels[0].size() == engine.maximumBlockFrames() + 100
            && near(file.channels[0][69], -0.5, 1.0e-4),
        "only the right input is recorded: " + file.error);
    std::filesystem::remove_all(folder);
}

void testThePlayback()
{
    TapeRecorderEngine engine{16};
    auto clip = std::make_shared<std::vector<std::vector<float>>>(
        std::vector<std::vector<float>>{std::vector<float>(40, 0.5F), std::vector<float>(40, -0.5F)});
    engine.setPlaybackClip(clip);
    auto idle = callback(engine, 10, 0.0F, 0.0F);
    require(idle[0][0] == 0.0F && engine.playedFrames() == 0, "a recording is heard only while playing");

    engine.setPlaying(true);
    auto played = callback(engine, 30, 0.0F, 0.0F);
    require(near(played[0][29], 0.5) && near(played[1][0], -0.5) && engine.playedFrames() == 30,
        "play plays the recording back");
    auto end = callback(engine, 30, 0.0F, 0.0F);
    require(near(end[0][9], 0.5) && end[0][10] == 0.0F && engine.playedFrames() == 40 && !engine.playing(),
        "the playback stops at the recording's end");

    engine.rewind();
    engine.setPlaying(true);
    auto again = callback(engine, 4, 0.0F, 0.0F);
    require(near(again[1][3], -0.5) && engine.playedFrames() == 4, "play again starts from the beginning");

    auto mono = std::make_shared<std::vector<std::vector<float>>>(
        std::vector<std::vector<float>>{std::vector<float>(8, 0.25F)});
    engine.setPlaybackClip(mono);
    engine.setPlaying(true);
    auto both = callback(engine, 8, 0.0F, 0.0F);
    require(near(both[0][7], 0.25) && near(both[1][7], 0.25), "a mono recording plays on both outputs");
}

// The recording saved as AIFF, of 8, 16 or 24 bits.
void testTheAiffFormat()
{
    const auto folder = directory();
    const auto destination = folder / "tape.aif";
    const std::vector<std::vector<double>> channels{{0.0, 0.5, -0.5, 0.25, -1.0}, {0.75, -0.75, 0.0, 0.125, 1.0}};
    for (const std::uint16_t bits : {8, 16, 24})
    {
        const auto error = talas::audio::writeAiffFile(destination, 44100.0, channels, bits);
        require(error.empty(), "the AIFF file is written: " + error);
        const auto file = talas::audio::readSampleFile(destination);
        const auto tolerance = bits == 8 ? 1.0e-2 : 1.0e-4;
        require(file.ok() && file.sampleRate == 44100 && file.channels.size() == 2 && file.channels[1].size() == 5
                && near(file.channels[0][1], 0.5, tolerance) && near(file.channels[1][1], -0.75, tolerance)
                && near(file.channels[0][4], -1.0, tolerance),
            "the AIFF file reads back at " + std::to_string(bits) + " bits: " + file.error);
    }
    // A take recorded as WAV saved as AIFF instead.
    std::string wavError;
    const auto take = folder / "take.wav";
    require(talas::audio::writePcm16Wav(take, 48000.0, channels, wavError), "the take is written: " + wavError);
    const auto converted = talas::audio::convertRecordingToAiff(take, folder / "take.aif", 24);
    const auto saved = talas::audio::readSampleFile(folder / "take.aif");
    require(converted.empty() && !std::filesystem::exists(take) && saved.ok() && saved.sampleRate == 48000
            && saved.channels.size() == 2 && near(saved.channels[0][1], 0.5, 1.0e-4) && near(saved.channels[1][4], 1.0, 1.0e-4),
        "the take should be saved as AIFF and its WAV removed: " + converted + saved.error);
    require(!talas::audio::convertRecordingToAiff(folder / "none.wav", folder / "none.aif", 16).empty(),
        "a take that is not there is not converted");
    require(!talas::audio::writeAiffFile(destination, 44100.0, channels, 12).empty(), "12 bits is refused");
    require(!talas::audio::writeAiffFile(destination, 44100.0, {{0.0}, {0.0, 0.0}}, 16).empty(),
        "channels of different lengths are refused");
    std::filesystem::remove_all(folder);
}

// Talas
// records audio tracks as AIFF, SD-I, SD-II (on a Macintosh), WAVE and
// IRCAM/MTU files and works with sample files of each. A take recorded as
// WAV is saved in every format at 8, 16 and 24 bits, as a DiskRecorder's
// take is saved in its Format: the file reads back as the samples
// recorded, at the take's rate, and imports as a table, and the WAV is
// removed. A stereo take is refused as SD-I (one channel), its WAV kept.
void testTakesAreSavedInEveryTalasFormat()
{
    using talas::audio::SampleFileFormat;
    const auto folder = directory();
    const std::vector<std::vector<double>> stereo{{0.0, 0.5, -0.5, 0.25, -1.0}, {0.75, -0.75, 0.0, 0.125, 1.0}};
    for (const auto format : talas::audio::sampleFileFormats())
    {
        const auto name = std::string{talas::audio::sampleFileFormatName(format)};
        const auto channels = format == SampleFileFormat::SoundDesignerI
            ? std::vector<std::vector<double>>{stereo[0]}
            : stereo;
        for (const std::uint16_t bits : {8, 16, 24})
        {
            const auto take = folder / "take.wav";
            std::string wavError;
            require(talas::audio::writePcmWav(take, 48000.0, channels, talas::audio::WavWordSize{bits}, wavError),
                "the take is written: " + wavError);
            auto destination = folder / ("take " + std::to_string(bits));
            destination += std::string{talas::audio::sampleFileFormatExtension(format)};
            const auto converted = talas::audio::convertRecordingToSampleFile(take, destination, format, bits);
            if (format == SampleFileFormat::SoundDesignerII && !talas::audio::soundDesignerIIAvailable())
            {
                require(!converted.empty() && std::filesystem::exists(take),
                    "away from a Macintosh an SD-II take should be refused, its WAV kept");
                continue;
            }
            const auto saved = talas::audio::readSampleFile(destination);
            const auto tolerance = bits == 8 ? 1.0 / 127.0 : 1.0e-4;
            require(converted.empty() && !std::filesystem::exists(take) && saved.ok() && saved.sampleRate == 48000
                    && saved.channels.size() == channels.size() && saved.channels[0].size() == 5
                    && near(saved.channels[0][1], 0.5, tolerance) && near(saved.channels[0][4], -1.0, tolerance)
                    && near(saved.channels.back()[1], channels.back()[1], tolerance),
                "a take should be saved as " + name + " at " + std::to_string(bits)
                    + " bits and read back: " + converted + saved.error);
            const auto table = talas::audio::loadTableResource(destination);
            require(table.ok() && table.sourceSampleRate == 48000 && table.samples.size() == 5
                    && near(table.samples[2], -0.5, tolerance),
                "the " + name + " file should import as a sample: " + table.error);
        }
    }
    const auto take = folder / "stereo.wav";
    std::string wavError;
    require(talas::audio::writePcm16Wav(take, 48000.0, stereo, wavError), "the stereo take is written: " + wavError);
    require(!talas::audio::convertRecordingToSampleFile(take, folder / "stereo.sd1", SampleFileFormat::SoundDesignerI, 16)
                 .empty()
            && std::filesystem::exists(take) && !std::filesystem::exists(folder / "stereo.sd1"),
        "an SD-I file holds one channel: a stereo take should be refused and its WAV kept");
    std::filesystem::remove_all(folder);
}
}

int main()
{
    try
    {
        testTheTransport();
        testMonitoringLevelsAndFaders();
        testTheRecording();
        testOneChannelRecordsAMonoFile();
        testThePlayback();
        testTheAiffFormat();
        testTakesAreSavedInEveryTalasFormat();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
