#pragma once

#include "talas/app/AnalysisToolsModel.h"
#include "talas/app/SpectralAnalysisSteps.h"
#include "talas/audio/QuasiHarmonicAnalysis.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace talas::app::workbench
{
// The Tools menu's Spectral Analysis and RE Analysis tools: choose a
// sample, set the analysis, write the file or files, and leave an example
// Sound that plays them. Spectral Analysis guides its steps, from selecting
// the sample to a harmonic spectrum file.
class AnalysisToolPanel : public juce::Component
{
public:
    enum class Kind
    {
        Spectral,
        Re,
        // The GA Analysis tool.
        Ga,
    };

    struct Completed
    {
        Kind kind = Kind::Spectral;
        std::filesystem::path sample;
        // The spectrum file, or the RE table.
        std::filesystem::path output;
        // A stereo sample's right channel spectrum file, beside the left's
        // in output.
        std::filesystem::path secondOutput;
        // The EX file, when one was written.
        std::filesystem::path excitation;
        double durationSeconds = 0.0;
        // A GA analysis's base pitch.
        double basePitch = 0.0;
        juce::String summary;
        // A harmonic analysis: its
        // SumOfSines table, the table's frames and harmonics, and the
        // pitch given for it.
        std::filesystem::path harmonicTable;
        std::size_t harmonicFrames = 0;
        std::size_t harmonicPartials = 0;
        double pitchHz = 0.0;
    };

    using SpectralStep = talas::app::SpectralAnalysisStep;

    enum class TraceAction
    {
        AutoTraceLower,
        AutoTraceHigher,
        HandTrace,
    };

    using StatusCallback = std::function<void(const juce::String&)>;
    using CompletedCallback = std::function<void(Completed)>;
    // A mono buffer for the Workbench to play looped, or none to stop.
    using AuditionCallback = std::function<void(std::vector<double> samples, double sampleRate)>;

    ~AnalysisToolPanel() override = default;

    // Stops a running analysis and waits for it.
    virtual void cancelAnalysis() = 0;
    // Smoke access: the sample a Choose... would pick, and the analysis a
    // Create button would start, run to its end before returning.
    [[nodiscard]] virtual bool setSampleForSmoke(
        const std::filesystem::path& sample) = 0;
    [[nodiscard]] virtual bool analyzeNowForSmoke(
        const std::filesystem::path& output,
        bool withExcitation) = 0;
    virtual void setSpectralSettingsForSmoke(
        int lowestSetting,
        int response,
        bool quasiHarmonic) = 0;
    // Smoke access to Spectral Analysis's steps: the step shown, Next and
    // Back, Audition run to its end, the oscillators it uses, the
    // unpitched fader, the trace and its buttons, what the trace's step
    // plays, and Create Quasi-Harmonic Spectrum File with the pitch given.
    [[nodiscard]] virtual SpectralStep spectralStepForSmoke() const { return SpectralStep::Select; }
    virtual bool nextForSmoke() { return false; }
    virtual bool backForSmoke() { return false; }
    [[nodiscard]] virtual bool auditionNowForSmoke() { return false; }
    [[nodiscard]] virtual std::size_t oscillatorsForSmoke() const { return 0; }
    virtual void setUnpitchedForSmoke(double) {}
    [[nodiscard]] virtual std::vector<talas::audio::FundamentalPoint> traceForSmoke() const { return {}; }
    virtual bool traceActionForSmoke(TraceAction) { return false; }
    virtual bool setTracedHarmonicForSmoke(int) { return false; }
    virtual bool addTracePointForSmoke(talas::audio::FundamentalPoint) { return false; }
    virtual bool deleteTracePointForSmoke(std::size_t) { return false; }
    virtual bool setTraceAuditionForSmoke(talas::app::SpectralAnalysisAudition) { return false; }
    [[nodiscard]] virtual bool createSpectrumNowForSmoke(const std::filesystem::path&, talas::app::StereoAnalysisChoice)
    {
        return false;
    }
    [[nodiscard]] virtual bool createHarmonicNowForSmoke(const std::filesystem::path&, double) { return false; }
};

[[nodiscard]] std::unique_ptr<AnalysisToolPanel> makeAnalysisToolPanel(
    AnalysisToolPanel::Kind kind,
    AnalysisToolPanel::StatusCallback onStatus,
    AnalysisToolPanel::CompletedCallback onCompleted,
    AnalysisToolPanel::AuditionCallback onAudition = {});

// Spectral Analysis's guided steps (WorkbenchSpectralAnalysisWizard.cpp).
[[nodiscard]] std::unique_ptr<AnalysisToolPanel> makeSpectralAnalysisWizard(
    AnalysisToolPanel::StatusCallback onStatus,
    AnalysisToolPanel::CompletedCallback onCompleted,
    AnalysisToolPanel::AuditionCallback onAudition);
}
