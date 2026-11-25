#include "talas/app/VcsEditorWindow.h"

#include <algorithm>

namespace talas::app
{
namespace
{
[[nodiscard]] VcsRect keptOnScreen(VcsRect window, const VcsRect& screen) noexcept
{
    window.left = std::max(screen.left, std::min(window.left, screen.right() - window.width));
    window.top = std::max(screen.top, std::min(window.top, screen.bottom() - window.height));
    return window;
}

[[nodiscard]] bool within(const VcsRect& window, const VcsRect& screen) noexcept
{
    return window.left >= screen.left && window.top >= screen.top && window.right() <= screen.right()
        && window.bottom() <= screen.bottom();
}
}

bool vcsRectsOverlap(const VcsRect& first, const VcsRect& second) noexcept
{
    return first.left < second.right() && second.left < first.right() && first.top < second.bottom()
        && second.top < first.bottom();
}

VcsRect vcsEditorWindowPlace(
    const VcsRect& surface, const VcsRect& screen, int width, int height, const std::optional<VcsRect>& last) noexcept
{
    if (last.has_value())
        return keptOnScreen({last->left, last->top, width, height}, screen);
    const auto gap = vcsEditorWindowGap;
    // Level with the surface's top beside it, centred on it below or above.
    const auto top = std::max(screen.top, std::min(surface.top, screen.bottom() - height));
    const auto left = std::max(screen.left, std::min(surface.left + (surface.width - width) / 2, screen.right() - width));
    for (const VcsRect candidate : {VcsRect{surface.right() + gap, top, width, height},
             VcsRect{surface.left - gap - width, top, width, height}, VcsRect{left, surface.bottom() + gap, width, height},
             VcsRect{left, surface.top - gap - height, width, height}})
    {
        if (within(candidate, screen) && !vcsRectsOverlap(candidate, surface))
            return candidate;
    }
    return keptOnScreen({screen.right() - width, screen.top, width, height}, screen);
}

VcsRect vcsEditorWindowMoved(const VcsRect& window, const VcsRect& screen, int dx, int dy) noexcept
{
    return keptOnScreen({window.left + dx, window.top + dy, window.width, window.height}, screen);
}
}
