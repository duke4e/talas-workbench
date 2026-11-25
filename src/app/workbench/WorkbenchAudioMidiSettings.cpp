#include "app/workbench/WorkbenchAudioMidiSettings.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/project/VirtualControlOsc.h"

#include <algorithm>
#include <array>

namespace talas::app::workbench
{
namespace
{
class AudioMidiSettingsComponent final : public juce::Component,
                                         private juce::ChangeListener
{
public:
    AudioMidiSettingsComponent(
        juce::AudioDeviceManager& deviceManager,
        std::vector<WorkbenchTransportSyncSourceOption> syncSources,
        talas::project::ProjectTransportSyncSettings selectedSync,
        std::function<void(talas::project::ProjectTransportSyncSettings)>
            onSyncSourceChanged,
        talas::project::ProjectOscIngressSettings oscSettings,
        talas::project::ProjectOscEgressSettings oscEgressSettings,
        juce::String oscProfileState,
        std::function<juce::String(
            talas::project::ProjectOscIngressSettings,
            talas::project::ProjectOscEgressSettings)>
            onOscProfileChanged)
        : deviceManager_(deviceManager),
          selector_(deviceManager, 0, 8, 0, 8, true, false, false, false),
          syncSources_(std::move(syncSources)),
          selectedSync_(std::move(selectedSync)),
          onSyncSourceChanged_(std::move(onSyncSourceChanged)),
          oscSettings_(oscSettings),
          oscEgressSettings_(oscEgressSettings),
          onOscProfileChanged_(std::move(onOscProfileChanged))
    {
        addAndMakeVisible(selector_);
        syncSourceLabel_.setText(
            "Transport sync source",
            juce::dontSendNotification);
        syncSourceLabel_.setJustificationType(
            juce::Justification::centredLeft);
        syncSourceLabel_.setTitle(
            "Choose internal transport, MIDI Clock tempo and phase, or MTC SMPTE position from one exact MIDI input");
        addAndMakeVisible(syncSourceLabel_);

        refreshSyncSources(false);
        syncSourceBox_.setTooltip(
            "Internal keeps Workbench transport controls active. MIDI Clock follows musical tempo and phase. MTC follows SMPTE position. External sync never time-stretches audio.");
        syncSourceBox_.onChange = [this] {
            const auto selectedId = syncSourceBox_.getSelectedId();
            if (selectedId <= 1)
            {
                selectedSync_ = {};
                onSyncSourceChanged_(selectedSync_);
                return;
            }
            const auto sourceIndex = static_cast<std::size_t>(selectedId - 2);
            if (sourceIndex < syncSources_.size())
            {
                selectedSync_.sourceMode =
                    syncSources_[sourceIndex].sourceMode;
                selectedSync_.sourceIdentifier =
                    syncSources_[sourceIndex].identifier;
                onSyncSourceChanged_(selectedSync_);
            }
            else if (selectedSync_.sourceMode
                     != talas::project::TransportSyncSourceMode::Internal)
            {
                onSyncSourceChanged_(selectedSync_);
            }
        };
        addAndMakeVisible(syncSourceBox_);

        syncHelp_.setText(
            juceString(
                "Clock follows tempo and phase · MTC follows SMPTE position · audio is not time-stretched"),
            juce::dontSendNotification);
        syncHelp_.setJustificationType(
            juce::Justification::centredLeft);
        syncHelp_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        syncHelp_.setTitle(syncHelp_.getText());
        addAndMakeVisible(syncHelp_);

        oscEnabled_.setButtonText("Enable OSC input");
        oscEnabled_.setToggleState(
            oscSettings_.enabled,
            juce::dontSendNotification);
        oscEnabled_.setTooltip(
            "Listen on this Mac only. OSC NTP timetags use this Mac's local clock and may schedule up to 2 seconds ahead.");
        addAndMakeVisible(oscEnabled_);

        oscPortLabel_.setText("UDP port", juce::dontSendNotification);
        oscPortLabel_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(oscPortLabel_);
        oscPort_.setText(
            juce::String(static_cast<int>(oscSettings_.port)),
            false);
        oscPort_.setInputRestrictions(5, "0123456789");
        oscPort_.setTooltip("Local OSC UDP port, from 1024 to 65535.");
        oscPort_.onReturnKey = [this] { commitOscProfile(); };
        addAndMakeVisible(oscPort_);

        oscApply_.setButtonText("Apply profile");
        oscApply_.setTooltip("Apply both local OSC directions atomically.");
        oscApply_.onClick = [this] { commitOscProfile(); };
        addAndMakeVisible(oscApply_);

        oscState_.setText(
            std::move(oscProfileState), juce::dontSendNotification);
        oscState_.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(oscState_);
        oscHelp_.setText(
            juceString(
                "Loopback only · local-clock NTP ≤2 s · no authentication · no echo"),
            juce::dontSendNotification);
        oscHelp_.setJustificationType(juce::Justification::centredLeft);
        oscHelp_.setColour(juce::Label::textColourId, secondaryTextColour);
        oscHelp_.setTitle(oscHelp_.getText());
        addAndMakeVisible(oscHelp_);

        oscEgressEnabled_.setButtonText("Enable OSC feedback");
        oscEgressEnabled_.setToggleState(
            oscEgressSettings_.enabled,
            juce::dontSendNotification);
        oscEgressEnabled_.setTooltip(
            "Send persisted Event Value feedback to one app on this Mac only.");
        addAndMakeVisible(oscEgressEnabled_);

        oscEgressPortLabel_.setText(
            "Feedback UDP port",
            juce::dontSendNotification);
        oscEgressPortLabel_.setJustificationType(
            juce::Justification::centredLeft);
        addAndMakeVisible(oscEgressPortLabel_);
        oscEgressPort_.setText(
            juce::String(static_cast<int>(oscEgressSettings_.port)),
            false);
        oscEgressPort_.setInputRestrictions(5, "0123456789");
        oscEgressPort_.setTooltip(
            "Local OSC feedback destination port, from 1024 to 65535; it must differ from the listener port.");
        oscEgressPort_.onReturnKey = [this] { commitOscProfile(); };
        addAndMakeVisible(oscEgressPort_);

        oscDisableBoth_.setButtonText("Disable both");
        oscDisableBoth_.setTooltip(
            "Disable input and feedback as one OSC profile action.");
        oscDisableBoth_.onClick = [this] {
            oscEnabled_.setToggleState(false, juce::dontSendNotification);
            oscEgressEnabled_.setToggleState(
                false, juce::dontSendNotification);
            commitOscProfile();
        };
        addAndMakeVisible(oscDisableBoth_);

        // Section titles bold, explanations muted, in the theme's sizes.
        syncSourceLabel_.setFont(WorkbenchType::bodyStrong());
        for (auto* help : {&syncHelp_, &oscHelp_, &oscState_})
        {
            help->setFont(WorkbenchType::label());
            help->setColour(juce::Label::textColourId, secondaryTextColour);
        }
        for (auto* label : {&oscPortLabel_, &oscEgressPortLabel_})
            label->setFont(WorkbenchType::body());

        deviceManager_.addChangeListener(this);
        setSize(540, 820);
    }

    // Each section on a glass card over the window's backdrop: the devices,
    // the transport sync, and OSC.
    void paint(juce::Graphics& graphics) override
    {
        for (const auto& card : cards())
        {
            WorkbenchStyle::paintPanelShadow(graphics, card.toFloat(), WorkbenchStyle::panelRadius());
            WorkbenchStyle::paintPanel(graphics, card.toFloat(), WorkbenchStyle::panelRadius());
        }
    }

    ~AudioMidiSettingsComponent() override
    {
        deviceManager_.removeChangeListener(this);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(cardMargin);
        // The devices' card keeps its whole area, however tall the selector
        // makes itself for the device open.
        deviceCard_ = bounds.removeFromTop(480 + 2 * cardInset);
        selector_.setBounds(deviceCard_.reduced(cardInset));
        bounds.removeFromTop(cardMargin + cardInset);
        bounds.reduce(cardInset, 0);
        syncSourceLabel_.setBounds(bounds.removeFromTop(24));
        syncSourceBox_.setBounds(bounds.removeFromTop(30));
        bounds.removeFromTop(4);
        syncHelp_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(2 * cardInset + cardMargin);
        oscEnabled_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(3);
        auto portRow = bounds.removeFromTop(28);
        oscApply_.setBounds(portRow.removeFromRight(112));
        portRow.removeFromRight(6);
        oscPort_.setBounds(portRow.removeFromRight(96));
        portRow.removeFromRight(6);
        oscPortLabel_.setBounds(portRow);
        bounds.removeFromTop(3);
        oscHelp_.setBounds(bounds.removeFromTop(22));
        bounds.removeFromTop(10);
        oscEgressEnabled_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(3);
        auto egressPortRow = bounds.removeFromTop(28);
        oscDisableBoth_.setBounds(egressPortRow.removeFromRight(96));
        egressPortRow.removeFromRight(6);
        oscEgressPort_.setBounds(egressPortRow.removeFromRight(96));
        egressPortRow.removeFromRight(6);
        oscEgressPortLabel_.setBounds(egressPortRow);
        bounds.removeFromTop(3);
        oscState_.setBounds(bounds.removeFromTop(22));
    }

private:
    static constexpr int cardMargin = 10;
    static constexpr int cardInset = 8;
    juce::Rectangle<int> deviceCard_;

    [[nodiscard]] std::array<juce::Rectangle<int>, 3> cards() const
    {
        const auto around = [](juce::Rectangle<int> first, juce::Rectangle<int> last) {
            return first.getUnion(last).expanded(cardInset);
        };
        return {
            deviceCard_,
            around(syncSourceLabel_.getBounds(), syncHelp_.getBounds()),
            around(oscEnabled_.getBounds(), oscState_.getBounds()),
        };
    }

    void commitOscProfile()
    {
        auto nextIngress = oscSettings_;
        nextIngress.enabled = oscEnabled_.getToggleState();
        if (!nextIngress.enabled)
        {
            nextIngress.port = talas::project::defaultOscIngressPort;
            oscPort_.setText(
                juce::String(talas::project::defaultOscIngressPort), false);
        }
        else
        {
            const auto text = oscPort_.getText().trim();
            const auto port = text.getLargeIntValue();
            if (text.isEmpty() || !text.containsOnly("0123456789")
                || port < 1024 || port > 65535)
            {
                oscState_.setText(
                    "Enter an input UDP port from 1024 to 65535",
                    juce::dontSendNotification);
                return;
            }
            nextIngress.port = static_cast<std::int32_t>(port);
        }

        auto nextEgress = oscEgressSettings_;
        nextEgress.enabled = oscEgressEnabled_.getToggleState();
        if (!nextEgress.enabled)
        {
            nextEgress.port = talas::project::defaultOscEgressPort;
            oscEgressPort_.setText(
                juce::String(talas::project::defaultOscEgressPort),
                false);
        }
        else
        {
            const auto text = oscEgressPort_.getText().trim();
            const auto port = text.getLargeIntValue();
            if (text.isEmpty() || !text.containsOnly("0123456789")
                || port < 1024 || port > 65535)
            {
                oscState_.setText(
                    "Enter a feedback UDP port from 1024 to 65535",
                    juce::dontSendNotification);
                return;
            }
            nextEgress.port = static_cast<std::int32_t>(port);
        }
        if (!talas::project::isValidOscEndpointPair(
                nextIngress, nextEgress))
        {
            oscState_.setText(
                "Enabled OSC input and feedback ports must differ",
                juce::dontSendNotification);
            return;
        }
        oscSettings_ = nextIngress;
        oscEgressSettings_ = nextEgress;
        if (onOscProfileChanged_)
        {
            oscState_.setText(
                onOscProfileChanged_(oscSettings_, oscEgressSettings_),
                juce::dontSendNotification);
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster* source) override
    {
        if (source == &deviceManager_)
            refreshSyncSources(true);
    }

    void refreshSyncSources(bool queryDeviceManager)
    {
        if (queryDeviceManager)
        {
            auto devices = juce::MidiInput::getAvailableDevices();
            std::ranges::sort(devices, {}, [](const auto& device) {
                return device.identifier;
            });
            syncSources_.clear();
            for (const auto& device : devices)
            {
                if (deviceManager_.isMidiInputDeviceEnabled(
                        device.identifier))
                {
                    syncSources_.push_back({
                        device.name,
                        device.identifier.toStdString(),
                        talas::project::TransportSyncSourceMode::MidiClockExactInput,
                    });
                    syncSources_.push_back({
                        device.name,
                        device.identifier.toStdString(),
                        talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput,
                    });
                }
            }
        }

        syncSourceBox_.clear(juce::dontSendNotification);
        syncSourceBox_.addItem("Internal", 1);
        int selectedId = 1;
        int itemId = 2;
        for (const auto& source : syncSources_)
        {
            const auto authorityLabel = source.sourceMode
                    == talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput
                ? juce::String{"MTC"}
                : juce::String{"MIDI Clock"};
            auto deviceLabel = source.displayName;
            if (deviceLabel.isEmpty())
                deviceLabel = "MIDI Input";
            const auto label = authorityLabel + juceString(" · ")
                + deviceLabel + juceString(" — ")
                + juce::String(source.identifier);
            syncSourceBox_.addItem(label, itemId);
            if (selectedSync_.sourceMode == source.sourceMode
                && selectedSync_.sourceIdentifier == source.identifier)
            {
                selectedId = itemId;
            }
            ++itemId;
        }
        if (selectedSync_.sourceMode
                != talas::project::TransportSyncSourceMode::Internal
            && selectedId == 1)
        {
            const auto authorityLabel = selectedSync_.sourceMode
                    == talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput
                ? juce::String{"MTC"}
                : juce::String{"MIDI Clock"};
            syncSourceBox_.addItem(
                authorityLabel + juceString(" unavailable — ")
                    + juce::String(selectedSync_.sourceIdentifier),
                itemId);
            selectedId = itemId;
        }
        syncSourceBox_.setSelectedId(
            selectedId,
            juce::dontSendNotification);
    }

    juce::AudioDeviceManager& deviceManager_;
    juce::AudioDeviceSelectorComponent selector_;
    std::vector<WorkbenchTransportSyncSourceOption> syncSources_;
    talas::project::ProjectTransportSyncSettings selectedSync_;
    std::function<void(talas::project::ProjectTransportSyncSettings)>
        onSyncSourceChanged_;
    talas::project::ProjectOscIngressSettings oscSettings_;
    juce::Label syncSourceLabel_;
    juce::ComboBox syncSourceBox_;
    juce::Label syncHelp_;
    juce::ToggleButton oscEnabled_;
    juce::Label oscPortLabel_;
    juce::TextEditor oscPort_;
    juce::TextButton oscApply_;
    juce::Label oscState_;
    juce::Label oscHelp_;
    talas::project::ProjectOscEgressSettings oscEgressSettings_;
    std::function<juce::String(
        talas::project::ProjectOscIngressSettings,
        talas::project::ProjectOscEgressSettings)>
        onOscProfileChanged_;
    juce::ToggleButton oscEgressEnabled_;
    juce::Label oscEgressPortLabel_;
    juce::TextEditor oscEgressPort_;
    juce::TextButton oscDisableBoth_;
};
}

std::unique_ptr<juce::Component> createAudioMidiSettingsComponent(
    juce::AudioDeviceManager& deviceManager,
    std::vector<WorkbenchTransportSyncSourceOption> syncSources,
    talas::project::ProjectTransportSyncSettings selectedSync,
    std::function<void(talas::project::ProjectTransportSyncSettings)>
        onSyncSourceChanged,
    talas::project::ProjectOscIngressSettings oscSettings,
    talas::project::ProjectOscEgressSettings oscEgressSettings,
    juce::String oscProfileState,
    std::function<juce::String(
        talas::project::ProjectOscIngressSettings,
        talas::project::ProjectOscEgressSettings)>
        onOscProfileChanged)
{
    return std::make_unique<AudioMidiSettingsComponent>(
        deviceManager,
        std::move(syncSources),
        std::move(selectedSync),
        std::move(onSyncSourceChanged),
        oscSettings,
        oscEgressSettings,
        std::move(oscProfileState),
        std::move(onOscProfileChanged));
}
}
