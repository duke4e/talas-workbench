#pragma once

#include "talas/app/AnalysisWorkbenchModel.h"
#include "talas/app/AnalysisDiagnosticsModel.h"
#include "talas/app/TalasFiles.h"
#include "talas/runtime/RealtimeAnalysisCapture.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::app::workbench
{
struct AnalysisPanelCallbacks
{
    std::function<void()> pinSelectedTarget;
    std::function<void(std::string)> removeTrace;
    std::function<void(std::string)> selectTrace;
    std::function<void(std::string, int)> moveTrace;
    std::function<void(std::string, bool)> setTraceVisible;
    std::function<void(std::string, double)> setTraceScale;
    std::function<void(std::uint32_t)> setRefreshRateHz;
    std::function<void(std::uint32_t)> setHistoryFrames;
    std::function<void(bool)> setFrozen;
    std::function<void(talas::project::WorkspaceAnalysisAuditionState)>
        startAudition;
    std::function<void()> stopAudition;
    std::function<void()> commitAudition;
    std::function<void()> chooseOutputTake;
    std::function<void()> stopOutputTake;
    std::function<void()> cancelOutputTake;
};

struct AnalysisAuditionPanelOption
{
    std::string id;
    std::string label;

    bool operator==(const AnalysisAuditionPanelOption&) const = default;
};

struct AnalysisAuditionPanelState
{
    std::vector<AnalysisAuditionPanelOption> destinationInputs;
    std::vector<AnalysisAuditionPanelOption> frozenAssets;
    std::optional<talas::project::WorkspaceAnalysisAuditionState> request;
    std::string status;
    bool preparing = false;
    bool active = false;
    bool canCommit = false;
    bool transportExternallyControlled = false;
};

struct AnalysisOutputTakePanelState
{
    std::string status;
    std::string destinationName;
    double sampleRate = 0.0;
    std::uint64_t writtenFrames = 0;
    std::uint64_t droppedBlocks = 0;
    std::uint64_t rejectedBlocks = 0;
    std::size_t channelCount = 0;
    std::string destinationNameHint;
    bool deviceReady = false;
    bool diskRecorder = false;
    bool diskRecorderWritable = false;
    bool recording = false;
    bool stopping = false;
    bool incomplete = false;
};

class AnalysisPanel : public juce::Component
{
public:
    ~AnalysisPanel() override = default;

    virtual void setSelectedNode(
        std::string nodeId,
        std::string_view typeId) = 0;
    // Info > Oscilloscope and Spectrum analyzer: the display staged after the
    // selected Sound, shown whatever
    // is selected until the project is staged again.
    virtual void showInfoDisplay(std::string nodeId, std::string_view typeId) = 0;
    virtual void clearInfoDisplay() = 0;
    // The Spectrum Analyzer preferences: the
    // Info menu's analyzer's length and window, the panel's own settings
    // back once it closes.
    virtual void setInfoSpectrumSettings(talas::app::SpectrumAnalyzerSettings) {}
    [[nodiscard]] virtual std::optional<std::string> infoDisplayForSmoke() const = 0;
    virtual void setWorkspaceProjection(
        talas::app::AnalysisWorkbenchProjection projection,
        bool frozen,
        std::uint32_t refreshRateHz,
        std::uint32_t historyFrames) = 0;
    virtual void setCallbacks(
        AnalysisPanelCallbacks callbacks) = 0;
    virtual void setCapture(
        std::shared_ptr<talas::runtime::RealtimeAnalysisCapture> capture,
        double sampleRate,
        std::shared_ptr<const talas::runtime::PreparedScoreSourceAnalysisSet> scoreSources = {}) = 0;
    [[nodiscard]] virtual bool selectScoreSourceForSmoke(std::string_view clipId, std::uint32_t iteration) = 0;
    [[nodiscard]] virtual bool sourceControlsUsableForSmoke() const = 0;
    virtual void clearCapture() = 0;
    virtual void setPlaying(bool playing) = 0;
    virtual void setPreparedStatus(juce::String status) = 0;
    virtual void setAuditionState(
        AnalysisAuditionPanelState state) = 0;
    virtual void setOutputTakeState(
        AnalysisOutputTakePanelState state) = 0;
    [[nodiscard]] virtual bool preparedStatusContainsForSmoke(
        const juce::String& text) const = 0;
    [[nodiscard]] virtual bool traceLayoutControlsUsableForSmoke() const = 0;
    [[nodiscard]] virtual bool auditionControlsUsableForSmoke() const = 0;
    [[nodiscard]] virtual bool outputTakeControlsUsableForSmoke() const = 0;
    [[nodiscard]] virtual bool outputTakeActionContainsForSmoke(
        const juce::String& text) const = 0;
    [[nodiscard]] virtual bool traceKeyboardCursorUsableForSmoke() = 0;
    [[nodiscard]] virtual bool traceStatusContainsForSmoke(
        const juce::String& text) const = 0;
    [[nodiscard]] virtual std::optional<std::uint64_t>
    selectedTraceSequenceForSmoke() const noexcept = 0;
    [[nodiscard]] virtual talas::app::AnalysisMonitorSourceInput
    eventMonitorSnapshot() const = 0;
    virtual bool focusDiagnosticMonitor(
        const talas::app::AnalysisDiagnosticIdentity& identity,
        juce::String summary) = 0;
    [[nodiscard]] virtual bool monitorFocusContainsForSmoke(
        const juce::String& text) const = 0;
    virtual void poll() = 0;
};

[[nodiscard]] std::unique_ptr<AnalysisPanel> makeAnalysisPanel();
}
