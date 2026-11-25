#pragma once

#include "talas/app/PartialTrackEditorModel.h"
#include "talas/app/PartialTrackSpectrumEditing.h"
#include "talas/project/ProjectEditor.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>

namespace talas::app::workbench
{
struct PartialTrackEditorSourceSnapshot
{
    talas::project::ProjectAsset manifest;
    std::shared_ptr<const talas::project::PartialTrackAsset> source;
    std::optional<talas::project::ProjectPartialTrackEditSet> edits;
    // What the preview plays: all, the selection,
    // or between or outside the loop markers.
    talas::app::PartialTrackPlay play = talas::app::PartialTrackPlay::All;
    talas::app::PartialTrackSelection selection;
    talas::app::PartialTrackMarkers markers;
    // The scrub bar moved or a stroke drawn: from this frame
    // so many frames, of all the tracks or the selected ones.
    struct Scrub
    {
        std::uint64_t frame = 0;
        std::uint64_t frames = 0;
        talas::app::PartialTrackPlay tracks = talas::app::PartialTrackPlay::All;
    };
    std::optional<Scrub> scrub;
};

struct PartialTrackEditorOperationState
{
    bool previewPreparing = false;
    bool previewActive = false;
    bool derivativeRunning = false;
    bool canStartPreview = false;
    bool canStartAudioDerivative = false;
    bool canStartPartialDerivative = false;
    bool canImportDerivative = false;
    std::uint16_t progressPermille = 0;
    std::string status;
};

struct PartialTrackEditorOperationSourceInfo
{
    double sampleRate = 0.0;
    std::size_t maximumRenderedSegmentTrackCount = 0;
};

struct PartialTrackEditorEditingHooks
{
    std::function<talas::project::EditResult(
        const talas::project::PartialTrackAsset&,
        talas::project::ProjectPartialTrackEditBatch,
        talas::project::ProjectEditor::HistoryTransactionToken)> apply;
    std::function<talas::project::ProjectEditor::HistoryTransactionToken()>
        beginGesture;
    std::function<void(talas::project::ProjectEditor::HistoryTransactionToken)>
        endGesture;
    std::function<void(talas::project::ProjectEditor::HistoryTransactionToken)>
        cancelGesture;
    std::function<void()> documentChanged;
    std::function<void(const juce::String&)> setStatus;
    std::function<void()> startPreview;
    std::function<void()> stopPreview;
    std::function<void()> renderAudioDerivative;
    std::function<void()> renderPartialDerivative;
    std::function<void()> cancelDerivative;
    std::function<void()> importDerivative;
    // Copy: the selected region written as a new
    // spectrum file, and a Sound made to play it; whether it was.
    std::function<bool(const talas::project::PartialTrackAsset&, const std::filesystem::path&)> copyRegion;
    // Save as...: the spectrum as edited, its
    // clears and cuts permanent, written as a new spectrum file and opened.
    std::function<bool(const talas::project::PartialTrackAsset&, const std::filesystem::path&)> saveAs;
};

class PartialTrackEditorPanel : public juce::Component
{
public:
    ~PartialTrackEditorPanel() override = default;

    virtual void setLoading() = 0;
    virtual void setUnavailable(juce::String message) = 0;
    [[nodiscard]] virtual bool setSource(
        talas::project::ProjectAsset manifest,
        std::shared_ptr<const talas::project::PartialTrackAsset> source,
        std::optional<talas::project::ProjectPartialTrackEditSet> edits) = 0;
    virtual void refreshEdits(
        const talas::project::ProjectDocument& document) = 0;
    virtual void setOperationState(
        PartialTrackEditorOperationState state) = 0;
    [[nodiscard]] virtual std::optional<
        PartialTrackEditorSourceSnapshot> sourceSnapshot() const = 0;
    [[nodiscard]] virtual std::optional<
        PartialTrackEditorOperationSourceInfo>
    operationSourceInfo() const noexcept = 0;

    [[nodiscard]] virtual talas::app::PartialTrackEditorState
    stateForSmoke() const noexcept = 0;
    [[nodiscard]] virtual std::size_t
    visibleAccessibleItemCountForSmoke() const noexcept = 0;
    [[nodiscard]] virtual bool
    sourceTextContainsForSmoke(const juce::String& text) const = 0;
    [[nodiscard]] virtual bool
    compactLayoutForSmoke() const noexcept = 0;
    [[nodiscard]] virtual bool
    frequencyLabelsFitForSmoke() const noexcept = 0;
    [[nodiscard]] virtual bool
    nudgeFirstPointForSmoke() = 0;
    // Copy of the selected region to a spectrum file at a path, for the
    // smoke: the whole spectrum selected first.
    [[nodiscard]] virtual bool copyRegionForSmoke(const std::filesystem::path& destination) = 0;
    // Save as..., asking the new file's name; false with no spectrum shown.
    virtual bool promptSaveAs() = 0;
    [[nodiscard]] virtual bool saveAsForSmoke(const std::filesystem::path& destination) = 0;
    // Drawing mode: the first track selected and
    // the pen put down over its first point at a height (0 top, 1 bottom);
    // that point's value on the axis shown after.
    [[nodiscard]] virtual std::optional<double> drawForSmoke(double heightRatio) = 0;
    // MIDI pitch bend (0 to 16383) places the scrub bar across the file
    //; the frame it is at after.
    virtual std::optional<std::uint64_t> scrubForPitchBend(int bend) = 0;
    [[nodiscard]] virtual bool
    exerciseEditCommandsForSmoke() = 0;
    [[nodiscard]] virtual bool
    exerciseSpectrumEditingForSmoke() = 0;
    [[nodiscard]] virtual bool operationControlsMatchForSmoke(
        bool previewEnabled,
        bool audioDerivativeEnabled,
        bool partialDerivativeEnabled) const = 0;
};

[[nodiscard]] std::unique_ptr<PartialTrackEditorPanel>
makePartialTrackEditorPanel(PartialTrackEditorEditingHooks hooks);
}
