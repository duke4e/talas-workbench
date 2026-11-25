#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/PartialTrackAudioAnalysis.h"

namespace talas::app::workbench
{
// The Tools menu's Spectral Analysis and RE Analysis
// write their files from a sample and leave example Sounds that play them.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runAnalysisToolsSmoke(
    const std::filesystem::path& smokeDirectory)
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto sample = (smokeDirectory / "analysis-tool-voice.wav").lexically_normal();
    const auto spectrum = (smokeDirectory / "analysis-tool-voice.talaspartial").lexically_normal();
    const auto table = (smokeDirectory / "analysis-tool-voice RE.txt").lexically_normal();
    const auto excitation = (smokeDirectory / "analysis-tool-voice EX.wav").lexically_normal();
    const auto gaFile = (smokeDirectory / "analysis-tool-voice GA.wav").lexically_normal();
    const auto removeFiles = [&] {
        std::error_code ignored;
        for (const auto& file : {sample, spectrum, table, excitation, gaFile})
            std::filesystem::remove(file, ignored);
    };
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        stopSampleBuffer();
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(
            previousCleanDocument, previousProjectPath);
        documentChanged();
        removeFiles();
        if (!restored.ok() && error.empty())
            error = "analysis tools smoke could not restore the project";
        return error.empty()
            ? std::nullopt
            : std::optional<std::string>{std::move(error)};
    };
    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto spectralListed = false;
    auto reListed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
    {
        spectralListed = spectralListed || item.getItem().itemID == commandShowSpectralAnalysis;
        reListed = reListed || item.getItem().itemID == commandShowReAnalysis;
    }
    if (!spectralListed || !reListed)
        return finish("the Tools menu does not offer Spectral Analysis and RE Analysis");

    removeFiles();
    constexpr double sampleRate = 22050.0;
    std::vector<double> voice(11025);
    for (std::size_t frame = 0; frame < voice.size(); ++frame)
    {
        const auto t = static_cast<double>(frame) / sampleRate;
        voice[frame] = 0.4 * std::sin(2.0 * std::numbers::pi * 330.0 * t)
            + 0.2 * std::sin(2.0 * std::numbers::pi * 660.0 * t);
    }
    std::string writeError;
    if (!talas::audio::writePcm16Wav(
            sample, sampleRate, std::span<const std::vector<double>>{&voice, 1}, writeError))
        return finish("the analysis sample could not be written: " + writeError);
    if (const auto replaced = projectBridge_.editor().replaceDocument(talas::project::ProjectDocument{});
        !replaced.ok())
        return finish("the analysis tools project was rejected");
    documentChanged();

    if (!commandManager_.invokeDirectly(commandShowSpectralAnalysis, false)
        || spectralAnalysisPanel_ == nullptr || spectralAnalysisWindow_ != nullptr)
        return finish("Spectral Analysis did not open (without a window off screen)");
    spectralAnalysisPanel_->setSpectralSettingsForSmoke(3, 0, false);
    if (!spectralAnalysisPanel_->setSampleForSmoke(sample)
        || !spectralAnalysisPanel_->analyzeNowForSmoke(spectrum, false)
        || !std::filesystem::exists(spectrum))
        return finish("Spectral Analysis did not write its spectrum file");
    const auto& document = projectBridge_.editor().document();
    const auto boundResource = [&](std::string_view definitionId,
                                   std::string_view nodeId,
                                   std::string_view parameterId)
        -> const talas::project::ProjectAsset* {
        const auto definition = std::ranges::find(
            document.soundDefinitions, definitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == document.soundDefinitions.end())
            return nullptr;
        const auto node = std::ranges::find(
            definition->graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
        if (node == definition->graph.nodes.end())
            return nullptr;
        const auto parameter = std::ranges::find(
            node->parameters, parameterId,
            &talas::project::ProjectParameterExpression::parameterId);
        if (parameter == node->parameters.end() || !parameter->resource.has_value())
            return nullptr;
        const auto asset = std::ranges::find(
            document.assets, *parameter->resource, &talas::project::ProjectAsset::id);
        return asset == document.assets.end() ? nullptr : &*asset;
    };
    const auto* partials = boundResource("spectrumExample1", "partials", "partialTrack");
    if (partials == nullptr
        || partials->kind != talas::project::ProjectAssetKind::PartialSpectrum)
        return finish("Spectral Analysis did not leave an example Sound playing its spectrum file");

    if (!commandManager_.invokeDirectly(commandShowReAnalysis, false)
        || reAnalysisPanel_ == nullptr)
        return finish("RE Analysis did not open");
    // The sample chosen is played once.
    if (!reAnalysisPanel_->setSampleForSmoke(sample) || sampleAuditionOwned_ == nullptr || sampleAuditionOwned_->loop)
        return finish("RE Analysis did not play the sample chosen once");
    if (!reAnalysisPanel_->analyzeNowForSmoke(table, true)
        || !std::filesystem::exists(table) || !std::filesystem::exists(excitation))
        return finish("RE Analysis did not write its RE and EX files");
    const auto* resonance = boundResource("reExample1", "resonator", "wavetable");
    const auto* source = boundResource("reExample1", "excitation", "sample");
    if (resonance == nullptr || source == nullptr)
        return finish("RE Analysis did not leave an example Sound on its RE and EX files");
    // Gain, Rate and ExAmp in the VCS, at 1.
    for (const auto* name : {"Gain", "Rate", "ExAmp"})
    {
        const auto& controls = document.virtualControls;
        const auto control = std::ranges::find(controls, std::string{name}, &talas::project::VirtualControl::eventName);
        if (control == controls.end() || control->value != 1.0)
            return finish(std::string{"the RE example's "} + name + " control did not start at 1");
    }

    // GA Analysis writes a GA file and leaves a GAOscillators
    // playing it.
    bool gaListed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        gaListed = gaListed || item.getItem().itemID == commandShowGaAnalysis;
    if (!gaListed || !commandManager_.invokeDirectly(commandShowGaAnalysis, false) || gaAnalysisPanel_ == nullptr)
        return finish("GA Analysis did not open from the Tools menu");
    if (!gaAnalysisPanel_->setSampleForSmoke(sample)
        || !gaAnalysisPanel_->analyzeNowForSmoke(gaFile, false)
        || !std::filesystem::exists(gaFile))
        return finish("GA Analysis did not write its GA file");
    if (boundResource("gaExample1", "oscillators", "analysis0") == nullptr)
        return finish("GA Analysis did not leave an example GAOscillators on its GA file");

    // GA Analysis takes a harmonic spectrum file.
    const auto harmonicFile = (smokeDirectory / "analysis-tool-voice h.talaspartial").lexically_normal();
    const auto spectrumGaFile = (smokeDirectory / "analysis-tool-voice h GA.wav").lexically_normal();
    std::vector<talas::project::PartialTrackFrame> harmonicFrames;
    for (std::uint64_t index = 0; index < 50; ++index)
        harmonicFrames.push_back({1, index * 480, {{1, 0.4, 330.0, 0.0}, {2, 0.2, 660.0, 0.0}, {3, 0.1, 990.0, 0.0}}});
    std::string spectrumError;
    const auto spectrumAnalyzed =
        talas::audio::publishPartialTrackAssetNoReplace(harmonicFile, {48000.0, 24000, std::move(harmonicFrames)}, spectrumError)
        && gaAnalysisPanel_->setSampleForSmoke(harmonicFile) && gaAnalysisPanel_->analyzeNowForSmoke(spectrumGaFile, false)
        && std::filesystem::exists(spectrumGaFile) && boundResource("gaExample2", "oscillators", "analysis0") != nullptr;
    std::error_code ignored;
    std::filesystem::remove(harmonicFile, ignored);
    std::filesystem::remove(spectrumGaFile, ignored);
    if (!spectrumAnalyzed)
        return finish("GA Analysis did not analyze a harmonic spectrum file: " + spectrumError);
    return finish();
}
}
