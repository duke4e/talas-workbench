#pragma once

#include "talas/project/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
enum class SampleAssetPresentation
{
    Ready,
    Missing,
    Changed,
};

struct SampleRegionInspectorState
{
    std::size_t selectionCount = 0;
    std::optional<talas::project::ProjectSampleRegion> primaryRegion;
    std::optional<talas::project::ProjectAsset> asset;
    SampleAssetPresentation assetState = SampleAssetPresentation::Ready;
    std::string statusText;
    bool assetActionRunning = false;
    bool canShowAsset = false;
    bool canEmbedAsset = false;
    bool canReferenceAsset = false;
};

struct SampleRegionInspectorCallbacks
{
    std::function<void(talas::project::ProjectSampleRegion)> applyRegion;
    std::function<void()> removeSelection;
    std::function<void(std::string)> relinkAsset;
    std::function<void(std::string)> reloadAsset;
    std::function<void(std::string)> showAsset;
    std::function<void(std::string)> embedAsset;
    std::function<void(std::string)> referenceAsset;
};

class SampleRegionInspector : public juce::Component
{
public:
    ~SampleRegionInspector() override = default;

    virtual void setState(const SampleRegionInspectorState& state) = 0;
    [[nodiscard]] virtual bool hasNumericEditorsForSmoke() const = 0;
    [[nodiscard]] virtual bool showsMultiSelectionForSmoke() const = 0;
    [[nodiscard]] virtual bool hasAssetActionsForSmoke() const = 0;
};

[[nodiscard]] std::unique_ptr<SampleRegionInspector>
makeSampleRegionInspector(SampleRegionInspectorCallbacks callbacks);
}
