#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/runtime/ProjectRenderSettings.h"

namespace talas::app::workbench
{
// Spectral Analysis's steps, from the sample
// looped with a silence to a harmonic spectrum file and its two example
// SumOfSines.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runSpectralAnalysisWizardSmoke(const std::filesystem::path& smokeDirectory)
{
    using Step = AnalysisToolPanel::SpectralStep;
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto sample = (smokeDirectory / "wizard voice.wav").lexically_normal();
    const auto harmonic = (smokeDirectory / "wizard voice h.talaspartial").lexically_normal();
    const auto table = talas::app::harmonicAnalysisTablePath(harmonic);
    const auto stereoSample = (smokeDirectory / "wizard loop.wav").lexically_normal();
    const auto stereoFiles = talas::app::stereoAnalysisFiles(
        (smokeDirectory / "wizard loop s256.talaspartial").lexically_normal(), talas::app::StereoAnalysisChoice::LeftAndRight);
    const auto removeFiles = [&] {
        std::error_code ignored;
        for (const auto& file : {sample, harmonic, table, stereoSample, stereoFiles[0], stereoFiles[1]})
            std::filesystem::remove(file, ignored);
    };
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        stopSampleBuffer();
        spectralAnalysisWindow_.reset();
        spectralAnalysisPanel_.reset();
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        removeFiles();
        if (!restored.ok() && error.empty())
            error = "Spectral Analysis steps smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };

    // A voiced half, a 220 Hz tone and its harmonics, then a breath of noise.
    removeFiles();
    constexpr double sampleRate = 22050.0;
    std::vector<double> voice(11025, 0.0);
    std::uint32_t state = 7;
    for (std::size_t frame = 0; frame < voice.size(); ++frame)
    {
        const auto t = static_cast<double>(frame) / sampleRate;
        if (frame < voice.size() / 2)
        {
            for (int k = 1; k <= 5; ++k)
                voice[frame] += 0.3 / k * std::sin(2.0 * std::numbers::pi * 220.0 * k * t);
        }
        else
        {
            state = state * 1664525U + 1013904223U;
            voice[frame] = 0.3 * (static_cast<double>(state >> 8) / static_cast<double>(1U << 24) - 0.5);
        }
    }
    std::string writeError;
    if (!talas::audio::writePcm16Wav(sample, sampleRate, std::span<const std::vector<double>>{&voice, 1}, writeError))
        return finish("the steps' sample could not be written: " + writeError);
    if (!projectBridge_.editor().replaceDocument(talas::project::ProjectDocument{}).ok())
        return finish("the Spectral Analysis steps project was rejected");
    documentChanged();

    spectralAnalysisPanel_.reset();
    if (!commandManager_.invokeDirectly(commandShowSpectralAnalysis, false) || spectralAnalysisPanel_ == nullptr)
        return finish("Spectral Analysis did not open");
    auto& panel = *spectralAnalysisPanel_;
    const auto playing = [this] { return sampleAuditionOwned_ != nullptr && sampleAuditionOwned_->loop; };
    const auto heard = [this](std::size_t from, std::size_t to) {
        double energy = 0.0;
        const auto& channel = sampleAuditionOwned_->channels.front();
        for (auto frame = from; frame < to && frame < channel.size(); ++frame)
            energy += static_cast<double>(channel[frame]) * channel[frame];
        return std::sqrt(energy / static_cast<double>(std::max<std::size_t>(1, to - from)));
    };
    if (panel.spectralStepForSmoke() != Step::Select || !panel.setSampleForSmoke(sample) || !playing()
        || sampleAuditionOwned_->channels.front().size() != voice.size() + 22050)
        return finish("selecting the sample did not loop it with a second of silence");

    if (!panel.nextForSmoke() || panel.spectralStepForSmoke() != Step::Parameters)
        return finish("Next did not go on to the analysis parameters");
    panel.setSpectralSettingsForSmoke(2, 1, false);
    if (!panel.auditionNowForSmoke() || panel.oscillatorsForSmoke() == 0 || !playing() || heard(0, voice.size()) < 0.01)
        return finish("Audition did not play the resynthesis with its oscillators");

    if (!panel.nextForSmoke() || panel.spectralStepForSmoke() != Step::Save || !panel.nextForSmoke()
        || panel.spectralStepForSmoke() != Step::Unpitched)
        return finish("Set Quasi-Harmonic Parameters did not go on to the unpitched fader");
    panel.setUnpitchedForSmoke(0.4);
    const auto half = voice.size() / 2;
    if (!playing() || !(heard(400, half - 400) < 0.25 * heard(half + 400, voice.size() - 400)))
        return finish("the unpitched fader did not leave the noise alone");

    if (!panel.nextForSmoke() || panel.spectralStepForSmoke() != Step::Trace)
        return finish("Next did not go on to the fundamental's trace");
    const auto trace = panel.traceForSmoke();
    if (trace.empty() || std::abs(talas::audio::fundamentalAt(trace, 0.1) - 220.0) > 220.0 * 0.05)
        return finish("the initial guess did not follow the fundamental");
    if (!panel.traceActionForSmoke(AnalysisToolPanel::TraceAction::HandTrace) || !panel.traceForSmoke().empty()
        || !panel.addTracePointForSmoke({0.0, 220.0}) || !panel.addTracePointForSmoke({0.2, 221.0})
        || !panel.deleteTracePointForSmoke(1) || !panel.addTracePointForSmoke({0.24, 220.0})
        || panel.traceForSmoke().size() != 2)
        return finish("Hand trace did not take the points added by hand");
    if (!panel.setTraceAuditionForSmoke(talas::app::SpectralAnalysisAudition::AllHarmonics) || !playing()
        || heard(400, half - 400) < 0.01)
        return finish("All Harmonics did not resynthesize the harmonics of the trace");

    if (!panel.nextForSmoke() || panel.spectralStepForSmoke() != Step::Harmonic
        || !panel.createHarmonicNowForSmoke(harmonic, 57.0) || !std::filesystem::exists(harmonic)
        || !std::filesystem::exists(table))
        return finish("Create Quasi-Harmonic Spectrum File did not write the spectrum and its table");
    const auto& document = projectBridge_.editor().document();
    const auto* morph = [&]() -> const talas::project::ProjectSoundDefinition* {
        const auto found = std::ranges::find(
            document.soundDefinitions, "harmonicExample2", &talas::project::ProjectSoundDefinition::definitionId);
        return found == document.soundDefinitions.end() ? nullptr : &*found;
    }();
    if (morph == nullptr || morph->displayName != "wizard voice h morph" || document.soundDefinitions.size() != 2)
        return finish("the harmonic spectrum did not leave two example Sounds, one with Morph controls");

    // The example with Morph controls plays the table at the pitch given.
    if (!projectBridge_.editor().addSoundInstance("harmonicMorph", "harmonicExample2", {120, 120}).ok()
        || !projectBridge_.editor().addNode({"harmonicOut", "talas.audioOutput", {}, {}, {}}, {360, 120}).ok()
        || !projectBridge_.editor().connect({"harmonicMorph", "audioOut", "harmonicOut", "audioIn"}).ok())
        return finish("the example with Morph controls could not be played");
    documentChanged();
    talas::runtime::RenderSettings settings;
    settings.sampleRate = sampleRate;
    settings.maxBlockFrames = 256;
    // Its table loaded, and the project's Event Values (Gate and Morph)
    // bound, as staging does.
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        projectBridge_.editor().document(), registry_, projectPath());
    if (!loaded.ok())
        return finish("the example's table could not be loaded: "
            + (loaded.failure ? talas::audio::describeProjectAssetLoadFailure(*loaded.failure) : std::string{}));
    settings.resourceSnapshots = std::move(loaded.snapshots);
    auto configured = talas::runtime::prepareProjectRenderSettings(
        projectBridge_.editor().document(), registry_, std::move(settings));
    if (!configured.ok())
        return finish("the example with Morph controls had no render settings: " + configured.error);
    auto compiled = talas::runtime::compileProject(projectBridge_.editor().document(), registry_, std::move(*configured.settings));
    if (!compiled.ok())
        return finish("the example with Morph controls did not compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 2048);
    double peak = 0.0;
    for (const auto value : rendered.mono)
        peak = std::max(peak, std::abs(value));
    if (!rendered.ok() || peak < 0.01)
        return finish("the example with Morph controls was silent");

    // A stereo sample's left and right into separate files, played
    // by a ChannelJoin of their resynthesis.
    const std::vector<std::vector<double>> loop{voice, std::vector<double>(voice.rbegin(), voice.rend())};
    if (!talas::audio::writePcm16Wav(stereoSample, sampleRate, loop, writeError) || !panel.setSampleForSmoke(stereoSample)
        || !panel.createSpectrumNowForSmoke(stereoFiles[0].parent_path() / "wizard loop s256.talaspartial",
            talas::app::StereoAnalysisChoice::LeftAndRight)
        || !std::filesystem::exists(stereoFiles[0]) || !std::filesystem::exists(stereoFiles[1]))
        return finish("a stereo sample's channels were not analyzed into files tagged L and R");
    const auto& stereoDocument = projectBridge_.editor().document();
    const auto stereo = std::ranges::find(
        stereoDocument.soundDefinitions, "stereoSpectrumExample1", &talas::project::ProjectSoundDefinition::definitionId);
    if (stereo == stereoDocument.soundDefinitions.end() || stereo->outputs.size() != 2)
        return finish("the stereo spectrum files did not leave a ChannelJoin of their resynthesis");
    return finish();
}
}
