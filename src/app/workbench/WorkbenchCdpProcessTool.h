#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace talas::app::workbench
{
// The Tools menu's CDP Process to Sample (ADR-0018 amendment, 2026-10-19):
// CDP8's spectral processes that change a sound's length or order (stretch
// time, focus hold, spec cut, grab and magnify, combine interleave, blur
// drunk, shuffle and weave) render chosen samples through `pvoc anal`, the
// process and `pvoc synth` into a new sample, and leave an example Sound
// that plays it.
class CdpProcessToolPanel : public juce::Component
{
public:
    struct Completed
    {
        std::filesystem::path output;
        juce::String summary;
    };

    using StatusCallback = std::function<void(const juce::String&)>;
    using CompletedCallback = std::function<void(Completed)>;

    ~CdpProcessToolPanel() override = default;

    // Stops a running process and waits for it.
    virtual void cancelProcess() = 0;
    // Smoke access: the samples a Choose... would pick, a process and its
    // field texts, and the render a Process button would start, run to its
    // end before returning.
    [[nodiscard]] virtual bool setInputsForSmoke(
        const std::vector<std::filesystem::path>& inputs) = 0;
    [[nodiscard]] virtual bool setProcessForSmoke(
        const std::string& process,
        int points,
        const std::map<std::string, std::string>& fields) = 0;
    [[nodiscard]] virtual bool processNowForSmoke(
        const std::filesystem::path& output) = 0;
};

[[nodiscard]] std::unique_ptr<CdpProcessToolPanel> makeCdpProcessToolPanel(
    CdpProcessToolPanel::StatusCallback onStatus,
    CdpProcessToolPanel::CompletedCallback onCompleted);
}
