#include "talas/app/SoundClassIcon.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <system_error>

namespace talas::app
{
std::size_t soundIconWidth(bool large) noexcept
{
    return large ? talas::project::soundIconLargeWidth : talas::project::soundIconSmallWidth;
}

std::size_t soundIconHeight(bool large) noexcept
{
    return large ? talas::project::soundIconLargeHeight : talas::project::soundIconSmallHeight;
}

bool soundIconDot(const talas::project::ProjectSoundIcon& icon, bool large, SoundIconDot dot) noexcept
{
    if (dot.x >= soundIconWidth(large) || dot.y >= soundIconHeight(large))
        return false;
    const auto bit = soundIconWidth(large) - 1 - dot.x;
    if (large)
        return dot.y < icon.large.size() && ((icon.large[dot.y] >> bit) & 1U) != 0;
    return dot.y < icon.small.size() && ((icon.small[dot.y] >> bit) & 1U) != 0;
}

void setSoundIconDot(talas::project::ProjectSoundIcon& icon, bool large, SoundIconDot dot, bool on)
{
    if (dot.x >= soundIconWidth(large) || dot.y >= soundIconHeight(large))
        return;
    const auto bit = soundIconWidth(large) - 1 - dot.x;
    if (large)
    {
        icon.large.resize(talas::project::soundIconLargeHeight, 0);
        const auto mask = std::uint64_t{1} << bit;
        icon.large[dot.y] = on ? (icon.large[dot.y] | mask) : (icon.large[dot.y] & ~mask);
        if (std::ranges::all_of(icon.large, [](std::uint64_t row) { return row == 0; }))
            icon.large.clear();
        return;
    }
    icon.small.resize(talas::project::soundIconSmallHeight, 0);
    const auto mask = std::uint32_t{1} << bit;
    icon.small[dot.y] = on ? (icon.small[dot.y] | mask) : (icon.small[dot.y] & ~mask);
    if (std::ranges::all_of(icon.small, [](std::uint32_t row) { return row == 0; }))
        icon.small.clear();
}

std::vector<SoundIconDot> soundIconStroke(SoundIconDot from, SoundIconDot to)
{
    // Bresenham's line from one dot to the next.
    std::vector<SoundIconDot> dots;
    auto x = static_cast<long>(from.x);
    auto y = static_cast<long>(from.y);
    const auto x1 = static_cast<long>(to.x);
    const auto y1 = static_cast<long>(to.y);
    const auto dx = std::labs(x1 - x);
    const auto dy = -std::labs(y1 - y);
    const auto sx = x < x1 ? 1L : -1L;
    const auto sy = y < y1 ? 1L : -1L;
    auto error = dx + dy;
    for (;;)
    {
        dots.push_back({static_cast<std::size_t>(x), static_cast<std::size_t>(y)});
        if (x == x1 && y == y1)
            break;
        const auto twice = 2 * error;
        if (twice >= dy)
        {
            error += dy;
            x += sx;
        }
        if (twice <= dx)
        {
            error += dx;
            y += sy;
        }
    }
    return dots;
}

void drawSoundIconStroke(
    talas::project::ProjectSoundIcon& icon, bool large, const std::vector<SoundIconDot>& stroke, bool on)
{
    // A drawn icon is the class's own: no other Sound's.
    icon.sameAs.clear();
    for (const auto dot : stroke)
        setSoundIconDot(icon, large, dot, on);
}

talas::project::ProjectSoundIcon soundIconSameAs(std::string sameAs)
{
    talas::project::ProjectSoundIcon icon;
    icon.sameAs = std::move(sameAs);
    return icon;
}

namespace
{
constexpr std::array<std::pair<std::string_view, IconEdge>, 8> iconTypes{{
    {"none", IconEdge::None},
    {"audio", IconEdge::Audio},
    {"subaudio", IconEdge::Subaudio},
    {"spectrum", IconEdge::Spectrum},
    {"transparent", IconEdge::Transparent},
    {"constructor", IconEdge::Constructor},
    {"variable", IconEdge::Variable},
    {"environment", IconEdge::Environment},
}};
}

std::optional<IconEdge> soundIconEdgeOfType(std::string_view type) noexcept
{
    const auto found = std::ranges::find(iconTypes, type, &std::pair<std::string_view, IconEdge>::first);
    return found == iconTypes.end() ? std::nullopt : std::optional<IconEdge>{found->second};
}

std::string_view soundIconTypeOfEdge(IconEdge edge) noexcept
{
    const auto found = std::ranges::find(iconTypes, edge, &std::pair<std::string_view, IconEdge>::second);
    return found == iconTypes.end() ? std::string_view{} : found->first;
}

SoundIconShape soundClassIconShape(SoundIconShape fromPorts, const talas::project::ProjectSoundIcon& icon) noexcept
{
    if (const auto left = soundIconEdgeOfType(icon.inputType))
        fromPorts.left = *left;
    if (const auto right = soundIconEdgeOfType(icon.outputType))
        fromPorts.right = *right;
    fromPorts.pointedTop = fromPorts.pointedTop || icon.sideEffects;
    return fromPorts;
}

std::string soundIconPngFileName(std::string_view className)
{
    std::string name;
    name.reserve(className.size() + 4);
    for (const auto character : className)
        name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    return name + ".png";
}

SoundIconPlacement soundIconPngPlacement(double imageWidth, double imageHeight, const SoundIconPlacement& place) noexcept
{
    if (!(imageWidth > 0.0) || !(imageHeight > 0.0) || !(place.width > 0.0) || !(place.height > 0.0))
        return {place.x, place.y, 0.0, 0.0};
    const auto scale = std::min(place.width / imageWidth, place.height / imageHeight);
    const auto width = imageWidth * scale;
    const auto height = imageHeight * scale;
    return {place.x + (place.width - width) / 2.0, place.y + (place.height - height) / 2.0, width, height};
}

std::optional<std::filesystem::path> findSoundIconPng(
    std::string_view className, const std::vector<std::filesystem::path>& folders)
{
    const auto name = soundIconPngFileName(className);
    for (const auto& folder : folders)
    {
        std::error_code error;
        const auto candidate = folder / name;
        if (std::filesystem::is_regular_file(candidate, error))
            return candidate;
    }
    return std::nullopt;
}

bool soundIconDrawsSmall(const talas::project::ProjectSoundIcon& icon, bool largeIconsShown) noexcept
{
    return !largeIconsShown && !icon.small.empty();
}

std::optional<unsigned> soundClassConnectionLines(
    const talas::project::ProjectSoundIcon* from, const talas::project::ProjectSoundIcon* to) noexcept
{
    if (from != nullptr && from->outputChannels != 0)
        return from->outputChannels;
    if (to != nullptr && to->inputChannels != 0)
        return to->inputChannels;
    return std::nullopt;
}
}
