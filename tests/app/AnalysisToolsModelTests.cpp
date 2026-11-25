#include "talas/dsp/ReAnalysis.h"
#include "talas/app/AnalysisToolsModel.h"
#include "talas/app/DiskButton.h"
#include "talas/app/TalasFiles.h"
#include "talas/app/SpectralAnalysisSteps.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/SosSpectrumFile.h"
#include "talas/project/ProjectEditor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

const talas::core::NodeRegistry& registry()
{
    static const talas::core::NodeRegistry builtins = [] {
        talas::core::NodeRegistry value;
        talas::core::registerBuiltinNodeTypes(value);
        return value;
    }();
    return builtins;
}

const talas::project::ProjectNode* node(
    const talas::project::ProjectSoundDefinition& definition,
    std::string_view id)
{
    const auto found = std::ranges::find(
        definition.graph.nodes, id, &talas::project::ProjectNode::instanceId);
    return found == definition.graph.nodes.end() ? nullptr : &*found;
}

bool hasExpression(
    const talas::project::ProjectNode& value,
    std::string_view parameter,
    std::string_view expression)
{
    return std::ranges::any_of(value.parameters, [&](const auto& candidate) {
        return candidate.parameterId == parameter
            && candidate.expression == std::optional<std::string>{std::string{expression}};
    });
}

// Five lowest-frequency settings, the highest one below the
// sample's fundamental to be taken, Above 3F for middle C; BestFreq to
// BestTime.
void testSpectralAnalysisSettings()
{
    require(std::abs(talas::app::spectralAnalysisLowestFrequencyHz(1) - 43.6535) < 1.0e-3
            && std::abs(talas::app::spectralAnalysisLowestFrequencyHz(3) - 174.614) < 1.0e-2
            && std::abs(talas::app::spectralAnalysisLowestFrequencyHz(5) - 698.456) < 1.0e-2,
        "the settings should be the F of octaves 1 to 5");
    int below = 0;
    for (int setting = 1; setting <= talas::app::spectralAnalysisLowestFrequencySettings; ++setting)
        if (talas::app::spectralAnalysisLowestFrequencyHz(setting) < 261.626)
            below = setting;
    require(below == 3, "middle C should take Above 3F");
    require(talas::app::spectralAnalysisLowestFrequencyLabel(3) == "Above 3F (175 Hz)",
        "a setting's label should name it and its frequency");
    require(talas::app::spectralAnalysisResponseLabels()
                == std::vector<std::string>{"BestFreq", "BetterFreq", "BetterTime", "BestTime"}
            && talas::app::spectralAnalysisResponseBins(0) == 1
            && talas::app::spectralAnalysisResponseBins(3) == 4,
        "the responses should run from one to four bins");
    require(talas::app::analysisOutputPath("/sounds/voice.wav", " RE.txt")
            == std::filesystem::path{"/sounds/voice RE.txt"},
        "a tool's file should go beside the sample under its name");
    // Create RE File Only saves into the RE folder of the
    // Wavetables folder.
    require(talas::app::reAnalysisOutputPath("/sounds/voice.wav", "/Users/me/Talas")
            == std::filesystem::path{"/Users/me/Talas/Wavetables/RE/voice RE.txt"},
        "the RE file should go into the RE folder of the Wavetables folder");
    // Create GA File asks a name for the GA file, the
    // sample's own with GA after it.
    require(talas::app::analysisOutputPath("/sounds/voice.wav", " GA.wav") == std::filesystem::path{"/sounds/voice GA.wav"},
        "the GA file should be named for the sample");
    // 512 filters for 1F, 256 for 2F, halving each octave up;
    // the file is named after the sample, an s and the number of tracks.
    for (int setting = 1; setting <= talas::app::spectralAnalysisLowestFrequencySettings; ++setting)
    {
        require(talas::app::spectralAnalysisTracks(setting) == std::size_t{512} >> (setting - 1),
            "Above " + std::to_string(setting) + "F should analyze with "
                + std::to_string(512 >> (setting - 1)) + " tracks");
    }
    require(talas::app::analysisOutputPath("/sounds/voice.wav", talas::app::spectralAnalysisFileSuffix(2))
            == std::filesystem::path{"/sounds/voice s256.talaspartial"},
        "a spectrum file should take the sample's name, an s and its tracks");
}

// A spectrum file holds envelopes, not audio
// samples, so it has an extension of its own, .spc. The Spectral Analysis
// tool and the spectrum editor offer it for the spectrum file they save,
// beside Talas's own .talaspartial. Named .spc the analysis is written as
// a spectrum file: an AIFF whose 24-bit words are each partial's
// amplitude and frequency envelopes, frame by frame. The name marks it: as
// .spc it is a spectrum (opening in the spectrum editor, playing through
// the Sound that resynthesizes it), the same file named .aif a sample; and
// it reads back as the analysis, as the File Organizer shows.
void testSpectrumFilesHaveTheirOwnExtension()
{
    const std::string patterns{talas::project::spectrumFilePatterns};
    require(patterns.find("*.spc") != std::string::npos && patterns.find("*.talaspartial") != std::string::npos,
        "a spectrum file should be offered as .spc, and as .talaspartial");
    auto named = talas::app::analysisOutputPath("/sounds/voice.wav", talas::app::spectralAnalysisFileSuffix(1));
    named.replace_extension(talas::project::sosSpectrumFileExtension);
    require(named == std::filesystem::path{"/sounds/voice s512.spc"}, "the analysis should take its .spc name");

    talas::project::PartialTrackAsset analysis{44100.0, 4410, {}};
    for (std::uint64_t frame = 0; frame < 10; ++frame)
        analysis.frames.push_back({1, frame * 441, {{1, 0.5, 220.0, 0.0}, {2, 0.25, 440.0, 0.0}}});
    const auto spectrum = talas::project::encodeSpectrumFileFor(named, analysis);
    require(spectrum.ok(), "the analysis should write as a .spc");
    const auto& bytes = spectrum.bytes;
    const auto at = [&](std::size_t offset) {
        return (std::uint32_t{bytes[offset]} << 16U) | (std::uint32_t{bytes[offset + 1]} << 8U) | bytes[offset + 2];
    };
    require(std::string(bytes.begin(), bytes.begin() + 4) == "FORM" && std::string(bytes.begin() + 8, bytes.begin() + 12) == "AIFF"
            && std::string(bytes.begin() + 38, bytes.begin() + 42) == "SSND" && talas::project::isSosSpectrumFile(bytes),
        "the .spc should be a spectrum file, an AIFF with its SOSe chunk");
    // The frames' first words, from byte 54: the two partials' envelope
    // values, the same in each frame, then silence.
    for (std::size_t frame = 0; frame < 10; ++frame)
    {
        const auto first = 54 + frame * 32 * 3;
        require(at(first) == talas::project::encodeSosSpectrumPartial(0.5, 220.0, 44100.0)
                && at(first + 3) == talas::project::encodeSosSpectrumPartial(0.25, 440.0, 44100.0) && (at(first + 6) >> 16U) == 0,
            "the file should hold envelope values, not audio samples, frame " + std::to_string(frame));
    }
    require(talas::project::encodeSpectrumFileFor("/sounds/voice s512.talaspartial", analysis).bytes
            == talas::project::encodePartialTrackAsset(analysis).bytes,
        "a .talaspartial should still be written as Talas's own spectrum file");

    // The extension, not the AIFF inside, makes it a spectrum.
    require(talas::app::talasFileKindOf(named) == talas::app::TalasFileKind::Spectrum
            && talas::app::talasFileKindOf("/sounds/voice s512.aif") == talas::app::TalasFileKind::Sample
            && talas::app::fileEditorFor("voice s512.spc", false) == talas::app::FileEditor::Spectrum
            && talas::app::fileEditorFor("voice s512.aif", false) == talas::app::FileEditor::Sample,
        "the .spc extension should mark a spectrum file");
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-spectrum-extension-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto file = folder / named.filename();
    {
        std::ofstream output(file, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const auto information = talas::app::talasFileInformation(file);
    const auto player = talas::app::talasFilePlayer(file);
    std::filesystem::remove_all(folder);
    require(std::ranges::find(information, std::string{"2 tracks \u00b7 10 frames \u00b7 0.100 s"}) != information.end(),
        "the File Organizer should read the .spc's tracks and frames");
    require(player.has_value() && player->soundFile.assets.size() == 1
            && player->soundFile.assets.front().kind == talas::project::ProjectAssetKind::PartialSpectrum,
        "the .spc should play through the Sound that resynthesizes a spectrum");
    const auto decoded = talas::project::decodePartialTrackAsset(bytes);
    require(decoded.ok() && decoded.asset.frames.size() == 10 && decoded.asset.frames[3].points.size() == 2
            && std::abs(decoded.asset.frames[3].points[1].frequencyHz / 440.0 - 1.0) < 1.0e-4,
        "the .spc should read back as the analysis");
}

// An example Sound plays the files an analysis
// wrote.
void testExampleSounds()
{
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry()};
    auto spectrum = talas::app::spectralAnalysisExampleSound(
        editor.document(), "/sounds/voice.talaspartial");
    require(spectrum.definition.definitionId == "spectrumExample1"
            && spectrum.definition.displayName == "voice resynthesis"
            && node(spectrum.definition, "partials") != nullptr
            && node(spectrum.definition, "partials")->typeId == "talas.partialTrackPlayerV1"
            && spectrum.resources.size() == 1
            && spectrum.resources.front().parameterId == "partialTrack",
        "a spectrum's example should be a partial-track player on the file");
    const auto added = editor.addSoundDefinition(spectrum.definition);
    require(added.ok(), "the spectrum example should be a valid Sound: " + added.message);
    require(talas::app::spectralAnalysisExampleSound(editor.document(), "x.talaspartial")
                .definition.definitionId == "spectrumExample2",
        "the next example should take a fresh id");

    auto withExcitation = talas::app::reAnalysisExampleSound(
        editor.document(), "/sounds/voice RE.txt", "/sounds/voice EX.wav", 2.5);
    const auto* ramp = node(withExcitation.definition, "ramp");
    const auto* excitation = node(withExcitation.definition, "excitation");
    require(ramp != nullptr && hasExpression(*ramp, "onDuration", "2.5 s / !Rate")
            && excitation != nullptr && excitation->typeId == "talas.sample"
            && hasExpression(*excitation, "frequency", "default * !Rate") && hasExpression(*excitation, "scale", "!ExAmp")
            && node(withExcitation.definition, "output") != nullptr
            && hasExpression(*node(withExcitation.definition, "output"), "left", "!Gain")
            && withExcitation.controls.size() == 3 && withExcitation.controls[0].eventName == "Gain"
            && withExcitation.controls[1].value == 1.0
            && withExcitation.resources.size() == 2
            && withExcitation.resources[0].nodeId == "resonator"
            && withExcitation.resources[0].parameterId == "wavetable"
            && withExcitation.resources[1].nodeId == "excitation",
        "an RE example should ramp the resonator over the sound at !Rate and play the EX file through it at !ExAmp, "
        "its output at !Gain, each control at 1 to start");
    const auto withAdded = editor.addSoundDefinition(withExcitation.definition);
    require(withAdded.ok(), "the RE example should be a valid Sound: " + withAdded.message);

    const auto noise = talas::app::reAnalysisExampleSound(
        editor.document(), "/sounds/voice RE.txt", {}, 1.0);
    const auto* source = node(noise.definition, "excitation");
    require(source != nullptr && source->typeId == "talas.noise"
            && hasExpression(*source, "scale", "0.001 * !ExAmp")
            && noise.resources.size() == 1
            && noise.definition.definitionId == "reExample2",
        "without an EX file the resonator should take noise attenuated by 0.001");
    const auto noiseAdded = editor.addSoundDefinition(noise.definition);
    require(noiseAdded.ok(), "the noise RE example should be a valid Sound: " + noiseAdded.message);

    // A GA analysis played by a GAOscillators at its base pitch, TimeIndex
    // ramped over the sound.
    const auto ga = talas::app::gaAnalysisExampleSound(editor.document(), "/sounds/voice GA.wav", 220.0, 1.5);
    const auto* gaRamp = node(ga.definition, "ramp");
    const auto* oscillators = node(ga.definition, "oscillators");
    require(ga.definition.definitionId == "gaExample1" && gaRamp != nullptr
            && hasExpression(*gaRamp, "onDuration", "1.5 s") && oscillators != nullptr
            && oscillators->typeId == "talas.gaOscillators" && hasExpression(*oscillators, "frequency", "220 hz")
            && ga.resources.size() == 1 && ga.resources.front().parameterId == "analysis0",
        "a GA analysis's example should be a GAOscillators at its base pitch, its TimeIndex ramped over the sound");
    const auto gaAdded = editor.addSoundDefinition(ga.definition);
    require(gaAdded.ok(), "the GA example should be a valid Sound: " + gaAdded.message);

    // CDP Process to Sample's sample, played by a Sample.
    const auto cdp = talas::app::cdpProcessExampleSound(editor.document(), "/sounds/voice stretch time.wav");
    const auto* player = node(cdp.definition, "sample");
    require(cdp.definition.definitionId == "cdpExample1" && cdp.definition.displayName == "voice stretch time"
            && player != nullptr && player->typeId == "talas.sample" && cdp.resources.size() == 1
            && cdp.resources.front().parameterId == "sample",
        "a CDP process's example should be a Sample playing the written file");
    const auto cdpAdded = editor.addSoundDefinition(cdp.definition);
    require(cdpAdded.ok(), "the CDP example should be a valid Sound: " + cdpAdded.message);

    // A sample the sample editor saved, played by a Sample.
    const auto edited = talas::app::sampleEditorExampleSound(editor.document(), "/sounds/drawn wave.wav");
    const auto* editedPlayer = node(edited.definition, "sample");
    require(edited.definition.definitionId == "editedSample1" && edited.definition.displayName == "drawn wave"
            && editedPlayer != nullptr && editedPlayer->typeId == "talas.sample" && edited.resources.size() == 1
            && edited.resources.front().file == std::filesystem::path{"/sounds/drawn wave.wav"},
        "a saved sample's Sound should be a Sample playing the file");
    const auto editedAdded = editor.addSoundDefinition(edited.definition);
    require(editedAdded.ok(), "the edited sample's Sound should be valid: " + editedAdded.message);
}

// A stereo sample analyzed as a mono mix, one
// channel, or the left and right into separate files tagged L and R,
// played by a resynthesis of each combined by a ChannelJoin.
void testStereoAnalysis()
{
    using talas::app::StereoAnalysisChoice;
    require(talas::app::stereoAnalysisChoiceLabels().size() == 4, "a mono mix, one channel or both into separate files");
    const std::filesystem::path file{"/loops/SwS 100 Grime s256.talaspartial"};
    require(talas::app::stereoAnalysisFiles(file, StereoAnalysisChoice::MonoMix) == std::vector<std::filesystem::path>{file}
            && talas::app::stereoAnalysisFiles(file, StereoAnalysisChoice::LeftAndRight)
                == std::vector<std::filesystem::path>{"/loops/SwS 100 Grime s256 L.talaspartial",
                    "/loops/SwS 100 Grime s256 R.talaspartial"},
        "the left channel's file is tagged L and the right's R");
    require(talas::app::stereoAnalysisChannels(StereoAnalysisChoice::MonoMix) == std::vector<std::optional<std::size_t>>{std::nullopt}
            && talas::app::stereoAnalysisChannels(StereoAnalysisChoice::Right) == std::vector<std::optional<std::size_t>>{1}
            && talas::app::stereoAnalysisChannels(StereoAnalysisChoice::LeftAndRight).size() == 2,
        "each file analyzes its channel, or the mix");
    const std::vector<std::vector<double>> channels{{0.5, 0.25}, {-0.5, 0.75}};
    require(talas::app::stereoAnalysisSamples(channels, StereoAnalysisChoice::MonoMix)
                == std::vector<std::vector<double>>{{0.0, 0.5}}
            && talas::app::stereoAnalysisSamples(channels, StereoAnalysisChoice::LeftAndRight) == channels,
        "the mix is the two channels' average, each channel its own samples");
    // Until the file is saved, the analysis and its
    // resynthesis are of the left channel only.
    require(talas::app::spectralAnalysisAuditionedChannel(channels) == channels.front()
            && talas::app::spectralAnalysisAuditionedChannel({{0.1}}) == std::vector<double>{0.1},
        "the audition should analyze the left channel only");
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry()};
    const auto stereo = talas::app::stereoSpectralAnalysisExampleSound(
        editor.document(), "/loops/SwS 100 Grime s256 L.talaspartial", "/loops/SwS 100 Grime s256 R.talaspartial");
    const auto* join = node(stereo.definition, "join");
    require(stereo.definition.displayName == "SwS 100 Grime s256 stereo resynthesis" && join != nullptr
            && join->typeId == "talas.channelJoin" && stereo.definition.outputs.size() == 2 && stereo.resources.size() == 2
            && stereo.resources[1].nodeId == "right",
        "the stereo resynthesis joins a resynthesis of each channel");
    const auto added = editor.addSoundDefinition(stereo.definition);
    require(added.ok(), "the stereo example should be a valid Sound: " + added.message);
}

// The steps the tool
// guides through, what each plays and the last page's way back.
void testSpectralAnalysisSteps()
{
    using talas::app::SpectralAnalysisAudition;
    using talas::app::SpectralAnalysisStep;
    talas::app::SpectralAnalysisSteps steps;
    require(steps.step() == SpectralAnalysisStep::Select && !steps.canGoOn() && !steps.next() && !steps.back(),
        "the first step is selecting a sample, and Next waits for one");
    steps.sampleSelected();
    require(steps.hasSample() && steps.audition() == SpectralAnalysisAudition::OriginalSample && steps.canGoOn()
            && talas::app::defaultSpectralAnalysisSilenceSeconds == 1.0,
        "a selected sample is heard repeatedly, a second of silence between, and Next goes on");
    require(steps.next() && steps.step() == SpectralAnalysisStep::Parameters
            && steps.audition() == SpectralAnalysisAudition::Resynthesis
            && talas::app::SpectralAnalysisSteps::auditions(SpectralAnalysisStep::Parameters)
                == std::vector<SpectralAnalysisAudition>{SpectralAnalysisAudition::Resynthesis,
                    SpectralAnalysisAudition::OriginalSample},
        "the parameters' step plays the resynthesis, its radio buttons switching to the original");
    require(steps.chooseAudition(SpectralAnalysisAudition::OriginalSample)
            && !steps.chooseAudition(SpectralAnalysisAudition::FundamentalOnly),
        "only the step's own radio buttons choose what it plays");
    require(steps.next() && steps.step() == SpectralAnalysisStep::Save
            && steps.audition() == SpectralAnalysisAudition::OriginalSample,
        "the spectrum file's step is next, straight spectrum files done there");
    require(steps.next() && steps.step() == SpectralAnalysisStep::Unpitched
            && steps.audition() == SpectralAnalysisAudition::UnpitchedOnly,
        "Set Quasi-Harmonic Parameters goes on to the unpitched fader, which plays what it leaves");
    require(steps.next() && steps.step() == SpectralAnalysisStep::Trace
            && steps.audition() == SpectralAnalysisAudition::FundamentalOnly
            && talas::app::SpectralAnalysisSteps::auditions(SpectralAnalysisStep::Trace).size() == 3,
        "tracing the fundamental plays it as a sine, with Original Sample and All Harmonics beside it");
    require(steps.chooseAudition(SpectralAnalysisAudition::AllHarmonics) && steps.next()
            && steps.step() == SpectralAnalysisStep::Harmonic && !steps.canGoOn() && !steps.next(),
        "the quasi-harmonic file's step is the last");
    require(steps.goBackTo(SpectralAnalysisStep::Unpitched) && steps.step() == SpectralAnalysisStep::Unpitched
            && !steps.goBackTo(SpectralAnalysisStep::Trace),
        "Adjust pitched/unpitched goes back to the fourth page, never forward");
    require(steps.next() && steps.audition() == SpectralAnalysisAudition::AllHarmonics,
        "the trace's step plays what it was left on");
    require(steps.next() && steps.goBackTo(SpectralAnalysisStep::Select) && steps.step() == SpectralAnalysisStep::Select
            && steps.audition() == SpectralAnalysisAudition::OriginalSample,
        "New sample goes back to the first page");
    require(talas::app::SpectralAnalysisSteps::title(SpectralAnalysisStep::Trace) == "Trace the fundamental"
            && talas::app::SpectralAnalysisSteps::title(SpectralAnalysisStep::Save) == "Create the spectrum file",
        "each step is titled, the spectrum file's created on the page after the parameters'");
}

// A harmonic analysis's files and its two
// example Sounds, SumOfSines on its table, one with Morph controls.
void testHarmonicAnalysisExamples()
{
    require(talas::app::analysisOutputPath("/sounds/hi fu mi yo.aif", talas::app::harmonicAnalysisFileSuffix())
                == std::filesystem::path{"/sounds/hi fu mi yo h.talaspartial"}
            && talas::app::harmonicAnalysisTablePath("/sounds/hi fu mi yo h.talaspartial")
                == std::filesystem::path{"/sounds/hi fu mi yo h.txt"},
        "the harmonic file is the sample's name and an h, its table beside it");
    talas::project::ProjectEditor editor{talas::project::ProjectDocument{}, registry()};
    const auto examples = talas::app::harmonicAnalysisExampleSounds(
        editor.document(), "/sounds/hi fu mi yo h.txt", 120, 40, 261.6255653, 2.5);
    require(examples.size() == 2, "two example Sounds, one with Morph controls and one without");
    const auto& plain = examples[0];
    const auto& morph = examples[1];
    const auto* sines = node(plain.definition, "sines");
    require(plain.definition.definitionId == "harmonicExample1" && plain.definition.displayName == "hi fu mi yo h"
            && sines != nullptr && sines->typeId == "talas.sumOfSines" && hasExpression(*sines, "onDuration", "2.5 s")
            && hasExpression(*sines, "analysisFrequency0", "261.6255653 hz")
            && hasExpression(*sines, "analysisFrames0", "120") && hasExpression(*sines, "nbrPartials", "40")
            && plain.resources.size() == 1 && plain.resources.front().parameterId == "analysis0",
        "the plain example plays the table's frames over the sample's duration at the analysis's pitch");
    const auto* morphing = node(morph.definition, "sines");
    require(morph.definition.definitionId == "harmonicExample2" && morph.definition.displayName == "hi fu mi yo h morph"
            && morphing != nullptr && hasExpression(*morphing, "onDuration", "2.5 s + (!Gate * 100 s)")
            && hasExpression(*morphing, "dbMorph", "!Morph") && hasExpression(*morphing, "pchMorph", "!Morph")
            && morph.resources.size() == 2 && morph.resources[1].parameterId == "analysis1",
        "the Gate button nearly freezes time and Morph cross-fades toward Analysis1");
    for (const auto& example : examples)
    {
        const auto added = editor.addSoundDefinition(example.definition);
        require(added.ok(), "a harmonic example should be a valid Sound: " + added.message);
    }
}
}

// Ctrl and a box drawn in a spectrum view
// magnify the box's times and frequencies to fill the view; Return shows the
// whole spectrum again; a box of no size is ignored.
void testZoomingASpectrumView()
{
    const talas::app::SpectrumViewWindow whole{0.0, 4.0, 0.0, 2000.0};
    const auto zoomed = talas::app::spectrumViewZoomedTo(whole, 0.25, 0.5, 0.5, 1.0);
    require(zoomed == std::optional<talas::app::SpectrumViewWindow>{talas::app::SpectrumViewWindow{1.0, 2.0, 0.0, 1000.0}},
        "the box's times and frequencies should fill the view, its top the highest frequency");
    const auto again = talas::app::spectrumViewZoomedTo(*zoomed, 0.0, 0.0, 0.5, 0.5);
    require(again.has_value() && again->lowTime == 1.0 && again->highTime == 1.5 && again->lowHz == 500.0 && again->highHz == 1000.0,
        "zooming again should magnify the part shown");
    require(!talas::app::spectrumViewZoomedTo(whole, 0.3, 0.3, 0.3, 0.6).has_value(),
        "a box of no width should be ignored, the view as it was (Return shows it all again)");
    // Control or Command held, the cursor is a magnifying
    // glass whose plus marks the corner the box starts from.
    require(talas::app::spectrumViewCursor(true) == talas::app::SpectrumViewCursor::Magnifier
            && talas::app::spectrumViewCursor(false) == talas::app::SpectrumViewCursor::Arrow
            && talas::app::spectrumViewMagnifierHotSpot == std::pair{9, 9},
        "holding Control or Command should give the magnifying glass, its plus the box's point");
    require(talas::app::spectrumTraceHandles(true) == talas::app::SpectrumTraceHandles::Squares
            && talas::app::spectrumTraceHandles(false) == talas::app::SpectrumTraceHandles::None,
        "the selected display's white line should show its square endpoints");
    // Return shows it all, lowercase L toggles connected
    // frames, Delete removes a selected point.
    using talas::app::TraceViewAction;
    using talas::app::TraceViewKey;
    require(talas::app::traceViewAction(TraceViewKey::LowercaseL, false) == TraceViewAction::ToggleConnectedFrames
            && talas::app::traceViewAction(TraceViewKey::Return, true) == TraceViewAction::ShowWhole
            && talas::app::traceViewAction(TraceViewKey::Delete, true) == TraceViewAction::DeletePoint
            && talas::app::traceViewAction(TraceViewKey::Delete, false) == TraceViewAction::None
            && talas::app::traceViewAction(TraceViewKey::Other, true) == TraceViewAction::None,
        "the trace view's keys");
}

// The sample chosen is
// heard, looped in Spectral Analysis, once in RE and GA Analysis.
void testWhatTheToolsPlay()
{
    // RE Analysis opens a window of several parameters, the
    // filter order, updates and averaging time, at the analysis's defaults.
    const auto parameters = talas::app::reAnalysisWindowParameters();
    const talas::dsp::ReAnalysisSettings defaults;
    require(parameters.size() == 3 && parameters[0].name == "Filter order"
            && parameters[0].defaultValue == static_cast<double>(defaults.order)
            && parameters[1].defaultValue == defaults.updatesPerSecond
            && parameters[2].name == "Averaging time" && parameters[2].unit == "ms"
            && parameters[2].defaultValue == defaults.averagingTimeSeconds * 1000.0
            && std::ranges::all_of(parameters, [](const auto& parameter) {
                   return parameter.minimum <= parameter.defaultValue && parameter.defaultValue <= parameter.maximum;
               }),
        "the RE Analysis window should hold its parameters at the analysis's defaults");
    using talas::app::AnalysisToolKind;
    require(talas::app::analysisToolPreviewLoops(AnalysisToolKind::Spectral) && !talas::app::analysisToolPreviewLoops(AnalysisToolKind::Re)
            && !talas::app::analysisToolPreviewLoops(AnalysisToolKind::Ga),
        "RE and GA Analysis should play the sample chosen once, Spectral Analysis loop it");
    // One oscillator a track.
    talas::project::PartialTrackAsset analysis;
    analysis.frames = {{1, 0, {{1, 0.5, 220.0, 0.0}, {2, 0.25, 440.0, 0.0}}}, {1, 480, {{2, 0.2, 440.0, 0.0}, {5, 0.1, 1100.0, 0.0}}}};
    require(talas::app::resynthesisOscillatorCount(analysis) == 3 && talas::app::resynthesisOscillatorCount({}) == 0,
        "the resynthesis should use an oscillator for each track");
    // More oscillators at once than the processor plays are
    // warned of.
    talas::project::PartialTrackAsset dense;
    dense.frames = {{1, 0, {}}};
    for (std::uint64_t track = 1; track <= 129; ++track)
        dense.frames.front().points.push_back({track, 0.01, 100.0 * static_cast<double>(track), 0.0});
    const auto warning = talas::app::resynthesisCapacityWarning(dense);
    require(!talas::app::resynthesisCapacityWarning(analysis).has_value() && warning.has_value() && warning->starts_with("129 oscillators"),
        "a warning should come when the resynthesis needs more oscillators than the processor has");
}

// The Design Alternate Tunings Tool's main page holds
// the tonic, the instrument and a button for each kind of tuning; a kind's
// page its fields, the octave and Create Example, Load, Save and Back, which
// takes it back to the main page.
void testTheTuningToolsPages()
{
    using talas::app::TuningToolPart;
    using talas::project::tunings::Method;
    const std::optional<Method> main;
    require(talas::app::tuningToolPageShows(main, TuningToolPart::Tonic) && talas::app::tuningToolPageShows(main, TuningToolPart::Instrument)
            && talas::app::tuningToolPageShows(main, TuningToolPart::KindButtons)
            && !talas::app::tuningToolPageShows(main, TuningToolPart::KindFields),
        "the main page should hold the global settings and the buttons for the kinds of tuning");
    for (const auto kind : {Method::RatioScale, Method::CentsScale, Method::EqualTempered, Method::TwoInterval, Method::FromTextFile})
    {
        require(talas::app::tuningToolPageShows(kind, TuningToolPart::KindFields) && talas::app::tuningToolPageShows(kind, TuningToolPart::Octave)
                && talas::app::tuningToolPageShows(kind, TuningToolPart::PageButtons)
                && !talas::app::tuningToolPageShows(kind, TuningToolPart::KindButtons),
            "a kind's page should hold its fields, the octave, and Create Example, Load, Save and Back");
    }
}

int main()
{
    try
    {
        testSpectralAnalysisSettings();
        testSpectrumFilesHaveTheirOwnExtension();
        testWhatTheToolsPlay();
        testExampleSounds();
        testHarmonicAnalysisExamples();
        testZoomingASpectrumView();
        testSpectralAnalysisSteps();
        testStereoAnalysis();
        testTheTuningToolsPages();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
