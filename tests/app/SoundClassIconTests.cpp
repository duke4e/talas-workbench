#include "talas/app/SoundClassIcon.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

using Dot = talas::app::SoundIconDot;

// A click on a clear dot
// draws a dot, on a set one erases it, and holding the button down while
// moving draws or erases continuously; each class has a large and a small
// icon.
void testDrawingTheIcon()
{
    talas::project::ProjectSoundIcon icon;
    require(talas::app::soundIconWidth(true) == 64 && talas::app::soundIconHeight(true) == 32
            && talas::app::soundIconWidth(false) == 32 && talas::app::soundIconHeight(false) == 16,
        "two icons for each class, a large and a small one");
    talas::app::drawSoundIconStroke(icon, true, {{3, 4}}, !talas::app::soundIconDot(icon, true, {3, 4}));
    require(talas::app::soundIconDot(icon, true, {3, 4}) && icon.large.size() == 32 && icon.small.empty(),
        "a click in a white area draws a black dot");
    talas::app::drawSoundIconStroke(icon, true, {{3, 4}}, !talas::app::soundIconDot(icon, true, {3, 4}));
    require(!talas::app::soundIconDot(icon, true, {3, 4}) && icon.large.empty(), "a click in a black area erases");
    const auto stroke = talas::app::soundIconStroke({0, 0}, {5, 2});
    require(stroke.front() == Dot{0, 0} && stroke.back() == Dot{5, 2} && stroke.size() == 6,
        "a stroke passes every dot between the button's positions");
    talas::app::drawSoundIconStroke(icon, false, stroke, true);
    require(std::ranges::all_of(stroke, [&](Dot dot) { return talas::app::soundIconDot(icon, false, dot); })
            && icon.large.empty() && icon.small.size() == 16,
        "holding the button down draws continuously, here on the small icon");
    talas::app::drawSoundIconStroke(icon, false, talas::app::soundIconStroke({0, 0}, {1, 0}), false);
    require(!talas::app::soundIconDot(icon, false, {1, 0}) && talas::app::soundIconDot(icon, false, {5, 2}),
        "a stroke begun on a black dot erases continuously");
    require(!talas::app::soundIconDot(icon, true, {64, 0}), "outside the icon there are no dots");
    // The small icon shows with small icons chosen under
    // View, the large one by default.
    talas::project::ProjectSoundIcon both;
    talas::app::setSoundIconDot(both, true, {1, 1}, true);
    talas::app::setSoundIconDot(both, false, {1, 1}, true);
    talas::project::ProjectSoundIcon largeOnly;
    talas::app::setSoundIconDot(largeOnly, true, {1, 1}, true);
    require(talas::app::soundIconDrawsSmall(both, false) && !talas::app::soundIconDrawsSmall(both, true)
            && !talas::app::soundIconDrawsSmall(largeOnly, false),
        "small icons should show the class's small icon, else its large one");
}

// Another Sound dragged into the icon editor makes
// the class's icons that Sound's; the PNG file is the class's name in
// lowercase with .png, in a frequently used folder.
void testOtherIconsAndPngFiles()
{
    const auto copied = talas::app::soundIconSameAs("talas.absoluteValue");
    require(copied.sameAs == "talas.absoluteValue" && copied.large.empty() && copied.small.empty(),
        "dragging a Sound in sets the icons to that Sound's");
    require(talas::app::soundIconPngFileName("StereoLocator") == "stereolocator.png",
        "the PNG file is the class's name in lowercase letters followed by .png");
    const auto folder = std::filesystem::temp_directory_path() / "talas-sound-icon";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder / "second");
    {
        std::ofstream{folder / "second" / "stereolocator.png"} << "png";
    }
    require(talas::app::findSoundIconPng("StereoLocator", {folder, folder / "second"}) == folder / "second" / "stereolocator.png"
            && !talas::app::findSoundIconPng("Other", {folder, folder / "second"}).has_value(),
        "the icon file is found in the frequently used folders");
    std::filesystem::remove_all(folder);
}

// The Class Editor's input and output types
// shape the outline, the edges the class's ports give where it chose none;
// the Nonaudio side effects box gives it a pointed top. They change only
// the icon, not the signal.
void testInputAndOutputTypesShapeTheIcon()
{
    using talas::app::IconEdge;
    using talas::app::SoundIconShape;
    const std::array<std::pair<std::string_view, IconEdge>, 8> table{{{"none", IconEdge::None}, {"audio", IconEdge::Audio},
        {"subaudio", IconEdge::Subaudio}, {"spectrum", IconEdge::Spectrum}, {"transparent", IconEdge::Transparent},
        {"constructor", IconEdge::Constructor}, {"variable", IconEdge::Variable}, {"environment", IconEdge::Environment}}};
    for (const auto& [name, edge] : table)
    {
        require(talas::app::soundIconEdgeOfType(name) == edge && talas::app::soundIconTypeOfEdge(edge) == name,
            "the types are None, Audio, Subaudio, Spectrum, Transparent, Constructor, Variable and Environment");
    }
    require(!talas::app::soundIconEdgeOfType("stereo").has_value(), "only the table's types");
    talas::project::ProjectSoundIcon icon;
    const SoundIconShape ports{IconEdge::None, IconEdge::Audio, false};
    require(talas::app::soundClassIconShape(ports, icon) == ports, "without types the ports give the outline");
    icon.inputType = "audio";
    icon.outputType = "audio";
    require(talas::app::soundClassIconShape(ports, icon) == SoundIconShape{IconEdge::Audio, IconEdge::Audio, false},
        "Input and Output Types both Audio shape both edges");
    icon.outputType.clear();
    icon.inputType = "environment";
    icon.sideEffects = true;
    require(talas::app::soundClassIconShape(ports, icon) == SoundIconShape{IconEdge::Environment, IconEdge::Audio, true},
        "a type set on one edge only, and the Nonaudio side effects box pointing the top");
}

// The number of channels a class chose is the
// number of lines drawn into and out of its Sounds: the output channels of
// the Sound a line comes from, else the input channels of the one it goes to.
void testChannelsAreTheLinesInAndOut()
{
    talas::project::ProjectSoundIcon mono;
    mono.inputChannels = 1;
    mono.outputChannels = 1;
    talas::project::ProjectSoundIcon stereo;
    stereo.inputChannels = 2;
    stereo.outputChannels = 2;
    talas::project::ProjectSoundIcon typed;
    typed.inputType = "audio";
    require(talas::app::soundClassConnectionLines(&mono, &stereo) == std::optional<unsigned>{1U}
            && talas::app::soundClassConnectionLines(&stereo, &mono) == std::optional<unsigned>{2U},
        "a mono class's output is one line, two independent outputs two lines");
    require(talas::app::soundClassConnectionLines(nullptr, &stereo) == std::optional<unsigned>{2U}
            && talas::app::soundClassConnectionLines(&typed, &mono) == std::optional<unsigned>{1U},
        "a line into a class's Sound is drawn as its input channels");
    require(!talas::app::soundClassConnectionLines(nullptr, nullptr).has_value()
            && !talas::app::soundClassConnectionLines(&typed, &typed).has_value(),
        "lines between Sounds of no chosen channels stay as they are");
}

// Format 139: the icon is saved with the class and read back; an older
// format refuses it, and an icon naming no Sound is refused.
void testTheIconIsSavedWithTheClass()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectSoundDefinition stereo;
    stereo.definitionId = "stereo";
    stereo.displayName = "Stereo";
    stereo.graph.nodes.push_back({"tone", "talas.oscillator", {}});
    stereo.outputs.push_back({"audioOut", talas::core::PortType::Audio, "tone", "audioOut"});
    talas::project::ProjectSoundIcon icon;
    talas::app::drawSoundIconStroke(icon, true, talas::app::soundIconStroke({0, 0}, {63, 31}), true);
    talas::app::setSoundIconDot(icon, false, {31, 15}, true);
    icon.png = true;
    icon.inputType = "audio";
    icon.sideEffects = true;
    stereo.icon = icon;
    document.soundDefinitions.push_back(stereo);
    talas::project::reconcileProjectPresentation(document);
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    require(talas::project::validateProjectDocument(document, registry).ok(), "a drawn icon should be valid");
    const auto saved = talas::project::serializeProjectDocument(document);
    const auto reopened = talas::project::parseProjectDocument(saved);
    require(reopened.ok() && reopened.document.soundDefinitions.front().icon == icon, "format 139: the icon should be saved");
    auto older = saved;
    older.replace(older.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 138");
    require(!talas::project::parseProjectDocument(older).ok(), "format 138 should refuse a class's icon");
    auto beforeTypes = saved;
    beforeTypes.replace(beforeTypes.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 140");
    require(!talas::project::parseProjectDocument(beforeTypes).ok(), "format 140 should refuse a class's input type");
    // Format 143: the numbers of input and output channels.
    auto channelled = document;
    channelled.soundDefinitions.front().icon->inputChannels = 1;
    channelled.soundDefinitions.front().icon->outputChannels = 2;
    const auto savedChannels = talas::project::serializeProjectDocument(channelled);
    const auto reopenedChannels = talas::project::parseProjectDocument(savedChannels);
    require(reopenedChannels.ok() && reopenedChannels.document.soundDefinitions.front().icon->inputChannels == 1
            && reopenedChannels.document.soundDefinitions.front().icon->outputChannels == 2,
        "format 143: the class's numbers of channels should be saved");
    auto beforeChannels = savedChannels;
    beforeChannels.replace(beforeChannels.find("\"version\": 156"), std::string_view{"\"version\": 156"}.size(), "\"version\": 142");
    auto threeChannels = savedChannels;
    threeChannels.replace(threeChannels.find("\"outputChannels\": 2"), std::string_view{"\"outputChannels\": 2"}.size(),
        "\"outputChannels\": 3");
    require(!talas::project::parseProjectDocument(beforeChannels).ok() && !talas::project::parseProjectDocument(threeChannels).ok(),
        "format 142 should refuse a class's channels, and a class has 1 or 2");
    document.soundDefinitions.front().icon = talas::app::soundIconSameAs("talas.noSuchSound");
    require(!talas::project::validateProjectDocument(document, registry).ok(), "an icon of an unknown Sound should be refused");
    document.soundDefinitions.front().icon = talas::app::soundIconSameAs("talas.absoluteValue");
    require(talas::project::validateProjectDocument(document, registry).ok(), "an icon of a prototype should be valid");
}
}

// The size of a PNG icon is continuously variable:
// any picture is scaled to its place, keeping its proportions, centred.
void testPngIconsTakeAnySize()
{
    const talas::app::SoundIconPlacement place{10.0, 20.0, 64.0, 32.0};
    require(talas::app::soundIconPngPlacement(216.0, 216.0, place) == talas::app::SoundIconPlacement{26.0, 20.0, 32.0, 32.0}
            && talas::app::soundIconPngPlacement(300.0, 75.0, place) == talas::app::SoundIconPlacement{10.0, 28.0, 64.0, 16.0}
            && talas::app::soundIconPngPlacement(16.0, 8.0, place) == talas::app::SoundIconPlacement{10.0, 20.0, 64.0, 32.0},
        "a PNG icon of any size should be scaled to its place, its proportions kept");
}

// An icon's picture is cached, so a change to its
// file is not seen at once; Recycle flushes the cache and the file is read
// again.
void testIconPicturesAreCachedUntilRecycled()
{
    talas::app::SoundIconPictureCache<int> pictures;
    int version = 1;
    const auto load = [&](const std::filesystem::path&) { return version; };
    const std::filesystem::path file{"stereolocator.png"};
    require(pictures.get(file, load) == 1, "the picture should be read the first time");
    version = 2;
    require(pictures.get(file, load) == 1 && pictures.size() == 1,
        "a cached picture should not show a change to its file at once");
    pictures.recycle();
    require(pictures.size() == 0 && pictures.get(file, load) == 2,
        "Recycle should flush the cache, the file read again");
}

int main()
{
    try
    {
        testDrawingTheIcon();
        testOtherIconsAndPngFiles();
        testPngIconsTakeAnySize();
        testIconPicturesAreCachedUntilRecycled();
        testInputAndOutputTypesShapeTheIcon();
        testChannelsAreTheLinesInAndOut();
        testTheIconIsSavedWithTheClass();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
