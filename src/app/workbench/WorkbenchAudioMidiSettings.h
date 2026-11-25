#pragma once

#include "talas/project/ProjectDocument.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
struct WorkbenchTransportSyncSourceOption
{
    juce::String displayName;
    std::string identifier;
    talas::project::TransportSyncSourceMode sourceMode =
        talas::project::TransportSyncSourceMode::Internal;
};

[[nodiscard]] std::unique_ptr<juce::Component>
createAudioMidiSettingsComponent(
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
        onOscProfileChanged);
}
