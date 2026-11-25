#pragma once

#include "talas/app/SampleMapInteraction.h"
#include "talas/app/SampleMapWorkbenchModel.h"
#include "talas/project/ProjectSampleMaps.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::app::workbench
{
enum class SampleMapImportPresentation
{
    Idle,
    Running,
    Preview,
    Cancelled,
    Failed,
};

struct SampleMapPanelState
{
    talas::app::SampleMapWorkbenchProjection projection;
    talas::app::SampleMapViewport viewport;
    talas::app::SampleMapCanvas projectionCanvas{1000.0, 400.0};
    std::optional<talas::app::SampleMapMidiPoint> midiPosition;
    SampleMapImportPresentation importState =
        SampleMapImportPresentation::Idle;
    double importProgress = 0.0;
    std::string statusText;
    bool embedImports = false;
    std::vector<talas::app::SampleMapAutoMapPreviewRegion>
        importPreviewRegions;
    std::uint8_t importLowNote = 0;
    std::uint8_t importAnchorRootNote = 60;
    std::uint8_t importHighNote = 127;
    bool importPreviewValid = false;
};

struct SampleMapPanelCallbacks
{
    std::function<void(std::string)> selectMap;
    std::function<void()> createMap;
    std::function<void(std::string)> renameMap;
    std::function<void(std::string)> removeMap;
    std::function<void()> importFiles;
    std::function<void()> replaceFiles;
    std::function<void()> cancelImport;
    std::function<void(std::uint8_t, std::uint8_t, std::uint8_t)>
        setImportRange;
    std::function<void()> applyImport;
    std::function<void(bool)> setEmbedImports;
    std::function<void(
        std::string,
        talas::app::SampleMapSelectionMode)> selectRegion;
    std::function<void(std::uint8_t, std::uint8_t)> selectByMidi;
    std::function<void(int, int)> moveSelection;
    std::function<void(talas::app::SampleMapResizeEdge, int)>
        resizePrimaryRegion;
};

class SampleMapPanel : public juce::Component
{
public:
    ~SampleMapPanel() override = default;

    virtual void setState(const SampleMapPanelState& state) = 0;
    [[nodiscard]] virtual bool hasReadySurfaceForSmoke() const = 0;
    [[nodiscard]] virtual bool hasAccessibleEditorsForSmoke() const = 0;
    [[nodiscard]] virtual std::size_t accessibleRegionCountForSmoke() const = 0;
    [[nodiscard]] virtual bool invokeKeyboardRegionSelectionForSmoke() = 0;
    [[nodiscard]] virtual bool invokeKeyboardMoveForSmoke() = 0;
    [[nodiscard]] virtual bool hasImportPreviewControlsForSmoke() const = 0;
    [[nodiscard]] virtual bool hasAccessibleImportPreviewForSmoke() const = 0;
    [[nodiscard]] virtual bool setImportPreviewRangeForSmoke(
        std::uint8_t lowNote,
        std::uint8_t anchorRootNote,
        std::uint8_t highNote) = 0;
    [[nodiscard]] virtual bool applyImportPreviewForSmoke() = 0;
    [[nodiscard]] virtual bool cancelImportPreviewForSmoke() = 0;
};

[[nodiscard]] std::unique_ptr<SampleMapPanel> makeSampleMapPanel(
    SampleMapPanelCallbacks callbacks);
}
