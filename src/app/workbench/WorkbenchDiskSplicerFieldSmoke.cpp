#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/WavWriter.h"
#include "talas/runtime/ProjectRenderSettings.h"

#include <chrono>

namespace talas::app::workbench
{
// A DiskSplicer's Segments field, its
// selection, the Edit operations, the icon buttons and the readouts, and
// the file the disk button pastes over everything selected.
[[nodiscard]] std::optional<std::string> WorkbenchComponent::runDiskSplicerFieldSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-disk-splicer-smoke-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        stopSampleBuffer();
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        if (!restored.ok() && error.empty())
            error = "the DiskSplicer smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    std::filesystem::create_directories(folder);
    std::string error;
    const std::vector<std::vector<double>> quarter{std::vector<double>(1000, 0.25)};
    const std::vector<std::vector<double>> half{std::vector<double>(400, 0.5)};
    if (!talas::audio::writePcm16Wav(folder / "a.wav", 48000.0, quarter, error)
        || !talas::audio::writePcm16Wav(folder / "b take.wav", 48000.0, half, error))
        return finish("the smoke's sample files could not be written: " + error);

    talas::project::ProjectDocument document;
    document.assets = {
        {"a.wav", talas::project::ProjectAssetKind::Table, talas::project::pathToUtf8(folder / "a.wav")},
        {"b take.wav", talas::project::ProjectAssetKind::Table, talas::project::pathToUtf8(folder / "b take.wav")},
    };
    talas::project::ProjectParameterExpression segments;
    segments.parameterId = "segments";
    segments.text = "a.wav 0 500\n'b take.wav'";
    document.graph.nodes = {{"out", "talas.audioOutput", {}}, {"splicer", "talas.diskSplicer", {segments}}};
    document.graph.connections = {{"splicer", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(document);
    if (!projectBridge_.editor().replaceDocument(document).ok())
        return finish("the DiskSplicer smoke's project was rejected");
    documentChanged();
    selectNode("splicer");
    inspectorPanel_->focusParameter("segments");
    auto* field = inspectorPanel_->diskSplicerFieldForSmoke();
    if (field == nullptr)
        return finish("the Segments field did not show graphically");
    if (field->splicer().frames() != 900 || field->problem().isNotEmpty() || field->view().frames != 900)
        return finish("the overview did not show the files as they are spliced: "
            + field->problem().toStdString());
    const auto shownText = [&] {
        const auto& nodes = projectBridge_.editor().document().graph.nodes;
        const auto node = std::ranges::find(nodes, std::string_view{"splicer"}, &talas::project::ProjectNode::instanceId);
        if (node == nodes.end())
            return std::string{};
        const auto parameter = std::ranges::find(node->parameters, std::string_view{"segments"},
            &talas::project::ProjectParameterExpression::parameterId);
        return parameter != node->parameters.end() && parameter->text.has_value() ? *parameter->text : std::string{};
    };

    // A drag on the right half selects across the splice; Cut and Paste
    // from the Edit menu work on the selection, the files left as they are.
    field->selectSamples(400, 600);
    if (!field->perform("Cut") || shownText() != "a.wav 0 400\n'b take.wav' 100 400")
        return finish("Cut did not remove the selected samples: " + shownText());
    if (!field->perform("Paste") || shownText() != "a.wav 0 500\n'b take.wav'")
        return finish("Paste did not put them back: " + shownText());
    const auto readout = field->readout();
    if (!readout.contains("200 samp") || !readout.contains("400 - 600 samp"))
        return finish("the selection's size and range were not shown: " + readout.toStdString());
    field->switchUnits();
    if (!field->readout().contains(" s · ") || field->readout().contains("samp"))
        return finish("a click did not change their units: " + field->readout().toStdString());

    // The icon buttons.
    if (!field->perform("Fill") || field->view().start != 400 || field->view().frames != 200
        || !field->perform("Time In") || field->view().frames != 100 || !field->perform("Level In")
        || field->view().level != 2.0 || !field->perform(">") || field->view().start != 500
        || !field->perform("<") || field->view().start != 400 || !field->perform("Time Out")
        || field->view().frames != 200 || !field->perform("Level Out") || field->view().level != 1.0)
        return finish("the icon buttons did not move or scale the display");
    field->showSamples(100, 300);
    if (field->view().start != 100 || field->view().frames != 200)
        return finish("a drag in the overview did not pick what the right half shows");
    if (!field->perform("Play"))
        return finish("the play button did not play the selected samples");
    field->selectSamples(0, 100);
    if (!field->perform("Trim") || shownText() != "a.wav 0 100")
        return finish("Trim did not keep only the selection: " + shownText());

    // Select All, then the file chosen with the disk button.
    if (!field->perform("Select All") || !field->pasteFile("b take.wav") || shownText() != "'b take.wav'"
        || field->splicer().frames() != 400)
        return finish("the disk button did not replace the samples with the file's: " + shownText());

    // It plays the file it splices.
    talas::runtime::RenderSettings settings;
    settings.sampleRate = 48000.0;
    settings.maxBlockFrames = 256;
    auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(projectBridge_.editor().document(), registry_, projectPath());
    if (!loaded.ok())
        return finish("the spliced file could not be loaded: "
            + (loaded.failure ? talas::audio::describeProjectAssetLoadFailure(*loaded.failure) : std::string{}));
    settings.resourceSnapshots = std::move(loaded.snapshots);
    auto configured = talas::runtime::prepareProjectRenderSettings(projectBridge_.editor().document(), registry_, std::move(settings));
    if (!configured.ok())
        return finish("the DiskSplicer had no render settings: " + configured.error);
    auto compiled = talas::runtime::compileProject(projectBridge_.editor().document(), registry_, std::move(*configured.settings));
    if (!compiled.ok())
        return finish("the DiskSplicer did not compile: "
            + (compiled.diagnostics.empty() ? std::string{} : compiled.diagnostics.front().message));
    const auto rendered = talas::runtime::renderOffline(*compiled.graph, 600);
    if (!rendered.ok() || std::abs(rendered.mono[100] - 0.5) > 1.0e-3 || std::abs(rendered.mono[500]) > 1.0e-9)
        return finish("the DiskSplicer did not play the file it splices");
    return finish();
}
}
