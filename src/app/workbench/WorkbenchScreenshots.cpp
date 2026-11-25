#include "app/workbench/WorkbenchComponentInternal.h"
#include "app/workbench/WorkbenchPluginExport.h"
#include "app/workbench/WorkbenchScreenshotViews.h"

#include "talas/audio/WavWriter.h"
#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/PartialTrackAsset.h"

#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>
#include <tuple>

// --render-screenshots: the Workbench laid out at a fixed size around a
// showcase project, one PNG written for each main workspace. It runs off
// screen without audio or MIDI devices and without user state.
namespace talas::app::workbench
{
namespace
{
constexpr int screenshotWidth = 1440;
constexpr int screenshotHeight = 900;

[[nodiscard]] talas::project::ProjectSoundDefinition showcaseSound(
    std::string definitionId,
    std::string displayName,
    std::vector<talas::project::ProjectNode> nodes,
    std::vector<talas::core::Connection> connections,
    std::string outputNode)
{
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = std::move(definitionId);
    definition.displayName = std::move(displayName);
    definition.graph.nodes = std::move(nodes);
    definition.graph.connections = std::move(connections);
    definition.outputs.push_back({"audioOut", talas::core::PortType::Audio, std::move(outputNode), "audioOut"});
    return definition;
}

[[nodiscard]] talas::project::ScoreTime seconds(double value)
{
    talas::project::ScoreTime time;
    time.seconds = value;
    return time;
}

[[nodiscard]] talas::project::SnapshotValueEntry eventValue(std::string name, double value,
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar)
{
    talas::project::SnapshotValueEntry entry;
    entry.target.kind = talas::project::SnapshotTargetKind::EventValue;
    entry.target.eventName = std::move(name);
    entry.target.domain = domain;
    entry.value.domain = domain;
    entry.value.number = value;
    return entry;
}

[[nodiscard]] talas::project::ProjectDocument makeScreenshotShowcase()
{
    using talas::project::ProjectNode;
    talas::project::ProjectDocument document;
    document.metadata.title = "Talas Showcase";
    const auto at = [&](std::string id, std::string type,
                        std::vector<talas::project::ProjectParameterExpression> parameters, int x, int y) {
        appendNode(document, std::move(id), std::move(type), std::move(parameters), {x, y});
    };
    at("osc1", "talas.oscillator", {expressionParameter("frequency", "!Tone"), expressionParameter("amplitude", "0.6")},
        40, 40);
    at("osc2", "talas.oscillator",
        {expressionParameter("frequency", "!Tone * 1.01"), expressionParameter("amplitude", "0.45")}, 40, 330);
    at("mixer", "talas.mixerV2", {}, 300, 120);
    at("lfo", "talas.lfo",
        {expressionParameter("frequency", "!Rate"), expressionParameter("minimum", "300"),
            expressionParameter("maximum", "2400")},
        300, 400);
    at("filter", "talas.filter", {expressionParameter("q", "!Resonance")}, 560, 120);
    at("adsr", "talas.adsr", {expressionParameter("gate", "!Gate")}, 560, 400);
    at("vca", "talas.vca", {}, 820, 120);
    at("delay", "talas.delayWithFeedback", {expressionParameter("feedback", "!Feedback")}, 820, 400);
    at("out", "talas.audioOutput", {}, 1080, 220);
    document.graph.connections = {
        connection("osc1", "audioOut", "mixer", "inputs"),
        connection("osc2", "audioOut", "mixer", "inputs"),
        connection("mixer", "leftOut", "filter", "audioIn"),
        connection("lfo", "value", "filter", "frequency"),
        connection("filter", "audioOut", "vca", "audioIn"),
        connection("adsr", "value", "vca", "gain"),
        connection("vca", "audioOut", "out", "audioIn"),
        connection("filter", "audioOut", "delay", "audioIn"),
        connection("delay", "audioOut", "out", "rightIn"),
    };
    const auto control = [&](std::string name, double minimum, double maximum, double value,
                             talas::project::VirtualControlKind kind = talas::project::VirtualControlKind::Fader,
                             talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar) {
        talas::project::VirtualControl virtualControl;
        virtualControl.eventName = std::move(name);
        virtualControl.kind = kind;
        virtualControl.domain = domain;
        virtualControl.minimum = minimum;
        virtualControl.maximum = maximum;
        virtualControl.value = value;
        document.virtualControls.push_back(std::move(virtualControl));
    };
    control("Tone", 55.0, 880.0, 220.0, talas::project::VirtualControlKind::Fader, talas::expr::ValueDomain::Hertz);
    control("Rate", 0.05, 8.0, 1.5, talas::project::VirtualControlKind::Fader, talas::expr::ValueDomain::Hertz);
    control("Resonance", 0.0, 1.0, 0.55);
    control("Feedback", 0.0, 0.95, 0.4);
    control("Gate", 0.0, 1.0, 1.0, talas::project::VirtualControlKind::Gate);

    document.soundDefinitions.push_back(showcaseSound("glassPluck", "Glass Pluck",
        {ProjectNode{"tone", "talas.oscillator", {expressionParameter("frequency", "440 hz")}},
            ProjectNode{"level", "talas.vca", {}}},
        {connection("tone", "audioOut", "level", "audioIn")}, "level"));
    document.soundDefinitions.push_back(showcaseSound("airPad", "Air Pad",
        {ProjectNode{"air", "talas.noise", {}}, ProjectNode{"tone", "talas.filter", {}}},
        {connection("air", "audioOut", "tone", "audioIn")}, "tone"));
    document.soundDefinitions.push_back(showcaseSound("subPulse", "Sub Pulse",
        {ProjectNode{"sub", "talas.oscillator", {expressionParameter("frequency", "55 hz")}},
            ProjectNode{"level", "talas.vca", {}}},
        {connection("sub", "audioOut", "level", "audioIn")}, "level"));
    talas::project::reconcileProjectPresentation(document);

    auto& score = document.timeline.score;
    const auto track = [&](std::string id, std::string name, talas::project::ScoreTrackKind kind) {
        talas::project::ScoreTrack scoreTrack;
        scoreTrack.trackId = std::move(id);
        scoreTrack.displayName = std::move(name);
        scoreTrack.kind = kind;
        scoreTrack.overlapMode = talas::project::ScoreOverlapMode::Layer;
        if (kind != talas::project::ScoreTrackKind::Master)
            scoreTrack.outputTrackId = "master";
        score.tracks.push_back(std::move(scoreTrack));
    };
    track("lead", "Lead", talas::project::ScoreTrackKind::Track);
    track("pad", "Pad", talas::project::ScoreTrackKind::Track);
    track("sub", "Sub", talas::project::ScoreTrackKind::Track);
    track("master", "Master", talas::project::ScoreTrackKind::Master);
    const auto clip = [&](std::string id, std::string trackId, std::string sound, double start, double duration,
                          std::uint32_t repeats, std::uint32_t colour) {
        talas::project::ScoreClip scoreClip;
        scoreClip.clipId = std::move(id);
        scoreClip.trackId = std::move(trackId);
        scoreClip.soundDefinitionId = std::move(sound);
        scoreClip.start = seconds(start);
        scoreClip.duration = seconds(duration);
        scoreClip.fadeIn = seconds(0.1);
        scoreClip.fadeOut = seconds(0.3);
        scoreClip.repeatCount = repeats;
        scoreClip.colour = colour;
        score.clips.push_back(std::move(scoreClip));
    };
    clip("pluckA", "lead", "glassPluck", 0.5, 1.0, 3, 0xa47dffu);
    clip("pluckB", "lead", "glassPluck", 5.0, 1.5, 2, 0xa47dffu);
    clip("padA", "pad", "airPad", 0.0, 4.0, 1, 0x2fd3f0u);
    clip("padB", "pad", "airPad", 4.5, 3.5, 1, 0x2fd3f0u);
    clip("subA", "sub", "subPulse", 2.0, 6.0, 1, 0xff5fa8u);
    talas::project::ScoreAutomationLane lane;
    lane.laneId = "padSwell";
    lane.scope = talas::project::ScoreAutomationScope::Track;
    lane.ownerId = "pad";
    lane.eventName = "Resonance";
    for (const auto& [position, value] : std::initializer_list<std::pair<double, double>>{
             {0.0, 0.2}, {1.5, 0.75}, {3.0, 0.4}, {4.5, 0.9}, {6.5, 0.3}, {8.0, 0.6}})
    {
        lane.points.push_back({seconds(position), value, talas::core::AutomationInterpolation::Linear});
    }
    score.automation.push_back(std::move(lane));
    // Regions, each marker with its end, so Live Grid cells can launch them.
    const auto marker = [&](std::string id, std::string title, double start, double end) {
        talas::project::ScoreMarker scoreMarker;
        scoreMarker.markerId = std::move(id);
        scoreMarker.title = std::move(title);
        scoreMarker.start = seconds(start);
        scoreMarker.end = seconds(end);
        score.markers.push_back(std::move(scoreMarker));
    };
    marker("intro", "Intro", 0.0, 3.0);
    marker("build", "Build", 3.0, 6.0);
    marker("drop", "Drop", 6.0, 8.0);

    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "moods";
    bank.displayName = "Moods";
    bank.smoothingSeconds = 0.25;
    const auto snapshot = [&](std::string id, std::string name, double pitch, double rate, double resonance,
                              double feedback) {
        talas::project::ProjectSnapshot entry;
        entry.snapshotId = std::move(id);
        entry.displayName = std::move(name);
        entry.values = {eventValue("Tone", pitch, talas::expr::ValueDomain::Hertz), eventValue("Rate", rate, talas::expr::ValueDomain::Hertz),
            eventValue("Resonance", resonance),
            eventValue("Feedback", feedback), eventValue("Gate", 1.0)};
        bank.snapshots.push_back(std::move(entry));
    };
    snapshot("glass", "Glass", 440.0, 0.4, 0.7, 0.35);
    snapshot("deep", "Deep", 82.5, 0.15, 0.3, 0.6);
    snapshot("bright", "Bright", 660.0, 2.5, 0.85, 0.2);
    snapshot("drift", "Drift", 220.0, 0.08, 0.5, 0.8);
    document.snapshotLibrary.banks.push_back(std::move(bank));

    talas::project::ProjectLiveSet liveSet;
    liveSet.liveSetId = "showcase";
    liveSet.displayName = "Showcase Set";
    const auto cell = [&](std::string id, std::string name, std::uint32_t row, std::uint32_t column,
                          talas::project::ProjectLiveSetCellTarget target) {
        talas::project::ProjectLiveSetCell liveCell;
        liveCell.cellId = std::move(id);
        liveCell.displayName = std::move(name);
        liveCell.row = row;
        liveCell.column = column;
        liveCell.quantization.kind = talas::project::ProjectLiveSetQuantizationKind::NextBeat;
        liveCell.target = std::move(target);
        liveCell.mappings.push_back(talas::project::ProjectLiveSetWorkbenchMapping{});
        liveSet.cells.push_back(std::move(liveCell));
    };
    cell("pluck", "Glass Pluck", 0, 0, talas::project::ProjectLiveSetSoundTarget{"glassPluck", {}});
    cell("pad", "Air Pad", 1, 0, talas::project::ProjectLiveSetSoundTarget{"airPad", {}});
    cell("sub", "Sub Pulse", 2, 0, talas::project::ProjectLiveSetSoundTarget{"subPulse", {}});
    cell("glass", "Glass", 0, 1, talas::project::ProjectLiveSetSnapshotTarget{"moods", "glass"});
    cell("deep", "Deep", 1, 1, talas::project::ProjectLiveSetSnapshotTarget{"moods", "deep"});
    cell("bright", "Bright", 2, 1, talas::project::ProjectLiveSetSnapshotTarget{"moods", "bright"});
    cell("drift", "Drift", 3, 1, talas::project::ProjectLiveSetSnapshotTarget{"moods", "drift"});
    cell("build", "Build", 0, 2, talas::project::ProjectLiveSetScoreRegionTarget{"build"});
    cell("drop", "Drop", 1, 2, talas::project::ProjectLiveSetScoreRegionTarget{"drop"});
    cell("stop", "Stop", 3, 2, talas::project::ProjectLiveSetStopTarget{});
    document.liveSetLibrary.liveSets.push_back(std::move(liveSet));
    return document;
}

// A short, bright tone with a decaying envelope, for the sample map.
[[nodiscard]] std::array<std::vector<double>, 1> showcaseSample(double frequency, double brightness)
{
    std::vector<double> frames(24000);
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        const auto time = static_cast<double>(index) / 48000.0;
        const auto phase = 2.0 * std::numbers::pi * frequency * time;
        const auto envelope = std::exp(-time * 5.0) * std::min(1.0, time * 400.0);
        frames[index] = 0.55 * envelope
            * (std::sin(phase) + brightness * 0.5 * std::sin(2.0 * phase) + brightness * 0.25 * std::sin(3.0 * phase));
    }
    return {std::move(frames)};
}

// A partial-track spectrum with drifting harmonics, for the Analysis view.
[[nodiscard]] talas::project::PartialTrackAsset showcasePartials()
{
    talas::project::PartialTrackAsset asset;
    asset.sampleRate = 48000.0;
    asset.sourceFrameCount = 96000;
    for (std::uint64_t frame = 0; frame < 96; ++frame)
    {
        talas::project::PartialTrackFrame partials;
        partials.segmentId = 1;
        partials.sourceFrame = frame * 1000;
        const auto time = static_cast<double>(frame) / 48.0;
        for (std::uint64_t harmonic = 1; harmonic <= 10; ++harmonic)
        {
            const auto order = static_cast<double>(harmonic);
            const auto vibrato = 1.0 + 0.012 * std::sin(2.0 * std::numbers::pi * 5.0 * time + order * 0.4);
            const auto amplitude = 0.8 / order * (0.6 + 0.4 * std::sin(time * 3.0 + order));
            partials.points.push_back({harmonic, std::max(0.02, amplitude), 196.0 * order * vibrato, 6.0 + order});
        }
        asset.frames.push_back(std::move(partials));
    }
    return asset;
}
}

std::optional<std::string> WorkbenchComponent::writeScreenshot(const std::filesystem::path& file)
{
    // Opaque, so the PNG carries no alpha channel and stays small.
    const auto image = createComponentSnapshot(getLocalBounds(), true, 1.0F).convertedToFormat(juce::Image::RGB);
    if (!image.isValid())
        return "screenshot " + file.filename().string() + " did not produce an image";
    const juce::File output{juceString(file.lexically_normal().string())};
    output.deleteFile();
    auto stream = output.createOutputStream();
    if (stream == nullptr || !stream->openedOk() || !juce::PNGImageFormat{}.writeImageToStream(image, *stream))
        return "screenshot " + file.filename().string() + " could not be written";
    stream->flush();
    return std::nullopt;
}

std::optional<std::string> WorkbenchComponent::installScreenshotShowcase(const std::filesystem::path& sampleDirectory)
{
    auto document = makeScreenshotShowcase();
    if (const auto check = projectBridge_.editor().checkReplacementDocument(document); !check.ok())
        return "showcase project was rejected: " + check.message;
    prepareForDocumentReplacement();
    if (const auto replaced = projectBridge_.editor().replaceDocument(std::move(document)); !replaced.ok())
        return "showcase project could not be installed: " + replaced.message;
    resetForDocumentReplacement();
    editBoundary_.markNew(projectBridge_.editor().document());
    documentChanged();
    applyWorkspaceSessionProjection(false);

    // Four tones across the keyboard for the Samples workspace.
    std::vector<std::filesystem::path> samples;
    std::string writeError;
    for (const auto& [name, frequency, brightness] : std::initializer_list<std::tuple<const char*, double, double>>{
             {"showcase-low.wav", 110.0, 0.9}, {"showcase-mid.wav", 220.0, 0.6}, {"showcase-high.wav", 440.0, 0.4},
             {"showcase-air.wav", 880.0, 0.2}})
    {
        samples.push_back((sampleDirectory / name).lexically_normal());
        if (!talas::audio::writePcm16Wav(samples.back(), 48000.0, showcaseSample(frequency, brightness), writeError))
            return "showcase sample could not be written: " + writeError;
    }
    createSampleMap();
    startSampleMapImport(samples);
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
    while (!sampleImportWorkerFinished_.load(std::memory_order_acquire)
           && juce::Time::getMillisecondCounterHiRes() < deadline)
        std::this_thread::yield();
    finishSampleMapImportIfReady();
    if (sampleImportPresentation_ != SampleMapImportPresentation::Preview)
        return "showcase samples did not reach the import preview";
    applySampleMapImportPreview();
    // One octave each, low to high, each rooted at its tone.
    if (const auto* map = workspaceSession_.samples.activeMapId.has_value()
            ? talas::project::findProjectSampleMap(projectBridge_.editor().document(), *workspaceSession_.samples.activeMapId)
            : nullptr)
    {
        auto regions = map->regions;
        for (auto& region : regions)
        {
            const auto octave = region.assetId.find("low") != std::string::npos ? 0
                : region.assetId.find("mid") != std::string::npos               ? 1
                : region.assetId.find("high") != std::string::npos              ? 2
                                                                                : 3;
            region.lowNote = static_cast<std::uint8_t>(36 + 12 * octave);
            region.highNote = static_cast<std::uint8_t>(47 + 12 * octave);
            region.rootNote = static_cast<std::uint8_t>(45 + 12 * octave);
            region.lowVelocity = 1;
            region.highVelocity = 127;
            applySampleRegion(std::move(region));
        }
    }
    return std::nullopt;
}

std::optional<std::string> WorkbenchComponent::renderScreenshots(const std::filesystem::path& directory)
{
    if (hardwareEnabled_ || persistentStateEnabled_)
        return "screenshot component was not isolated";
    // The screenshots picture a session whose audio device runs, so the
    // transport's commands are as they would be then; no device is opened.
    struct DeviceStandIn
    {
        bool& initialised;
        ~DeviceStandIn()
        {
            initialised = false;
        }
    } deviceStandIn{hardwareInitialised_};
    hardwareInitialised_ = true;
    setSize(screenshotWidth, screenshotHeight);
    // The samples are written beside the run's scratch state, not into the
    // screenshot folder.
    if (auto error = installScreenshotShowcase(recoveryPath_.parent_path()))
        return error;
    const auto shoot = [&](std::string_view name) {
        // The toolbar's commands and the transport time, as the message
        // loop and the timer would have brought them up to date.
        for (const auto& [button, command] : {std::pair{&playButton_, commandPlay}, std::pair{&pauseButton_, commandPause},
                 std::pair{&stopButton_, commandStop}, std::pair{&undoButton_, commandUndo},
                 std::pair{&redoButton_, commandRedo}, std::pair{&deleteButton_, commandDeleteSelectedNode},
                 std::pair{&saveButton_, commandSaveProject}})
            button->setCommandToTrigger(&commandManager_, command, true);
        refreshTransportLabel();
        timelinePanel_->setTransportSnapshot(projectBridge_.musicalTransportSnapshot());
        resized();
        return writeScreenshot(directory / (std::string{"talas-"} + std::string{name} + ".png"));
    };
    const auto shootView = [&](std::string_view name, const juce::Image& image) {
        return screenshot_views::writePng(image, directory / (std::string{"talas-view-"} + std::string{name} + ".png"));
    };

    // Played offline, block by block, as the audio callback would.
    constexpr std::size_t blockFrames = 256;
    std::array<std::vector<float>, 2> buffers{std::vector<float>(blockFrames), std::vector<float>(blockFrames)};
    const std::array<float*, 2> channels{buffers[0].data(), buffers[1].data()};
    const auto play = [&](int blocks) {
        for (int block = 0; block < blocks; ++block)
            (void)projectBridge_.render(channels.data(), channels.size(), blockFrames);
    };
    // The project staged and played for a moment, so meters, controls and
    // traces show live values.
    const auto stageAndPlay = [&] {
        talas::runtime::RenderSettings settings;
        settings.sampleRate = 48000.0;
        settings.maxBlockFrames = blockFrames;
        if (!stageProject(std::move(settings), "screenshots"))
        {
            std::cerr << "WORKBENCH_SCREENSHOTS_NOTE: staging failed: " << statusLabel_.getText().toStdString() << '\n';
            return;
        }
        setPlaying(true);
        play(800);
        refreshPreparedGraphStatus();
    };
    stageAndPlay();

    // Patch: the signal flow, its parameters open on the filter.
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    showRootGraph();
    selectNode("filter");
    graphCanvas_->zoomToFit(false);
    if (auto error = shoot("patch"))
        return error;

    // Performance: the control tiles and the snapshot bank, then the Live Grid.
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Performance);
    performanceCanvas_->refresh();
    performanceCanvas_->refreshRuntimeState();
    performancePanel_->refresh();
    performancePanel_->refreshRuntimeState();
    performancePanel_->showSnapshotsForSmoke();
    if (auto error = shoot("performance"))
        return error;
    parametersWidth_ = 600;
    performancePanel_->showLiveGridForSmoke();
    // Two cells launched and played into, so the grid shows one lit, then
    // a third launched to wait for the next beat.
    for (const auto* cellId : {"pad", "deep"})
    {
        (void)performancePanel_->activateLiveGridCell("showcase", cellId);
        play(200);
    }
    (void)performancePanel_->activateLiveGridCell("showcase", "drop");
    performancePanel_->refreshRuntimeState();
    performancePanel_->refresh();
    if (auto error = shoot("live-grid"))
        return error;

    // Timeline: the Score, widened over the signal flow.
    parametersWidth_ = 820;
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Automation);
    timelinePanel_->refresh();
    if (auto error = shoot("timeline"))
        return error;
    parametersWidth_.reset();

    // Samples: the map and one region's waveform.
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Samples);
    if (const auto* map = workspaceSession_.samples.activeMapId.has_value()
            ? talas::project::findProjectSampleMap(projectBridge_.editor().document(), *workspaceSession_.samples.activeMapId)
            : nullptr;
        map != nullptr && !map->regions.empty())
    {
        // The low tone, whose cycles read as a waveform at this width.
        const auto low = std::ranges::find_if(map->regions, [](const auto& region) {
            return region.assetId.find("low") != std::string::npos;
        });
        selectSampleRegion((low != map->regions.end() ? *low : map->regions.front()).regionId,
            talas::app::SampleMapSelectionMode::Replace);
        const auto deadline = juce::Time::getMillisecondCounterHiRes() + 10000.0;
        while (!sampleWaveformWorkerFinished_.load(std::memory_order_acquire)
               && juce::Time::getMillisecondCounterHiRes() < deadline)
            std::this_thread::yield();
        finishSampleWaveformIfReady();
    }
    if (auto error = shoot("samples"))
        return error;

    // Analysis: two traces pinned on the root graph beside a partial-track
    // spectrum. A Score would play in the root graph's place, so the
    // Analysis view is taken of the project without one.
    setPlaying(false);
    auto withoutScore = projectBridge_.editor().document();
    withoutScore.timeline.score = {};
    withoutScore.liveSetLibrary = {};
    // The tone raised, so the scope shows several cycles of it.
    for (auto& control : withoutScore.virtualControls)
    {
        if (control.eventName == "Tone")
            control.value = 660.0;
    }
    if (const auto replaced = projectBridge_.editor().replaceDocument(std::move(withoutScore)); !replaced.ok())
        return "showcase project without its Score was rejected: " + replaced.message;
    documentChanged();
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Patch);
    showRootGraph();
    for (const auto* node : {"filter", "osc2"})
    {
        selectNode(node);
        pinSelectedAnalysisTarget();
    }
    stageAndPlay();
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Analysis);
    talas::project::ProjectAsset manifest;
    manifest.id = "showcase_partials";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = "showcase.talaspartials";
    const auto partials = showcasePartials();
    const auto encoded = talas::project::encodePartialTrackAsset(partials);
    if (encoded.ok())
    {
        manifest.contentHash = talas::project::calculateProjectAssetSha256(encoded.bytes);
        manifest.sampleRate = partials.sampleRate;
        manifest.frameCount = partials.sourceFrameCount;
        manifest.encoding = std::string{talas::project::partialTrackAssetEncoding};
        manifest.generation = 1;
        (void)partialTrackEditorPanel_->setSource(
            manifest, std::make_shared<const talas::project::PartialTrackAsset>(partials), std::nullopt);
    }
    // A history long enough to show a few cycles of each trace, played on
    // in display-rate steps and polled as the Workbench's timer would.
    setAnalysisHistoryFrames(600);
    for (int step = 0; step < 150; ++step)
    {
        play(6);
        analysisPanel_->poll();
    }
    if (auto error = shoot("analysis"))
        return error;

    // The views beyond the workspaces.
    juce::PopupMenu file = getMenuForIndex(static_cast<int>(talas::app::TalasMenu::File), {});
    juce::PopupMenu view = getMenuForIndex(static_cast<int>(talas::app::TalasMenu::View), {});
    juce::PopupMenu context;
    context.addSectionHeader("Filter");
    context.addItem(1, "Open Parameters", true, false);
    context.addItem(2, "Audition", true, false);
    context.addItem(3, "Pin to Analysis", true, true);
    context.addSeparator();
    juce::PopupMenu replace;
    replace.addItem(10, "Filter Bank");
    context.addSubMenu("Replace With", replace);
    context.addItem(4, "Duplicate", true, false);
    context.addItem(5, "Delete", false, false);
    if (auto error = shootView("menus", screenshot_views::popupMenus({{file, 2}, {view, 4}, {context, 2}})))
        return error;
    if (auto error = shootView("help-and-alerts", screenshot_views::helpAndAlerts()))
        return error;
    if (auto error = shootView("controls", screenshot_views::controlSheet()))
        return error;
    if (auto error = shootView("audio-midi-settings", screenshot_views::audioMidiSettings()))
        return error;
    if (auto panel = makePluginExportPanelForScreenshots(projectBridge_.editor().document(), std::nullopt))
    {
        if (auto error = shootView("export-plugin", screenshot_views::dialogWindow(*panel, "Export Plugin")))
            return error;
    }
    if (auto error = shootView("plugin-editor", screenshot_views::pluginEditor(projectBridge_.editor().document())))
        return error;

    // Enhanced Contrast over the Performance workspace: solid panels, strong
    // outlines, no glow.
    activateWorkspaceMode(talas::app::WorkbenchWorkspaceMode::Performance);
    const auto accessibility = workspaceSession_.accessibility;
    workspaceSession_.accessibility.enhancedContrast = true;
    applyAccessibilityPreferences();
    performanceCanvas_->refresh();
    performancePanel_->refresh();
    const auto enhancedShot = shoot("view-enhanced-contrast");
    workspaceSession_.accessibility = accessibility;
    applyAccessibilityPreferences();
    if (enhancedShot)
        return enhancedShot;
    setPlaying(false);
    return std::nullopt;
}
}
