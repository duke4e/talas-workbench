#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The displays on the Virtual Control Surface: an OscilloscopeDisplay of the
// project's signal flow shows the waveform of its Input in the surface as an
// oscilloscope would, labelled with the Sound's name, and the buttons along
// its bottom zoom its trace in or out in time and in amplitude.
namespace talas::app
{
inline constexpr std::string_view vcsOscilloscopeTypeId = "talas.oscilloscopeDisplay";

struct VcsDisplay
{
    std::string nodeId;
    // The Sound's name (here its node's name, our Sounds having no other).
    std::string label;

    bool operator==(const VcsDisplay&) const = default;
};

// Each OscilloscopeDisplay of the project's structure, in the structure's
// order, side by side on the surface.
[[nodiscard]] std::vector<VcsDisplay> vcsOscilloscopeDisplays(const talas::project::ProjectDocument& document);

// A trace's zoom: the frames across the display's width and the amplitude
// at its top edge (full scale at first).
inline constexpr std::uint32_t minimumVcsScopeFrames = 16;
inline constexpr std::uint32_t maximumVcsScopeFrames = 262144;
inline constexpr std::uint32_t defaultVcsScopeFrames = 256;
inline constexpr double minimumVcsScopeAmplitude = 1.0 / 64.0;
inline constexpr double maximumVcsScopeAmplitude = 16.0;

struct VcsScopeZoom
{
    std::uint32_t frames = defaultVcsScopeFrames;
    double amplitude = 1.0;

    bool operator==(const VcsScopeZoom&) const = default;
};

// The buttons along the bottom: the inward-pointing arrows zoom out in time
// (twice the frames across, up to the most the display keeps), the outward
// ones zoom in (half, down to the fewest); in amplitude, out doubles the
// amplitude at the top and in halves it.
enum class VcsScopeButton
{
    TimeOut,
    TimeIn,
    AmplitudeOut,
    AmplitudeIn,
};

[[nodiscard]] VcsScopeZoom vcsScopeZoomed(VcsScopeZoom zoom, VcsScopeButton button) noexcept;
// The button's face, as the surface draws it: ">|<" for the inward arrows.
[[nodiscard]] std::string_view vcsScopeButtonText(VcsScopeButton button) noexcept;

// The frames a display keeps, the newest last. Each capture of its Input
// (the latest frames, from their absolute sample position) adds the frames
// newer than those kept; frames the captures skipped are kept as missing
// (NaN), so that the time across the display stays true when zoomed out
// past one capture; a capture older than those kept starts over.
class VcsScopeHistory
{
public:
    explicit VcsScopeHistory(std::size_t capacity = maximumVcsScopeFrames);

    void add(std::uint64_t position, std::span<const double> frames);
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::uint64_t end() const noexcept { return end_; }
    // The newest frames, as many as asked for or as are kept, oldest first.
    [[nodiscard]] std::vector<double> latest(std::size_t count) const;

private:
    void push(double value);

    std::vector<double> ring_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
    std::uint64_t end_ = 0;
};

// The trace across a display `columns` wide, the frames (oldest first)
// spread over the full zoom so that a short history sits at the right:
// each column's lowest and highest value among the frames it covers, none
// for a column whose frames are all missing.
struct VcsScopeColumn
{
    double low = 0.0;
    double high = 0.0;
    bool present = false;

    bool operator==(const VcsScopeColumn&) const = default;
};

[[nodiscard]] std::vector<VcsScopeColumn> vcsScopeColumns(
    std::span<const double> frames, std::size_t zoomFrames, std::size_t columns);
}
