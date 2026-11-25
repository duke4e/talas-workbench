#include "app/workbench/WorkbenchAnalysisTools.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/AnalysisToolsModel.h"
#include "talas/audio/GaAnalysis.h"
#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/audio/ReAnalysisFiles.h"
#include "talas/audio/WavReader.h"
#include "talas/project/SosSpectrumFile.h"
#include "talas/project/ProjectPathEncoding.h"

#include <array>
#include <atomic>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
struct AnalysisJob
{
    AnalysisToolPanel::Kind kind = AnalysisToolPanel::Kind::Re;
    std::vector<std::vector<double>> channels;
    double sampleRate = 0.0;
    std::filesystem::path sample;
    std::filesystem::path output;
    std::filesystem::path excitation;
    talas::dsp::ReAnalysisSettings re;
    talas::audio::GaAnalysisSettings ga;
    // A GA analysis of a quasi-harmonic spectrum file, as the tool takes.
    std::optional<talas::project::PartialTrackAsset> spectrum;
};

struct AnalysisOutcome
{
    bool ok = false;
    juce::String message;
    double basePitch = 0.0;
    double durationSeconds = 0.0;
};

[[nodiscard]] AnalysisOutcome runAnalysisJob(const AnalysisJob& job, const std::function<void(talas::audio::GaAnalysisStage)>& stage)
{
    if (job.kind == AnalysisToolPanel::Kind::Ga)
    {
        const auto result = job.spectrum
            ? talas::audio::writeGaAnalysisFileFromSpectrum(*job.spectrum, job.ga, job.output, stage)
            : talas::audio::writeGaAnalysisFile(job.channels, job.sampleRate, job.ga, job.output);
        if (!result.ok())
            return {false, juceString(result.error)};
        return {true,
            "GA analysis: " + juce::String(static_cast<int>(job.ga.waveforms)) + " waveforms of "
                + juce::String(static_cast<int>(result.harmonics)) + " harmonics, base pitch "
                + juce::String(result.basePitch, 1) + " Hz",
            result.basePitch, result.durationSeconds};
    }
    const auto result = talas::audio::writeReAnalysisFiles(
        job.channels, job.sampleRate, job.re, job.output, job.excitation);
    if (!result.ok())
        return {false, juceString(result.error)};
    auto message = "RE table: order " + juce::String(static_cast<int>(result.order)) + ", "
        + juce::String(static_cast<int>(result.coefficientSets)) + " coefficient sets";
    if (result.excitationWritten)
    {
        message += " · EX file written";
        if (result.excitationScale != 1.0)
            message += ", scaled by " + juce::String(result.excitationScale, 4) + " to fit";
    }
    return {true, message};
}

class AnalysisToolPanelImpl final
    : public AnalysisToolPanel
    , private juce::Timer
{
public:
    AnalysisToolPanelImpl(Kind kind, StatusCallback onStatus, CompletedCallback onCompleted, AuditionCallback onAudition)
        : kind_(kind)
        , onStatus_(std::move(onStatus))
        , onCompleted_(std::move(onCompleted))
        , onAudition_(std::move(onAudition))
    {
        setOpaque(true);
        heading_.setText(
            kind_ == Kind::Re ? "RE Analysis" : "GA Analysis",
            juce::dontSendNotification);
        heading_.setFont(WorkbenchType::panelTitle());
        addAndMakeVisible(heading_);
        // GA Analysis takes a harmonic spectrum file (or,
        // ours, a sample whose harmonics it finds).
        // Browse... chooses the sample, or GA's spectrum.
        chooseSample_.setButtonText("Browse...");
        chooseSample_.onClick = [this] { chooseSample(); };
        addAndMakeVisible(chooseSample_);
        sampleLabel_.setText("No sample chosen", juce::dontSendNotification);
        addAndMakeVisible(sampleLabel_);
        progress_.setText({}, juce::dontSendNotification);
        addAndMakeVisible(progress_);
        addChildComponent(thermometer_);
        cancel_.setButtonText("Cancel");
        cancel_.onClick = [this] { cancelRequested_.store(true); };
        addChildComponent(cancel_);
        if (kind_ == Kind::Ga)
        {
            // The harmonics and the waveforms (oscillators),
            // 124 and 3 by default.
            order_.setRange(1.0, 1024.0, 1.0);
            order_.setValue(124.0, juce::dontSendNotification);
            updates_.setRange(1.0, 64.0, 1.0);
            updates_.setValue(3.0, juce::dontSendNotification);
            averaging_.setRange(1.0, 1000.0, 1.0);
            averaging_.setValue(100.0, juce::dontSendNotification);
            for (auto* slider : {&order_, &updates_, &averaging_})
            {
                slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 22);
                addAndMakeVisible(*slider);
            }
            describeControl(order_, "Number of harmonics", "The harmonics the complex waveforms hold");
            describeControl(updates_, "Number of waveforms", "The complex waveforms, one an oscillator");
            describeControl(averaging_, "Envelope frames a second");
            create_.setButtonText("Create GA File...");
            create_.onClick = [this] { chooseOutput(false); };
            addAndMakeVisible(create_);
        }
        else
        {
            // The RE Analysis window's parameters.
            const auto parameters = talas::app::reAnalysisWindowParameters();
            const std::array sliders{&order_, &updates_, &averaging_};
            for (std::size_t index = 0; index < sliders.size() && index < parameters.size(); ++index)
            {
                auto* slider = sliders[index];
                slider->setRange(parameters[index].minimum, parameters[index].maximum, 1.0);
                slider->setValue(parameters[index].defaultValue, juce::dontSendNotification);
                if (!parameters[index].unit.empty())
                    slider->setTextValueSuffix(" " + juceString(parameters[index].unit));
                slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 22);
                addAndMakeVisible(*slider);
            }
            describeControl(order_, "Filter order",
                "About twice the number of resonant peaks to follow");
            describeControl(updates_, "Filter updates per second");
            describeControl(averaging_, "Averaging time",
                "About twice the period of the sample's fundamental");
            create_.setButtonText("Create RE File Only...");
            create_.onClick = [this] { chooseOutput(false); };
            createBoth_.setButtonText("Create RE and EX Files...");
            createBoth_.onClick = [this] { chooseOutput(true); };
            addAndMakeVisible(create_);
            addAndMakeVisible(createBoth_);
        }
        for (auto* label : {&labelA_, &labelB_, &labelC_})
            addAndMakeVisible(*label);
        const auto reParameters = talas::app::reAnalysisWindowParameters();
        labelA_.setText(kind_ == Kind::Ga ? "Harmonics" : juceString(reParameters[0].name), juce::dontSendNotification);
        labelB_.setText(kind_ == Kind::Ga ? "Waveforms" : juceString(reParameters[1].name), juce::dontSendNotification);
        labelC_.setText(kind_ == Kind::Ga ? "Frames a second" : juceString(reParameters[2].name), juce::dontSendNotification);
        setSize(560, 300);
    }

    ~AnalysisToolPanelImpl() override
    {
        stopTimer();
        cancelAnalysis();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(12);
        heading_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(8);
        auto sampleRow = bounds.removeFromTop(28);
        chooseSample_.setBounds(sampleRow.removeFromLeft(140));
        sampleRow.removeFromLeft(8);
        sampleLabel_.setBounds(sampleRow);
        bounds.removeFromTop(10);
        const auto row = [&](juce::Label& label, juce::Component& control) {
            auto line = bounds.removeFromTop(28);
            label.setBounds(line.removeFromLeft(140));
            control.setBounds(line);
            bounds.removeFromTop(6);
        };
        row(labelA_, order_);
        row(labelB_, updates_);
        row(labelC_, averaging_);
        bounds.removeFromTop(6);
        auto buttons = bounds.removeFromTop(28);
        create_.setBounds(buttons.removeFromLeft(190));
        buttons.removeFromLeft(8);
        if (kind_ == Kind::Re)
        {
            createBoth_.setBounds(buttons.removeFromLeft(200));
            buttons.removeFromLeft(8);
        }
        cancel_.setBounds(buttons.removeFromLeft(80));
        bounds.removeFromTop(6);
        progress_.setBounds(bounds.removeFromTop(24));
        thermometer_.setBounds(bounds.removeFromTop(10));
    }

    void cancelAnalysis() override
    {
        cancelRequested_.store(true);
        if (worker_.joinable())
            worker_.join();
    }

    [[nodiscard]] bool setSampleForSmoke(const std::filesystem::path& sample) override
    {
        return loadSample(sample);
    }

    [[nodiscard]] bool analyzeNowForSmoke(
        const std::filesystem::path& output,
        bool withExcitation) override
    {
        if (!startAnalysis(output, withExcitation))
            return false;
        if (worker_.joinable())
            worker_.join();
        return finishIfDone();
    }

    void setSpectralSettingsForSmoke(int, int, bool) override {}

private:
    void chooseSample()
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            kind_ == Kind::Ga ? "Choose a Quasi-Harmonic Spectrum or a Sample to Analyze" : "Choose a Sample to Analyze",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            kind_ == Kind::Ga ? "*.talaspartial;*.spc;*.wav;*.aif;*.aiff;*.aifc" : "*.wav;*.aif;*.aiff;*.aifc");
        juce::Component::SafePointer<AnalysisToolPanelImpl> safeThis(this);
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void)safeThis->loadSample(
                    talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
            });
    }

    bool loadSample(const std::filesystem::path& sample)
    {
        if (running_)
        {
            onStatus_("Wait for the running analysis or cancel it first");
            return false;
        }
        if (kind_ == Kind::Ga && talas::project::isSpectrumFilePath(sample))
            return loadSpectrum(sample);
        spectrum_.reset();
        auto read = talas::audio::readSampleFile(sample);
        if (!read.ok() || read.channels.empty() || read.channels.front().empty())
        {
            const auto message = "The sample could not be read: "
                + juceString(read.ok() ? std::string{"it has no frames"} : read.error);
            sampleLabel_.setText(message, juce::dontSendNotification);
            onStatus_(message);
            return false;
        }
        sample_ = sample;
        channels_ = std::move(read.channels);
        sampleRate_ = static_cast<double>(read.sampleRate);
        // The tool plays the sample chosen, to check
        // it is the one wanted.
        if (onAudition_)
            onAudition_(channels_.front(), sampleRate_);
        const auto seconds = static_cast<double>(channels_.front().size()) / sampleRate_;
        sampleLabel_.setText(
            juceString(talas::project::pathToUtf8(sample.filename())) + " · "
                + juce::String(seconds, 2) + " s, " + juce::String(read.sampleRate) + " Hz, "
                + (channels_.size() == 1 ? "mono" : juce::String(static_cast<int>(channels_.size())) + " channels"),
            juce::dontSendNotification);
        return true;
    }

    bool loadSpectrum(const std::filesystem::path& file)
    {
        std::ifstream input(file, std::ios::binary);
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        auto decoded = talas::project::decodePartialTrackAsset(bytes);
        if (!decoded.ok() || decoded.asset.frames.empty())
        {
            const juce::String message = "The spectrum file could not be read";
            sampleLabel_.setText(message, juce::dontSendNotification);
            onStatus_(message);
            return false;
        }
        std::uint64_t harmonics = 0;
        for (const auto& frame : decoded.asset.frames)
        {
            for (const auto& point : frame.points)
                harmonics = std::max(harmonics, point.trackId);
        }
        sample_ = file;
        channels_.clear();
        sampleRate_ = decoded.asset.sampleRate;
        sampleLabel_.setText(juceString(talas::project::pathToUtf8(file.filename())) + " · "
                + juce::String(static_cast<juce::int64>(decoded.asset.frames.size())) + " frames, "
                + juce::String(static_cast<juce::int64>(harmonics)) + " harmonics",
            juce::dontSendNotification);
        spectrum_ = std::move(decoded.asset);
        // The tool plays the spectrum's resynthesis, to check
        // it is the one wanted.
        if (onAudition_)
        {
            auto rendered = talas::audio::renderPartialTrackAssetOffline(*spectrum_);
            if (rendered.ok())
                onAudition_(std::move(rendered.samples), sampleRate_);
        }
        return true;
    }

    [[nodiscard]] bool hasSource() const noexcept { return !channels_.empty() || spectrum_.has_value(); }

    void chooseOutput(bool withExcitation)
    {
        if (!hasSource())
        {
            onStatus_("Choose a sample to analyze first");
            return;
        }
        // An RE file goes into the RE folder of the
        // Wavetables folder; a GA file beside its sample.
        const auto talasFolder = talas::project::pathFromUtf8(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                                              .getChildFile("Talas")
                                                              .getFullPathName()
                                                              .toStdString());
        const auto initial = kind_ == Kind::Ga ? talas::app::analysisOutputPath(sample_, " GA.wav")
                                               : talas::app::reAnalysisOutputPath(sample_, talasFolder);
        if (kind_ != Kind::Ga)
        {
            std::error_code ignored;
            std::filesystem::create_directories(initial.parent_path(), ignored);
        }
        chooser_ = std::make_unique<juce::FileChooser>(
            kind_ == Kind::Ga ? "Save the GA File" : "Save the RE File",
            juce::File{juceString(talas::project::pathToUtf8(initial))},
            kind_ == Kind::Ga ? "*.wav" : "*.txt");
        juce::Component::SafePointer<AnalysisToolPanelImpl> safeThis(this);
        chooser_->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis, withExcitation](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void)safeThis->startAnalysis(
                    talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()),
                    withExcitation);
            });
    }

    bool startAnalysis(const std::filesystem::path& output, bool withExcitation)
    {
        if (running_ || !hasSource())
        {
            onStatus_(running_ ? "An analysis is already running" : "Choose a sample to analyze first");
            return false;
        }
        if (worker_.joinable())
            worker_.join();
        AnalysisJob job;
        job.kind = kind_;
        job.channels = channels_;
        job.spectrum = kind_ == Kind::Ga ? spectrum_ : std::nullopt;
        job.sampleRate = sampleRate_;
        job.sample = sample_;
        job.output = output;
        if (kind_ == Kind::Ga)
        {
            job.ga.harmonics = static_cast<std::size_t>(order_.getValue());
            job.ga.waveforms = static_cast<std::size_t>(updates_.getValue());
            job.ga.framesPerSecond = averaging_.getValue();
        }
        else
        {
            job.re.order = static_cast<std::size_t>(order_.getValue());
            job.re.updatesPerSecond = updates_.getValue();
            job.re.averagingTimeSeconds = averaging_.getValue() / 1000.0;
            if (withExcitation)
            {
                auto name = talas::project::pathToUtf8(output.stem());
                if (name.ends_with(" RE"))
                    name.erase(name.size() - 3);
                job.excitation = output.parent_path() / talas::project::pathFromUtf8(name + " EX.wav");
            }
        }
        completed_ = Completed{};
        completed_.kind = kind_;
        completed_.sample = sample_;
        completed_.output = output;
        completed_.excitation = job.excitation;
        completed_.durationSeconds = spectrum_
            ? static_cast<double>(spectrum_->sourceFrameCount) / sampleRate_
            : static_cast<double>(channels_.front().size()) / sampleRate_;
        running_ = true;
        cancelRequested_.store(false);
        done_.store(false);
        cancel_.setVisible(true);
        create_.setEnabled(false);
        createBoth_.setEnabled(false);
        progress_.setText("Analyzing...", juce::dontSendNotification);
        stage_.store(0);
        shownStage_ = 0;
        progressValue_ = 0.0;
        thermometer_.setVisible(true);
        try
        {
            worker_ = std::thread([this, job = std::move(job)] {
                auto outcome = runAnalysisJob(job, [this](talas::audio::GaAnalysisStage stage) {
                    stage_.store(static_cast<int>(stage) + 1);
                });
                {
                    const std::scoped_lock lock{outcomeMutex_};
                    outcome_ = std::move(outcome);
                }
                done_.store(true);
            });
        }
        catch (const std::system_error&)
        {
            running_ = false;
            onStatus_("The analysis could not start");
            return false;
        }
        startTimerHz(10);
        return true;
    }

    // Each stage of the GA analysis shown as it begins, with
    // a progress thermometer.
    void timerCallback() override
    {
        if (const auto stage = stage_.load(); stage != shownStage_ && stage > 0)
        {
            shownStage_ = stage;
            progressValue_ = stage == 1 ? 0.25 : 0.75;
            progress_.setText(juceString(talas::audio::gaAnalysisStageText(static_cast<talas::audio::GaAnalysisStage>(stage - 1))),
                juce::dontSendNotification);
        }
        (void)finishIfDone();
    }

    bool finishIfDone()
    {
        if (!running_ || !done_.load())
            return false;
        stopTimer();
        if (worker_.joinable())
            worker_.join();
        running_ = false;
        thermometer_.setVisible(false);
        cancel_.setVisible(false);
        create_.setEnabled(true);
        createBoth_.setEnabled(true);
        AnalysisOutcome outcome;
        {
            const std::scoped_lock lock{outcomeMutex_};
            outcome = std::move(outcome_);
        }
        if (!outcome.ok)
        {
            const auto message = cancelRequested_.load()
                ? juce::String{"Analysis cancelled"}
                : outcome.message;
            progress_.setText(message, juce::dontSendNotification);
            onStatus_(message);
            return false;
        }
        progress_.setText(outcome.message, juce::dontSendNotification);
        completed_.summary = outcome.message;
        if (kind_ == Kind::Ga)
        {
            completed_.basePitch = outcome.basePitch;
            completed_.durationSeconds = outcome.durationSeconds;
        }
        if (onCompleted_)
            onCompleted_(completed_);
        return true;
    }

    Kind kind_;
    StatusCallback onStatus_;
    CompletedCallback onCompleted_;
    AuditionCallback onAudition_;
    std::filesystem::path sample_;
    std::vector<std::vector<double>> channels_;
    std::optional<talas::project::PartialTrackAsset> spectrum_;
    double sampleRate_ = 0.0;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::thread worker_;
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> done_{false};
    std::atomic<int> stage_{0};
    int shownStage_ = 0;
    double progressValue_ = 0.0;
    juce::ProgressBar thermometer_{progressValue_};
    bool running_ = false;
    std::mutex outcomeMutex_;
    AnalysisOutcome outcome_;
    Completed completed_;
    juce::Label heading_;
    juce::TextButton chooseSample_;
    juce::Label sampleLabel_;
    juce::Label labelA_;
    juce::Label labelB_;
    juce::Label labelC_;
    juce::Slider order_;
    juce::Slider updates_;
    juce::Slider averaging_;
    juce::TextButton create_;
    juce::TextButton createBoth_;
    juce::TextButton cancel_;
    juce::Label progress_;
};
}

std::unique_ptr<AnalysisToolPanel> makeAnalysisToolPanel(
    AnalysisToolPanel::Kind kind,
    AnalysisToolPanel::StatusCallback onStatus,
    AnalysisToolPanel::CompletedCallback onCompleted,
    AnalysisToolPanel::AuditionCallback onAudition)
{
    if (kind == AnalysisToolPanel::Kind::Spectral)
        return makeSpectralAnalysisWizard(std::move(onStatus), std::move(onCompleted), std::move(onAudition));
    return std::make_unique<AnalysisToolPanelImpl>(
        kind, std::move(onStatus), std::move(onCompleted), std::move(onAudition));
}
}
