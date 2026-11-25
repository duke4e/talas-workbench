#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app
{
// The project with a selected Sound of its structure in the output's place,
// as the Info menu's operations take the selected Sound: its first audio
// output feeds the output's left input and its second, if it has one, the
// right; the output's other lines go. Nothing when the structure has no output
// or the Sound is not in it.
[[nodiscard]] std::optional<talas::project::ProjectDocument> documentPlayingSound(
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry, std::string_view nodeId);

// Info > Oscilloscope and Spectrum analyzer:
// the selected Sound played, an OscilloscopeDisplay or a
// SpectrumAnalyzerDisplay showing a mix of its left and right outputs
// (half of each, a Mixer before the display), or of its one output, the
// Sound heard as it was. The display is named `displayNodeId` and is of
// the type `displayTypeId`.
enum class InfoDisplay : std::uint8_t
{
    Oscilloscope,
    SpectrumAnalyzer,
};

struct InfoDisplayDocument
{
    talas::project::ProjectDocument document;
    std::string displayNodeId;
    std::string displayTypeId;
    // The node of the Sound loaded alone in the project's place, whose
    // controls the surface then shows; empty for the project.
    std::string loadedSound;
};

// Ctrl+Space: in the signal flow, the one Sound
// selected compiled, loaded and started alone, the project left as it is;
// nothing, for the whole project as before, outside the signal flow, with
// no Sound or several selected, or with the output selected.
[[nodiscard]] std::optional<talas::project::ProjectDocument> documentForControlSpace(
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry,
    const std::vector<std::string>& selected, bool inSignalFlow);

[[nodiscard]] std::optional<InfoDisplayDocument> documentWithInfoDisplay(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view nodeId, InfoDisplay display);
// Full waveform's suggested length: the length of
// the longest sample the Sound selected in the signal flow plays, it or the
// Sounds feeding it; none when it plays no sample.
[[nodiscard]] std::optional<double> selectedSoundSampleSeconds(
    const talas::project::ProjectDocument& document, std::string_view nodeId);

// The same for a Sound selected in the Sound file window: an instance of it
// played through the display in place of the project's output.
[[nodiscard]] std::optional<InfoDisplayDocument> soundWithInfoDisplay(const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry, std::string_view definitionId, InfoDisplay display);

// GenericSource's Ask box: a Source of
// Ask asks for the source each time the Sound plays, the recording on disk
// (the default Enter gives), the live input or a sample in RAM. The
// GenericSources asking, in the project's structure and its Sounds; and
// the project with each one's Source answered.
enum class GenericSourceAnswer : std::uint8_t
{
    Disk,
    Live,
    Ram,
};
[[nodiscard]] std::vector<std::string> genericSourcesAsking(const talas::project::ProjectDocument& document);
[[nodiscard]] talas::project::ProjectDocument documentWithGenericSourceAnswer(
    talas::project::ProjectDocument document, GenericSourceAnswer answer);
[[nodiscard]] std::string_view genericSourceAnswerText(GenericSourceAnswer answer) noexcept;
}
