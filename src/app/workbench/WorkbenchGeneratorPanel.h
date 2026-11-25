#pragma once

#include "talas/project/ProjectGenerators.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace talas::runtime
{
class ProjectRuntimeBridge;
}

namespace talas::app::workbench
{
class GeneratorPanel : public juce::Component
{
public:
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;

    ~GeneratorPanel() override = default;

    virtual void refresh() = 0;
    virtual void focusGenerators() = 0;
    virtual void createGeneratorForSmoke(
        talas::project::ProjectGeneratorProgram program) = 0;
    [[nodiscard]] virtual bool selectGeneratorForSmoke(
        std::string_view generatorId) = 0;
    [[nodiscard]] virtual bool generateForSmoke() = 0;
    [[nodiscard]] virtual bool generateTimelineForSmoke() = 0;
    [[nodiscard]] virtual bool generateScoreForSmoke() = 0;
    [[nodiscard]] virtual bool generateSoundForSmoke() = 0;
    [[nodiscard]] virtual bool projectionFinishedForSmoke() = 0;
    [[nodiscard]] virtual bool configureProjectionForSmoke(
        std::uint64_t timelineBaseMicroseconds,
        std::uint64_t scoreBaseTick,
        std::string_view outputNamespace) = 0;
    [[nodiscard]] virtual bool configureSoundProjectionForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view outputNamespace) = 0;
    [[nodiscard]] virtual bool projectionActionsEnabledForSmoke(
        bool timelineExpected,
        bool scoreExpected,
        bool soundExpected = false) = 0;
    [[nodiscard]] virtual bool requestDeleteForSmoke() = 0;
    [[nodiscard]] virtual bool acceptDeleteForSmoke() = 0;
    [[nodiscard]] virtual bool surfaceReadyForSmoke(
        std::string_view generatorId,
        std::string_view expectedPreview) const = 0;
    [[nodiscard]] virtual bool soundSurfaceReadyForSmoke(
        std::string_view definitionId,
        std::string_view displayName,
        std::string_view expectedPreview) = 0;
    [[nodiscard]] virtual bool graphNodePickerReadyForSmoke() = 0;
};

[[nodiscard]] std::unique_ptr<GeneratorPanel> makeGeneratorPanel(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    GeneratorPanel::DocumentChangeCallback onDocumentChange,
    GeneratorPanel::StatusCallback onStatus);
}
