#include "talas/app/DiskButton.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/SmalltalkSubset.h"

#include <iostream>
#include <stdexcept>
#include <string>

// // the disk button beside a file name field.
namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// A click chooses the file; Command or Control opens an editor on it,
// Shift a window of information.
void testTheButtonsModifiers()
{
    using talas::app::DiskButtonAction;
    require(talas::app::diskButtonAction(false, false) == DiskButtonAction::ChooseFile, "a click chooses a file from a list");
    require(talas::app::diskButtonAction(true, false) == DiskButtonAction::OpenEditor
            && talas::app::diskButtonAction(true, true) == DiskButtonAction::OpenEditor,
        "Command or Control held opens an editor on the file");
    require(talas::app::diskButtonAction(false, true) == DiskButtonAction::ShowInformation,
        "Shift held opens an information window");
}

// A sample file or wavetable opens in the sample editor, a spectrum file in
// the spectrum editor.
void testTheEditorAFileOpensIn()
{
    using talas::app::FileEditor;
    require(talas::app::fileEditorFor("Samples/Celthrp2a.AIF", false) == FileEditor::Sample
            && talas::app::fileEditorFor("take.wav", false) == FileEditor::Sample
            && talas::app::fileEditorFor("FullRamp", true) == FileEditor::Sample,
        "sample files and wavetables open in the sample editor");
    require(talas::app::fileEditorFor("hi fu mi yo h.talaspartial", false) == FileEditor::Spectrum
            && talas::app::fileEditorFor("horse h.spc", false) == FileEditor::Spectrum,
        "spectrum files open in the spectrum editor");
    require(talas::app::fileEditorFor("widgetSamplerLog.txt", false) == FileEditor::None
            && talas::app::fileEditorFor("my.dir/table", false) == FileEditor::None,
        "other files open in no editor");
}

// The information window: the file's header, and for a spectrum file its
// frames and duration.
void testTheInformationWindow()
{
    talas::app::SampleFileInformation file;
    file.name = "Celthrp2a.aif";
    file.channels = 1;
    file.sampleRate = 44100.0;
    file.frames = 88200;
    file.loop = std::pair<std::uint64_t, std::uint64_t>{100, 2000};
    file.pitch = 60.5;
    require(talas::app::sampleFileInformationText(file)
            == "Celthrp2a.aif\nAIFF, 1 channel, 44100 Hz\nDuration: 88200 samp (2 s)\nLoop: 100 - 2000 samp\nPitch: 60.5 nn",
        "a sample file's format, channels, rate, duration, loop and pitch: " + talas::app::sampleFileInformationText(file));
    talas::project::PartialTrackAsset spectrum{48000.0, 96000,
        {{1, 0, {{1, 0.5, 220.0, 0.0}, {2, 0.25, 440.0, 0.0}}}, {1, 480, {{1, 0.5, 220.0, 0.0}}}}};
    require(talas::app::spectrumFileInformationText("hi fu mi yo h.talaspartial", spectrum)
            == "hi fu mi yo h.talaspartial\nSpectrum file, 2 frames of up to 2 partials\nFrame: 10 ms\nDuration: 2 s, analysed at 48000 Hz",
        "a spectrum file's frames, partials and duration: "
            + talas::app::spectrumFileInformationText("hi fu mi yo h.talaspartial", spectrum));
    require(talas::app::wavetableInformationText("Sine", 4096) == "Sine\nOne of the built-in wavetables, 4096 samples",
        "a wavetable's length");

    // More than an information window, it takes
    // typing and evaluates it; bpm typed after the duration's seconds
    // gives the tempo of one pass through the sample.
    auto typed = talas::app::sampleFileInformationText(file);
    const auto seconds = typed.find("2 s)");
    typed.insert(seconds + 3, " bpm");
    talas::project::smalltalk::EvaluationHost host;
    const auto evaluated = talas::project::smalltalk::evaluateSelection(typed, seconds, seconds + 7, host);
    require(evaluated.ok() && typed.substr(seconds, 7) == "2 s bpm"
            && evaluated.text.substr(evaluated.resultStart, evaluated.resultEnd - evaluated.resultStart) == "30 bpm",
        "what is typed into the information window should evaluate: "
            + evaluated.text.substr(evaluated.resultStart, evaluated.resultEnd - evaluated.resultStart));
}
}

// Just below a file name field, a check box reads the
// recording of the MemoryWriter it names instead of a file.
void testTheFromMemoryWriterCheckBox()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto boxOf = [&](std::string_view typeId, std::string_view field) {
        const auto* descriptor = registry.findNodeType(typeId);
        require(descriptor != nullptr, std::string{typeId} + " should be registered");
        return talas::app::fromMemoryWriterCheckBox(*descriptor, field);
    };
    for (const auto& [typeId, field] : {std::pair{"talas.sample", "sample"}, std::pair{"talas.sampleCloud", "sample"},
             std::pair{"talas.timeFrequencyScale", "sample"}, std::pair{"talas.functionGenerator", "wavetable"},
             std::pair{"talas.triggeredTableRead", "wavetable"}, std::pair{"talas.waveshaper", "shapingFunction"}})
    {
        require(boxOf(typeId, field) == std::optional<std::string>{"fromMemoryWriter"},
            std::string{""} + typeId + "'s " + field + " field should have its FromMemoryWriter box");
    }
    require(!boxOf("talas.diskPlayer", "fileName").has_value(),
        "a file field the Sound never reads from a MemoryWriter has no box");
}

// A Sound's second file field has a FromMemoryWriter box
// of its own, as a SampleCloud's GrainEnv does (grainEnvFromMemoryWriter).
void testASecondFileFieldHasItsOwnFromMemoryWriterBox()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto* cloud = registry.findNodeType("talas.sampleCloud");
    require(cloud != nullptr, "talas.sampleCloud should be registered");
    require(talas::app::fromMemoryWriterCheckBox(*cloud, "grainEnv") == std::optional<std::string>{"grainEnvFromMemoryWriter"}
            && talas::app::fromMemoryWriterCheckBox(*cloud, "sample") == std::optional<std::string>{"fromMemoryWriter"},
        "a SampleCloud's GrainEnv should have its own FromMemoryWriter box");
    require(!talas::app::fromMemoryWriterCheckBox(*cloud, "maxGrains").has_value(),
        "a field that names no file has no box");
}

int main()
{
    try
    {
        testTheButtonsModifiers();
        testTheEditorAFileOpensIn();
        testTheInformationWindow();
        testTheFromMemoryWriterCheckBox();
        testASecondFileFieldHasItsOwnFromMemoryWriterBox();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
