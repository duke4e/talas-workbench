#pragma once

#include "talas/app/AssetRecoveryWorkbenchModel.h"
#include "talas/app/WorkbenchSurfaceState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace talas::app::workbench
{
struct AssetRecoveryPanelCallbacks
{
    std::function<void(std::string)> locateFile;
    std::function<void()> searchFolder;
    std::function<void()> applyRelocations;
    std::function<void(std::string)> replace;
    std::function<void(std::string)> ignore;
    std::function<void()> cancel;
    std::function<void(std::string, std::size_t)> selectCandidate;
    std::function<void()> focusIdentityChanged;
};

class AssetRecoveryPanel : public juce::Component,
                           public talas::app::WorkbenchSurfaceStateSource
{
public:
    ~AssetRecoveryPanel() override = default;

    virtual void setState(
        const talas::app::AssetRecoveryWorkbenchState& state) = 0;
    [[nodiscard]] virtual bool focusAsset(std::string_view assetId) = 0;
    [[nodiscard]] virtual bool hasUsableStateForSmoke() const = 0;
    [[nodiscard]] virtual bool confirmationVisibleForSmoke() const = 0;
    [[nodiscard]] virtual bool exactApplyVisibleForSmoke() const = 0;
    [[nodiscard]] virtual bool activeOperationVisibleForSmoke() const = 0;
};

[[nodiscard]] std::unique_ptr<AssetRecoveryPanel>
makeAssetRecoveryPanel(AssetRecoveryPanelCallbacks callbacks);
}
