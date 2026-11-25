#pragma once

#include "talas/project/ProjectScore.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

// Compiling and playing part of a Timeline: the
// Sounds marked by a click with Option (Shift too to toggle one) or a box
// drawn around them, and the mode the Timeline plays in, its marked Sounds
// only (Shift+Space) or all of them (Ctrl+Space); Space plays on in the mode
// it is in.
namespace talas::app
{
class TimelineMarks
{
public:
    void mark(const std::string& clipId);
    void toggle(const std::string& clipId);
    void markAll(const std::vector<std::string>& clipIds);
    void clear() noexcept;
    [[nodiscard]] bool marked(const std::string& clipId) const;
    [[nodiscard]] const std::set<std::string>& clipIds() const noexcept;

    void playMarkedOnly() noexcept;
    void playAll() noexcept;
    [[nodiscard]] bool playsMarkedOnly() const noexcept;

    // The clips a play compiles: the marked ones still in the Score when it
    // plays them only, every clip otherwise.
    [[nodiscard]] std::optional<std::vector<std::string>> playedClips(const talas::project::ProjectScore& score) const;

private:
    std::set<std::string> marked_;
    bool markedOnly_ = false;
};

// How a clip's outline and name are drawn: a marked
// Sound's turn light grey; an overlapping clip's outline keeps the error
// colour and a selected one the selection's.
enum class TimelineClipLook : std::uint8_t
{
    Plain,
    Marked,
    Selected,
    Overlapping,
};

[[nodiscard]] TimelineClipLook timelineClipOutline(bool overlaps, bool selected, bool marked) noexcept;
[[nodiscard]] TimelineClipLook timelineClipText(bool marked) noexcept;
}
