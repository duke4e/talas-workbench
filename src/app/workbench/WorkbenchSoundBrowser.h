#pragma once

#include "talas/app/SoundBrowserModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// What a Sound Browser asks of the Workbench: playing, pausing, stopping and
// replaying (Space, Ctrl+Space, Ctrl+K, Ctrl+R), a Sound dragged into the
// project, a sub-browser for a folder, and the one replacement input every
// browser shares.
struct SoundBrowserHost
{
    std::function<void(const talas::app::SoundBrowserRow&)> play;
    // Play clicked with Control held: a short
    // sample once, not looping.
    std::function<void(const talas::app::SoundBrowserRow&)> playOnce;
    std::function<void()> pauseOrResume;
    std::function<void()> stop;
    std::function<void()> replay;
    std::function<void(const talas::app::SoundBrowserRow&)> addToProject;
    // A sample or a spectrum opened in its editor.
    std::function<void(const talas::app::SoundBrowserRow&)> open;
    // A Sound file opened in a Sound file window.
    std::function<void(const std::filesystem::path&)> openSoundFileWindow;
    // Ctrl+C: the selected Sound's widgets copied.
    std::function<void(const talas::app::SoundBrowserRow&)> copy;
    std::function<void(const std::filesystem::path&)> spawn;
    std::function<talas::app::SoundBrowserReplacementInput()> input;
    std::function<void(talas::app::SoundBrowserReplacementInput)> setInput;
    std::function<unsigned()> audioInputCount;
};

class SoundBrowserPanel : public juce::Component
{
public:
    ~SoundBrowserPanel() override = default;

    virtual void setRoots(std::vector<std::filesystem::path> roots) = 0;
    virtual void refresh() = 0;
    // The replacement input changed in another browser.
    virtual void showInput() = 0;
    // The row selected, which the Info menu takes while the browser is in
    // front.
    [[nodiscard]] virtual std::optional<talas::app::SoundBrowserRow> selectedRow() const = 0;

    [[nodiscard]] virtual std::vector<talas::app::SoundBrowserRow> rowsForSmoke() const = 0;
    virtual bool selectRowForSmoke(std::size_t row) = 0;
    [[nodiscard]] virtual juce::String informationForSmoke() const = 0;
    virtual void toggleRowForSmoke(std::size_t row) = 0;
    // A coloured box clicked, with Shift or not; no kind for the grey box.
    virtual void clickKindForSmoke(std::optional<talas::app::SoundBrowserKind> kind, bool shift) = 0;
    virtual bool findForSmoke(const std::string& text) = 0;
    virtual bool findAgainForSmoke() = 0;
    [[nodiscard]] virtual std::optional<talas::app::SoundBrowserRow> selectedRowForSmoke() const = 0;
    virtual void pressAudioInputForSmoke() = 0;
    virtual void pressSubstituteForSmoke() = 0;
    virtual void pressConnectionForSmoke() = 0;
    virtual void pressPlayForSmoke(bool control) = 0;
    [[nodiscard]] virtual juce::String inputTextForSmoke() const = 0;
    virtual bool keyForSmoke(const juce::KeyPress& key) = 0;
    virtual void activateRowForSmoke(std::size_t row) = 0;
    // What a row dragged out carries; empty for one not dragged.
    [[nodiscard]] virtual juce::String dragDescriptionForSmoke(std::size_t row) const = 0;
};

[[nodiscard]] std::unique_ptr<SoundBrowserPanel> makeSoundBrowserPanel(SoundBrowserHost host);
}
