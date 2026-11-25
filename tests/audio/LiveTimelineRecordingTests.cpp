#include "talas/app/LiveRecordingModel.h"
#include "talas/app/VcsPresets.h"
#include "talas/audio/LiveTimelineRecording.h"
#include "talas/audio/TimelineRecording.h"
#include "talas/audio/WavReader.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/ProjectRuntimeBridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Record to disk plays the Timeline live and records
// what is heard, so a Timeline waiting for a MIDI key down starts at the key,
// and MIDI faders and the arrow keys' presets, but not the mouse, change its
// parameters while it records.
namespace
{
constexpr double rate = 1000.0;
constexpr std::size_t block = 8;

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

talas::project::ProjectParameterExpression expression(std::string id, std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(text);
    return parameter;
}

talas::project::ProjectNode level(std::string id, std::string value)
{
    return {std::move(id), "talas.valToSound", {expression("value", std::move(value))}};
}

// A Sound whose channels play the Val given, one each.
talas::project::ProjectSoundDefinition levels(std::string id, std::vector<std::string> values)
{
    talas::project::ProjectSoundDefinition sound;
    sound.definitionId = id;
    sound.displayName = id;
    for (std::size_t channel = 0; channel < values.size(); ++channel)
    {
        const auto node = "level" + std::to_string(channel + 1);
        sound.graph.nodes.push_back(level(node, values[channel]));
        sound.outputs.push_back({channel == 0 ? std::string{"audioOut"} : "out" + std::to_string(channel + 1),
            talas::core::PortType::Audio, node, "audioOut"});
    }
    return sound;
}

// One track into a Master of `outputs` channels.
talas::project::ProjectDocument timeline(std::uint32_t outputs)
{
    talas::project::ProjectDocument document;
    document.timeline.score.tracks = {
        {"trk1", "Trk 1", talas::project::ScoreTrackKind::Track, talas::project::ScoreTimeDomain::Seconds, "master", outputs,
            talas::project::ScoreOverlapMode::Layer},
        {"master", "Master", talas::project::ScoreTrackKind::Master, talas::project::ScoreTimeDomain::Seconds, {}, outputs,
            talas::project::ScoreOverlapMode::Layer}};
    return document;
}

// The Haiku's start: a WaitUntil whose Resume is !KeyDown for its
// first 4 ms, then 16 ms of a tone at 0.5.
talas::project::ProjectDocument waitingForAKey()
{
    auto document = timeline(1);
    talas::project::ProjectSoundDefinition gate;
    gate.definitionId = "waitForKey";
    gate.displayName = "WaitUntil key down";
    gate.graph.nodes = {level("silence", "0"), {"wait", "talas.waitUntil", {expression("resume", "!KeyDown")}}};
    gate.graph.connections = {{"silence", "audioOut", "wait", "audioIn"}};
    gate.outputs = {{"audioOut", talas::core::PortType::Audio, "wait", "audioOut"}};
    document.soundDefinitions = {gate, levels("tone", {"0.5"})};
    document.timeline.score.clips = {
        {"gate", "trk1", "waitForKey", seconds(0), seconds(0.004), seconds(0), seconds(0), seconds(0), 1},
        {"tone", "trk1", "tone", seconds(0.004), seconds(0.016), seconds(0), seconds(0), seconds(0), 1},
    };
    return document;
}

std::filesystem::path directory()
{
    const auto path = std::filesystem::temp_directory_path()
        / ("talas-live-timeline-recording-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path;
}

talas::audio::TimelineRecordingOptions options(const std::filesystem::path& folder, std::string name)
{
    talas::audio::TimelineRecordingOptions result;
    result.destination = folder / std::move(name);
    result.sampleRate = rate;
    result.maxBlockFrames = block;
    result.live = true;
    return result;
}

// The Timeline staged and played as the Workbench plays it, its audio
// device having `deviceChannels` outputs; each callback renders a block and
// hands it to the recording, as the Workbench's audio callback does.
struct LivePlayer
{
    talas::runtime::ProjectRuntimeBridge bridge;
    std::size_t masterChannels = 1;
    std::size_t deviceChannels = 1;
    double time = 10.0;
    std::array<std::array<float, block>, 8> device{};

    LivePlayer(talas::project::ProjectDocument document, std::size_t master, std::size_t outputs)
        : bridge(std::move(document), registry()), masterChannels(master), deviceChannels(outputs)
    {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = rate;
        settings.maxBlockFrames = block;
        const auto staged = bridge.compileAndStage(settings);
        require(staged.ok(), "the Timeline should stage for playing live: " + staged.message);
        bridge.setPlaying(true);
    }

    void play(talas::audio::LiveTimelineRecording& recording)
    {
        std::array<float*, 8> channels{};
        for (std::size_t channel = 0; channel < deviceChannels; ++channel)
            channels[channel] = device[channel].data();
        recording.lendChannels(std::span<float*>{channels.data(), masterChannels}, block);
        require(bridge.renderOrdered({}, time, channels.data(), masterChannels, block), "the Timeline should play");
        recording.publishFromAudio(
            channels.data(), masterChannels, block, bridge.scorePlaybackFrame(), bridge.activeStageId());
        time += static_cast<double>(block) / rate;
    }
};

std::vector<std::vector<double>> read(const std::filesystem::path& path)
{
    const auto file = talas::audio::readSampleFile(path);
    require(file.ok() && file.sampleRate == static_cast<std::uint32_t>(rate),
        "the recording should be a sample file at the device's rate: " + file.error);
    return file.channels;
}

// Record to disk compiles the Timeline and starts
// recording it, and you still have to start the Timeline with a MIDI key
// down: it waits at its WaitUntil while it records, starts at the key and
// the recording ends with it.
void testRecordingWaitsForTheKeyDown()
{
    const auto folder = directory();
    LivePlayer player{waitingForAKey(), 1, 1};
    auto request = options(folder, "haiku.wav");
    request.channels = {1};
    request.bits = 24;
    std::string error;
    auto recording = talas::audio::LiveTimelineRecording::start(player.bridge.editor().document(), request, error);
    require(recording != nullptr && recording->endFrame() == 20 && recording->masterChannelCount() == 1,
        "a live recording should start for the Timeline's 20 ms: " + error);

    for (int callback = 0; callback < 6; ++callback)
        player.play(*recording);
    require(recording->started() && !recording->reachedEnd() && player.bridge.scorePlaybackFrame() == 0
            && recording->progress() == 0.0 && recording->recordedFrames() == 6 * block,
        "the Timeline should wait for its key down while it records");

    require(player.bridge.enqueueMidiNoteOn(1, 60, 100, player.time), "a MIDI key down");
    for (int callback = 0; callback < 12 && !recording->reachedEnd(); ++callback)
        player.play(*recording);
    require(recording->reachedEnd() && recording->progress() == 1.0,
        "the recording should end with the Timeline, after the key");
    const auto result = recording->finish();
    require(result.ok() && result.files.size() == 1 && result.files[0] == folder / "haiku 1.wav",
        "the live recording should be written: " + result.error);

    const auto samples = read(result.files[0]).front();
    const auto first = std::ranges::find_if(samples, [](double sample) { return std::abs(sample) > 1e-6; });
    require(first != samples.end(), "the Timeline should be heard in the recording once its key is down");
    const auto tone = static_cast<std::size_t>(first - samples.begin());
    // The key came after 48 frames; the WaitUntil's own 4 ms follow it.
    require(tone >= 6 * block + 4 && tone <= 7 * block + 4,
        "nothing after the WaitUntil plays before the key, and the Timeline starts at it: "
            + std::to_string(tone));
    require(samples.size() == tone + 16 && result.frameCount == samples.size(),
        "the recording should stop where the Timeline ends: " + std::to_string(samples.size()));
    for (std::size_t frame = tone; frame < samples.size(); ++frame)
        require(std::abs(samples[frame] - 0.5) < 1e-5, "the tone after the key at frame " + std::to_string(frame));

    // Rendered offline, as Record to disk did before, no key reaches the
    // Timeline: it stays at its WaitUntil and the file is silent.
    auto offline = options(folder, "offline.wav");
    offline.channels = {1};
    offline.live = false;
    const auto rendered = talas::audio::recordTimelineToDisk(waitingForAKey(), registry(), offline);
    require(rendered.ok(), "the offline render: " + rendered.error);
    const auto silent = read(rendered.files[0]).front();
    require(std::ranges::all_of(silent, [](double sample) { return sample == 0.0; }),
        "offline, the Timeline never gets its key down");
    std::filesystem::remove_all(folder);
}

// While recording to disk the onscreen faders do not
// take the mouse, but an external MIDI fader box still controls parameters
// and the up and down arrows still change presets, and the recording hears
// both.
void testMidiFadersAndPresetsWhileRecording()
{
    require(!talas::app::liveRecordingTakesValueFrom(talas::app::PerformanceActivitySource::UserInterface, true),
        "while recording to disk the faders do not take the mouse");
    require(talas::app::liveRecordingTakesValueFrom(talas::app::PerformanceActivitySource::Midi, true)
            && talas::app::liveRecordingTakesValueFrom(talas::app::PerformanceActivitySource::Snapshot, true),
        "MIDI faders and presets still change the parameters while it records");
    require(talas::app::liveRecordingTakesValueFrom(talas::app::PerformanceActivitySource::UserInterface, false),
        "the mouse moves the faders when nothing records");

    // !Volume follows controller 7, a fader of a MIDI fader box, on the
    // left; the surface's Level fader on the right, with a second preset.
    auto document = timeline(2);
    document.soundDefinitions = {levels("mix", {"!Volume", "!Level"})};
    document.timeline.score.clips = {
        {"mix", "trk1", "mix", seconds(0), seconds(0.064), seconds(0), seconds(0), seconds(0), 1}};
    document.virtualControls.push_back(
        {"Level", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.25, 0.0, 1.0});
    auto made = talas::app::vcsPresetBank(document, registry(), {});
    require(made.ok(), "the surface's presets: " + made.diagnostic);
    auto loud = made.bank->snapshots.front();
    loud.snapshotId = "loud";
    loud.displayName = "Loud";
    const auto levelValue = std::ranges::find_if(
        loud.values, [](const auto& entry) { return entry.target.eventName == "Level"; });
    require(levelValue != loud.values.end(), "the presets should hold the Level fader");
    levelValue->value.number = 0.75;
    made.bank->snapshots.push_back(loud);
    document.snapshotLibrary.banks.push_back(*made.bank);

    const auto folder = directory();
    LivePlayer player{document, 2, 2};
    auto request = options(folder, "mix.wav");
    request.channels = {1, 2};
    request.interleavedStereo = true;
    request.bits = 24;
    std::string error;
    auto recording = talas::audio::LiveTimelineRecording::start(player.bridge.editor().document(), request, error);
    require(recording != nullptr, "a live recording: " + error);
    for (int callback = 0; callback < 2; ++callback)
        player.play(*recording);

    require(player.bridge.enqueueMidiControlChange(1, 7, 100, player.time), "controller 7 from a MIDI fader box");
    for (int callback = 0; callback < 2; ++callback)
        player.play(*recording);

    // The down arrow chooses the next preset.
    const auto* bank = talas::app::findVcsPresetBank(player.bridge.editor().document());
    const auto next = bank == nullptr ? std::optional<std::string>{} : talas::app::vcsAdjacentPreset(*bank, "default", 1);
    require(next == std::optional<std::string>{"loud"}, "the down arrow chooses the next preset");
    const auto recalled = player.bridge.recallSnapshot(talas::app::vcsPresetBankId, *next);
    require(recalled.ok(), "a preset chosen while recording: " + recalled.message);
    for (int callback = 0; callback < 8 && !recording->reachedEnd(); ++callback)
        player.play(*recording);
    require(recording->reachedEnd(), "the recording should end with the Timeline");
    const auto result = recording->finish();
    require(result.ok() && result.files == std::vector<std::filesystem::path>{folder / "mix.wav"},
        "the stereo recording: " + result.error);
    const auto stereo = read(result.files[0]);
    require(stereo.size() == 2 && stereo[0].size() == 64, "a stereo file of the whole Timeline");
    // Each is heard from within two blocks of when it came (frames 16 and
    // 32) to the end, and not before.
    const auto from = [](const std::vector<double>& samples, double value) {
        const auto first = std::ranges::find_if(samples, [&](double sample) { return std::abs(sample - value) < 1e-5; });
        return std::ranges::all_of(first, samples.end(), [&](double sample) { return std::abs(sample - value) < 1e-5; })
            ? static_cast<std::size_t>(first - samples.begin())
            : samples.size();
    };
    const auto fader = from(stereo[0], 100.0 / 127.0);
    require(fader > 2 * block && fader <= 4 * block,
        "the MIDI fader should be heard in the recording from when it moved: " + std::to_string(fader));
    const auto preset = from(stereo[1], 0.75);
    require(preset > 4 * block && preset <= 6 * block
            && std::ranges::all_of(stereo[1].begin(), stereo[1].begin() + 4 * block,
                [](double sample) { return std::abs(sample - 0.25) < 1e-5; }),
        "the preset chosen with the arrow keys should be heard in the recording from then: "
            + std::to_string(preset));
    std::filesystem::remove_all(folder);
}

// A fader moved while the Timeline plays reaches the
// Sound playing and a Sound starting later: the first clip plays !Level at
// the surface's 0.25 until the fader moves to 0.375 during it, and the
// second, which starts after and plays twice !Level, plays 0.75 from its
// first frame.
void testFaderMovedWhilePlayingReachesLaterSounds()
{
    auto document = timeline(1);
    document.soundDefinitions = {levels("first", {"!Level"}), levels("second", {"!Level * 2"})};
    document.timeline.score.clips = {
        {"first", "trk1", "first", seconds(0), seconds(0.024), seconds(0), seconds(0), seconds(0), 1},
        {"second", "trk1", "second", seconds(0.024), seconds(0.016), seconds(0), seconds(0), seconds(0), 1}};
    document.virtualControls.push_back(
        {"Level", talas::expr::ValueDomain::Scalar, talas::project::VirtualControlKind::Fader, 0.25, 0.0, 1.0});

    LivePlayer player{document, 1, 1};
    std::vector<float> heard;
    const auto play = [&] {
        std::array<float*, 1> channels{player.device[0].data()};
        require(player.bridge.renderOrdered({}, player.time, channels.data(), 1, block), "the Timeline should play");
        heard.insert(heard.end(), player.device[0].begin(), player.device[0].begin() + block);
        player.time += static_cast<double>(block) / rate;
    };
    play();
    require(player.bridge.updateEventValue("Level", 0.375, talas::expr::ValueDomain::Scalar), "the fader moves");
    for (int callback = 0; callback < 4; ++callback)
        play();
    require(std::abs(heard[1] - 0.25F) < 1e-6F && std::abs(heard[23] - 0.375F) < 1e-6F,
        "the first Sound should follow the fader while it plays: " + std::to_string(heard[1]) + ", "
            + std::to_string(heard[23]));
    require(std::abs(heard[24] - 0.75F) < 1e-6F && std::abs(heard[39] - 0.75F) < 1e-6F,
        "a Sound starting after the fader moved should start with its value: " + std::to_string(heard[24]));
}

// The live recording writes the files the
// offline one does, AIFF too, records the Master's channels the hardware
// lacks, and is left incomplete when the Timeline is compiled again.
void testLiveRecordingFiles()
{
    const auto folder = directory();
    auto document = timeline(2);
    document.soundDefinitions = {levels("pair", {"0.5", "-0.25"})};
    document.timeline.score.clips = {
        {"pair", "trk1", "pair", seconds(0), seconds(0.016), seconds(0), seconds(0), seconds(0), 1}};
    LivePlayer player{document, 2, 1};
    auto request = options(folder, "pair.aiff");
    request.channels = {1, 2};
    request.format = talas::audio::TimelineRecordingFormat::Aiff;
    std::string error;
    auto recording = talas::audio::LiveTimelineRecording::start(player.bridge.editor().document(), request, error);
    require(recording != nullptr, "an AIFF live recording: " + error);
    for (int callback = 0; callback < 4 && !recording->reachedEnd(); ++callback)
        player.play(*recording);
    const auto result = recording->finish();
    require(result.ok() && result.files.size() == 2 && result.files[1].filename() == "pair 2.aiff",
        "each channel into an AIFF file of its own: " + result.error);
    const auto one = read(result.files[0]).front();
    const auto two = read(result.files[1]).front();
    require(one.size() == 16 && two.size() == 16 && std::abs(one[3] - 0.5) < 1e-4 && std::abs(two[3] + 0.25) < 1e-4,
        "channel 2 is recorded although the device has one output");
    require(!std::filesystem::exists(folder / "pair 1.aiff.talas-take.wav"), "the WAV a take records into is removed");

    // Compiled again while it records: what it recorded is not kept.
    auto again = talas::audio::LiveTimelineRecording::start(player.bridge.editor().document(), request, error);
    require(again != nullptr, "a second recording: " + error);
    LivePlayer restaged{document, 2, 1};
    restaged.play(*again);
    talas::runtime::RenderSettings settings;
    settings.sampleRate = rate;
    settings.maxBlockFrames = block;
    require(restaged.bridge.compileAndStage(settings).ok(), "the Timeline compiled again");
    restaged.play(*again);
    restaged.play(*again);
    const auto incomplete = again->finish();
    require(!incomplete.ok() && incomplete.files.empty() && !std::filesystem::exists(folder / "pair 1.aiff.talas-take.wav"),
        "a recording whose Timeline was compiled again is incomplete: " + incomplete.error);

    // It starts with the stage compiled for it, not one still playing
    // before it.
    auto awaiting = talas::audio::LiveTimelineRecording::start(document, request, error);
    require(awaiting != nullptr, "a recording awaiting its stage: " + error);
    awaiting->startsWithStage(7);
    std::array<float, block> left{};
    std::array<float, block> right{};
    std::array<float*, 2> heard{left.data(), right.data()};
    awaiting->publishFromAudio(heard.data(), 2, block, 0, 3);
    require(!awaiting->started(), "a block of an earlier stage does not start the recording");
    awaiting->publishFromAudio(heard.data(), 2, block, 0, 7);
    require(awaiting->started() && awaiting->recordedFrames() == block, "the stage awaited starts it");
    (void) awaiting->cancel();

    request.channels = {3};
    require(talas::audio::LiveTimelineRecording::start(document, request, error) == nullptr, "a channel the Master has");
    request.channels = {1};
    auto empty = document;
    empty.timeline.score.clips.clear();
    require(talas::audio::LiveTimelineRecording::start(empty, request, error) == nullptr, "a Timeline with Sounds");
    std::filesystem::remove_all(folder);
}
}

int main()
{
    try
    {
        testRecordingWaitsForTheKeyDown();
        testMidiFadersAndPresetsWhileRecording();
        testFaderMovedWhilePlayingReachesLaterSounds();
        testLiveRecordingFiles();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
