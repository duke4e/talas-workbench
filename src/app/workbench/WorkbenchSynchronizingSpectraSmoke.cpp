#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/PartialTrackAudioAnalysis.h"
#include "talas/runtime/ProjectRenderSettings.h"

namespace talas::app::workbench
{
// Synchronizing Spectra
// marks two spectra, saves the markers with them, and leaves a Mix and a
// Morph of the two.
[[nodiscard]] std::optional<std::string>
WorkbenchComponent::runSynchronizingSpectraSmoke(const std::filesystem::path& smokeDirectory)
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto guideFile = (smokeDirectory / "sync guide h.talaspartial").lexically_normal();
    const auto warpedFile = (smokeDirectory / "sync warped h.talaspartial").lexically_normal();
    const auto removeFiles = [&] {
        std::error_code ignored;
        for (const auto& file : {guideFile, warpedFile})
        {
            std::filesystem::remove(file, ignored);
            std::filesystem::remove(talas::app::spectrumMarkersPath(file), ignored);
            auto table = file;
            table.replace_filename(talas::project::pathFromUtf8(talas::project::pathToUtf8(file.stem()) + " table.txt"));
            std::filesystem::remove(table, ignored);
        }
    };
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        stopSampleBuffer();
        synchronizingSpectraWindow_.reset();
        synchronizingSpectraPanel_.reset();
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        removeFiles();
        if (!restored.ok() && error.empty())
            error = "Synchronizing Spectra smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };

    const auto names = getMenuBarNames();
    const auto toolsMenu = getMenuForIndex(names.indexOf("Tools"), "Tools");
    auto listed = false;
    for (juce::PopupMenu::MenuItemIterator item{toolsMenu}; item.next();)
        listed = listed || item.getItem().itemID == commandShowSynchronizingSpectra;
    if (!listed)
        return finish("the Tools menu does not offer Synchronizing Spectra");

    // Two spoken words each: a tone, then another, changing at a different
    // frame in each spectrum.
    removeFiles();
    const auto spectrum = [](double low, double high, std::uint64_t change) {
        std::vector<talas::project::PartialTrackFrame> frames;
        for (std::uint64_t index = 0; index < 20; ++index)
        {
            const auto hz = index < change ? low : high;
            frames.push_back({1, index * 480, {{1, index < change ? 0.2 : 0.6, hz, 0.0}, {2, 0.1, hz * 2.0, 0.0}}});
        }
        return talas::project::PartialTrackAsset{48000.0, 9600, std::move(frames)};
    };
    std::string error;
    if (!talas::audio::publishPartialTrackAssetNoReplace(guideFile, spectrum(440.0, 550.0, 10), error)
        || !talas::audio::publishPartialTrackAssetNoReplace(warpedFile, spectrum(660.0, 770.0, 8), error))
        return finish("the smoke's spectra could not be written: " + error);
    if (!projectBridge_.editor().replaceDocument(talas::project::ProjectDocument{}).ok())
        return finish("the Synchronizing Spectra project was rejected");
    documentChanged();

    if (!commandManager_.invokeDirectly(commandShowSynchronizingSpectra, false) || synchronizingSpectraPanel_ == nullptr
        || synchronizingSpectraWindow_ != nullptr)
        return finish("Synchronizing Spectra did not open (without a window off screen)");
    auto& panel = *synchronizingSpectraPanel_;
    if (!panel.onDescriptionForSmoke() || !panel.continueForSmoke() || panel.onDescriptionForSmoke())
        return finish("the description page did not lead on with Continue");
    if (!panel.loadForSmoke(true, guideFile) || !panel.loadForSmoke(false, warpedFile))
        return finish("the Guide and Warped spectra did not open");

    const juce::KeyPress keyA{'a'};
    const juce::KeyPress keyM{'m'};
    const juce::KeyPress keyS{'s'};
    if (!panel.keyForSmoke(true, keyA) || !panel.keyForSmoke(false, keyA)
        || panel.markersForSmoke(true).size() != 3 || panel.markersForSmoke(false).size() != 3
        || panel.markersForSmoke(false)[1].frame != 8)
        return finish("a did not suggest markers at the ends and the change: "
            + std::to_string(panel.markersForSmoke(true).size()) + " and " + std::to_string(panel.markersForSmoke(false).size()));
    panel.selectMarkerForSmoke(false, 1);
    // A marker clicked moves the scrub bar to it.
    if (panel.scrubbedForSmoke(false) != panel.markersForSmoke(false)[1].frame)
        return finish("a marker selected did not move the scrub bar to it");
    if (!panel.keyForSmoke(false, juce::KeyPress{juce::KeyPress::deleteKey}) || panel.markersForSmoke(false).size() != 2)
        return finish("Delete did not remove the selected marker");
    if (panel.exampleForSmoke(SynchronizingSpectraPanel::Example::Mix))
        return finish("Mix was made from spectra of different numbers of markers");
    panel.scrubForSmoke(false, 8);
    if (!panel.keyForSmoke(false, keyM) || panel.markersForSmoke(false).size() != 3)
        return finish("m did not add a marker at the scrub bar");
    if (!panel.keyForSmoke(false, juce::KeyPress{juce::KeyPress::tabKey}) || !panel.guideIsActiveForSmoke())
        return finish("Tab did not switch to the other view");
    // MIDI pitch bend scrubs the active view's spectrum.
    if (!panel.scrubForPitchBend(16383) || panel.scrubbedForSmoke(true) == 0 || !panel.scrubForPitchBend(0)
        || panel.scrubbedForSmoke(true) != 0)
        return finish("pitch bend did not scrub the active view's spectrum");
    if (!panel.keyForSmoke(false, keyS) || !std::filesystem::exists(talas::app::spectrumMarkersPath(warpedFile)))
        return finish("s did not save the markers with the spectrum");

    if (!panel.exampleForSmoke(SynchronizingSpectraPanel::Example::Mix)
        || !panel.exampleForSmoke(SynchronizingSpectraPanel::Example::Morph))
        return finish("Mix and Morph did not leave their example Sounds");
    const auto& document = projectBridge_.editor().document();
    const auto definition = [&](std::string_view id) {
        return std::ranges::find(document.soundDefinitions, id, &talas::project::ProjectSoundDefinition::definitionId);
    };
    if (definition("spectrumMix1") == document.soundDefinitions.end()
        || definition("spectrumMix1")->outputs.size() != 2
        || definition("spectrumMorph1") == document.soundDefinitions.end())
        return finish("the Mix should play both spectra, one in each channel, and the Morph one into the other");

    // A spectrum file chosen for a SpectrumInRAM's Analysis plays through
    // its table.
    if (!projectBridge_.editor().addNode({"syncRam", "talas.spectrumInRam", {}, {}, {}}, {120, 320}).ok()
        || !bindSpectrumFileAsTable(std::nullopt, "syncRam", "analysis", guideFile))
        return finish("a spectrum file was not taken in a SpectrumInRAM's Analysis");
    {
        const auto& ram = *std::ranges::find(projectBridge_.editor().document().graph.nodes, std::string{"syncRam"},
            &talas::project::ProjectNode::instanceId);
        const auto field = [&](std::string_view id) -> std::optional<std::string> {
            const auto found = std::ranges::find(ram.parameters, id, &talas::project::ProjectParameterExpression::parameterId);
            return found == ram.parameters.end() ? std::nullopt : (found->expression ? found->expression : found->resource);
        };
        if (field("analysisFrames") != std::optional<std::string>{"20"} || field("analysisFrequency") != std::optional<std::string>{"1 hz"}
            || !field("analysis").has_value() || !std::filesystem::exists(talas::app::spectrumTablePath(guideFile)))
            return finish("the SpectrumInRAM did not take the spectrum's table and frames");
    }
    if (!projectBridge_.editor().removeNode("syncRam").ok())
        return finish("the smoke's SpectrumInRAM could not be removed");

    // The morph plays the two tables.
    if (!projectBridge_.editor().addSoundInstance("syncMorph", "spectrumMorph1", {120, 120}).ok()
        || !projectBridge_.editor().addNode({"syncOut", "talas.audioOutput", {}, {}, {}}, {360, 120}).ok()
        || !projectBridge_.editor().connect({"syncMorph", "audioOut", "syncOut", "audioIn"}).ok())
        return finish("the Morph could not be played");
    documentChanged();
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(projectBridge_.editor().document(), registry_, projectPath());
    if (!loaded.ok())
        return finish("the Morph's tables could not be loaded: "
            + (loaded.failure ? talas::audio::describeProjectAssetLoadFailure(*loaded.failure) : std::string{}));
    settings.resourceSnapshots = std::move(loaded.snapshots);
    auto configured = talas::runtime::prepareProjectRenderSettings(projectBridge_.editor().document(), registry_, std::move(settings));
    if (!configured.ok())
        return finish("the Morph had no render settings: " + configured.error);
    auto compiled = talas::runtime::compileProject(projectBridge_.editor().document(), registry_, std::move(*configured.settings));
    if (!compiled.ok())
        return finish("the Morph did not compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 4800);
    double peak = 0.0;
    for (const auto value : rendered.mono)
        peak = std::max(peak, std::abs(value));
    if (!rendered.ok() || peak < 0.01)
        return finish("the Morph was silent");
    return finish();
}
}
