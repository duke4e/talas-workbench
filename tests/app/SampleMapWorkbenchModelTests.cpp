#include "talas/app/SampleMapWorkbenchModel.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireContains(
    std::string_view text,
    std::string_view fragment,
    const std::string& message)
{
    require(text.find(fragment) != std::string_view::npos, message);
}

talas::project::ProjectSampleRegion sampleRegion(
    std::string id,
    std::string assetId,
    std::uint8_t lowNote,
    std::uint8_t rootNote,
    std::uint8_t highNote,
    std::uint8_t lowVelocity,
    std::uint8_t highVelocity)
{
    talas::project::ProjectSampleRegion result;
    result.regionId = std::move(id);
    result.assetId = std::move(assetId);
    result.lowNote = lowNote;
    result.rootNote = rootNote;
    result.highNote = highNote;
    result.lowVelocity = lowVelocity;
    result.highVelocity = highVelocity;
    return result;
}

talas::project::ProjectDocument sampleDocument()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectAsset piano;
    piano.id = "piano";
    piano.kind = talas::project::ProjectAssetKind::Audio;
    piano.path = "Samples/Piano C4.wav";
    piano.frameCount = 48000;
    document.assets.push_back(piano);

    auto soft = sampleRegion("soft", "piano", 48, 60, 72, 1, 80);
    soft.sustainLoop = talas::project::ProjectSampleLoop{
        talas::project::ProjectSampleLoopMode::Alternate,
        100,
        1000,
    };
    auto loud = sampleRegion("loud", "piano", 48, 60, 72, 70, 127);
    loud.reverse = true;

    document.sampleMapLibrary.maps = {
        {"keys", "Layered Keys", {soft, loud}},
        {"empty", "Empty Map", {}},
    };
    return document;
}

void testProjectionUsesStableMapRegionAndAssetIdentity()
{
    const auto document = sampleDocument();
    talas::project::WorkspaceSamplesSessionState session;
    session.activeMapId = "keys";
    session.selectedRegionIds = {"loud", "soft", "stale"};

    const auto model = talas::app::projectSampleMapWorkbench(
        document,
        session,
        {},
        {1280.0, 1270.0});
    require(
        model.maps.size() == 2
            && model.maps[0].mapId == "keys"
            && model.maps[0].active
            && model.maps[0].regionCount == 2,
        "library projection should preserve portable map order and stable identity");
    require(
        model.regions.size() == 2
            && model.regions[0].regionId == "soft"
            && model.regions[0].layerIndex == 0
            && model.regions[0].selected
            && !model.regions[0].primary
            && model.regions[1].regionId == "loud"
            && model.regions[1].primary,
        "region projection should preserve layer order while deriving local selection and primary state");
    requireContains(
        model.regions[0].accessibleDescription,
        "Piano C4.wav",
        "accessible region name should identify the asset");
    requireContains(
        model.regions[0].accessibleDescription,
        "notes 48-72",
        "accessible region name should contain the note range");
    requireContains(
        model.regions[0].accessibleDescription,
        "velocities 1-80",
        "accessible region name should contain the velocity range");
    requireContains(
        model.regions[0].accessibleDescription,
        "root 60; forward; sustain alternate loop",
        "accessible region name should contain root, direction, and loop state");
    requireContains(
        model.regions[1].accessibleDescription,
        "reverse; no sustain loop; no release loop",
        "accessible region name should announce absent loops without relying on color");
}

void testMidiSelectionReturnsEveryOverlappingLayerInProjectOrder()
{
    const auto document = sampleDocument();
    const auto& map = document.sampleMapLibrary.maps.front();
    require(
        talas::app::sampleMapRegionIdsAtMidi(map, 60, 75)
            == std::vector<std::string>({"soft", "loud"}),
        "select-by-MIDI should include every overlapping layer in stable project order");
    require(
        talas::app::sampleMapRegionIdsAtMidi(map, 60, 100)
            == std::vector<std::string>({"loud"}),
        "select-by-MIDI should respect inclusive velocity boundaries");
    require(
        talas::app::sampleMapRegionIdsAtMidi(map, 60, 0).empty(),
        "velocity zero remains note-off and must not select a region");
}

void testSessionViewportProjectionUsesNormalizedLocalState()
{
    talas::project::WorkspaceSamplesSessionState session;
    session.mapViewport = {36.0, 48.0, 32.0, 64.0};
    session.waveformViewport = {0.125, 0.25};

    require(
        talas::app::sampleMapViewportFromSession(session)
            == talas::app::SampleMapViewport{36.0, 84.0, 32.0, 96.0},
        "map viewport should project local origin/span state into exact interaction boundaries");
    require(
        talas::app::waveformViewportFromSession(session, 48000)
            == talas::app::WaveformViewport{6000.0, 18000.0},
        "normalized waveform state should project onto the selected immutable generation without persisting asset frames");
}

void testAutoMapPreviewIsDeterministicAndDoesNotMutateTheDocument()
{
    const auto document = sampleDocument();
    talas::project::ProjectSampleMapAutoMapRequest request;
    request.lowNote = 48;
    request.highNote = 72;
    request.anchorRootNote = 60;
    request.candidates = {
        {"zulu", "asset-z", "Zulu.wav", "/samples/Zulu.wav"},
        {"alpha", "asset-a", "alpha.wav", "/samples/alpha.wav"},
    };

    const auto preview = talas::app::makeSampleMapAutoMapPreview(
        request,
        {},
        {1280.0, 1270.0});
    require(preview.ok(), "valid auto-map preview should project without editing");
    require(
        preview.regions.size() == 2
            && preview.regions[0].region.regionId == "alpha"
            && preview.regions[0].region.rootNote == 60
            && preview.regions[1].region.regionId == "zulu"
            && preview.regions[1].region.rootNote == 61,
        "auto-map preview should reuse the canonical deterministic candidate ordering");
    require(
        document.sampleMapLibrary.maps.front().regions.size() == 2,
        "preview generation must not become a second project mutation authority");

    std::ranges::reverse(request.candidates);
    const auto reordered = talas::app::makeSampleMapAutoMapPreview(
        request,
        {},
        {1280.0, 1270.0});
    require(
        reordered.ok()
            && reordered.regions[0].region == preview.regions[0].region
            && reordered.regions[1].region == preview.regions[1].region,
        "preview output should be invariant to import completion order");
}
}

int main()
{
    testProjectionUsesStableMapRegionAndAssetIdentity();
    testMidiSelectionReturnsEveryOverlappingLayerInProjectOrder();
    testSessionViewportProjectionUsesNormalizedLocalState();
    testAutoMapPreviewIsDeterministicAndDoesNotMutateTheDocument();
    return 0;
}
