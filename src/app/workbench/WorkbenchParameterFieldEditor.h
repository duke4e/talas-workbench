#pragma once

#include "talas/app/ParameterFieldEditors.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <string>

namespace talas::app::workbench
{
// The inspector's graphical field editors: a
// GraphicalEnvelope's breakpoints, or an array's values as bars. Each edit
// hands the field's new text to the inspector, which commits it.
class ParameterFieldEditor final : public juce::Component
{
public:
    enum class Mode
    {
        None,
        Envelope,
        Array,
    };
    using CommitCallback = std::function<void(std::string)>;

    explicit ParameterFieldEditor(CommitCallback onCommit);

    // Shows the editor that fits the field, or none; `envelope` for a
    // GraphicalEnvelope's Envelope field, `rate` its Rate, which the grid's
    // heavy lines are labelled by.
    void showField(const std::string& text, bool envelope, double rate = 1.0);
    // Hidden while another field shows; the same field shown again keeps
    // its selection.
    void hideField()
    {
        setVisible(false);
    }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }
    [[nodiscard]] int preferredHeight() const noexcept;

    // The edits the mouse and keys make, also for the smoke.
    bool addEnvelopePoint(double seconds, double value);
    bool selectEnvelopePoint(std::size_t index);
    bool moveSelectedEnvelopePoint(double seconds, double value);
    bool removeSelectedEnvelopePoint();
    bool toggleSelectedLoopStart();
    bool toggleSelectedLoopEnd();
    void zoom(bool in);
    [[nodiscard]] double visibleSeconds() const noexcept { return visibleSeconds_; }
    [[nodiscard]] double rate() const noexcept { return rate_; }
    bool setArrayValue(std::size_t index, double value);
    bool appendArrayValue();
    bool removeLastArrayValue();

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    [[nodiscard]] juce::Rectangle<float> plotArea() const;
    [[nodiscard]] juce::Point<float> envelopePointPosition(const talas::app::EnvelopeBreakpoint& point) const;
    [[nodiscard]] std::pair<double, double> envelopeValueAt(juce::Point<float> position) const;
    [[nodiscard]] std::optional<std::size_t> arrayIndexAt(juce::Point<float> position) const;
    [[nodiscard]] double arrayValueAt(juce::Point<float> position) const;
    void commit();
    void updateButtons();

    CommitCallback onCommit_;
    Mode mode_ = Mode::None;
    std::string shownText_;
    bool shownEnvelope_ = false;
    talas::app::EnvelopeField envelope_;
    std::optional<std::size_t> selected_;
    std::vector<talas::app::ArrayFieldElement> array_;
    talas::app::ArrayFieldRange arrayRange_;
    double visibleSeconds_ = 1.0;
    double rate_ = 1.0;
    bool dragging_ = false;
    bool changed_ = false;
    juce::TextButton loopStartButton_{"Loop Start"};
    juce::TextButton loopEndButton_{"Loop End"};
    juce::TextButton zoomInButton_{"Zoom In"};
    juce::TextButton zoomOutButton_{"Zoom Out"};
    juce::TextButton addValueButton_{"+"};
    juce::TextButton removeValueButton_{"-"};
};
}
