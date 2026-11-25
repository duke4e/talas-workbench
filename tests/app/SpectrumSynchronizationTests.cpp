#include "talas/app/SpectrumSynchronization.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The
// Synchronizing Spectra tool's markers.
namespace
{
using talas::app::SpectrumMarker;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Twenty frames of 10 ms: a tone for the first ten, then a louder other one.
talas::project::PartialTrackAsset twoEvents()
{
    std::vector<talas::project::PartialTrackFrame> frames;
    for (std::uint64_t index = 0; index < 20; ++index)
    {
        if (index < 10)
            frames.push_back({1, index * 480, {{1, 0.2, 220.0, 0.0}}});
        else
            frames.push_back({1, index * 480, {{2, 0.8, 330.0, 0.0}}});
    }
    return {48000.0, 9600, std::move(frames)};
}

// The a key: markers at the ends and where the spectrum changes.
void testTalasSuggestsMarkers()
{
    const auto spectrum = twoEvents();
    const auto markers = talas::app::autoSpectrumMarkers(spectrum);
    require(markers == std::vector<SpectrumMarker>{{"m0", 0}, {"m10", 10}, {"m19", 19}},
        "markers at the beginning, the end and the change between the two events: " + std::to_string(markers.size()));
    require(talas::app::spectrumFrameSeconds(spectrum, 10) == 0.1 && talas::app::spectrumFrameSeconds(spectrum, 20) == 0.2,
        "a frame's time, the spectrum's end after its last");
    require(talas::app::spectrumTimePoints(spectrum, markers) == "{0 s} {0.1 s} {0.19 s}",
        "the markers' times as WarpedTimeIndex's time points");
    require(talas::app::autoSpectrumMarkers({48000.0, 0, {}}).empty(), "an empty spectrum has no markers");
}

// m adds a marker at the scrub bar's frame, Enter names it, Delete removes
// it; the colours run red, green, blue.
void testEditingMarkers()
{
    auto markers = talas::app::spectrumMarkersWith({}, 5);
    markers = talas::app::spectrumMarkersWith(*markers, 2, "start");
    require(markers && *markers == std::vector<SpectrumMarker>{{"start", 2}, {"m5", 5}}, "markers in the order of their frames");
    require(!talas::app::spectrumMarkersWith(*markers, 5), "one marker to a frame");
    // M at the left edge marks the file's very beginning.
    require(talas::app::spectrumMarkersWith({}, 0) == std::optional<std::vector<SpectrumMarker>>{std::vector<SpectrumMarker>{{"m0", 0}}},
        "a marker at the very beginning should be the first frame's");
    const auto renamed = talas::app::spectrumMarkersRenamed(*markers, 1, "hi");
    require(renamed && (*renamed)[1].name == "hi" && !talas::app::spectrumMarkersRenamed(*markers, 1, ""),
        "a selected marker is named with Enter");
    const auto deleted = talas::app::spectrumMarkersWithout(*renamed, 0);
    require(deleted && deleted->size() == 1 && !talas::app::spectrumMarkersWithout(*deleted, 3), "Delete removes the selected marker");
    require(talas::app::spectrumMarkerColour(0) == talas::app::SpectrumMarkerColour::Red
            && talas::app::spectrumMarkerColour(1) == talas::app::SpectrumMarkerColour::Green
            && talas::app::spectrumMarkerColour(2) == talas::app::SpectrumMarkerColour::Blue
            && talas::app::spectrumMarkerColour(3) == talas::app::SpectrumMarkerColour::Red,
        "the markers are coloured red, green, blue and red again");
    require(talas::app::spectrumMarkerColour(1, true) == talas::app::SpectrumMarkerColour::White,
        "the selected marker is shown in white");
    // Dragging the scrub bar plays the spectrum there, a
    // tenth of a second of its resynthesis from the frame's time.
    talas::project::PartialTrackAsset spectrum{48000.0, 9600, {{1, 0, {}}, {1, 4800, {}}, {1, 9000, {}}}};
    require(talas::app::spectrumScrubSlice(spectrum, 1, 9600) == std::pair<std::size_t, std::size_t>{4800, 9600}
            && talas::app::spectrumScrubSlice(spectrum, 2, 9600) == std::pair<std::size_t, std::size_t>{9000, 9600}
            && talas::app::spectrumScrubSlice(spectrum, 0, 9600, 0.05) == std::pair<std::size_t, std::size_t>{0, 2400},
        "dragging the yellow scrub bar should play the spectrum from there");
    require(!talas::app::spectrumMarkersCorrespond({{"a", 0}}, {{"b", 0}})
            && !talas::app::spectrumMarkersCorrespond({{"a", 0}, {"b", 4}}, {{"c", 0}})
            && talas::app::spectrumMarkersCorrespond({{"a", 0}, {"b", 4}}, {{"c", 0}, {"d", 9}}),
        "Mix and Morph need the same number of markers in both spectra");
}

// Save Markers keeps them with the spectrum file for the next time it opens.
void testMarkersAreSavedWithTheSpectrum()
{
    const auto folder = std::filesystem::temp_directory_path()
        / ("talas-spectrum-markers-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto file = folder / "hi fu mi yo h.talaspartial";
    std::string error;
    require(!talas::app::readSpectrumMarkers(file, error).has_value() && error.empty(), "a spectrum saved without markers has none");
    const std::vector<SpectrumMarker> markers{{"m0", 0}, {"hi fu", 12}, {"end", 40}};
    require(talas::app::writeSpectrumMarkers(file, markers, error)
            && talas::app::spectrumMarkersPath(file) == folder / "hi fu mi yo h.talaspartial.markers",
        "the markers are saved beside the spectrum file: " + error);
    const auto read = talas::app::readSpectrumMarkers(file, error);
    require(read && *read == markers, "the markers are read back with their names: " + error);
    std::filesystem::remove_all(folder);
}

// A spectrum file in a field that reads
// a table plays through the table written beside it.
void testSpectrumFilesInTableFields()
{
    using talas::app::SpectrumTableUse;
    require(talas::app::spectrumTableUse("talas.spectrumInRam", "analysis") == SpectrumTableUse::Triples
            && talas::app::spectrumTableUse("talas.spectrumOnDisk", "fileName") == SpectrumTableUse::Triples
            && talas::app::spectrumTableUse("talas.sumOfSines", "analysis1") == SpectrumTableUse::HarmonicAmplitudes
            && talas::app::spectrumTableUse("talas.sample", "sample") == SpectrumTableUse::None,
        "SpectrumInRAM and SpectrumOnDisk take triples, SumOfSines harmonic amplitudes");
    require(talas::app::spectrumTablePath("/voices/hi fu mi yo h.talaspartial") == std::filesystem::path{"/voices/hi fu mi yo h table.txt"},
        "the table is written beside the spectrum file");
    const auto triples = talas::app::spectrumTableFields("analysis", SpectrumTableUse::Triples, 120);
    const auto amplitudes = talas::app::spectrumTableFields("analysis1", SpectrumTableUse::HarmonicAmplitudes, 80);
    require(triples.size() == 2 && triples[0].parameterId == "analysisFrames" && triples[0].expression == std::optional<std::string>{"120"}
            && triples[1].expression == std::optional<std::string>{"1 hz"} && amplitudes.size() == 1
            && amplitudes[0].parameterId == "analysisFrames1",
        "the table's frames, and the AnalysisFrequency its triples are in hertz for");
}
}

// The keys act in the active
// view; Tab makes the other active, Space auditions the active spectrum, the
// arrows scrub, m and a add and suggest markers, s saves them, Delete and
// Return remove and name the selected marker, Return with none showing the
// whole spectrum; those needing a spectrum or a marker do nothing without.
void testTheKeysActInTheActiveView()
{
    using talas::app::SpectrumSyncAction;
    using talas::app::SpectrumSyncKey;
    const auto action = [](SpectrumSyncKey key, bool loaded, bool selected) {
        return talas::app::spectrumSyncAction(key, loaded, selected);
    };
    require(action(SpectrumSyncKey::Tab, false, false) == SpectrumSyncAction::SwitchView
            && action(SpectrumSyncKey::Tab, true, true) == SpectrumSyncAction::SwitchView,
        "Tab should make the other view active");
    require(action(SpectrumSyncKey::Space, true, false) == SpectrumSyncAction::Audition
            && action(SpectrumSyncKey::Space, false, false) == SpectrumSyncAction::None,
        "Space should audition the active view's spectrum");
    require(action(SpectrumSyncKey::Left, true, false) == SpectrumSyncAction::ScrubBack
            && action(SpectrumSyncKey::Right, true, false) == SpectrumSyncAction::ScrubOn
            && action(SpectrumSyncKey::Right, false, false) == SpectrumSyncAction::None,
        "the arrow keys should move the active scrub bar");
    require(action(SpectrumSyncKey::M, true, false) == SpectrumSyncAction::AddMarker
            && action(SpectrumSyncKey::A, true, false) == SpectrumSyncAction::SuggestMarkers
            && action(SpectrumSyncKey::M, false, false) == SpectrumSyncAction::None
            && action(SpectrumSyncKey::S, false, false) == SpectrumSyncAction::SaveMarkers,
        "m, a and s should add, suggest and save the active view's markers");
    require(action(SpectrumSyncKey::Delete, true, true) == SpectrumSyncAction::DeleteMarker
            && action(SpectrumSyncKey::Delete, true, false) == SpectrumSyncAction::None
            && action(SpectrumSyncKey::Return, true, true) == SpectrumSyncAction::NameMarker
            && action(SpectrumSyncKey::Return, true, false) == SpectrumSyncAction::ShowWhole
            && action(SpectrumSyncKey::Other, true, true) == SpectrumSyncAction::None,
        "Delete and Return should act on the selected marker, Return with none showing the whole spectrum");
}

// The tool opens on its instructions and Continue, which
// starts it, the spectra shown in their place.
void testTheInstructionsComeFirst()
{
    using Part = talas::app::SynchronizingSpectraPart;
    require(talas::app::synchronizingSpectraShows(false, Part::Instructions)
            && talas::app::synchronizingSpectraShows(false, Part::ContinueButton)
            && !talas::app::synchronizingSpectraShows(false, Part::Spectra),
        "the tool should open on its instructions and a Continue button");
    require(!talas::app::synchronizingSpectraShows(true, Part::Instructions)
            && !talas::app::synchronizingSpectraShows(true, Part::ContinueButton)
            && talas::app::synchronizingSpectraShows(true, Part::Spectra),
        "Continue should start the tool, the spectra in the instructions' place");
}

int main()
{
    try
    {
        testTalasSuggestsMarkers();
        testTheInstructionsComeFirst();
        testEditingMarkers();
        testMarkersAreSavedWithTheSpectrum();
        testSpectrumFilesInTableFields();
        testTheKeysActInTheActiveView();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
