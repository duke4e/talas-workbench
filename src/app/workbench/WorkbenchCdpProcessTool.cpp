#include "app/workbench/WorkbenchCdpProcessTool.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/AnalysisToolsModel.h"
#include "talas/audio/CdpProcessToSample.h"
#include "talas/cdp/SpectralWholeFile.h"
#include "talas/project/ProjectPathEncoding.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

namespace talas::app::workbench
{
namespace
{
class CdpProcessToolPanelImpl final
    : public CdpProcessToolPanel
    , private juce::Timer
{
public:
    CdpProcessToolPanelImpl(StatusCallback onStatus, CompletedCallback onCompleted)
        : onStatus_(std::move(onStatus))
        , onCompleted_(std::move(onCompleted))
    {
        setOpaque(true);
        heading_.setText("CDP Process to Sample", juce::dontSendNotification);
        heading_.setFont(WorkbenchType::panelTitle());
        addAndMakeVisible(heading_);
        const auto& processes = talas::cdp::offlineSpectralProcesses();
        for (std::size_t index = 0; index < processes.size(); ++index)
        {
            process_.addItem(juceString(processes[index].label) + " (" + juceString(processes[index].cdpCommand) + ")",
                static_cast<int>(index) + 1);
        }
        process_.onChange = [this] { showFields(); };
        describeControl(process_, "CDP process", "A CDP8 spectral process that changes a sound's length or order");
        addAndMakeVisible(process_);
        chooseInputs_.setButtonText("Choose Samples...");
        chooseInputs_.onClick = [this] { chooseInputs(); };
        addAndMakeVisible(chooseInputs_);
        inputsLabel_.setText("No sample chosen", juce::dontSendNotification);
        addAndMakeVisible(inputsLabel_);
        pointsLabel_.setText("Points, overlap", juce::dontSendNotification);
        addAndMakeVisible(pointsLabel_);
        points_.setText("1024", juce::dontSendNotification);
        overlap_.setText("3", juce::dontSendNotification);
        describeControl(points_, "Analysis points", "pvoc anal's -c, 2 to 8192");
        describeControl(overlap_, "Analysis overlap", "pvoc anal's -o, 1 to 4");
        addAndMakeVisible(points_);
        addAndMakeVisible(overlap_);
        create_.setButtonText("Process to Sample...");
        create_.onClick = [this] { chooseOutput(); };
        addAndMakeVisible(create_);
        cancel_.setButtonText("Cancel");
        cancel_.onClick = [this] { cancelRequested_.store(true); };
        addChildComponent(cancel_);
        addAndMakeVisible(progress_);
        process_.setSelectedId(1, juce::dontSendNotification);
        showFields();
        setSize(600, 420);
    }

    ~CdpProcessToolPanelImpl() override
    {
        stopTimer();
        cancelProcess();
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
        process_.setBounds(bounds.removeFromTop(28));
        bounds.removeFromTop(6);
        auto inputRow = bounds.removeFromTop(28);
        chooseInputs_.setBounds(inputRow.removeFromLeft(150));
        inputRow.removeFromLeft(8);
        inputsLabel_.setBounds(inputRow);
        bounds.removeFromTop(6);
        auto pointsRow = bounds.removeFromTop(28);
        pointsLabel_.setBounds(pointsRow.removeFromLeft(150));
        points_.setBounds(pointsRow.removeFromLeft(80));
        pointsRow.removeFromLeft(8);
        overlap_.setBounds(pointsRow.removeFromLeft(50));
        bounds.removeFromTop(6);
        auto buttons = bounds.removeFromBottom(28);
        auto status = bounds.removeFromBottom(24);
        progress_.setBounds(status);
        create_.setBounds(buttons.removeFromLeft(180));
        buttons.removeFromLeft(8);
        cancel_.setBounds(buttons.removeFromLeft(80));
        for (std::size_t index = 0; index < fieldEditors_.size(); ++index)
        {
            const auto multiLine = fieldEditors_[index]->isMultiLine();
            auto line = bounds.removeFromTop(multiLine ? 54 : 28);
            fieldLabels_[index]->setBounds(line.removeFromLeft(150));
            fieldEditors_[index]->setBounds(line);
            bounds.removeFromTop(4);
        }
    }

    void cancelProcess() override
    {
        cancelRequested_.store(true);
        if (worker_.joinable())
            worker_.join();
    }

    [[nodiscard]] bool setInputsForSmoke(const std::vector<std::filesystem::path>& inputs) override
    {
        return setInputs(inputs);
    }

    [[nodiscard]] bool setProcessForSmoke(
        const std::string& process,
        int points,
        const std::map<std::string, std::string>& fields) override
    {
        const auto& processes = talas::cdp::offlineSpectralProcesses();
        for (std::size_t index = 0; index < processes.size(); ++index)
        {
            if (processes[index].id != process)
                continue;
            process_.setSelectedId(static_cast<int>(index) + 1, juce::dontSendNotification);
            showFields();
            points_.setText(juce::String(points), juce::dontSendNotification);
            for (const auto& [id, value] : fields)
            {
                const auto field = std::ranges::find(fieldIds_, id);
                if (field == fieldIds_.end())
                    return false;
                fieldEditors_[static_cast<std::size_t>(field - fieldIds_.begin())]->setText(
                    juceString(value), juce::dontSendNotification);
            }
            return true;
        }
        return false;
    }

    [[nodiscard]] bool processNowForSmoke(const std::filesystem::path& output) override
    {
        if (!start(output))
            return false;
        if (worker_.joinable())
            worker_.join();
        return finishIfDone();
    }

private:
    [[nodiscard]] const talas::cdp::OfflineSpectralProcess* selected() const
    {
        const auto& processes = talas::cdp::offlineSpectralProcesses();
        const auto index = process_.getSelectedId() - 1;
        return index >= 0 && static_cast<std::size_t>(index) < processes.size()
            ? &processes[static_cast<std::size_t>(index)]
            : nullptr;
    }

    // One editor a field, holding its text (a number, a breakpoint table or
    // a data file's lines, a mode's name, true or false).
    void showFields()
    {
        for (auto& editor : fieldEditors_)
            removeChildComponent(editor.get());
        for (auto& label : fieldLabels_)
            removeChildComponent(label.get());
        fieldEditors_.clear();
        fieldLabels_.clear();
        fieldIds_.clear();
        const auto* process = selected();
        if (process == nullptr)
            return;
        for (const auto& field : process->fields)
        {
            auto label = std::make_unique<juce::Label>();
            label->setText(juceString(field.label), juce::dontSendNotification);
            auto editor = std::make_unique<juce::TextEditor>();
            const auto multiLine = field.kind == talas::cdp::OfflineSpectralField::Kind::Breakpoints
                || field.kind == talas::cdp::OfflineSpectralField::Kind::Text;
            editor->setMultiLine(multiLine);
            editor->setReturnKeyStartsNewLine(multiLine);
            editor->setText(juceString(field.defaultValue), juce::dontSendNotification);
            juce::String help = juceString(field.help);
            if (!field.choices.empty())
            {
                help += " (";
                for (std::size_t choice = 0; choice < field.choices.size(); ++choice)
                    help += (choice == 0 ? "" : ", ") + juceString(field.choices[choice]);
                help += ")";
            }
            editor->setTooltip(help);
            describeControl(*editor, juceString(field.label), help);
            addAndMakeVisible(*label);
            addAndMakeVisible(*editor);
            fieldIds_.emplace_back(field.id);
            fieldLabels_.push_back(std::move(label));
            fieldEditors_.push_back(std::move(editor));
        }
        resized();
    }

    void chooseInputs()
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Choose Samples to Process",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            "*.wav;*.aif;*.aiff;*.aifc");
        juce::Component::SafePointer<CdpProcessToolPanelImpl> safeThis(this);
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::canSelectMultipleItems,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResults().isEmpty())
                    return;
                std::vector<std::filesystem::path> inputs;
                for (const auto& file : chooser.getResults())
                    inputs.push_back(talas::project::pathFromUtf8(file.getFullPathName().toStdString()));
                (void)safeThis->setInputs(inputs);
            });
    }

    bool setInputs(const std::vector<std::filesystem::path>& inputs)
    {
        if (running_)
        {
            onStatus_("Wait for the running process or cancel it first");
            return false;
        }
        for (const auto& input : inputs)
        {
            if (!std::filesystem::is_regular_file(input))
            {
                onStatus_("The sample " + juceString(talas::project::pathToUtf8(input)) + " is not a file");
                return false;
            }
        }
        inputs_ = inputs;
        juce::String names;
        for (const auto& input : inputs_)
            names += (names.isEmpty() ? "" : ", ") + juceString(talas::project::pathToUtf8(input.filename()));
        inputsLabel_.setText(inputs_.empty() ? juce::String{"No sample chosen"} : names, juce::dontSendNotification);
        return !inputs_.empty();
    }

    void chooseOutput()
    {
        const auto* process = selected();
        if (inputs_.empty() || process == nullptr)
        {
            onStatus_("Choose a sample to process first");
            return;
        }
        const auto initial = talas::app::analysisOutputPath(
            inputs_.front(), " " + std::string{process->cdpCommand} + ".wav");
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save the Processed Sample", juce::File{juceString(talas::project::pathToUtf8(initial))}, "*.wav");
        juce::Component::SafePointer<CdpProcessToolPanelImpl> safeThis(this);
        chooser_->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis == nullptr || chooser.getResult() == juce::File{})
                    return;
                (void)safeThis->start(talas::project::pathFromUtf8(chooser.getResult().getFullPathName().toStdString()));
            });
    }

    bool start(const std::filesystem::path& output)
    {
        const auto* process = selected();
        if (running_ || inputs_.empty() || process == nullptr)
        {
            onStatus_(running_ ? "A process is already running" : "Choose a sample to process first");
            return false;
        }
        if (worker_.joinable())
            worker_.join();
        talas::cdp::OfflineSpectralRequest request;
        request.process = std::string{process->id};
        request.points = points_.getText().getIntValue();
        request.overlap = overlap_.getText().getIntValue();
        for (std::size_t index = 0; index < fieldIds_.size(); ++index)
            request.fields[fieldIds_[index]] = fieldEditors_[index]->getText().toStdString();
        output_ = output;
        running_ = true;
        cancelRequested_.store(false);
        done_.store(false);
        cancel_.setVisible(true);
        create_.setEnabled(false);
        progress_.setText("Processing...", juce::dontSendNotification);
        try
        {
            worker_ = std::thread([this, request = std::move(request), inputs = inputs_, output] {
                auto result = talas::audio::processSamplesThroughCdp(request, inputs, output);
                {
                    const std::scoped_lock lock{outcomeMutex_};
                    outcome_ = std::move(result);
                }
                done_.store(true);
            });
        }
        catch (const std::system_error&)
        {
            running_ = false;
            onStatus_("The process could not start");
            return false;
        }
        startTimerHz(10);
        return true;
    }

    void timerCallback() override
    {
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
        cancel_.setVisible(false);
        create_.setEnabled(true);
        talas::audio::CdpProcessToSampleResult outcome;
        {
            const std::scoped_lock lock{outcomeMutex_};
            outcome = std::move(outcome_);
        }
        // CDP's process runs to its end; a cancelled one leaves its file
        // unused.
        if (!outcome.ok() || cancelRequested_.load())
        {
            const auto message = outcome.ok() ? juce::String{"Process cancelled"} : juceString(outcome.error);
            progress_.setText(message, juce::dontSendNotification);
            onStatus_(message);
            return false;
        }
        const auto summary = "Processed sample: " + juce::String(static_cast<juce::int64>(outcome.frames))
            + " frames at " + juce::String(static_cast<int>(outcome.sampleRate)) + " Hz";
        progress_.setText(summary, juce::dontSendNotification);
        if (onCompleted_)
            onCompleted_({output_, summary});
        return true;
    }

    StatusCallback onStatus_;
    CompletedCallback onCompleted_;
    std::vector<std::filesystem::path> inputs_;
    std::filesystem::path output_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::thread worker_;
    std::atomic<bool> cancelRequested_{false};
    std::atomic<bool> done_{false};
    bool running_ = false;
    std::mutex outcomeMutex_;
    talas::audio::CdpProcessToSampleResult outcome_;
    juce::Label heading_;
    juce::ComboBox process_;
    juce::TextButton chooseInputs_;
    juce::Label inputsLabel_;
    juce::Label pointsLabel_;
    juce::TextEditor points_;
    juce::TextEditor overlap_;
    std::vector<std::string> fieldIds_;
    std::vector<std::unique_ptr<juce::Label>> fieldLabels_;
    std::vector<std::unique_ptr<juce::TextEditor>> fieldEditors_;
    juce::TextButton create_;
    juce::TextButton cancel_;
    juce::Label progress_;
};
}

std::unique_ptr<CdpProcessToolPanel> makeCdpProcessToolPanel(
    CdpProcessToolPanel::StatusCallback onStatus,
    CdpProcessToolPanel::CompletedCallback onCompleted)
{
    return std::make_unique<CdpProcessToolPanelImpl>(std::move(onStatus), std::move(onCompleted));
}
}
