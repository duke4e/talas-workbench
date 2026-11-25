#pragma once

#include "talas/app/SoundIcons.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// A Sound class's icon as the Class Editor draws it: dots set and cleared on
// the large or small icon, a stroke drawing or erasing continuously, the icon
// of another Sound dragged in, and the PNG file named for the class.
namespace talas::app
{
struct SoundIconDot
{
    std::size_t x = 0;
    std::size_t y = 0;

    bool operator==(const SoundIconDot&) const = default;
};

[[nodiscard]] std::size_t soundIconWidth(bool large) noexcept;
[[nodiscard]] std::size_t soundIconHeight(bool large) noexcept;
[[nodiscard]] bool soundIconDot(const talas::project::ProjectSoundIcon& icon, bool large, SoundIconDot dot) noexcept;
// Sets or clears a dot, the icon's rows made when it has none; outside the
// icon nothing changes.
void setSoundIconDot(talas::project::ProjectSoundIcon& icon, bool large, SoundIconDot dot, bool on);
// The dots a stroke passes from one to the next, both included.
[[nodiscard]] std::vector<SoundIconDot> soundIconStroke(SoundIconDot from, SoundIconDot to);
// A click on a clear dot draws, on a set one erases: the stroke then sets
// or clears every dot it passes.
void drawSoundIconStroke(
    talas::project::ProjectSoundIcon& icon, bool large, const std::vector<SoundIconDot>& stroke, bool on);
// The icon of another Sound dragged in: its kind's, the drawn dots gone.
[[nodiscard]] talas::project::ProjectSoundIcon soundIconSameAs(std::string sameAs);
// The Class Editor's input and output types,
// by name (none, audio, subaudio, spectrum, transparent, constructor,
// variable, environment), and a class's outline from them and its side
// effects box, the edges its ports give where it chose none.
[[nodiscard]] std::optional<IconEdge> soundIconEdgeOfType(std::string_view type) noexcept;
[[nodiscard]] std::string_view soundIconTypeOfEdge(IconEdge edge) noexcept;
[[nodiscard]] SoundIconShape soundClassIconShape(SoundIconShape fromPorts, const talas::project::ProjectSoundIcon& icon) noexcept;
// The small icon is the one shown with small icons
// chosen under Edit > View..., when the class has one; the large icon is
// shown by default.
[[nodiscard]] bool soundIconDrawsSmall(const talas::project::ProjectSoundIcon& icon, bool largeIconsShown) noexcept;

// The lines the signal flow draws from one Sound to another: the output
// channels its class gave the Sound the line comes from, else the input
// channels of the class of the Sound it goes to; nothing when neither class
// chose.
[[nodiscard]] std::optional<unsigned> soundClassConnectionLines(
    const talas::project::ProjectSoundIcon* from, const talas::project::ProjectSoundIcon* to) noexcept;
// The PNG file for a class: its name in lowercase letters and .png, found
// in the first of the folders that has it.
[[nodiscard]] std::string soundIconPngFileName(std::string_view className);
[[nodiscard]] std::optional<std::filesystem::path> findSoundIconPng(
    std::string_view className, const std::vector<std::filesystem::path>& folders);
// A PNG icon's size is continuously variable: its
// picture, of any size, is scaled to fill as much of its place as it can,
// keeping its proportions, centred there.
struct SoundIconPlacement
{
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    bool operator==(const SoundIconPlacement&) const = default;
};
[[nodiscard]] SoundIconPlacement soundIconPngPlacement(
    double imageWidth, double imageHeight, const SoundIconPlacement& place) noexcept;

// The icons' pictures are cached, so a picture
// changed on the disk may not show at once; Recycle in the Status window
// flushes the cache, and the next drawing loads the file again.
template <typename Picture>
class SoundIconPictureCache
{
public:
    template <typename Load>
    [[nodiscard]] const Picture& get(const std::filesystem::path& file, Load&& load)
    {
        auto found = pictures_.find(file);
        if (found == pictures_.end())
            found = pictures_.emplace(file, load(file)).first;
        return found->second;
    }

    void recycle() noexcept
    {
        pictures_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return pictures_.size();
    }

private:
    std::map<std::filesystem::path, Picture> pictures_;
};
}
