#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/PartialTrackAsset.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// The disk button beside a file name field: a click chooses the file from a
// file list; held down with Command or Control, it opens an editor on the
// file, and with Shift a window of information on it.
namespace talas::app
{
enum class DiskButtonAction
{
    ChooseFile,
    OpenEditor,
    ShowInformation,
};

[[nodiscard]] DiskButtonAction diskButtonAction(bool commandOrControl, bool shift) noexcept;

// The editor a file opens in, by its name: a sample file (WAV or AIFF) or
// one of the system wavetables in the sample editor, a spectrum file in the
// spectrum editor; none for another file.
enum class FileEditor
{
    None,
    Sample,
    Spectrum,
};

[[nodiscard]] FileEditor fileEditorFor(std::string_view fileName, bool wavetable) noexcept;

// What a sample file's header says.
struct SampleFileInformation
{
    std::string name;
    std::size_t channels = 0;
    double sampleRate = 0.0;
    std::uint64_t frames = 0;
    // Its loop, first frame and the frame after its last.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> loop;
    // Its pitch as a MIDI note number, from the header's unity note.
    std::optional<double> pitch;
};

// The information window's text: the file, its format, channels, sample
// rate, duration in samples and seconds, and its loop and pitch.
[[nodiscard]] std::string sampleFileInformationText(const SampleFileInformation& file);
// For a spectrum file: its frames, partials, frame time and duration.
[[nodiscard]] std::string spectrumFileInformationText(
    std::string_view name, const talas::project::PartialTrackAsset& spectrum);
// For one of the system wavetables.
[[nodiscard]] std::string wavetableInformationText(std::string_view name, std::size_t samples);

// The FromMemoryWriter check box just below a file
// name field, checked when the Sound reads the recording of the
// MemoryWriter named there rather than a file: the Boolean parameter it
// sets, for the file field it stands beside (fromMemoryWriter for the
// Sound's first, <field>FromMemoryWriter for another, as a SampleCloud's
// grainEnvFromMemoryWriter). A field whose Sound cannot read a recording
// has none.
[[nodiscard]] std::optional<std::string> fromMemoryWriterCheckBox(
    const talas::core::NodeTypeDescriptor& descriptor, std::string_view fileParameterId);
}
