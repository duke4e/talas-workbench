#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/QuasiHarmonicAnalysis.h"
#include "talas/audio/SpectrumTables.h"
#include "talas/project/SosSpectrumFile.h"

#include <fstream>
#include <iterator>

namespace talas::app::workbench
{
// An analysis leaves an example Sound that plays its files.
void WorkbenchComponent::addAnalysisExample(
    const AnalysisToolPanel::Completed& completed)
{
    // A harmonic spectrum leaves two SumOfSines.
    if (!completed.harmonicTable.empty())
    {
        for (auto& harmonic : talas::app::harmonicAnalysisExampleSounds(projectBridge_.editor().document(),
                 completed.harmonicTable, completed.harmonicFrames, completed.harmonicPartials, completed.pitchHz,
                 completed.durationSeconds))
        {
            const auto id = harmonic.definition.definitionId;
            if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(harmonic.definition)); !added.ok())
                return setStatus(completed.summary + " · the example Sound was refused: " + juceString(added.message));
            documentChanged();
            for (const auto& resource : harmonic.resources)
                bindResourceFile(id, resource.nodeId, resource.parameterId, resource.file);
        }
        return setStatus(completed.summary + " · added the example Sounds, one with Morph controls");
    }
    auto example = !completed.secondOutput.empty()
        ? talas::app::stereoSpectralAnalysisExampleSound(
              projectBridge_.editor().document(), completed.output, completed.secondOutput)
        : completed.kind == AnalysisToolPanel::Kind::Spectral
        ? talas::app::spectralAnalysisExampleSound(
              projectBridge_.editor().document(), completed.output)
        : completed.kind == AnalysisToolPanel::Kind::Ga
        ? talas::app::gaAnalysisExampleSound(
              projectBridge_.editor().document(), completed.output, completed.basePitch, completed.durationSeconds)
        : talas::app::reAnalysisExampleSound(
              projectBridge_.editor().document(),
              completed.output,
              completed.excitation,
              completed.durationSeconds);
    const auto definitionId = example.definition.definitionId;
    const auto displayName = example.definition.displayName;
    if (const auto added = projectBridge_.editor().addSoundDefinition(
            std::move(example.definition));
        !added.ok())
    {
        setStatus(completed.summary + " · the example Sound was refused: "
            + juceString(added.message));
        return;
    }
    // The example's controls start where its Sound sounds, unless the
    // project already has controls of those names.
    for (auto& control : example.controls)
    {
        const auto& controls = projectBridge_.editor().document().virtualControls;
        if (std::ranges::none_of(controls, [&](const auto& existing) { return existing.eventName == control.eventName; }))
            (void) projectBridge_.editor().addVirtualControl(std::move(control));
    }
    documentChanged();
    for (const auto& resource : example.resources)
        bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    // RE and GA Analysis play their example as they
    // finish.
    const auto plays = completed.kind != AnalysisToolPanel::Kind::Spectral;
    if (plays)
        playFromSoundFileWindow(definitionId);
    setStatus(completed.summary + (plays ? " · playing the example Sound " : " · added the example Sound ")
        + juceString(displayName));
}

// The Synchronizing Spectra tool; its Mix
// and Morph leave example Sounds on the two spectra's SpectrumInRAM tables.
void WorkbenchComponent::showSynchronizingSpectra()
{
    if (synchronizingSpectraPanel_ == nullptr)
    {
        synchronizingSpectraPanel_ = makeSynchronizingSpectraPanel(
            [this](const juce::String& status) { setStatus(status); },
            [this](SynchronizingSpectraPanel::Completed completed) { addSpectrumExample(completed); },
            [this](std::vector<double> samples, double sampleRate, bool loop) {
                if (samples.empty())
                    return stopSampleBuffer();
                playSampleBuffer({std::move(samples)}, sampleRate, loop);
                setStatus("Synchronizing Spectra: auditioning");
            });
    }
    if (isShowing())
    {
        if (synchronizingSpectraWindow_ == nullptr)
            synchronizingSpectraWindow_ = makeToolWindow("Synchronizing Spectra", *synchronizingSpectraPanel_);
        synchronizingSpectraWindow_->setVisible(true);
        synchronizingSpectraWindow_->toFront(true);
    }
    setStatus("Synchronizing Spectra: read the description, then Continue");
}

void WorkbenchComponent::addSpectrumExample(const SynchronizingSpectraPanel::Completed& completed)
{
    const auto& document = projectBridge_.editor().document();
    auto example = completed.example == SynchronizingSpectraPanel::Example::Mix
        ? talas::app::spectrumMixExampleSound(document, completed.name, completed.guide, completed.warped)
        : talas::app::spectrumMorphExampleSound(document, completed.name, completed.guide, completed.warped);
    const auto definitionId = example.definition.definitionId;
    const auto displayName = example.definition.displayName;
    if (const auto added = projectBridge_.editor().addSoundDefinition(std::move(example.definition)); !added.ok())
        return setStatus("The example Sound was refused: " + juceString(added.message));
    documentChanged();
    for (const auto& resource : example.resources)
        bindResourceFile(definitionId, resource.nodeId, resource.parameterId, resource.file);
    setStatus("Added the example Sound " + juceString(displayName));
}

bool WorkbenchComponent::bindSpectrumFileAsTable(const std::optional<std::string>& definitionId, const std::string& nodeId,
    const std::string& parameterId, const std::filesystem::path& file)
{
    if (!talas::project::isSpectrumFilePath(file))
        return false;
    const auto* node = nodeForContext(definitionId, nodeId);
    const auto use = node == nullptr ? talas::app::SpectrumTableUse::None : talas::app::spectrumTableUse(node->typeId, parameterId);
    if (use == talas::app::SpectrumTableUse::None)
        return false;
    std::ifstream input(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto decoded = talas::project::decodePartialTrackAsset(bytes);
    if (!decoded.ok())
    {
        setStatus("The spectrum file could not be read: " + jucePathString(file.filename()));
        return true;
    }
    const auto table = talas::app::spectrumTablePath(file);
    std::string error;
    std::size_t frames = 0;
    if (use == talas::app::SpectrumTableUse::Triples)
    {
        const auto triples = talas::audio::spectrumInRamTable(decoded.asset);
        frames = triples.frames;
        if (!talas::audio::writeSpectrumInRamTable(table, triples, error))
            return setStatus("The spectrum's table could not be written: " + juceString(error)), true;
    }
    else
    {
        const auto amplitudes = talas::audio::harmonicAmplitudeTable(decoded.asset);
        frames = amplitudes.frames;
        if (!talas::audio::writeHarmonicAmplitudeTable(table, amplitudes, error))
            return setStatus("The spectrum's table could not be written: " + juceString(error)), true;
    }
    bindResourceFile(definitionId, nodeId, parameterId, table);
    for (auto& field : talas::app::spectrumTableFields(parameterId, use, frames))
    {
        const auto set = definitionId.has_value()
            ? projectBridge_.editor().setParameterInSoundDefinition(*definitionId, nodeId, std::move(field))
            : projectBridge_.editor().setParameter(nodeId, std::move(field));
        if (!set.ok())
            return setStatus("The spectrum's table was bound, but not its frames: " + juceString(set.message)), true;
    }
    documentChanged();
    setStatus("Playing " + jucePathString(file.filename()) + " through " + jucePathString(table.filename()));
    return true;
}
}
