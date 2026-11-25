#include "app/workbench/WorkbenchComponentInternal.h"

#include "talas/audio/WavWriter.h"

#include <chrono>
#include <fstream>

namespace talas::app::workbench
{
// The disk
// button beside a file name field, held down with Command or Control,
// opens an editor on the file; with Shift, a window of information on it.
[[nodiscard]] std::optional<std::string> WorkbenchComponent::runDiskButtonSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto previousProjectPath = projectPath();
    const auto previousCleanDocument = editBoundary_.cleanDocument();
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-disk-button-smoke-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto finish = [&](std::string error = {}) -> std::optional<std::string> {
        const auto restored = projectBridge_.editor().replaceDocument(previousDocument);
        editBoundary_.markRecovered(previousCleanDocument, previousProjectPath);
        documentChanged();
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        if (!restored.ok() && error.empty())
            error = "the disk button smoke could not restore the project";
        return error.empty() ? std::nullopt : std::optional<std::string>{std::move(error)};
    };
    std::filesystem::create_directories(folder);
    std::string error;
    const std::vector<std::vector<double>> harp{std::vector<double>(22050, 0.25)};
    if (!talas::audio::writePcm16Wav(folder / "harp.wav", 44100.0, harp, error))
        return finish("the smoke's sample file could not be written: " + error);
    {
        std::ofstream log{folder / "log.txt"};
        log << "1 2 3\n";
    }

    talas::project::ProjectDocument document;
    document.assets = {
        {"harp.wav", talas::project::ProjectAssetKind::Table, talas::project::pathToUtf8(folder / "harp.wav")},
        {"log.txt", talas::project::ProjectAssetKind::Table, talas::project::pathToUtf8(folder / "log.txt")},
    };
    talas::project::ProjectParameterExpression sample;
    sample.parameterId = "sample";
    sample.resource = "harp.wav";
    talas::project::ProjectParameterExpression wavetable;
    wavetable.parameterId = "wavetable";
    wavetable.resource = "FullRamp";
    talas::project::ProjectParameterExpression table;
    table.parameterId = "wavetable";
    table.resource = "log.txt";
    document.graph.nodes = {{"out", "talas.audioOutput", {}}, {"harp", "talas.sample", {sample}},
        {"ramp", "talas.oscillator", {wavetable}}, {"logged", "talas.oscillator", {table}}};
    document.graph.connections = {{"harp", "audioOut", "out", "audioIn"}};
    talas::project::reconcileProjectPresentation(document);
    if (const auto replaced = projectBridge_.editor().replaceDocument(document); !replaced.ok())
        return finish("the disk button smoke's project was rejected: " + replaced.message);
    documentChanged();

    // Shift: the sample file's header, or what the wavetable is.
    const auto information = fieldFileInformation(std::nullopt, "harp", "sample");
    if (!information || *information != "harp.wav\nWAV, 1 channel, 44100 Hz\nDuration: 22050 samp (0.5 s)")
        return finish("Shift and the disk button did not show the sample file's information: " + information.value_or(""));
    if (fieldFileInformation(std::nullopt, "ramp", "wavetable") != std::optional<std::string>{"FullRamp\nOne of the built-in wavetables, 4096 samples"})
        return finish("Shift and the disk button did not describe the wavetable");

    // Command or Control: the sample editor on the file, or on the wavetable.
    if (!openFieldFileEditor(std::nullopt, "harp", "sample") || sampleEditorPanel_ == nullptr
        || sampleEditorPanel_->model().frames() != 22050 || sampleEditorPanel_->model().sampleRate() != 44100.0)
        return finish("Command and the disk button did not open the sample editor on the file");
    if (!openFieldFileEditor(std::nullopt, "ramp", "wavetable") || sampleEditorPanel_->model().frames() != 4096
        || sampleEditorPanel_->model().channels().front()[4095] <= sampleEditorPanel_->model().channels().front()[0])
        return finish("Command and the disk button did not show FullRamp");
    if (openFieldFileEditor(std::nullopt, "logged", "wavetable"))
        return finish("a text file opened in an editor");
    return finish();
}
}
