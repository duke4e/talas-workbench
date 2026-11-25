#pragma once

#include "talas/app/DiskSplicerSegments.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app::workbench
{
// The DiskSplicer's Segments field: on the
// left an overview of the sample files as they are spliced together, where
// a drag picks what the right half shows; on the right the samples, where
// a drag selects; below the overview the selection's magnification, size
// and range, a click on the size or range changing their units; the icon
// buttons, and the Edit operations on the selection. Each edit hands the
// field's new text to the inspector, which commits it.
class DiskSplicerField final : public juce::Component
{
public:
    using Channels = std::vector<std::vector<double>>;
    struct Host
    {
        // A file the project holds, by its name: its samples, a channel
        // each, or none.
        std::function<std::optional<Channels>(std::string_view file)> samples;
        // The sample rate the DiskSplicer plays at.
        std::function<double()> sampleRate;
        // The play button: the selected samples at that rate.
        std::function<void(Channels, double)> play;
        // The disk button: the file list, then the file chosen by the name
        // the project holds it under.
        std::function<void(std::function<void(std::string)>)> chooseFile;
    };
    using CommitCallback = std::function<void(std::string)>;

    explicit DiskSplicerField(CommitCallback onCommit);

    void setHost(Host host);
    // The field's text; the field the editor itself just wrote keeps its
    // selection and view.
    void showSegments(const std::string& text);
    [[nodiscard]] int preferredHeight() const noexcept { return 210; }

    // The icon buttons ("<", "Play", "Disk", "Fill", "Time Out", "Time In",
    // "Level Out", "Level In", ">") and the Edit operations ("Cut", "Copy",
    // "Paste", "Clear", "Trim", "Select All"), as a click does them.
    bool perform(const juce::String& action);
    // A drag on the right half selects; on the left half it picks the
    // samples the right half shows.
    void selectSamples(std::size_t from, std::size_t to);
    void showSamples(std::size_t from, std::size_t to);
    // The file chosen with the disk button, over the selection.
    bool pasteFile(const std::string& file);
    void switchUnits();

    [[nodiscard]] const talas::app::DiskSplicerSegments& splicer() const noexcept { return splicer_; }
    [[nodiscard]] const talas::app::SampleEditorView& view() const noexcept { return view_; }
    // The field below the overview: magnification, then size and range.
    [[nodiscard]] juce::String readout() const;
    // Why the field's text cannot be shown graphically, if it cannot.
    [[nodiscard]] const juce::String& problem() const noexcept { return problem_; }

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    [[nodiscard]] std::optional<std::size_t> framesOf(std::string_view file);
    [[nodiscard]] const Channels* channelsOf(const std::string& file);
    [[nodiscard]] double sampleRate() const;
    [[nodiscard]] std::size_t frameAt(juce::Rectangle<int> area, float x, std::size_t start, std::size_t frames) const;
    void drawSamples(juce::Graphics& graphics, juce::Rectangle<int> area, std::size_t start, std::size_t frames,
        double level, bool overview);
    [[nodiscard]] Channels played();
    void commit();
    void showEditMenu();

    CommitCallback onCommit_;
    Host host_;
    std::string shownText_;
    talas::app::DiskSplicerSegments splicer_;
    talas::app::SampleEditorView view_;
    std::map<std::string, std::optional<Channels>, std::less<>> files_;
    juce::String problem_;
    bool inSamples_ = true;
    juce::Rectangle<int> overview_;
    juce::Rectangle<int> detail_;
    juce::Rectangle<int> info_;
    juce::Rectangle<int> units_;
    enum class Drag
    {
        None,
        Overview,
        Detail,
    };
    Drag drag_ = Drag::None;
    std::size_t dragFrom_ = 0;
    std::vector<std::unique_ptr<juce::TextButton>> buttons_;
    juce::TextButton editButton_{"Edit"};
};
}
