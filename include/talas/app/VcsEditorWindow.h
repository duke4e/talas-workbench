#pragma once

#include "talas/app/VcsLayout.h"

#include <optional>

// The VCS Editor: unlocking the Virtual Control
// Surface also opens its editor, a floating window that can be put wherever
// one likes on the screen so that it does not hide the surface; locking the
// surface again closes it. Rectangles here are in screen pixels.
namespace talas::app
{
inline constexpr int vcsEditorWindowGap = 8;

// Whether the editor is open: while the surface is unlocked.
[[nodiscard]] constexpr bool vcsEditorWindowOpen(bool unlocked) noexcept
{
    return unlocked;
}

// Where the editor opens: where it was put last, kept on the screen; the
// first time, beside the surface so that it covers none of it, at its right
// when the screen has room there, else at its left, below or above it, and
// failing those at the screen's right edge.
[[nodiscard]] VcsRect vcsEditorWindowPlace(const VcsRect& surface, const VcsRect& screen, int width, int height,
    const std::optional<VcsRect>& last = std::nullopt) noexcept;

// The editor dragged by (dx, dy): anywhere on the screen, kept within it.
[[nodiscard]] VcsRect vcsEditorWindowMoved(const VcsRect& window, const VcsRect& screen, int dx, int dy) noexcept;

// Whether two rectangles overlap (the editor hiding part of the surface).
[[nodiscard]] bool vcsRectsOverlap(const VcsRect& first, const VcsRect& second) noexcept;
}
