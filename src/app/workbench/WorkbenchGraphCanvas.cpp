#include "app/workbench/WorkbenchGraphCanvas.h"
#include "app/workbench/WorkbenchFocusCoordinator.h"
#include "app/workbench/WorkbenchSoundIconPainting.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/DescriptorPresentationModel.h"
#include "talas/app/GraphBranchLayout.h"
#include "talas/app/GraphLineContour.h"
#include "talas/app/MissingNodeWorkbenchModel.h"
#include "talas/app/PrototypeBrowserModel.h"
#include "talas/app/SoundBrowserModel.h"
#include "talas/app/SoundFileWindowModel.h"
#include "talas/app/SoundClassIcon.h"
#include "talas/app/SoundIcons.h"
#include "talas/app/SoundWorkbenchModel.h"
#include "talas/app/WorkbenchGraphViewportIndex.h"
#include "talas/app/WorkbenchUiPolicy.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::app::workbench
{
namespace
{
struct TransparentStringHash
{
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(
        std::string_view value) const noexcept
    {
        return std::hash<std::string_view>{}(value);
    }
};

struct TransparentStringEqual
{
    using is_transparent = void;

    [[nodiscard]] bool operator()(
        std::string_view left,
        std::string_view right) const noexcept
    {
        return left == right;
    }
};

juce::Colour colourForWireRole(talas::app::DescriptorWireColourRole role)
{
    switch (role)
    {
    case talas::app::DescriptorWireColourRole::Audio:
        return audioColour;
    case talas::app::DescriptorWireColourRole::Control:
        return controlColour;
    case talas::app::DescriptorWireColourRole::Event:
        return eventColour;
    case talas::app::DescriptorWireColourRole::Data:
        return dataColour;
    }
    return secondaryTextColour;
}

juce::Path polygonPath(
    juce::Point<float> centre,
    talas::app::DescriptorPortShape shape,
    float radius)
{
    juce::Path path;
    const auto sides = shape == talas::app::DescriptorPortShape::Diamond ? 4 : 6;
    const auto phase = shape == talas::app::DescriptorPortShape::Diamond
        ? 0.0F
        : juce::MathConstants<float>::halfPi;
    for (int index = 0; index < sides; ++index)
    {
        const auto angle = phase
            + juce::MathConstants<float>::twoPi
                * static_cast<float>(index)
                / static_cast<float>(sides);
        const auto point = centre
            + juce::Point<float>{std::cos(angle), std::sin(angle)} * radius;
        if (index == 0)
            path.startNewSubPath(point);
        else
            path.lineTo(point);
    }
    path.closeSubPath();
    return path;
}

void drawPortShape(
    juce::Graphics& graphics,
    juce::Point<float> centre,
    talas::app::DescriptorPortShape shape,
    float size,
    bool filled,
    float outlineWidth = 1.0F)
{
    const auto bounds = juce::Rectangle<float>{size, size}.withCentre(centre);
    if (shape == talas::app::DescriptorPortShape::Circle)
    {
        if (filled)
            graphics.fillEllipse(bounds);
        else
            graphics.drawEllipse(bounds, outlineWidth);
        return;
    }
    if (shape == talas::app::DescriptorPortShape::Square)
    {
        if (filled)
            graphics.fillRect(bounds);
        else
            graphics.drawRect(bounds, outlineWidth);
        return;
    }
    const auto path = polygonPath(centre, shape, size * 0.52F);
    if (filled)
        graphics.fillPath(path);
    else
        graphics.strokePath(path, juce::PathStrokeType{outlineWidth});
}

void drawPatternedPath(
    juce::Graphics& graphics,
    const juce::Path& path,
    talas::app::DescriptorWirePattern pattern,
    float width)
{
    const juce::PathStrokeType stroke{width};
    if (pattern == talas::app::DescriptorWirePattern::Solid)
    {
        graphics.strokePath(path, stroke);
        return;
    }
    const auto dashLengths = [&]() -> std::array<float, 4> {
        switch (pattern)
        {
        case talas::app::DescriptorWirePattern::LongDash:
            return {12.0F, 6.0F, 12.0F, 6.0F};
        case talas::app::DescriptorWirePattern::ShortDash:
            return {6.0F, 4.0F, 6.0F, 4.0F};
        case talas::app::DescriptorWirePattern::Dotted:
            return {2.0F, 4.0F, 2.0F, 4.0F};
        case talas::app::DescriptorWirePattern::DashDot:
            return {9.0F, 4.0F, 2.0F, 4.0F};
        case talas::app::DescriptorWirePattern::SparseDotted:
            return {2.0F, 8.0F, 2.0F, 8.0F};
        case talas::app::DescriptorWirePattern::Solid:
            return {1.0F, 0.0F, 1.0F, 0.0F};
        }
        return {2.0F, 4.0F, 2.0F, 4.0F};
    }();
    juce::Path dashed;
    stroke.createDashedStroke(
        dashed,
        path,
        dashLengths.data(),
        static_cast<int>(dashLengths.size()));
    graphics.fillPath(dashed);
}

void drawSemanticPath(
    juce::Graphics& graphics,
    const juce::Path& path,
    juce::Point<float> start,
    juce::Point<float> end,
    const talas::app::DescriptorWirePresentation& presentation,
    bool glow = true)
{
    const auto colour = colourForWireRole(presentation.colour)
                            .withAlpha(presentation.opacity);
    // A soft glow under the line; large views skip it to keep painting fast.
    if (glow && !WorkbenchStyle::enhancedContrast())
    {
        graphics.setColour(colour.withMultipliedAlpha(0.13F));
        graphics.strokePath(path, juce::PathStrokeType{presentation.width + 6.0F});
    }
    if (presentation.outlineWidth > 0.0F)
    {
        graphics.setColour(selectionColour.withAlpha(presentation.opacity));
        drawPatternedPath(
            graphics,
            path,
            presentation.pattern,
            presentation.width + presentation.outlineWidth);
    }
    graphics.setColour(colour);
    drawPatternedPath(
        graphics,
        path,
        presentation.pattern,
        presentation.width);

    graphics.setColour(colour);
    const auto approach = end.x >= start.x ? -1.0F : 1.0F;
    const auto terminalCentre = end.translated(approach * 7.0F, 0.0F);
    drawPortShape(
        graphics,
        terminalCentre,
        presentation.terminalShape,
        7.0F,
        false,
        presentation.width);

    if (presentation.marker == talas::app::DescriptorWireMarker::Gate)
    {
        const auto markerCentre = end.translated(approach * 18.0F, 0.0F);
        graphics.drawRect(
            juce::Rectangle<float>{8.0F, 8.0F}.withCentre(markerCentre),
            presentation.width);
    }
    else if (presentation.marker == talas::app::DescriptorWireMarker::Trigger)
    {
        const auto markerCentre = end.translated(approach * 18.0F, 0.0F);
        juce::Path marker;
        marker.addTriangle(
            markerCentre.x - 5.0F,
            markerCentre.y - 5.0F,
            markerCentre.x + 5.0F,
            markerCentre.y,
            markerCentre.x - 5.0F,
            markerCentre.y + 5.0F);
        graphics.fillPath(marker);
    }
}

class GraphCanvasImpl final : public GraphCanvas,
                              public juce::DragAndDropTarget
{
private:
    struct NodeLayout
    {
        talas::project::ProjectNode node;
        talas::core::NodeTypeDescriptor descriptor;
        std::optional<talas::app::MissingNodeWorkbenchProjection>
            preserved;
        juce::Rectangle<int> bounds;
        int headerHeight = 34;
        // A Sound with inputs has a dot where its
        // lines meet it, or an ellipsis while its branch is hidden.
        bool hasInputs = false;
        bool branchHidden = false;
    };

    struct PortHit
    {
        std::string nodeId;
        std::string portId;
        talas::core::PortDescriptor descriptor;
        juce::Point<float> anchor;
    };

    struct ConnectionLayout
    {
        std::string fromNode;
        std::string toNode;
        std::string toPort;
        talas::core::PortDescriptor sourcePort;
        juce::Point<float> start;
        juce::Point<float> end;
        juce::Rectangle<int> bounds;
        // How many lines join the same output to the same Sound; the first
        // of several carries the number.
        std::size_t useCount = 1;
        bool labelled = false;
    };

    class AccessibilityItem final : public juce::Component
    {
    public:
        enum class Kind
        {
            Node,
            InputPort,
            OutputPort
        };

        AccessibilityItem(
            GraphCanvasImpl& owner,
            Kind kind,
            std::string nodeId,
            std::optional<PortHit> port,
            juce::String title,
            juce::String help)
            : owner_(owner)
            , kind_(kind)
            , nodeId_(std::move(nodeId))
            , port_(std::move(port))
            , title_(std::move(title))
            , baseHelp_(std::move(help))
        {
            setAccessible(true);
            setInterceptsMouseClicks(false, false);
            setWantsKeyboardFocus(true);
            refreshDescription();
        }

        [[nodiscard]] bool representsNode(
            std::string_view nodeId) const noexcept
        {
            return kind_ == Kind::Node && nodeId_ == nodeId;
        }

        [[nodiscard]] bool isNode() const noexcept
        {
            return kind_ == Kind::Node;
        }

        [[nodiscard]] bool representsPort(
            std::string_view nodeId,
            std::string_view portId,
            bool output) const noexcept
        {
            return port_.has_value()
                && nodeId_ == nodeId
                && port_->portId == portId
                && (output
                    ? kind_ == Kind::OutputPort
                    : kind_ == Kind::InputPort);
        }

        [[nodiscard]] juce::String focusKey() const
        {
            const auto kindPrefix = kind_ == Kind::Node
                ? "node:"
                : (kind_ == Kind::InputPort
                       ? "input:"
                       : "output:");
            return juce::String{kindPrefix}
                + juceString(nodeId_)
                + ":"
                + (port_.has_value()
                       ? juceString(port_->portId)
                       : juce::String{});
        }

        void refreshGeometry()
        {
            const auto found = owner_.layoutIndexByNodeId_.find(nodeId_);
            if (found == owner_.layoutIndexByNodeId_.end())
                return;
            const auto& layout = owner_.layoutCache_[found->second];
            if (kind_ == Kind::Node)
            {
                setBounds(owner_.graphToCanvas(layout.bounds));
                return;
            }
            if (!port_.has_value())
                return;
            const auto anchor = owner_.portAnchor(
                layout,
                port_->portId,
                kind_ == Kind::OutputPort);
            if (!anchor.has_value())
                return;
            port_->anchor = *anchor;
            setBounds(
                juce::Rectangle<int>{28, 28}.withCentre(
                    owner_.graphToCanvas(*anchor).roundToInt()));
        }

        void setDiagnostic(bool diagnostic)
        {
            if (kind_ != Kind::Node || diagnostic_ == diagnostic)
                return;
            diagnostic_ = diagnostic;
            refreshDescription();
            if (auto* handler = getAccessibilityHandler())
            {
                handler->notifyAccessibilityEvent(
                    juce::AccessibilityEvent::titleChanged);
            }
        }

        void drawFocusIndicator(juce::Graphics& graphics) const
        {
            const auto found = owner_.layoutIndexByNodeId_.find(nodeId_);
            if (found == owner_.layoutIndexByNodeId_.end())
                return;
            graphics.setColour(workbenchTheme.focus);
            if (kind_ == Kind::Node)
            {
                graphics.drawRoundedRectangle(
                    owner_.layoutCache_[found->second]
                        .bounds.toFloat().reduced(2.0F),
                    6.0F,
                    WorkbenchType::focusOutlinePixels()
                        / owner_.viewScale_);
            }
            else if (port_.has_value())
            {
                const auto anchor = owner_.portAnchor(
                    owner_.layoutCache_[found->second],
                    port_->portId,
                    kind_ == Kind::OutputPort);
                if (!anchor.has_value())
                    return;
                graphics.drawEllipse(
                    juce::Rectangle<float>{28.0F / owner_.viewScale_,
                                            28.0F / owner_.viewScale_}
                        .withCentre(*anchor),
                    WorkbenchType::focusOutlinePixels()
                        / owner_.viewScale_);
            }
        }

        bool keyPressed(const juce::KeyPress& key) override
        {
            if (!owner_.activeContext_)
                return false;
            const auto command = key.getModifiers().isCommandDown()
                || key.getModifiers().isCtrlDown();
            const auto keyCode = key.getKeyCode();
            if (command && (keyCode == 'a' || keyCode == 'A'))
            {
                owner_.requestKeyboardCommand(
                    GraphKeyboardCommand::SelectAll);
                return true;
            }
            if (command && (keyCode == 'd' || keyCode == 'D'))
            {
                owner_.requestKeyboardCommand(
                    GraphKeyboardCommand::DuplicateSelection);
                return true;
            }
            if (command && (keyCode == 'x' || keyCode == 'X'))
            {
                owner_.requestKeyboardCommand(
                    GraphKeyboardCommand::CutSelection);
                return true;
            }
            if (command && (keyCode == 'c' || keyCode == 'C'))
            {
                owner_.requestKeyboardCommand(
                    GraphKeyboardCommand::CopySelection);
                return true;
            }
            if (command && (keyCode == 'v' || keyCode == 'V'))
            {
                owner_.requestKeyboardCommand(
                    GraphKeyboardCommand::Paste);
                return true;
            }
            if (!command && !key.getModifiers().isAltDown()
                && (keyCode == 'a' || keyCode == 'A'))
            {
                owner_.requestKeyboardCommand(
                    GraphKeyboardCommand::FocusPrototypeSearch);
                return true;
            }
            if (keyCode == juce::KeyPress::deleteKey
                || keyCode == juce::KeyPress::backspaceKey)
            {
                owner_.requestKeyboardCommand(
                    GraphKeyboardCommand::DeleteSelection);
                return true;
            }
            if (key.getKeyCode() == juce::KeyPress::returnKey
                || key.getKeyCode() == juce::KeyPress::spaceKey)
            {
                activate();
                return true;
            }
            if (key.getKeyCode() == juce::KeyPress::escapeKey)
            {
                owner_.cancelKeyboardInteraction(nodeId_);
                return true;
            }
            if (kind_ != Kind::Node)
                return false;

            const auto step = key.getModifiers().isShiftDown() ? 1 : 32;
            if (key.getKeyCode() == juce::KeyPress::leftKey)
                return owner_.moveSelectedAccessibilityNodes(-step, 0);
            if (key.getKeyCode() == juce::KeyPress::rightKey)
                return owner_.moveSelectedAccessibilityNodes(step, 0);
            if (key.getKeyCode() == juce::KeyPress::upKey)
                return owner_.moveSelectedAccessibilityNodes(0, -step);
            if (key.getKeyCode() == juce::KeyPress::downKey)
                return owner_.moveSelectedAccessibilityNodes(0, step);
            return false;
        }

        bool keyStateChanged(bool isKeyDown) override
        {
            if (!isKeyDown
                && !juce::KeyPress::isKeyCurrentlyDown(
                    juce::KeyPress::leftKey)
                && !juce::KeyPress::isKeyCurrentlyDown(
                    juce::KeyPress::rightKey)
                && !juce::KeyPress::isKeyCurrentlyDown(
                    juce::KeyPress::upKey)
                && !juce::KeyPress::isKeyCurrentlyDown(
                    juce::KeyPress::downKey))
            {
                owner_.finishKeyboardNudge();
            }
            return false;
        }

        void focusGained(juce::Component::FocusChangeType) override
        {
            refreshGeometry();
            owner_.focusedAccessibilityItem_ = this;
            owner_.focusAccessibilityItem(kind_, nodeId_);
            owner_.repaint();
        }

        void focusLost(juce::Component::FocusChangeType) override
        {
            owner_.finishKeyboardNudge();
            if (owner_.focusedAccessibilityItem_ == this)
                owner_.focusedAccessibilityItem_ = nullptr;
            owner_.repaint();
        }

        std::unique_ptr<juce::AccessibilityHandler>
        createAccessibilityHandler() override
        {
            juce::Component::SafePointer<AccessibilityItem> safeThis(this);
            auto actions = juce::AccessibilityActions{}
                .addAction(
                    juce::AccessibilityActionType::press,
                    [safeThis] {
                        if (safeThis != nullptr)
                            safeThis->activate();
                    })
                .addAction(
                    juce::AccessibilityActionType::focus,
                    [safeThis] {
                        if (safeThis != nullptr)
                            safeThis->grabKeyboardFocus();
                    });
            return std::make_unique<ItemAccessibilityHandler>(
                *this,
                kind_ == Kind::Node
                    ? juce::AccessibilityRole::treeItem
                    : juce::AccessibilityRole::button,
                std::move(actions));
        }

    private:
        class ItemAccessibilityHandler final
            : public juce::AccessibilityHandler
        {
        public:
            ItemAccessibilityHandler(
                AccessibilityItem& item,
                juce::AccessibilityRole role,
                juce::AccessibilityActions actions)
                : juce::AccessibilityHandler(
                      item,
                      role,
                      std::move(actions))
                , item_(item)
            {
            }

            juce::AccessibleState getCurrentState() const override
            {
                auto state = juce::AccessibilityHandler::getCurrentState()
                    .withSelectable()
                    .withAccessibleOffscreen();
                return item_.isSelected()
                    ? state.withSelected()
                    : state;
            }

        private:
            AccessibilityItem& item_;
        };

        [[nodiscard]] bool isSelected() const
        {
            if (kind_ == Kind::Node)
                return owner_.selectedNodeIds_.contains(nodeId_);
            return port_.has_value()
                && owner_.pendingConnection_.has_value()
                && owner_.pendingConnection_->nodeId == port_->nodeId
                && owner_.pendingConnection_->portId == port_->portId;
        }

        void activate()
        {
            if (kind_ == Kind::Node)
                owner_.selectAccessibilityNode(nodeId_);
            else if (port_.has_value())
                owner_.activateAccessibilityPort(
                    *port_,
                    kind_ == Kind::OutputPort);
        }

        void refreshDescription()
        {
            auto help = baseHelp_;
            if (diagnostic_)
                help += " This node has compile diagnostics.";
            describeControl(*this, title_, help);
        }

        GraphCanvasImpl& owner_;
        Kind kind_ = Kind::Node;
        std::string nodeId_;
        std::optional<PortHit> port_;
        juce::String title_;
        juce::String baseHelp_;
        bool diagnostic_ = false;
    };

    class AccessibilityContainer final : public juce::Component
    {
    public:
        std::unique_ptr<juce::AccessibilityHandler>
        createAccessibilityHandler() override
        {
            return juce::Component::createIgnoredAccessibilityHandler(*this);
        }
    };

    class AccessibilityItemTraverser final
        : public juce::ComponentTraverser
    {
    public:
        explicit AccessibilityItemTraverser(GraphCanvasImpl& owner)
            : owner_(owner)
        {
        }

        juce::Component* getDefaultComponent(
            juce::Component*) override
        {
            return owner_.accessibilityItems_.isEmpty()
                ? nullptr
                : owner_.accessibilityItems_.getFirst();
        }

        juce::Component* getNextComponent(
            juce::Component* current) override
        {
            return adjacent(current, 1);
        }

        juce::Component* getPreviousComponent(
            juce::Component* current) override
        {
            return adjacent(current, -1);
        }

        std::vector<juce::Component*> getAllComponents(
            juce::Component*) override
        {
            std::vector<juce::Component*> result;
            result.reserve(
                static_cast<std::size_t>(
                    owner_.accessibilityItems_.size()));
            for (auto* item : owner_.accessibilityItems_)
                result.push_back(item);
            return result;
        }

    private:
        juce::Component* adjacent(
            juce::Component* current,
            int direction) const
        {
            const auto found = owner_.accessibilityOrderByItem_.find(current);
            if (found == owner_.accessibilityOrderByItem_.end())
                return nullptr;
            const auto next = static_cast<std::ptrdiff_t>(found->second)
                + direction;
            return next >= 0
                    && next < owner_.accessibilityItems_.size()
                ? owner_.accessibilityItems_.getUnchecked(
                    static_cast<int>(next))
                : nullptr;
        }

        GraphCanvasImpl& owner_;
    };

public:
    using SelectionCallback = std::function<void(
        std::optional<std::string>,
        std::string,
        bool)>;
    using DocumentChangeCallback = std::function<void()>;
    using StatusCallback = std::function<void(const juce::String&)>;
    using PrototypeDropCallback = GraphCanvas::PrototypeDropCallback;
    using OpenChildCallback = std::function<void(
        std::optional<std::string>,
        std::string)>;
    using ViewChangeCallback = std::function<void(
        std::optional<std::string>,
        double,
        double,
        double)>;
    using KeyboardCommandCallback = GraphCanvas::KeyboardCommandCallback;

    GraphCanvasImpl(talas::runtime::ProjectRuntimeBridge& bridge,
                const talas::core::NodeRegistry& registry,
                SelectionCallback onSelection,
                DocumentChangeCallback onDocumentChange,
                StatusCallback onStatus,
                PrototypeDropCallback onPrototypeDrop,
                OpenChildCallback onOpenChild,
                ViewChangeCallback onViewChange,
                KeyboardCommandCallback onKeyboardCommand)
        : bridge_(bridge)
        , registry_(registry)
        , onSelection_(std::move(onSelection))
        , onDocumentChange_(std::move(onDocumentChange))
        , onStatus_(std::move(onStatus))
        , onPrototypeDrop_(std::move(onPrototypeDrop))
        , onOpenChild_(std::move(onOpenChild))
        , onViewChange_(std::move(onViewChange))
        , onKeyboardCommand_(std::move(onKeyboardCommand))
    {
        // A rounded card over the Workbench's backdrop, its corners clear.
        setOpaque(false);
        setFocusContainerType(
            juce::Component::FocusContainerType::focusContainer);
        setWantsKeyboardFocus(true);
        accessibilityContainer_.setAccessible(true);
        accessibilityContainer_.setInterceptsMouseClicks(false, false);
        accessibilityContainer_.setAlpha(0.0F);
        addAndMakeVisible(accessibilityContainer_);
        refreshAccessibleDescription();
        refreshModel();
    }

    void setCompactGraphProtection(bool enabled) override
    {
        if (compactGraphProtection_ == enabled)
            return;
        compactGraphProtection_ = enabled;
        autoFitOnResize_ = enabled;
        if (enabled)
            zoomToFit(false);
    }

    void setIconScale(float scale) override
    {
        if (scale > 0.0F && viewScale_ > 0.0F)
            zoomAround(getLocalBounds().getCentre().toFloat(), scale / viewScale_);
    }

    void setLineContour(talas::app::GraphLineContour contour) override
    {
        lineContour_ = contour;
        repaint();
    }

    [[nodiscard]] talas::app::GraphLineContour lineContour() const override
    {
        return lineContour_;
    }

    void setGraphViewMode(
        talas::project::WorkspaceGraphViewMode mode) override
    {
        if (graphViewMode_ == mode)
            return;
        graphViewMode_ = mode;
        graphViewProjection_ =
            talas::app::workbenchGraphViewProjection(mode);
        if (!graphViewProjection_.allowPortInteraction)
            pendingConnection_.reset();
        refreshAccessibleDescription();
        rebuildLayoutCache();
        if (autoFitOnResize_)
            applyFitTransform();
        rebuildAccessibilityItems();
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::structureChanged);
        }
        repaint();
    }

    void setContextPresentation(
        juce::String context,
        bool active) override
    {
        if (contextPresentation_ == context
            && activeContext_ == active)
        {
            return;
        }
        contextPresentation_ = std::move(context);
        activeContext_ = active;
        refreshAccessibleDescription();
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::titleChanged);
        }
        repaint();
    }

    [[nodiscard]] float viewScaleForSmoke() const noexcept override
    {
        return viewScale_;
    }

    [[nodiscard]] bool graphIsFullyVisibleForSmoke() const override
    {
        const auto content = graphContentBounds();
        return content.isEmpty()
            || getLocalBounds().contains(
                graphToCanvas(content));
    }

    [[nodiscard]] std::size_t totalNodeCountForSmoke() const noexcept override
    {
        return layoutCache_.size();
    }

    [[nodiscard]] std::size_t visibleNodeCountForSmoke() const noexcept override
    {
        return visibleLayoutIndices_.size();
    }

    [[nodiscard]] std::size_t accessibilityItemCountForSmoke() const noexcept override
    {
        return static_cast<std::size_t>(accessibilityItems_.size());
    }

    [[nodiscard]] std::size_t graphViewportMembershipCountForSmoke() const noexcept override
    {
        return nodeViewportIndex_.membershipCount()
            + connectionViewportIndex_.membershipCount();
    }

    [[nodiscard]] std::size_t graphViewportScratchCapacityForSmoke() const noexcept override
    {
        return visibleLayoutIndices_.capacity()
            + visibleConnectionIndices_.capacity()
            + nodeHitIndices_.capacity();
    }

    [[nodiscard]] std::uint64_t structuralGraphRebuildCountForSmoke() const noexcept override
    {
        return structuralGraphRebuildCount_;
    }

    [[nodiscard]] std::uint64_t accessibilityGraphRebuildCountForSmoke() const noexcept override
    {
        return accessibilityGraphRebuildCount_;
    }

    void rebuildStructuralGraphCachesForSmoke() override
    {
        rebuildLayoutCache();
        rebuildAccessibilityItems();
    }

    [[nodiscard]] bool contextIsActiveForSmoke() const noexcept override
    {
        return activeContext_;
    }

    [[nodiscard]] talas::project::WorkspaceGraphViewMode
    graphViewModeForSmoke() const noexcept override
    {
        return graphViewMode_;
    }

    [[nodiscard]] bool portsVisibleForSmoke() const noexcept override
    {
        return graphViewProjection_.showPorts;
    }

    [[nodiscard]] bool wiresVisibleForSmoke() const noexcept override
    {
        return graphViewProjection_.showWires;
    }

    [[nodiscard]] bool hasPortAccessibilityForSmoke() const override
    {
        return std::ranges::any_of(
            accessibilityItems_,
            [](const auto* item) {
                return item != nullptr && !item->isNode();
            });
    }

    [[nodiscard]] bool hasCompileDiagnostic(
        std::string_view nodeId) const override
    {
        return std::ranges::find(diagnosticNodeIds_, nodeId)
            != diagnosticNodeIds_.end();
    }

    [[nodiscard]] bool doubleClickNodeForSmoke(std::string_view nodeId) override
    {
        const auto found = layoutIndexByNodeId_.find(std::string{nodeId});
        if (found == layoutIndexByNodeId_.end())
            return false;
        doubleClickAt(layoutCache_[found->second].bounds.getCentre());
        return parametersOpenOn(nodeId);
    }

    void doubleClickEmptyForSmoke() override
    {
        auto right = 0;
        for (const auto& layout : layoutCache_)
            right = std::max(right, layout.bounds.getRight());
        doubleClickAt({right + 400, -400});
    }

    [[nodiscard]] bool hasNodeForSmoke(
        std::string_view nodeId) const override
    {
        return layoutIndexByNodeId_.contains(nodeId);
    }

    [[nodiscard]] bool preservedStatusContainsForSmoke(
        std::string_view nodeId,
        std::string_view status) const override
    {
        const auto found = layoutIndexByNodeId_.find(
            nodeId);
        if (found == layoutIndexByNodeId_.end())
            return false;
        const auto& projection =
            layoutCache_[found->second].preserved;
        return projection.has_value()
            && projection->statusLabel.find(status)
                != std::string::npos;
    }

    [[nodiscard]] bool preservedNodeIsAccessibleForSmoke(
        std::string_view nodeId,
        std::string_view status) const override
    {
        return std::ranges::any_of(
            accessibilityItems_,
            [&](const auto* item) {
                return item != nullptr
                    && item->representsNode(nodeId)
                    && item->getWantsKeyboardFocus()
                    && item->getTitle().contains(
                        juceString(status))
                    && item->getDescription().isNotEmpty();
            });
    }

    [[nodiscard]] bool interactiveNodeGeometryMatchesForSmoke(
        std::string_view nodeId,
        talas::project::CanvasPosition position) override
    {
        const auto found = layoutIndexByNodeId_.find(nodeId);
        if (found == layoutIndexByNodeId_.end())
            return false;
        const auto& bounds = layoutCache_[found->second].bounds;
        if (bounds.getPosition()
            != juce::Point<int>{position.x, position.y})
        {
            return false;
        }
        const auto hit = findNodeAt(bounds.getCentre());
        if (!hit.has_value()
            || layoutCache_[*hit].node.instanceId != nodeId)
        {
            return false;
        }
        const auto accessibility = accessibilityItemsByNodeId_.find(
            nodeId);
        if (accessibility == accessibilityItemsByNodeId_.end())
            return false;
        const auto item = std::ranges::find_if(
            accessibility->second,
            [&](const auto* candidate) {
                return candidate != nullptr
                    && candidate->representsNode(nodeId);
            });
        return item != accessibility->second.end()
            && (*item)->getBounds() == graphToCanvas(bounds);
    }

    void zoomStep(bool in) override
    {
        zoomAround(getLocalBounds().getCentre().toFloat(), in ? 1.25F : 0.8F);
    }

    void zoomToFit(bool announce = true) override
    {
        autoFitOnResize_ = true;
        applyFitTransform();
        publishViewChange();
        if (announce)
        {
            onStatus_(
                "Graph fitted to view at "
                + juce::String(static_cast<int>(std::round(viewScale_ * 100.0F)))
                + "%");
        }
    }

    void setSessionViewState(double panX, double panY, double zoom) override
    {
        if (!std::isfinite(panX)
            || !std::isfinite(panY)
            || !std::isfinite(zoom)
            || zoom <= 0.0)
        {
            return;
        }
        viewOffset_ = {
            static_cast<float>(panX),
            static_cast<float>(panY),
        };
        viewScale_ = static_cast<float>(zoom);
        autoFitOnResize_ = false;
        refreshViewportSelection(getLocalBounds());
        updateAccessibilityGeometry();
        repaint();
    }

    void setSelectedNodeId(std::string nodeId) override
    {
        selectedNodeIds_.clear();
        if (!nodeId.empty())
            selectedNodeIds_.insert(nodeId);
        selectedNodeId_ = std::move(nodeId);
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::rowSelectionChanged);
        }
        repaint();
    }

    void setSelectedNodeIds(
        const std::vector<std::string>& nodeIds,
        std::string primaryNodeId) override
    {
        selectedNodeIds_.clear();
        selectedNodeIds_.insert(nodeIds.begin(), nodeIds.end());
        selectedNodeId_ = std::move(primaryNodeId);
        if (auto* handler = getAccessibilityHandler())
        {
            handler->notifyAccessibilityEvent(
                juce::AccessibilityEvent::rowSelectionChanged);
        }
        repaint();
    }

    void refreshModel() override
    {
        if (currentModelRequiresStructuralRebuild())
        {
            rebuildLayoutCache();
            if (autoFitOnResize_)
                applyFitTransform();
            rebuildAccessibilityItems();
            if (auto* handler = getAccessibilityHandler())
            {
                handler->notifyAccessibilityEvent(
                    juce::AccessibilityEvent::structureChanged);
            }
        }
        else if (synchronizePresentationPositions())
        {
            rebuildViewportIndices();
            updateAccessibilityGeometry();
        }
        repaint();
    }

    [[nodiscard]] bool semanticPortHintContainsForSmoke(
        std::string_view nodeId,
        std::string_view portId,
        bool output,
        std::string_view expected) const override
    {
        const auto layout = layoutIndexByNodeId_.find(nodeId);
        if (layout == layoutIndexByNodeId_.end())
            return false;
        const auto& ports = output
            ? layoutCache_[layout->second].descriptor.outputs
            : layoutCache_[layout->second].descriptor.inputs;
        const auto port = std::ranges::find(
            ports,
            portId,
            &talas::core::PortDescriptor::id);
        return port != ports.end()
            && talas::app::descriptorPortInfoHint(*port, output)
                   .find(expected)
                != std::string::npos;
    }

    [[nodiscard]] bool selectedWireEmphasisForSmoke(
        std::string_view fromNode,
        std::string_view toNode) const override
    {
        const auto* graph = displayedGraph(bridge_.editor().document());
        if (graph == nullptr)
            return false;
        const auto connection = std::ranges::find_if(
            graph->connections,
            [&](const auto& candidate) {
                return candidate.fromNode == fromNode
                    && candidate.toNode == toNode;
            });
        if (connection == graph->connections.end()
            || (!selectedNodeIds_.contains(connection->fromNode)
                && !selectedNodeIds_.contains(connection->toNode)))
        {
            return false;
        }
        const auto layout = layoutIndexByNodeId_.find(connection->fromNode);
        if (layout == layoutIndexByNodeId_.end())
            return false;
        const auto& outputs = layoutCache_[layout->second].descriptor.outputs;
        const auto port = std::ranges::find(
            outputs,
            connection->fromPort,
            &talas::core::PortDescriptor::id);
        if (port == outputs.end())
            return false;
        const auto normal = talas::app::descriptorWirePresentation(*port);
        const auto selected = talas::app::descriptorWirePresentation(
            *port,
            talas::app::DescriptorConnectionEmphasis::SelectedIncident);
        return selected.colour == normal.colour
            && selected.pattern == normal.pattern
            && selected.terminalShape == normal.terminalShape
            && selected.width > normal.width
            && selected.outlineWidth > normal.outlineWidth
            && selected.opacity >= normal.opacity;
    }

    [[nodiscard]] bool adapterSuggestionContainsForSmoke(
        std::string_view sourceTypeId,
        std::string_view sourcePortId,
        std::string_view destinationTypeId,
        std::string_view destinationPortId,
        std::string_view expectedAdapterTypeId) const override
    {
        const auto* source = registry_.findNodeType(sourceTypeId);
        const auto* destination = registry_.findNodeType(destinationTypeId);
        if (source == nullptr || destination == nullptr)
            return false;
        const auto sourcePort = std::ranges::find(
            source->outputs,
            sourcePortId,
            &talas::core::PortDescriptor::id);
        const auto destinationPort = std::ranges::find(
            destination->inputs,
            destinationPortId,
            &talas::core::PortDescriptor::id);
        if (sourcePort == source->outputs.end()
            || destinationPort == destination->inputs.end())
        {
            return false;
        }
        const auto adapters = talas::app::descriptorAdapterSuggestions(
            *sourcePort,
            *destinationPort,
            registry_,
            8);
        return std::ranges::find(
                   adapters,
                   expectedAdapterTypeId,
                   &talas::app::DescriptorAdapterSuggestion::typeId)
            != adapters.end();
    }

    void focusGraph() override
    {
        auto* target = accessibilityItems_.isEmpty()
            ? nullptr
            : accessibilityItems_.getFirst();
        if (const auto found = accessibilityItemsByNodeId_.find(
                selectedNodeId_);
            found != accessibilityItemsByNodeId_.end())
        {
            const auto node = std::ranges::find_if(
                found->second,
                [&](const auto* item) {
                    return item != nullptr
                        && item->representsNode(selectedNodeId_);
                });
            if (node != found->second.end())
                target = *node;
        }
        if (target != nullptr)
        {
            if (target->isShowing())
                target->grabKeyboardFocus();
        }
        else if (isShowing())
            grabKeyboardFocus();
    }

    [[nodiscard]] bool pressNodeKeyForSmoke(
        std::string_view nodeId,
        const juce::KeyPress& key) override
    {
        const auto found = std::ranges::find_if(
            accessibilityItems_,
            [&](const auto* item) {
                return item != nullptr && item->representsNode(nodeId);
            });
        if (found == accessibilityItems_.end())
            return false;
        const auto handled = (*found)->keyPressed(key);
        drainPendingKeyboardCommands();
        return handled;
    }

    [[nodiscard]] bool queueNodeKeyForSmoke(
        std::string_view nodeId,
        const juce::KeyPress& key) override
    {
        const auto found = std::ranges::find_if(
            accessibilityItems_,
            [&](const auto* item) {
                return item != nullptr && item->representsNode(nodeId);
            });
        return found != accessibilityItems_.end()
            && (*found)->keyPressed(key);
    }

    void drainKeyboardCommandsForSmoke() override
    {
        drainPendingKeyboardCommands();
    }

    [[nodiscard]] bool pressPortKeyForSmoke(
        std::string_view nodeId,
        std::string_view portId,
        bool output,
        const juce::KeyPress& key) override
    {
        const auto found = std::ranges::find_if(
            accessibilityItems_,
            [&](const auto* item) {
                return item != nullptr
                    && item->representsPort(nodeId, portId, output);
            });
        if (found == accessibilityItems_.end())
            return false;
        const auto handled = (*found)->keyPressed(key);
        drainPendingKeyboardCommands();
        return handled;
    }

    [[nodiscard]] bool pendingKeyboardConnectionForSmoke() const override
    {
        return pendingConnection_.has_value();
    }

    void finishKeyboardNudgeForSmoke() override
    {
        finishKeyboardNudge();
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::tree);
    }

    std::unique_ptr<juce::ComponentTraverser>
    createFocusTraverser() override
    {
        return std::make_unique<AccessibilityItemTraverser>(*this);
    }

    void setSoundDefinitionContext(
        std::optional<std::string> definitionId,
        std::string selectedNodeId = {}) override
    {
        finishKeyboardNudge();
        soundDefinitionContext_ = std::move(definitionId);
        setSelectedNodeId(std::move(selectedNodeId));
        pendingConnection_.reset();
        draggingNodeId_.clear();
        refreshModel();
    }

    void setCompileDiagnostics(
        std::span<const talas::runtime::Diagnostic> diagnostics,
        const talas::project::ProjectDocument& sourceDocument) override
    {
        std::vector<talas::app::SoundWorkbenchDiagnosticIdentity> identities;
        identities.reserve(diagnostics.size());
        for (const auto& diagnostic : diagnostics)
        {
            identities.push_back({
                diagnostic.nodeId,
                diagnostic.structuralSourcePath,
            });
        }
        auto nextDiagnosticNodeIds =
            talas::app::soundWorkbenchDiagnosticNodeIds(
                sourceDocument,
                soundDefinitionContext_,
                identities);
        if (nextDiagnosticNodeIds == diagnosticNodeIds_)
            return;
        const auto previous = diagnosticNodeIds_;
        diagnosticNodeIds_ = std::move(nextDiagnosticNodeIds);
        const auto updateNode = [&](std::string_view nodeId) {
            const auto found = accessibilityItemsByNodeId_.find(
                nodeId);
            if (found == accessibilityItemsByNodeId_.end())
                return;
            const auto diagnostic = std::ranges::find(
                diagnosticNodeIds_, nodeId) != diagnosticNodeIds_.end();
            for (auto* item : found->second)
                item->setDiagnostic(diagnostic);
        };
        for (const auto& nodeId : previous)
            updateNode(nodeId);
        for (const auto& nodeId : diagnosticNodeIds_)
            updateNode(nodeId);
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        refreshViewportSelection(graphics.getClipBounds());
        // Gradients, shadows and glows are for views of a readable size; a
        // crowded view paints plainly so panning stays fast.
        decorated_ = visibleLayoutIndices_.size() <= 24 && visibleConnectionIndices_.size() <= 40;
        const auto card = getLocalBounds().toFloat();
        const auto radius = WorkbenchStyle::panelRadius();
        // A deep well, lit a little from above.
        if (decorated_)
            graphics.setGradientFill(juce::ColourGradient{workbenchTheme.well.interpolatedWith(workbenchTheme.surface, 0.6F),
                0.0F, card.getY(), workbenchTheme.well, 0.0F, card.getBottom(), false});
        else
            graphics.setColour(workbenchTheme.well.interpolatedWith(workbenchTheme.surface, 0.25F));
        graphics.fillRoundedRectangle(card, radius);
        drawGrid(graphics);
        {
            juce::Graphics::ScopedSaveState saveState(graphics);
            graphics.addTransform(viewTransform());
            if (graphViewProjection_.showWires)
                drawConnections(graphics);
            if (graphViewProjection_.allowPortInteraction)
                drawPendingConnection(graphics);
            const auto replaceable = replaceableInputNodeId();
            for (const auto index : visibleLayoutIndices_)
                drawNode(graphics, layoutCache_[index], replaceable);
            if (focusedAccessibilityItem_ != nullptr)
                focusedAccessibilityItem_->drawFocusIndicator(graphics);
            if (prototypeDropPosition_.has_value())
                drawDropTarget(graphics);
            if (prototypeDropPosition_.has_value())
            {
                const auto preview = juce::Rectangle<int>{
                    prototypeDropPosition_->x,
                    prototypeDropPosition_->y,
                    nodeWidth,
                    prototypePreviewHeight(),
                };
                graphics.setColour(selectionColour.withAlpha(0.14F));
                graphics.fillRoundedRectangle(preview.toFloat(), 6.0F);
                graphics.setColour(selectionColour.withAlpha(0.8F));
                graphics.drawRoundedRectangle(preview.toFloat(), 6.0F, 2.0F);
            }
        }
        drawViewStatus(graphics);
        drawContextBadge(graphics);
        graphics.setColour(WorkbenchStyle::enhancedContrast() ? panelBorderColour : workbenchTheme.glassEdge);
        graphics.drawRoundedRectangle(card.reduced(0.5F), radius, WorkbenchStyle::enhancedContrast() ? 1.5F : 1.0F);
    }

    void resized() override
    {
        accessibilityContainer_.setBounds(getLocalBounds());
        if (autoFitOnResize_)
            applyFitTransform();
        updateHorizontalScroll();
    }

    // The flow's Sounds and the view, in the flow's
    // units, for the scroll bar at its bottom, its owner told of a change.
    void updateHorizontalScroll()
    {
        auto contentLeft = 0.0;
        auto contentRight = 0.0;
        for (std::size_t index = 0; index < layoutCache_.size(); ++index)
        {
            const auto left = static_cast<double>(layoutCache_[index].bounds.getX());
            const auto right = static_cast<double>(layoutCache_[index].bounds.getRight());
            contentLeft = index == 0 ? left : std::min(contentLeft, left);
            contentRight = index == 0 ? right : std::max(contentRight, right);
        }
        std::optional<talas::app::WorkbenchGraphScroll> scroll;
        if (viewScale_ > 0.0F)
        {
            const auto viewLeft = static_cast<double>(-viewOffset_.x / viewScale_);
            const auto viewRight = viewLeft + static_cast<double>(static_cast<float>(getWidth()) / viewScale_);
            scroll = talas::app::workbenchGraphHorizontalScroll(contentLeft, contentRight, viewLeft, viewRight);
        }
        if (scroll == horizontalScroll_)
            return;
        horizontalScroll_ = scroll;
        if (onHorizontalScrollChange_)
            onHorizontalScrollChange_();
    }

    [[nodiscard]] std::optional<talas::app::WorkbenchGraphScroll> horizontalScroll() const override
    {
        return horizontalScroll_;
    }

    void scrollHorizontallyTo(double viewStart) override
    {
        viewOffset_.x = static_cast<float>(-viewStart) * viewScale_;
        autoFitOnResize_ = false;
        repaint();
        publishViewChange();
    }

    void setHorizontalScrollChanged(std::function<void()> changed) override
    {
        onHorizontalScrollChange_ = std::move(changed);
    }

    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return droppedItem(details.description).has_value();
    }

    void itemDragEnter(const SourceDetails& details) override
    {
        updatePrototypeDropPosition(details.localPosition);
    }

    void itemDragMove(const SourceDetails& details) override
    {
        updatePrototypeDropPosition(details.localPosition);
    }

    void itemDragExit(const SourceDetails&) override
    {
        prototypeDropPosition_.reset();
        prototypeDropTarget_ = {};
        repaint();
    }

    void itemDropped(const SourceDetails& details) override
    {
        const auto mods = juce::ModifierKeys::currentModifiers;
        if (auto item = droppedItem(details.description))
            (void)dropItem(std::move(*item), details.localPosition,
                mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown());
    }

    // A known prototype, or a Sound definition of this project.
    [[nodiscard]] std::optional<GraphDropItem> droppedItem(
        const juce::var& description) const
    {
        if (const auto typeId = prototypeTypeId(description);
            typeId.has_value()
            && (registry_.findNodeType(*typeId) != nullptr || talas::app::isPrototypeAssembly(*typeId)))
        {
            return GraphDropItem{*typeId, {}};
        }
        if (!description.isString())
            return std::nullopt;
        const auto value = description.toString().toStdString();
        if (talas::app::soundBrowserDrag(value).has_value() || talas::app::signalFlowSoundDrag(value).has_value())
            return GraphDropItem{{}, {}, value};
        if (!value.starts_with(soundDragPrefix))
            return std::nullopt;
        const auto definitionId = value.substr(soundDragPrefix.size());
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        if (definitionId.empty()
            || std::ranges::none_of(definitions, [&](const auto& definition) {
                   return definition.definitionId == definitionId;
               }))
        {
            return std::nullopt;
        }
        return GraphDropItem{{}, definitionId};
    }

    bool dropPrototype(
        std::string_view typeId,
        juce::Point<int> canvasPosition)
    {
        if (registry_.findNodeType(typeId) == nullptr)
            return false;
        return dropItem(GraphDropItem{std::string{typeId}, {}}, canvasPosition);
    }

    bool dropItem(
        GraphDropItem item,
        juce::Point<int> canvasPosition,
        bool shared = false)
    {
        updatePrototypeDropPosition(canvasPosition);
        const auto dropPosition = prototypeDropPosition_;
        auto target = std::move(prototypeDropTarget_);
        target.shared = shared;
        prototypeDropPosition_.reset();
        prototypeDropTarget_ = {};
        repaint();
        if (!dropPosition.has_value())
            return false;
        onPrototypeDrop_(
            soundDefinitionContext_,
            std::move(item),
            {
                static_cast<std::int32_t>(dropPosition->x),
                static_cast<std::int32_t>(dropPosition->y),
            },
            std::move(target));
        return true;
    }

    [[nodiscard]] std::optional<std::size_t> connectionUseCountForSmoke(
        const talas::core::Connection& connection) const override
    {
        for (const auto& cached : connectionCache_)
        {
            if (cached.fromNode == connection.fromNode
                && cached.sourcePort.id == connection.fromPort
                && cached.toNode == connection.toNode
                && cached.toPort == connection.toPort)
                return cached.labelled || cached.useCount == 1 ? cached.useCount : 0U;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string dragDescriptionForSmoke(std::string_view nodeId) const override
    {
        return talas::app::signalFlowSoundDragDescription(soundDefinitionContext_, nodeId);
    }

    [[nodiscard]] std::optional<juce::Point<int>> connectionMidpointForSmoke(
        const talas::core::Connection& connection) const override
    {
        return connectionPointForSmoke(connection, 0.5F);
    }

    [[nodiscard]] std::optional<juce::Point<int>> connectionPlusForSmoke(const talas::core::Connection& connection) const override
    {
        return connectionPointForSmoke(connection, plusAlong);
    }

    [[nodiscard]] std::optional<juce::Point<int>> connectionPointForSmoke(
        const talas::core::Connection& connection,
        float t) const override
    {
        for (const auto& cached : connectionCache_)
        {
            if (cached.fromNode == connection.fromNode
                && cached.sourcePort.id == connection.fromPort
                && cached.toNode == connection.toNode
                && cached.toPort == connection.toPort)
            {
                return graphToCanvas(
                    connectionPoint(cached, t)).roundToInt();
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<juce::Point<int>> nodeCentreForSmoke(
        std::string_view nodeId) const override
    {
        const auto found = layoutIndexByNodeId_.find(std::string{nodeId});
        if (found == layoutIndexByNodeId_.end())
            return std::nullopt;
        return graphToCanvas(
            layoutCache_[found->second].bounds.getCentre().toFloat())
            .roundToInt();
    }

    [[nodiscard]] GraphDropTarget dropTargetForSmoke(
        juce::Point<int> canvasPosition) override
    {
        return dropTargetAt(canvasToGraph(canvasPosition));
    }

    [[nodiscard]] bool dropPrototypeForSmoke(
        std::string_view typeId,
        juce::Point<int> canvasPosition) override
    {
        return dropPrototype(typeId, canvasPosition);
    }

    [[nodiscard]] bool dropDescriptionForSmoke(
        const juce::String& description,
        juce::Point<int> canvasPosition,
        bool shared) override
    {
        auto item = droppedItem(description);
        return item.has_value() && dropItem(std::move(*item), canvasPosition, shared);
    }

    [[nodiscard]] bool clickBranchMarkerForSmoke(
        std::string_view nodeId,
        bool fully) override
    {
        const auto layout = layoutIndexByNodeId_.find(nodeId);
        if (layout == layoutIndexByNodeId_.end()
            || !layoutCache_[layout->second].hasInputs)
            return false;
        const auto centre = branchMarkerBounds(layoutCache_[layout->second])
            .getCentre().roundToInt();
        const auto marked = branchMarkerAt(centre);
        return marked == std::optional<std::string>{std::string{nodeId}}
            && toggleBranch(*marked, fully);
    }

    [[nodiscard]] std::vector<std::string> shownNodeIdsForSmoke()
        const override
    {
        std::vector<std::string> shown;
        for (const auto& layout : layoutCache_)
            shown.push_back(layout.node.instanceId);
        std::ranges::sort(shown);
        return shown;
    }

    [[nodiscard]] juce::PopupMenu contextMenuForSmoke(
        juce::Point<int> canvasPosition) override
    {
        prepareContextMenu(canvasPosition);
        return buildContextMenu();
    }

    [[nodiscard]] bool chooseContextMenuItemForSmoke(
        juce::Point<int> canvasPosition,
        int itemId) override
    {
        prepareContextMenu(canvasPosition);
        const auto chosen = chooseContextMenuItem(
            itemId, canvasToGraph(canvasPosition));
        drainPendingKeyboardCommands();
        return chosen;
    }

    // The Edit menu actions on the Sounds under the pointer.
    enum ContextMenuItem
    {
        contextCut = 1,
        contextCopy,
        contextPaste,
        contextDuplicate,
        contextDelete,
        contextSelectAll,
        contextFindPrototype,
        contextToggleBranch,
        contextShowWholeBranch,
        contextRename,
        contextCleanUp,
        contextLinesCurved,
        contextLinesRadial,
        contextLinesRightAngle,
        contextDescribeSound,
        contextGetInfo,
        contextSetReplaceableInput,
    };

    // A right-click on a Sound outside the selection selects it first, as
    // a click would; on empty canvas it keeps the selection.
    void prepareContextMenu(juce::Point<int> canvasPosition)
    {
        finishKeyboardNudge();
        if (pendingConnection_.has_value())
            cancelPendingConnection();
        const auto nodeIndex = findNodeAt(canvasToGraph(canvasPosition));
        if (nodeIndex.has_value())
        {
            const auto nodeId = layoutCache_[*nodeIndex].node.instanceId;
            if (!selectedNodeIds_.contains(nodeId))
            {
                selectedNodeIds_.clear();
                selectedNodeIds_.insert(nodeId);
                selectedNodeId_ = nodeId;
                onSelection_(soundDefinitionContext_, nodeId, false);
                repaint();
                return;
            }
        }
        if (!activeContext_)
            onSelection_(soundDefinitionContext_, selectedNodeId_, false);
    }

    [[nodiscard]] juce::PopupMenu buildContextMenu() const
    {
        const auto hasSelection = !selectedNodeIds_.empty();
        juce::PopupMenu menu;
        menu.addItem(contextCut, "Cut", hasSelection);
        menu.addItem(contextCopy, "Copy", hasSelection);
        menu.addItem(contextPaste, "Paste");
        menu.addItem(contextDuplicate, "Duplicate", hasSelection);
        menu.addItem(contextDelete, "Delete", hasSelection);
        menu.addSeparator();
        const auto layout = layoutIndexByNodeId_.find(selectedNodeId_);
        const auto* selected = layout == layoutIndexByNodeId_.end()
            ? nullptr
            : &layoutCache_[layout->second];
        const auto withInputs = selected != nullptr && selected->hasInputs;
        menu.addItem(contextToggleBranch,
            withInputs && selected->branchHidden ? "Show Inputs" : "Hide Inputs",
            withInputs && selectedNodeIds_.size() == 1);
        menu.addItem(contextShowWholeBranch, "Show All Inputs",
            withInputs && selectedNodeIds_.size() == 1);
        menu.addItem(contextRename, "Rename...",
            selected != nullptr && selectedNodeIds_.size() == 1);
        menu.addItem(contextDescribeSound, "Describe Sound...",
            selected != nullptr && selectedNodeIds_.size() == 1);
        menu.addItem(contextGetInfo, "Get Info...",
            selected != nullptr && selectedNodeIds_.size() == 1);
        menu.addItem(contextSetReplaceableInput, "Set Replaceable Input",
            soundDefinitionContext_.has_value() && selected != nullptr && selectedNodeIds_.size() == 1);
        menu.addItem(contextCleanUp, "Clean Up", !layoutCache_.empty());
        juce::PopupMenu lines;
        for (const auto [item, contour] : {
                 std::pair{contextLinesCurved, talas::app::GraphLineContour::Curved},
                 std::pair{contextLinesRadial, talas::app::GraphLineContour::Radial},
                 std::pair{contextLinesRightAngle, talas::app::GraphLineContour::RightAngle}})
        {
            lines.addItem(item, juce::String{std::string{talas::app::graphLineContourName(contour)}},
                true, lineContour_ == contour);
        }
        menu.addSubMenu("Lines", lines);
        menu.addSeparator();
        menu.addItem(contextSelectAll, "Select All", !layoutCache_.empty());
        menu.addItem(contextFindPrototype, "Find Prototype...");
        return menu;
    }

    bool chooseContextMenuItem(
        int itemId,
        juce::Point<int> graphPosition)
    {
        switch (itemId)
        {
        case contextCut:
            requestKeyboardCommand(GraphKeyboardCommand::CutSelection);
            return true;
        case contextCopy:
            requestKeyboardCommand(GraphKeyboardCommand::CopySelection);
            return true;
        case contextPaste:
            requestKeyboardCommand(
                GraphKeyboardCommand::Paste,
                talas::project::CanvasPosition{
                    std::max(0, graphPosition.x),
                    std::max(0, graphPosition.y),
                });
            return true;
        case contextDuplicate:
            requestKeyboardCommand(GraphKeyboardCommand::DuplicateSelection);
            return true;
        case contextDelete:
            requestKeyboardCommand(GraphKeyboardCommand::DeleteSelection);
            return true;
        case contextSelectAll:
            requestKeyboardCommand(GraphKeyboardCommand::SelectAll);
            return true;
        case contextFindPrototype:
            requestKeyboardCommand(GraphKeyboardCommand::FocusPrototypeSearch);
            return true;
        case contextToggleBranch:
        case contextShowWholeBranch:
            return !selectedNodeId_.empty()
                && toggleBranch(selectedNodeId_, itemId == contextShowWholeBranch);
        case contextRename:
            requestKeyboardCommand(GraphKeyboardCommand::RenameSelection);
            return true;
        case contextCleanUp:
            requestKeyboardCommand(GraphKeyboardCommand::CleanUpLayout);
            return true;
        case contextDescribeSound:
            requestKeyboardCommand(GraphKeyboardCommand::DescribeSelection);
            return true;
        case contextGetInfo:
            requestKeyboardCommand(GraphKeyboardCommand::InfoOnSelection);
            return true;
        case contextSetReplaceableInput:
            requestKeyboardCommand(GraphKeyboardCommand::SetReplaceableInput);
            return true;
        case contextLinesCurved:
        case contextLinesRadial:
        case contextLinesRightAngle:
            lineContour_ = itemId == contextLinesRadial ? talas::app::GraphLineContour::Radial
                : itemId == contextLinesRightAngle      ? talas::app::GraphLineContour::RightAngle
                                                        : talas::app::GraphLineContour::Curved;
            repaint();
            return true;
        default:
            return false;
        }
    }

    void showContextMenu(juce::Point<int> canvasPosition)
    {
        prepareContextMenu(canvasPosition);
        const auto graphPosition = canvasToGraph(canvasPosition);
        juce::Component::SafePointer<GraphCanvasImpl> safeThis(this);
        buildContextMenu().showMenuAsync(
            juce::PopupMenu::Options{}
                .withTargetComponent(this)
                .withTargetScreenArea(juce::Rectangle<int>{
                    localPointToGlobal(canvasPosition), {1, 1}}),
            [safeThis, graphPosition](int result) {
                if (safeThis != nullptr && result != 0)
                    (void)safeThis->chooseContextMenuItem(result, graphPosition);
            });
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        finishKeyboardNudge();
        if (event.mods.isPopupMenu())
        {
            showContextMenu(event.getPosition());
            return;
        }
        if (event.mods.isMiddleButtonDown()
            || event.mods.isAltDown())
        {
            panning_ = true;
            panStart_ = event.getPosition();
            panStartOffset_ = viewOffset_;
            autoFitOnResize_ = false;
            setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            return;
        }

        const auto graphPosition = canvasToGraph(event.getPosition());
        if (graphViewProjection_.allowPortInteraction)
        {
            const auto output =
                findPortAt(
                    graphPosition,
                    true,
                    14.0F / viewScale_);
            if (output.has_value())
            {
                selectedNodeIds_.clear();
                selectedNodeIds_.insert(output->nodeId);
                selectedNodeId_ = output->nodeId;
                pendingConnection_ = output;
                pendingConnectionPointer_ =
                    graphPosition.toFloat();
                onSelection_(soundDefinitionContext_, selectedNodeId_, false);
                repaint();
                return;
            }
        }

        if (pendingConnection_.has_value())
        {
            pendingConnectionPointer_ = graphPosition.toFloat();
            repaint();
            return;
        }

        if (const auto marked = branchMarkerAt(graphPosition))
        {
            (void)toggleBranch(*marked,
                event.mods.isCtrlDown() || event.getNumberOfClicks() > 1);
            return;
        }

        queryNodeHits(graphPosition, 1.0F);
        for (auto iterator = nodeHitIndices_.rbegin();
             iterator != nodeHitIndices_.rend(); ++iterator)
        {
            const auto& layout = layoutCache_[*iterator];
            if (!layout.bounds.contains(graphPosition))
                continue;

            const auto clickedNodeId = layout.node.instanceId;
            const auto extendSelection = event.mods.isShiftDown();
            // A click selects the Sound; Shift adds or
            // takes it away.
            const auto change = talas::app::graphClickSelection(extendSelection, selectedNodeIds_.contains(clickedNodeId));
            if (change == talas::app::GraphClickSelection::SelectOnly)
            {
                selectedNodeIds_.clear();
                selectedNodeIds_.insert(clickedNodeId);
                selectedNodeId_ = clickedNodeId;
            }
            else if (change == talas::app::GraphClickSelection::Remove)
            {
                selectedNodeIds_.erase(clickedNodeId);
                selectedNodeId_ = selectedNodeIds_.empty()
                    ? std::string{}
                    : *selectedNodeIds_.begin();
            }
            else
            {
                selectedNodeIds_.insert(clickedNodeId);
                selectedNodeId_ = clickedNodeId;
            }
            dragOffset_ =
                graphPosition
                - layout.bounds.getPosition();
            // Shift and a drag move the Sound with all of its inputs
            //.
            branchDragStarts_.clear();
            if (extendSelection)
            {
                if (const auto* graph = displayedGraph(bridge_.editor().document()))
                {
                    for (const auto& id : talas::app::graphNodesShiftDragged(*graph, clickedNodeId))
                    {
                        if (const auto found = layoutIndexByNodeId_.find(id); found != layoutIndexByNodeId_.end())
                            branchDragStarts_.emplace_back(id, layoutCache_[found->second].bounds.getPosition());
                    }
                    branchDragOrigin_ = graphPosition;
                }
            }
            draggingNodeId_ = clickedNodeId;
            draggedNodeMoved_ = false;
            dragTransactionToken_ =
                bridge_.editor().beginOwnedHistoryTransaction();
            onSelection_(
                soundDefinitionContext_,
                clickedNodeId,
                extendSelection);
            repaint();
            return;
        }

        selectedNodeId_.clear();
        selectedNodeIds_.clear();
        draggingNodeId_.clear();
        draggedNodeMoved_ = false;
        onSelection_(soundDefinitionContext_, {}, false);
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (panning_)
        {
            const auto delta = event.getPosition() - panStart_;
            viewOffset_ = panStartOffset_
                + delta.toFloat();
            refreshViewportSelection(getLocalBounds());
            repaint();
            return;
        }
        const auto graphPosition = canvasToGraph(event.getPosition());
        if (pendingConnection_.has_value())
        {
            pendingConnectionPointer_ = graphPosition.toFloat();
            repaint();
            return;
        }

        if (draggingNodeId_.empty())
            return;

        // A Sound dragged out
        // of the signal flow is a copy of it, for a Sound file window or the
        // Timeline; it stays where it was here. With Control (Command on the
        // Mac) held it is the Sound itself, dropped on another here to take
        // its place.
        if (!getLocalBounds().contains(event.getPosition()) || event.mods.isCommandDown())
        {
            auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
            if (container != nullptr && !container->isDragAndDropActive())
            {
                const auto nodeId = draggingNodeId_;
                bridge_.editor().cancelOwnedHistoryTransaction(dragTransactionToken_);
                dragTransactionToken_ = 0;
                draggingNodeId_.clear();
                draggedNodeMoved_ = false;
                branchDragStarts_.clear();
                refreshModel();
                container->startDragging(juce::String{dragDescriptionForSmoke(nodeId)}, this);
                return;
            }
        }

        if (!branchDragStarts_.empty())
        {
            const auto delta = graphPosition - branchDragOrigin_;
            for (const auto& [id, start] : branchDragStarts_)
            {
                const auto moved = start + delta;
                if (moveNode(id, {moved.x, moved.y}, dragTransactionToken_).ok())
                    updateCachedNodePosition(id, moved);
            }
            draggedNodeMoved_ = true;
            repaint();
            return;
        }

        const auto position = graphPosition - dragOffset_;
        const auto result = moveNode(
            draggingNodeId_,
            {position.x, position.y},
            dragTransactionToken_);
        if (!result.ok())
            return;

        updateCachedNodePosition(draggingNodeId_, position);
        draggedNodeMoved_ = true;
        repaint();
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (panning_)
        {
            panning_ = false;
            setMouseCursor(juce::MouseCursor::NormalCursor);
            updateAccessibilityGeometry();
            publishViewChange();
            return;
        }
        const auto graphPosition = canvasToGraph(event.getPosition());
        if (pendingConnection_.has_value())
        {
            const auto source = *pendingConnection_;
            if (!draggingNodeId_.empty())
            {
                bridge_.editor().endOwnedHistoryTransaction(
                    dragTransactionToken_);
            }
            dragTransactionToken_ = 0;
            draggingNodeId_.clear();
            draggedNodeMoved_ = false;
            const auto input = findPortAt(
                graphPosition,
                false,
                14.0F / viewScale_);
            if (input.has_value())
            {
                const auto result = connect({
                    source.nodeId,
                    source.portId,
                    input->nodeId,
                    input->portId,
                });
                if (result.ok())
                {
                    onDocumentChange_();
                    onStatus_("Connection created");
                }
                else
                {
                    onStatus_(connectionFailureStatus(
                        result,
                        source,
                        *input));
                }
            }
            pendingConnection_.reset();
            repaint();
            return;
        }

        const auto documentChanged = draggedNodeMoved_;
        branchDragStarts_.clear();
        if (!draggingNodeId_.empty())
        {
            bridge_.editor().endOwnedHistoryTransaction(
                dragTransactionToken_);
        }
        dragTransactionToken_ = 0;
        draggingNodeId_.clear();
        draggedNodeMoved_ = false;
        if (documentChanged)
        {
            rebuildViewportIndices();
            onDocumentChange_();
        }
    }

    void mouseDoubleClick(const juce::MouseEvent& event) override
    {
        doubleClickAt(canvasToGraph(event.getPosition()));
    }

    void doubleClickAt(juce::Point<int> graphPosition)
    {
        if (branchMarkerAt(graphPosition).has_value())
            return;
        const auto nodeIndex = findNodeAt(graphPosition);
        const auto input = graphViewProjection_.allowPortInteraction
            ? findPortAt(graphPosition, false, 14.0F / viewScale_)
            : std::nullopt;
        talas::app::SignalFlowClickTarget target;
        target.onSound = nodeIndex.has_value();
        target.madeOfSounds = nodeIndex.has_value() && layoutCache_[*nodeIndex].node.soundDefinitionId.has_value();
        target.onInput = input.has_value();
        target.inputConnected = input.has_value() && portIsConnected(input->nodeId, input->portId, false);
        target.editsLines = graphViewProjection_.allowPortInteraction;
        switch (talas::app::signalFlowDoubleClick(target))
        {
        case talas::app::SignalFlowDoubleClick::OpenItsSounds:
            onOpenChild_(soundDefinitionContext_, *layoutCache_[*nodeIndex].node.soundDefinitionId);
            return;
        case talas::app::SignalFlowDoubleClick::OpenParameters:
        {
            // Its parameters below, in place of
            // the ones open, its icon patterned while they are.
            const auto nodeId = layoutCache_[*nodeIndex].node.instanceId;
            selectedNodeIds_ = {nodeId};
            selectedNodeId_ = nodeId;
            onSelection_(soundDefinitionContext_, nodeId, false);
            repaint();
            return;
        }
        case talas::app::SignalFlowDoubleClick::CloseParameters:
            selectedNodeId_.clear();
            selectedNodeIds_.clear();
            onSelection_(soundDefinitionContext_, {}, false);
            // And the diagram drawn again as it now is.
            rebuildLayoutCache();
            rebuildAccessibilityItems();
            repaint();
            return;
        case talas::app::SignalFlowDoubleClick::Nothing:
            onStatus_("Input has no connection");
            return;
        case talas::app::SignalFlowDoubleClick::Disconnect:
            break;
        }

        const auto result = disconnect(input->nodeId, input->portId);
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            return;
        }

        onDocumentChange_();
        onStatus_("Input disconnected");
        repaint();
    }

    void mouseWheelMove(
        const juce::MouseEvent& event,
        const juce::MouseWheelDetails& wheel) override
    {
        if (event.mods.isCommandDown()
            || event.mods.isCtrlDown())
        {
            const auto factor = std::exp(wheel.deltaY * 0.65F);
            zoomAround(event.position, factor);
            return;
        }

        autoFitOnResize_ = false;
        viewOffset_.x += wheel.deltaX * 180.0F;
        viewOffset_.y += wheel.deltaY * 180.0F;
        refreshViewportSelection(getLocalBounds());
        updateAccessibilityGeometry();
        repaint();
        publishViewChange();
    }

private:
    static constexpr int nodeWidth = 178;
    static constexpr int headerHeight = 34;
    static constexpr int preservedHeaderHeight = 50;
    static constexpr int portRowHeight = 20;

    [[nodiscard]] static std::size_t presentedPortCount(
        const std::vector<talas::core::PortDescriptor>& ports) noexcept
    {
        return static_cast<std::size_t>(std::ranges::count_if(
            ports,
            [](const auto& port) { return !port.compilerPopulated; }));
    }

    [[nodiscard]] static const talas::core::PortDescriptor* presentedPortAt(
        const std::vector<talas::core::PortDescriptor>& ports,
        std::size_t presentedIndex) noexcept
    {
        for (const auto& port : ports)
        {
            if (port.compilerPopulated)
                continue;
            if (presentedIndex-- == 0)
                return &port;
        }
        return nullptr;
    }

    void refreshAccessibleDescription()
    {
        juce::String presentation;
        switch (graphViewMode_)
        {
        case talas::project::WorkspaceGraphViewMode::Compact:
            presentation =
                "Compact node cards without ports or wires. Tab through nodes and use arrow keys to move a focused node.";
            break;
        case talas::project::WorkspaceGraphViewMode::Ports:
            presentation =
                "Nodes and typed ports without connection wires. Tab through nodes and ports; press a port to connect or disconnect.";
            break;
        case talas::project::WorkspaceGraphViewMode::PortsAndWires:
            presentation =
                "Nodes, typed ports, and connection wires. Tab through nodes and ports; press a port to connect or disconnect.";
            break;
        }
        describeControl(
            *this,
            contextPresentation_ + " Sound graph",
            (activeContext_
                 ? juce::String{"Active Structure edit scope. "}
                 : juce::String{"Adjacent Structure context. "})
                + presentation
                + " Pan with the mouse wheel or Option-drag, zoom with Command-wheel, and use Command-0 to fit the graph.");
    }

    [[nodiscard]] int prototypePreviewHeight() const noexcept
    {
        return graphViewProjection_.showPorts
            ? headerHeight + portRowHeight + 8
            : headerHeight + 12;
    }

    [[nodiscard]] juce::AffineTransform viewTransform() const
    {
        return juce::AffineTransform::scale(viewScale_)
            .translated(viewOffset_.x, viewOffset_.y);
    }

    [[nodiscard]] juce::Point<int> canvasToGraph(
        juce::Point<int> point) const
    {
        return juce::Point<float>{
            (static_cast<float>(point.x) - viewOffset_.x) / viewScale_,
            (static_cast<float>(point.y) - viewOffset_.y) / viewScale_,
        }.roundToInt();
    }

    [[nodiscard]] juce::Point<float> graphToCanvas(
        juce::Point<float> point) const
    {
        return {
            point.x * viewScale_ + viewOffset_.x,
            point.y * viewScale_ + viewOffset_.y,
        };
    }

    [[nodiscard]] juce::Rectangle<int> graphToCanvas(
        juce::Rectangle<int> bounds) const
    {
        const auto topLeft = graphToCanvas(
            bounds.getTopLeft().toFloat());
        const auto bottomRight = graphToCanvas(
            bounds.getBottomRight().toFloat());
        return juce::Rectangle<float>{topLeft, bottomRight}.getSmallestIntegerContainer();
    }

    [[nodiscard]] talas::app::WorkbenchGraphViewportRect canvasViewport(
        juce::Rectangle<int> canvasBounds,
        int graphPadding = 24) const noexcept
    {
        if (canvasBounds.isEmpty() || viewScale_ <= 0.0F)
            return {};
        const auto topLeft = canvasToGraph(canvasBounds.getTopLeft());
        const auto bottomRight = canvasToGraph(canvasBounds.getBottomRight());
        const auto left = std::min(topLeft.x, bottomRight.x) - graphPadding;
        const auto top = std::min(topLeft.y, bottomRight.y) - graphPadding;
        const auto right = std::max(topLeft.x, bottomRight.x) + graphPadding;
        const auto bottom = std::max(topLeft.y, bottomRight.y) + graphPadding;
        return {
            left,
            top,
            static_cast<std::int64_t>(right) - left + 1,
            static_cast<std::int64_t>(bottom) - top + 1,
        };
    }

    void refreshViewportSelection(juce::Rectangle<int> canvasBounds)
    {
        const auto viewport = canvasViewport(canvasBounds);
        nodeViewportIndex_.query(viewport, visibleLayoutIndices_);
        connectionViewportIndex_.query(
            viewport, visibleConnectionIndices_);
        appendMovingGeometry(viewport, true);
        std::ranges::sort(visibleLayoutIndices_);
        visibleLayoutIndices_.erase(
            std::ranges::unique(visibleLayoutIndices_).begin(),
            visibleLayoutIndices_.end());
        std::ranges::sort(visibleConnectionIndices_);
        visibleConnectionIndices_.erase(
            std::ranges::unique(visibleConnectionIndices_).begin(),
            visibleConnectionIndices_.end());
    }

    void queryNodeHits(juce::Point<int> point, float radius)
    {
        const auto extent = std::max(1, static_cast<int>(std::ceil(radius)));
        nodeViewportIndex_.query(
            {static_cast<std::int64_t>(point.x) - extent,
             static_cast<std::int64_t>(point.y) - extent,
             extent * 2 + 1,
             extent * 2 + 1},
            nodeHitIndices_);
        appendMovingGeometry(
            {static_cast<std::int64_t>(point.x) - extent,
             static_cast<std::int64_t>(point.y) - extent,
             extent * 2 + 1,
             extent * 2 + 1},
            false,
            &nodeHitIndices_);
        std::ranges::sort(nodeHitIndices_);
        nodeHitIndices_.erase(
            std::ranges::unique(nodeHitIndices_).begin(),
            nodeHitIndices_.end());
    }

    static std::int64_t viewportEnd(
        std::int64_t start,
        std::int64_t length) noexcept
    {
        if (length <= 0)
            return start;
        const auto maximum = std::numeric_limits<std::int64_t>::max();
        return start > maximum - (length - 1)
            ? maximum
            : start + length - 1;
    }

    static bool viewportIntersects(
        const talas::app::WorkbenchGraphViewportRect& left,
        const talas::app::WorkbenchGraphViewportRect& right) noexcept
    {
        return left.width > 0 && left.height > 0
            && right.width > 0 && right.height > 0
            && left.x <= viewportEnd(right.x, right.width)
            && right.x <= viewportEnd(left.x, left.width)
            && left.y <= viewportEnd(right.y, right.height)
            && right.y <= viewportEnd(left.y, left.height);
    }

    void appendMovingGeometry(
        const talas::app::WorkbenchGraphViewportRect& viewport,
        bool includeConnections,
        std::vector<std::size_t>* nodeResult = nullptr)
    {
        auto& nodes = nodeResult == nullptr
            ? visibleLayoutIndices_
            : *nodeResult;
        const auto appendNode = [&](std::string_view nodeId) {
            const auto found = layoutIndexByNodeId_.find(
                nodeId);
            if (found == layoutIndexByNodeId_.end())
                return;
            if (viewportIntersects(
                    viewportRect(layoutCache_[found->second].bounds),
                    viewport))
            {
                nodes.push_back(found->second);
            }
            if (!includeConnections)
                return;
            const auto connections = connectionIndicesByNodeId_.find(
                nodeId);
            if (connections == connectionIndicesByNodeId_.end())
                return;
            for (const auto index : connections->second)
            {
                if (index < connectionCache_.size()
                    && viewportIntersects(
                        viewportRect(connectionCache_[index].bounds),
                        viewport))
                {
                    visibleConnectionIndices_.push_back(index);
                }
            }
        };
        if (!draggingNodeId_.empty())
            appendNode(draggingNodeId_);
        if (keyboardNudgeActive_)
        {
            for (const auto& nodeId : keyboardNudgeNodeIds_)
                appendNode(nodeId);
        }
    }

    [[nodiscard]] std::optional<std::size_t> findNodeAt(
        juce::Point<int> point)
    {
        queryNodeHits(point, 1.0F);
        for (auto iterator = nodeHitIndices_.rbegin();
             iterator != nodeHitIndices_.rend(); ++iterator)
        {
            if (layoutCache_[*iterator].bounds.contains(point))
                return *iterator;
        }
        return std::nullopt;
    }

    [[nodiscard]] juce::Rectangle<int> graphContentBounds() const
    {
        if (layoutCache_.empty())
            return {};
        auto bounds = layoutCache_.front().bounds;
        for (const auto& layout : layoutCache_ | std::views::drop(1))
            bounds = bounds.getUnion(layout.bounds);
        return bounds;
    }

    void applyFitTransform()
    {
        const auto content = graphContentBounds();
        const auto transform = talas::app::workbenchGraphFitTransform(
            getWidth(),
            getHeight(),
            content.getX(),
            content.getY(),
            content.getWidth(),
            content.getHeight());
        viewScale_ = transform.scale;
        viewOffset_ = {transform.offsetX, transform.offsetY};
        refreshViewportSelection(getLocalBounds());
        updateAccessibilityGeometry();
        repaint();
    }

    void zoomAround(
        juce::Point<float> canvasAnchor,
        float factor)
    {
        const auto previousScale = viewScale_;
        const auto content = graphContentBounds();
        const auto fit = talas::app::workbenchGraphFitTransform(
            getWidth(),
            getHeight(),
            content.getX(),
            content.getY(),
            content.getWidth(),
            content.getHeight());
        const auto nextScale = talas::app::workbenchGraphZoomScale(
            previousScale,
            factor,
            fit.scale);
        if (nextScale == previousScale)
            return;
        const auto graphAnchor = juce::Point<float>{
            (canvasAnchor.x - viewOffset_.x) / previousScale,
            (canvasAnchor.y - viewOffset_.y) / previousScale,
        };
        viewScale_ = nextScale;
        viewOffset_ = {
            canvasAnchor.x - graphAnchor.x * nextScale,
            canvasAnchor.y - graphAnchor.y * nextScale,
        };
        autoFitOnResize_ = false;
        refreshViewportSelection(getLocalBounds());
        updateAccessibilityGeometry();
        repaint();
        publishViewChange();
    }

    void publishViewChange()
    {
        updateHorizontalScroll();
        if (!onViewChange_)
            return;
        onViewChange_(
            soundDefinitionContext_,
            static_cast<double>(viewOffset_.x),
            static_cast<double>(viewOffset_.y),
            static_cast<double>(viewScale_));
    }

    static std::optional<std::string> prototypeTypeId(const juce::var& description)
    {
        if (!description.isString())
            return std::nullopt;
        const auto value = description.toString().toStdString();
        if (!value.starts_with(prototypeDragPrefix))
            return std::nullopt;
        const auto typeId = value.substr(prototypeDragPrefix.size());
        return typeId.empty() ? std::nullopt : std::optional<std::string>{typeId};
    }

    void updatePrototypeDropPosition(juce::Point<int> pointer)
    {
        const auto graphPointer = canvasToGraph(pointer);
        prototypeDropPosition_ = {
            std::max(0, graphPointer.x - nodeWidth / 2),
            std::max(0, graphPointer.y - headerHeight / 2),
        };
        prototypeDropTarget_ = dropTargetAt(graphPointer);
        repaint();
    }

    [[nodiscard]] juce::Point<float> connectionPoint(
        const ConnectionLayout& connection,
        float t) const
    {
        const auto point = talas::app::graphLinePoint(
            {connection.start.x, connection.start.y},
            {connection.end.x, connection.end.y},
            lineContour_,
            t);
        return {point.x, point.y};
    }

    // The line from an output to an input in the chosen contour.
    [[nodiscard]] juce::Path linePath(
        juce::Point<float> start,
        juce::Point<float> end) const
    {
        const auto corners = talas::app::graphLineCorners(
            {start.x, start.y}, {end.x, end.y}, lineContour_);
        juce::Path path;
        path.startNewSubPath(start);
        if (lineContour_ == talas::app::GraphLineContour::Curved)
        {
            path.cubicTo(corners[1].x, corners[1].y, corners[2].x, corners[2].y, end.x, end.y);
            return path;
        }
        for (const auto& corner : corners | std::views::drop(1))
            path.lineTo(corner.x, corner.y);
        return path;
    }

    // The Sound under the pointer, or else the line within a few pixels of
    // it.
    [[nodiscard]] GraphDropTarget dropTargetAt(juce::Point<int> graphPoint)
    {
        GraphDropTarget target;
        if (const auto nodeIndex = findNodeAt(graphPoint))
        {
            target.nodeId = layoutCache_[*nodeIndex].node.instanceId;
            return target;
        }
        if (!graphViewProjection_.showWires)
            return target;
        const auto point = graphPoint.toFloat();
        const auto tolerance = 8.0F / viewScale_;
        // The plus near a line's end mixes; the rest of the line inserts.
        for (const auto& connection : connectionCache_)
        {
            if (connectionPoint(connection, plusAlong).getDistanceFrom(point) <= plusRadius / viewScale_)
            {
                target.connection = talas::core::Connection{
                    connection.fromNode, connection.sourcePort.id, connection.toNode, connection.toPort};
                target.mix = true;
                return target;
            }
        }
        auto best = tolerance;
        for (const auto& connection : connectionCache_)
        {
            if (!connection.bounds.toFloat().expanded(tolerance).contains(point))
                continue;
            constexpr int segments = 32;
            auto previous = connection.start;
            for (int segment = 1; segment <= segments; ++segment)
            {
                const auto next = connectionPoint(
                    connection,
                    static_cast<float>(segment) / static_cast<float>(segments));
                juce::Point<float> nearest;
                const auto distance = juce::Line<float>{previous, next}
                    .getDistanceFromPoint(point, nearest);
                if (distance <= best)
                {
                    best = distance;
                    target.connection = talas::core::Connection{
                        connection.fromNode,
                        connection.sourcePort.id,
                        connection.toNode,
                        connection.toPort,
                    };
                }
                previous = next;
            }
        }
        return target;
    }

    // While something is dragged over the flow, each line shows a gray
    // circle with a plus near the Sound it feeds, its middle left
    // for inserting.
    static constexpr float plusRadius = 7.0F;
    static constexpr float plusAlong = 0.8F;

    void drawPlusCircles(juce::Graphics& graphics) const
    {
        const auto radius = plusRadius / viewScale_;
        for (const auto& connection : connectionCache_)
        {
            const auto centre = connectionPoint(connection, plusAlong);
            const auto chosen = prototypeDropTarget_.mix && prototypeDropTarget_.connection.has_value()
                && prototypeDropTarget_.connection->fromNode == connection.fromNode
                && prototypeDropTarget_.connection->toNode == connection.toNode
                && prototypeDropTarget_.connection->toPort == connection.toPort;
            graphics.setColour((chosen ? selectionColour : juce::Colours::grey).withAlpha(0.9F));
            graphics.fillEllipse(juce::Rectangle<float>{2.0F * radius, 2.0F * radius}.withCentre(centre));
            graphics.setColour(juce::Colours::white);
            const auto arm = radius * 0.55F;
            graphics.drawLine(centre.x - arm, centre.y, centre.x + arm, centre.y, 1.5F / viewScale_);
            graphics.drawLine(centre.x, centre.y - arm, centre.x, centre.y + arm, 1.5F / viewScale_);
        }
    }

    void drawDropTarget(juce::Graphics& graphics) const
    {
        drawPlusCircles(graphics);
        if (prototypeDropTarget_.mix)
            return;
        if (!prototypeDropTarget_.nodeId.empty())
        {
            const auto found =
                layoutIndexByNodeId_.find(prototypeDropTarget_.nodeId);
            if (found == layoutIndexByNodeId_.end())
                return;
            graphics.setColour(selectionColour.withAlpha(0.9F));
            graphics.drawRoundedRectangle(
                layoutCache_[found->second].bounds.toFloat().expanded(4.0F),
                8.0F,
                3.0F / viewScale_);
            return;
        }
        if (!prototypeDropTarget_.connection.has_value())
            return;
        const auto& wanted = *prototypeDropTarget_.connection;
        for (const auto& connection : connectionCache_)
        {
            if (connection.fromNode != wanted.fromNode
                || connection.sourcePort.id != wanted.fromPort
                || connection.toNode != wanted.toNode
                || connection.toPort != wanted.toPort)
            {
                continue;
            }
            juce::Path path;
            path.startNewSubPath(connection.start);
            for (int segment = 1; segment <= 32; ++segment)
                path.lineTo(connectionPoint(connection, static_cast<float>(segment) / 32.0F));
            graphics.setColour(selectionColour.withAlpha(0.9F));
            graphics.strokePath(path, juce::PathStrokeType{5.0F / viewScale_});
            return;
        }
    }

    static bool portDescriptorsEqual(
        const talas::core::PortDescriptor& left,
        const talas::core::PortDescriptor& right) noexcept
    {
        return left.id == right.id
            && left.type == right.type
            && left.required == right.required
            && left.signal.rate == right.signal.rate
            && left.signal.audioLayout == right.signal.audioLayout
            && left.signal.edge == right.signal.edge
            && left.signal.spectralFormat == right.signal.spectralFormat
            && left.signal.timelineBearing == right.signal.timelineBearing;
    }

    static bool portListsEqual(
        std::span<const talas::core::PortDescriptor> left,
        std::span<const talas::core::PortDescriptor> right) noexcept
    {
        return left.size() == right.size()
            && std::ranges::equal(left, right, portDescriptorsEqual);
    }

    static bool descriptorPresentationEqual(
        const talas::core::NodeTypeDescriptor& left,
        const talas::core::NodeTypeDescriptor& right) noexcept
    {
        return left.typeId == right.typeId
            && left.displayName == right.displayName
            && portListsEqual(left.inputs, right.inputs)
            && portListsEqual(left.outputs, right.outputs);
    }

    static bool soundDefinitionPresentationEqual(
        const talas::project::ProjectSoundDefinition& definition,
        const talas::core::NodeTypeDescriptor& descriptor) noexcept
    {
        constexpr std::string_view typePrefix{"talas.project.sound."};
        const std::string_view typeId{descriptor.typeId};
        if (!typeId.starts_with(typePrefix)
            || typeId.substr(typePrefix.size()) != definition.definitionId
            || descriptor.displayName != definition.displayName
            || descriptor.inputs.size() != definition.inputs.size()
            || descriptor.outputs.size() != definition.outputs.size())
        {
            return false;
        }
        const auto portsEqual = [](
                                    const auto& projectPorts,
                                    const auto& descriptorPorts,
                                    bool required) noexcept {
            for (std::size_t index = 0;
                 index < projectPorts.size(); ++index)
            {
                const auto& project = projectPorts[index];
                const auto& cached = descriptorPorts[index];
                const auto expectedSignal =
                    talas::core::defaultPortSignalContract(project.type);
                if (cached.id != project.portId
                    || cached.type != project.type
                    || cached.required != required
                    || cached.signal.rate != expectedSignal.rate
                    || cached.signal.audioLayout
                        != expectedSignal.audioLayout
                    || cached.signal.edge != expectedSignal.edge
                    || cached.signal.spectralFormat
                        != expectedSignal.spectralFormat
                    || cached.signal.timelineBearing
                        != expectedSignal.timelineBearing)
                {
                    return false;
                }
            }
            return true;
        };
        return portsEqual(definition.inputs, descriptor.inputs, true)
            && portsEqual(definition.outputs, descriptor.outputs, false);
    }

    [[nodiscard]] bool currentModelRequiresStructuralRebuild() const
    {
        const auto& document = bridge_.editor().document();
        const auto* graph = displayedGraph(document);
        if (graph == nullptr)
            return !layoutCache_.empty() || !connectionCache_.empty();
        if (graph->nodes.size() != layoutCache_.size()
            || graph->connections.size() != connectionCache_.size())
        {
            return true;
        }
        const auto presentationScope = soundDefinitionContext_.has_value()
            ? std::optional<std::string_view>{*soundDefinitionContext_}
            : std::nullopt;
        const auto* presentation = talas::project::findProjectStructureLayout(
            document, presentationScope);
        if (presentation == nullptr
            || presentation->nodes.size() != graph->nodes.size())
        {
            return true;
        }
        for (const auto& presented : presentation->nodes)
        {
            if (!layoutIndexByNodeId_.contains(presented.nodeId))
                return true;
        }
        if (document.soundDefinitions.size()
            != definitionIndexById_.size())
        {
            return true;
        }
        for (std::size_t index = 0; index < graph->nodes.size(); ++index)
        {
            const auto& node = graph->nodes[index];
            const auto& cached = layoutCache_[index];
            if (node.instanceId != cached.node.instanceId
                || node.typeId != cached.node.typeId
                || node.soundDefinitionId != cached.node.soundDefinitionId
                || node.preservedComponent != cached.node.preservedComponent)
            {
                return true;
            }
            if (node.soundDefinitionId.has_value())
            {
                const auto definition = definitionIndexById_.find(
                    *node.soundDefinitionId);
                if (definition == definitionIndexById_.end()
                    || definition->second
                        >= document.soundDefinitions.size()
                    || document.soundDefinitions[definition->second]
                           .definitionId
                        != *node.soundDefinitionId
                    || !soundDefinitionPresentationEqual(
                        document.soundDefinitions[definition->second],
                        cached.descriptor))
                {
                    return true;
                }
            }
            else if (node.preservedComponent.has_value()
                     && [&] {
                    const auto descriptor =
                        talas::project::describeProjectNodeForEditing(
                            document, node, registry_);
                    return !descriptor.has_value()
                        || !descriptorPresentationEqual(
                            *descriptor, cached.descriptor);
                }())
            {
                return true;
            }
        }
        for (std::size_t index = 0;
             index < graph->connections.size(); ++index)
        {
            const auto& connection = graph->connections[index];
            const auto& cached = connectionCache_[index];
            if (connection.fromNode != cached.fromNode
                || connection.fromPort != cached.sourcePort.id
                || connection.toNode != cached.toNode
                || connection.toPort != cached.toPort)
            {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool synchronizePresentationPositions()
    {
        const auto& document = bridge_.editor().document();
        const auto* graph = displayedGraph(document);
        if (graph == nullptr || graph->nodes.size() != layoutCache_.size())
            return false;
        const auto presentationScope = soundDefinitionContext_.has_value()
            ? std::optional<std::string_view>{*soundDefinitionContext_}
            : std::nullopt;
        const auto* presentation = talas::project::findProjectStructureLayout(
            document, presentationScope);
        if (presentation == nullptr
            || presentation->nodes.size() != graph->nodes.size())
        {
            return false;
        }
        auto changed = false;
        for (const auto& presented : presentation->nodes)
        {
            const auto found = layoutIndexByNodeId_.find(presented.nodeId);
            if (found == layoutIndexByNodeId_.end())
                continue;
            const auto position = juce::Point<int>{
                presented.position.x, presented.position.y};
            if (layoutCache_[found->second].bounds.getPosition() == position)
                continue;
            updateCachedNodePosition(presented.nodeId, position);
            changed = true;
        }
        return changed;
    }

    void rebuildLayoutCache()
    {
        ++structuralGraphRebuildCount_;
        layoutCache_.clear();
        layoutIndexByNodeId_.clear();
        connectionCache_.clear();
        connectionIndicesByNodeId_.clear();
        connectedInputKeys_.clear();
        connectedOutputKeys_.clear();
        const auto& document = bridge_.editor().document();
        definitionIndexById_.clear();
        definitionIndexById_.reserve(document.soundDefinitions.size());
        for (std::size_t index = 0;
             index < document.soundDefinitions.size(); ++index)
        {
            definitionIndexById_.emplace(
                document.soundDefinitions[index].definitionId,
                index);
        }
        const auto* graph = displayedGraph(document);
        if (graph == nullptr)
        {
            rebuildViewportIndices();
            return;
        }
        layoutCache_.reserve(graph->nodes.size());
        layoutIndexByNodeId_.reserve(graph->nodes.size());
        const auto presentationScope = soundDefinitionContext_.has_value()
            ? std::optional<std::string_view>{*soundDefinitionContext_}
            : std::nullopt;
        const auto& collapsed = collapsedBranches();
        const auto hidden = talas::app::hiddenGraphNodes(*graph, collapsed);
        for (const auto& id : hidden)
        {
            selectedNodeIds_.erase(id);
            if (selectedNodeId_ == id)
                selectedNodeId_.clear();
        }
        for (const auto& projectNode : graph->nodes)
        {
            if (hidden.contains(projectNode.instanceId))
                continue;
            const auto descriptor =
                talas::project::describeProjectNodeForEditing(
                document,
                projectNode,
                registry_);
            if (!descriptor.has_value())
                continue;
            const auto preserved =
                talas::app::missingNodeWorkbenchProjection(
                    document,
                    presentationScope,
                    projectNode.instanceId,
                    registry_);
            const auto* presented =
                talas::project::findProjectNodePresentation(
                    document,
                    presentationScope,
                    projectNode.instanceId);
            if (presented == nullptr)
                continue;

            const auto portRows = graphViewProjection_.showPorts
                ? std::max({
                      std::size_t{1},
                      presentedPortCount(descriptor->inputs),
                      presentedPortCount(descriptor->outputs),
                  })
                : std::size_t{};
            const auto nodeHeaderHeight = preserved.has_value()
                ? preservedHeaderHeight
                : headerHeight;
            const auto height = nodeHeaderHeight
                + static_cast<int>(portRows) * portRowHeight
                + (graphViewProjection_.showPorts ? 8 : 12);
            layoutIndexByNodeId_.emplace(
                projectNode.instanceId,
                layoutCache_.size());
            layoutCache_.push_back({
                projectNode,
                *descriptor,
                preserved,
                {presented->position.x,
                 presented->position.y,
                 nodeWidth,
                 height},
                nodeHeaderHeight,
                talas::app::graphNodeHasInputs(*graph, projectNode.instanceId),
                collapsed.contains(projectNode.instanceId),
            });
        }
        rebuildViewportIndices();
        updateHorizontalScroll();
    }

    [[nodiscard]] std::string branchScopeKey() const
    {
        return soundDefinitionContext_.has_value()
            ? "definition:" + *soundDefinitionContext_
            : std::string{"root"};
    }

    [[nodiscard]] const talas::app::GraphNodeSet& collapsedBranches()
    {
        return collapsedBranches_[branchScopeKey()];
    }

    // The dot or ellipsis on a Sound's left edge, beside its title.
    [[nodiscard]] static juce::Rectangle<float> branchMarkerBounds(
        const NodeLayout& layout)
    {
        const auto centreY = static_cast<float>(layout.bounds.getY())
            + static_cast<float>(layout.headerHeight) * 0.5F;
        return layout.branchHidden
            ? juce::Rectangle<float>{22.0F, 14.0F}.withCentre(
                  {static_cast<float>(layout.bounds.getX()), centreY})
            : juce::Rectangle<float>{9.0F, 9.0F}.withCentre(
                  {static_cast<float>(layout.bounds.getX()), centreY});
    }

    [[nodiscard]] std::optional<std::string> branchMarkerAt(
        juce::Point<int> graphPosition) const
    {
        const auto point = graphPosition.toFloat();
        for (const auto& layout : layoutCache_)
        {
            if (layout.hasInputs
                && branchMarkerBounds(layout).expanded(4.0F).contains(point))
                return layout.node.instanceId;
        }
        return std::nullopt;
    }

    // A click on the dot hides the Sounds to its left, on the ellipsis
    // shows them; a double-click or Control-click opens the whole branch.
    bool toggleBranch(std::string_view nodeId, bool fully)
    {
        const auto* graph = displayedGraph(bridge_.editor().document());
        if (graph == nullptr || !talas::app::graphNodeHasInputs(*graph, nodeId))
            return false;
        auto& collapsed = collapsedBranches_[branchScopeKey()];
        const auto wasHidden = collapsed.contains(nodeId);
        collapsed = fully
            ? talas::app::fullyOpenedGraphBranch(*graph, std::move(collapsed), nodeId)
            : talas::app::toggledGraphBranch(*graph, std::move(collapsed), nodeId);
        rebuildLayoutCache();
        rebuildAccessibilityItems();
        repaint();
        onStatus_((fully ? juce::String{"Showed every Sound to the left of "}
                      : wasHidden ? juce::String{"Showed the inputs of "}
                                  : juce::String{"Hid the inputs of "})
            + juceString(std::string{nodeId}));
        return true;
    }

    static talas::app::WorkbenchGraphViewportRect viewportRect(
        juce::Rectangle<int> bounds) noexcept
    {
        return {bounds.getX(), bounds.getY(),
                bounds.getWidth(), bounds.getHeight()};
    }

    void rebuildViewportIndices()
    {
        nodeViewportRects_.clear();
        nodeViewportRects_.reserve(layoutCache_.size());
        for (const auto& layout : layoutCache_)
            nodeViewportRects_.push_back(viewportRect(layout.bounds));
        nodeViewportIndex_.rebuild(nodeViewportRects_);
        // Query + keyboard selection + the independently tracked pointer drag
        // are all bounded by N. Prepare their combined envelope once.
        visibleLayoutIndices_.reserve(
            scratchCapacity(layoutCache_.size(), 3U));
        nodeHitIndices_.reserve(
            scratchCapacity(layoutCache_.size(), 3U));

        connectionCache_.clear();
        connectionIndicesByNodeId_.clear();
        connectedInputKeys_.clear();
        connectedOutputKeys_.clear();
        rebuildConnectionCache();
        connectionViewportRects_.clear();
        connectionViewportRects_.reserve(connectionCache_.size());
        for (const auto& connection : connectionCache_)
            connectionViewportRects_.push_back(
                viewportRect(connection.bounds));
        connectionViewportIndex_.rebuild(connectionViewportRects_);
        // A full-viewport query may contribute E entries, a moving
        // multi-selection contributes each incident edge once per endpoint,
        // and a separately tracked pointer drag can contribute another E.
        // Prepare the exact E + 2E + E envelope outside paint/hit paths.
        visibleConnectionIndices_.reserve(
            scratchCapacity(connectionCache_.size(), 4U));
        refreshViewportSelection(getLocalBounds());
    }

    [[nodiscard]] static std::size_t scratchCapacity(
        std::size_t count,
        std::size_t multiplier)
    {
        if (multiplier != 0
            && count > std::numeric_limits<std::size_t>::max() / multiplier)
        {
            throw std::length_error{"graph scratch capacity overflow"};
        }
        return count * multiplier;
    }

    void rebuildConnectionCache()
    {
        const auto* graph = displayedGraph(bridge_.editor().document());
        if (graph == nullptr)
            return;
        connectionCache_.reserve(graph->connections.size());
        connectionIndicesByNodeId_.reserve(layoutCache_.size());
        connectedInputKeys_.reserve(graph->connections.size());
        connectedOutputKeys_.reserve(graph->connections.size());
        const auto useCounts = talas::app::graphConnectionUseCounts(*graph);
        std::set<std::tuple<std::string_view, std::string_view, std::string_view>> labelledUses;
        for (std::size_t graphIndex = 0; graphIndex < graph->connections.size(); ++graphIndex)
        {
            const auto& connection = graph->connections[graphIndex];
            const auto fromIndex = layoutIndexByNodeId_.find(
                connection.fromNode);
            const auto toIndex = layoutIndexByNodeId_.find(
                connection.toNode);
            if (fromIndex == layoutIndexByNodeId_.end()
                || toIndex == layoutIndexByNodeId_.end())
                continue;
            const auto& from = layoutCache_[fromIndex->second];
            const auto& to = layoutCache_[toIndex->second];
            const auto start = portAnchor(from, connection.fromPort, true);
            const auto end = portAnchor(to, connection.toPort, false);
            const auto sourcePort = std::ranges::find(
                from.descriptor.outputs,
                connection.fromPort,
                &talas::core::PortDescriptor::id);
            if (!start.has_value() || !end.has_value()
                || sourcePort == from.descriptor.outputs.end())
                continue;
            const auto controlDistance = std::max(
                42.0F, std::abs(end->x - start->x) * 0.45F);
            const std::array points{
                *start,
                juce::Point<float>{start->x + controlDistance, start->y},
                juce::Point<float>{end->x - controlDistance, end->y},
                *end,
            };
            auto minimumX = points.front().x;
            auto maximumX = points.front().x;
            auto minimumY = points.front().y;
            auto maximumY = points.front().y;
            for (const auto point : points | std::views::drop(1))
            {
                minimumX = std::min(minimumX, point.x);
                maximumX = std::max(maximumX, point.x);
                minimumY = std::min(minimumY, point.y);
                maximumY = std::max(maximumY, point.y);
            }
            constexpr auto padding = 18.0F;
            const auto bounds = juce::Rectangle<float>{
                minimumX - padding,
                minimumY - padding,
                maximumX - minimumX + padding * 2.0F,
                maximumY - minimumY + padding * 2.0F,
            }.getSmallestIntegerContainer();
            const auto index = connectionCache_.size();
            connectionCache_.push_back({
                connection.fromNode,
                connection.toNode,
                connection.toPort,
                *sourcePort,
                *start,
                *end,
                bounds,
                useCounts[graphIndex],
                useCounts[graphIndex] > 1
                    && labelledUses.insert({connection.fromNode, connection.fromPort, connection.toNode}).second,
            });
            connectionIndicesByNodeId_[connection.fromNode].push_back(index);
            connectionIndicesByNodeId_[connection.toNode].push_back(index);
            connectedOutputKeys_.insert(portIdentityKey(
                connection.fromNode, connection.fromPort));
            connectedInputKeys_.insert(portIdentityKey(
                connection.toNode, connection.toPort));
        }
    }

    static std::string portIdentityKey(
        std::string_view nodeId,
        std::string_view portId)
    {
        std::string key;
        key.reserve(nodeId.size() + portId.size() + 1U);
        key.append(nodeId);
        key.push_back('\0');
        key.append(portId);
        return key;
    }

    [[nodiscard]] bool portIsConnected(
        std::string_view nodeId,
        std::string_view portId,
        bool output) const
    {
        const auto key = portIdentityKey(nodeId, portId);
        return output
            ? connectedOutputKeys_.contains(key)
            : connectedInputKeys_.contains(key);
    }

    void rebuildAccessibilityItems()
    {
        ++accessibilityGraphRebuildCount_;
        juce::String focusedKey;
        for (auto* item : accessibilityItems_)
        {
            if (item->hasKeyboardFocus(false))
            {
                focusedKey = item->focusKey();
                break;
            }
        }
        focusedAccessibilityItem_ = nullptr;
        accessibilityItems_.clear(true);
        accessibilityItemsByNodeId_.clear();
        accessibilityItemsByNodeId_.reserve(layoutCache_.size());
        accessibilityOrderByItem_.clear();
        accessibilityOrderByItem_.reserve(
            accessibilityItems_.size());
        const auto* graph = displayedGraph(bridge_.editor().document());
        if (graph == nullptr)
        {
            if (focusedKey.isNotEmpty())
                grabKeyboardFocus();
            return;
        }

        int focusOrder = 1;
        for (const auto& layout : layoutCache_)
        {
            const auto nodeHasDiagnostic = std::ranges::find(
                diagnosticNodeIds_,
                layout.node.instanceId) != diagnosticNodeIds_.end();
            auto nodeHelp = juce::String{
                "Graph node with "
                + juce::String(static_cast<int>(
                    presentedPortCount(layout.descriptor.inputs)))
                + " inputs and "
                + juce::String(static_cast<int>(
                    presentedPortCount(layout.descriptor.outputs)))
                + " outputs. Press to select; use arrow keys to move by one grid step, or Shift plus arrow for one pixel."};
            auto nodeTitle = juceString(layout.descriptor.displayName)
                + ", node "
                + juceString(layout.node.instanceId);
            if (layout.preserved.has_value())
            {
                nodeTitle = juceString(
                    layout.preserved->accessibleName);
                nodeHelp = juceString(
                    layout.preserved->accessibleDescription)
                    + ". " + nodeHelp;
                if (!layout.preserved->parameterEditingEnabled)
                {
                    nodeHelp +=
                        " Existing connections may be removed, but new connections and parameter edits are unavailable until exact repair succeeds.";
                }
            }
            auto* nodeItem = accessibilityItems_.add(
                new AccessibilityItem(
                    *this,
                    AccessibilityItem::Kind::Node,
                    layout.node.instanceId,
                    std::nullopt,
                    nodeTitle,
                    nodeHelp));
            accessibilityOrderByItem_.emplace(
                nodeItem,
                static_cast<std::size_t>(
                    accessibilityItems_.size() - 1));
            nodeItem->setDiagnostic(nodeHasDiagnostic);
            accessibilityItemsByNodeId_[layout.node.instanceId]
                .push_back(nodeItem);
            const auto nodeFocusKey = nodeItem->focusKey().toStdString();
            const std::array nodeFocusFields{
                std::string_view{nodeFocusKey}};
            nodeItem->setComponentID(juceString(
                workbenchStableFocusItemId(
                    "graph.item", nodeFocusFields)));
            nodeItem->setBounds(graphToCanvas(layout.bounds));
            nodeItem->setExplicitFocusOrder(focusOrder++);
            accessibilityContainer_.addAndMakeVisible(nodeItem);

            const auto addPort = [&](
                                     const talas::core::PortDescriptor& port,
                                     bool output) {
                const auto anchor = portAnchor(layout, port.id, output);
                if (!anchor.has_value())
                    return;
                const auto connected = portIsConnected(
                    layout.node.instanceId, port.id, output);
                const auto direction = output ? juce::String{"Output"} : juce::String{"Input"};
                auto help = juceString(
                    talas::app::descriptorPortInfoHint(port, output));
                help += connected ? ". Connected. " : ". Not connected. ";
                help += output
                    ? "Press to begin a keyboard connection, then press a compatible input."
                    : "Press to complete a pending connection, or disconnect the current input.";
                PortHit hit{
                    layout.node.instanceId,
                    port.id,
                    port,
                    *anchor,
                };
                auto* portItem = accessibilityItems_.add(
                    new AccessibilityItem(
                        *this,
                        output
                            ? AccessibilityItem::Kind::OutputPort
                            : AccessibilityItem::Kind::InputPort,
                        layout.node.instanceId,
                        hit,
                        direction
                            + " "
                            + juceString(port.id)
                            + " on "
                            + juceString(layout.descriptor.displayName),
                        help));
                accessibilityOrderByItem_.emplace(
                    portItem,
                    static_cast<std::size_t>(
                        accessibilityItems_.size() - 1));
                accessibilityItemsByNodeId_[layout.node.instanceId]
                    .push_back(portItem);
                const auto portFocusKey = portItem->focusKey().toStdString();
                const std::array portFocusFields{
                    std::string_view{portFocusKey}};
                portItem->setComponentID(juceString(
                    workbenchStableFocusItemId(
                        "graph.item", portFocusFields)));
                portItem->setBounds(
                    juce::Rectangle<int>{28, 28}.withCentre(
                        graphToCanvas(*anchor).roundToInt()));
                portItem->setExplicitFocusOrder(focusOrder++);
                accessibilityContainer_.addAndMakeVisible(portItem);
            };

            if (graphViewProjection_.showPorts)
            {
                for (const auto& port : layout.descriptor.inputs)
                {
                    if (!port.compilerPopulated)
                        addPort(port, false);
                }
                for (const auto& port : layout.descriptor.outputs)
                {
                    if (!port.compilerPopulated)
                        addPort(port, true);
                }
            }
        }

        if (focusedKey.isNotEmpty())
        {
            for (auto* item : accessibilityItems_)
            {
                if (item->focusKey() == focusedKey)
                {
                    item->grabKeyboardFocus();
                    return;
                }
            }
            grabKeyboardFocus();
        }
    }

    void updateAccessibilityGeometry()
    {
        for (const auto layoutIndex : visibleLayoutIndices_)
        {
            if (layoutIndex >= layoutCache_.size())
                continue;
            const auto found = accessibilityItemsByNodeId_.find(
                layoutCache_[layoutIndex].node.instanceId);
            if (found == accessibilityItemsByNodeId_.end())
                continue;
            for (auto* item : found->second)
                item->refreshGeometry();
        }
    }

    void updateCachedNodePosition(
        std::string_view nodeId,
        juce::Point<int> position)
    {
        const auto found = layoutIndexByNodeId_.find(nodeId);
        if (found == layoutIndexByNodeId_.end())
            return;
        layoutCache_[found->second].bounds.setPosition(position);
        nodeViewportRects_[found->second] = viewportRect(
            layoutCache_[found->second].bounds);
        if (const auto accessibility = accessibilityItemsByNodeId_.find(
                nodeId);
            accessibility != accessibilityItemsByNodeId_.end())
        {
            for (auto* item : accessibility->second)
                item->refreshGeometry();
        }
        const auto connections = connectionIndicesByNodeId_.find(
            nodeId);
        if (connections == connectionIndicesByNodeId_.end())
            return;
        for (const auto connectionIndex : connections->second)
            updateConnectionGeometry(connectionIndex);
    }

    void updateConnectionGeometry(std::size_t index)
    {
        if (index >= connectionCache_.size())
            return;
        auto& connection = connectionCache_[index];
        const auto fromIndex = layoutIndexByNodeId_.find(
            connection.fromNode);
        const auto toIndex = layoutIndexByNodeId_.find(
            connection.toNode);
        if (fromIndex == layoutIndexByNodeId_.end()
            || toIndex == layoutIndexByNodeId_.end())
            return;
        const auto start = portAnchor(
            layoutCache_[fromIndex->second],
            connection.sourcePort.id,
            true);
        const auto end = portAnchor(
            layoutCache_[toIndex->second],
            connection.toPort,
            false);
        if (!start.has_value() || !end.has_value())
            return;
        connection.start = *start;
        connection.end = *end;
        const auto controlDistance = std::max(
            42.0F, std::abs(end->x - start->x) * 0.45F);
        const auto minimumX = std::min({
            start->x, start->x + controlDistance,
            end->x - controlDistance, end->x});
        const auto maximumX = std::max({
            start->x, start->x + controlDistance,
            end->x - controlDistance, end->x});
        constexpr auto padding = 18.0F;
        connection.bounds = juce::Rectangle<float>{
            minimumX - padding,
            std::min(start->y, end->y) - padding,
            maximumX - minimumX + padding * 2.0F,
            std::abs(end->y - start->y) + padding * 2.0F,
        }.getSmallestIntegerContainer();
        if (index < connectionViewportRects_.size())
            connectionViewportRects_[index] = viewportRect(connection.bounds);
    }

    void selectAccessibilityNode(std::string_view nodeId)
    {
        selectedNodeIds_.clear();
        selectedNodeIds_.insert(std::string{nodeId});
        selectedNodeId_ = std::string{nodeId};
        onSelection_(soundDefinitionContext_, selectedNodeId_, false);
        onStatus_("Selected " + juceString(nodeId));
        setSelectedNodeId(selectedNodeId_);
    }

    void focusAccessibilityItem(
        AccessibilityItem::Kind,
        std::string_view nodeId)
    {
        if (selectedNodeId_ != nodeId)
            selectAccessibilityNode(nodeId);
    }

    void activateAccessibilityPort(
        const PortHit& port,
        bool output)
    {
        if (!activeContext_)
        {
            onStatus_("Stale or hidden graph interaction rejected");
            return;
        }
        selectedNodeIds_.clear();
        selectedNodeIds_.insert(port.nodeId);
        selectedNodeId_ = port.nodeId;
        onSelection_(soundDefinitionContext_, selectedNodeId_, false);
        if (output)
        {
            pendingConnection_ = port;
            pendingConnectionPointer_ = port.anchor;
            onStatus_(
                "Connection started at "
                + juceString(port.nodeId)
                + "."
                + juceString(port.portId)
                + "; choose an input port");
            repaint();
            return;
        }

        talas::project::EditResult result;
        if (pendingConnection_.has_value())
        {
            const auto source = *pendingConnection_;
            result = connect({
                source.nodeId,
                source.portId,
                port.nodeId,
                port.portId,
            });
            if (!result.ok())
            {
                onStatus_(connectionFailureStatus(result, source, port));
                pendingConnection_.reset();
                repaint();
                return;
            }
        }
        else
        {
            result = disconnect(
                port.nodeId,
                port.portId);
        }
        pendingConnection_.reset();
        if (!result.ok())
        {
            onStatus_(juceString(result.message));
            repaint();
            return;
        }
        onStatus_(
            result.message.empty()
                ? juce::String{"Connection updated"}
                : juceString(result.message));
        scheduleDocumentChangedFromKeyboard();
    }

    void requestKeyboardCommand(
        GraphKeyboardCommand command,
        std::optional<talas::project::CanvasPosition> pastePosition = std::nullopt)
    {
        if (!activeContext_)
        {
            onStatus_("Stale or hidden graph command rejected");
            return;
        }
        if (pendingConnection_.has_value())
            cancelPendingConnection();
        // A Sound's inputs must show before it is deleted.
        if (const auto* graph = displayedGraph(bridge_.editor().document());
            command == GraphKeyboardCommand::DeleteSelection && graph != nullptr)
        {
            for (const auto& nodeId : selectedNodesInDocumentOrder())
            {
                if (!talas::app::graphNodeDeletable(*graph, collapsedBranches(), nodeId))
                {
                    onStatus_("Click the ellipsis to show the inputs of " + juceString(nodeId) + " before deleting it");
                    return;
                }
            }
        }
        GraphKeyboardCommandRequest request;
        request.definitionId = soundDefinitionContext_;
        request.command = command;
        request.pastePosition = pastePosition;
        request.selectedNodeIds = selectedNodesInDocumentOrder();
        request.expectedDocumentSnapshot =
            talas::project::serializeProjectDocument(
                bridge_.editor().document());
        pendingKeyboardCommands_.push_back(std::move(request));
        if (keyboardCommandDispatchScheduled_)
            return;
        keyboardCommandDispatchScheduled_ = true;
        juce::Component::SafePointer<GraphCanvasImpl> safeThis(this);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->drainPendingKeyboardCommands();
        });
    }

    void drainPendingKeyboardCommands()
    {
        keyboardCommandDispatchScheduled_ = false;
        if (pendingKeyboardCommands_.empty())
            return;
        auto pending = std::move(pendingKeyboardCommands_);
        pendingKeyboardCommands_.clear();
        finishKeyboardNudge();
        if (!onKeyboardCommand_)
            return;
        for (auto& request : pending)
            onKeyboardCommand_(std::move(request));
    }

    [[nodiscard]] std::vector<std::string>
    selectedNodesInDocumentOrder() const
    {
        std::vector<std::pair<std::size_t, std::string_view>> ordered;
        ordered.reserve(selectedNodeIds_.size());
        for (const auto& nodeId : selectedNodeIds_)
        {
            const auto found = layoutIndexByNodeId_.find(nodeId);
            if (found != layoutIndexByNodeId_.end())
                ordered.emplace_back(found->second, nodeId);
        }
        std::ranges::sort(
            ordered,
            [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
        std::vector<std::string> selected;
        selected.reserve(ordered.size());
        for (const auto& [index, nodeId] : ordered)
            selected.emplace_back(nodeId);
        return selected;
    }

    [[nodiscard]] bool keyboardNudgeSelectionIsCurrent() const
    {
        return selectedNodeIds_.size() == keyboardNudgeNodeIds_.size()
            && std::ranges::all_of(
                keyboardNudgeNodeIds_,
                [&](const auto& nodeId) {
                    return selectedNodeIds_.contains(nodeId);
                });
    }

    bool moveSelectedAccessibilityNodes(
        int deltaX,
        int deltaY)
    {
        if (!activeContext_)
        {
            onStatus_("Stale or hidden graph interaction rejected");
            return false;
        }
        std::vector<std::string> initialSelection;
        if (!keyboardNudgeActive_)
            initialSelection = selectedNodesInDocumentOrder();
        if (keyboardNudgeActive_
            && (!keyboardNudgeSelectionIsCurrent()
                || soundDefinitionContext_ != keyboardNudgeDefinitionId_))
        {
            onStatus_("Graph selection changed before keyboard nudge");
            cancelKeyboardNudge();
            return false;
        }
        const std::span<const std::string> selected = keyboardNudgeActive_
            ? std::span<const std::string>{keyboardNudgeNodeIds_}
            : std::span<const std::string>{initialSelection};
        auto minimumX = std::numeric_limits<int>::max();
        auto minimumY = std::numeric_limits<int>::max();
        auto maximumX = std::numeric_limits<int>::min();
        auto maximumY = std::numeric_limits<int>::min();
        for (const auto& nodeId : selected)
        {
            const auto found = layoutIndexByNodeId_.find(nodeId);
            if (found == layoutIndexByNodeId_.end())
                continue;
            const auto& layout = layoutCache_[found->second];
            const auto position = layout.bounds.getPosition();
            minimumX = std::min(minimumX, position.x);
            minimumY = std::min(minimumY, position.y);
            maximumX = std::max(maximumX, position.x);
            maximumY = std::max(maximumY, position.y);
        }
        if (selected.empty()
            || selected.size() != selectedNodeIds_.size())
        {
            onStatus_("Graph selection changed before keyboard nudge");
            return false;
        }

        const auto boundedDelta = [](int requested, int minimum, int maximum) {
            const auto low = -static_cast<std::int64_t>(minimum);
            const auto high =
                static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())
                - maximum;
            return static_cast<int>(std::clamp(
                static_cast<std::int64_t>(requested), low, high));
        };
        auto effectiveX = boundedDelta(deltaX, minimumX, maximumX);
        auto effectiveY = boundedDelta(deltaY, minimumY, maximumY);
        if (keyboardNudgeActive_)
        {
            const auto clampAccumulatedDelta = [](int requested, int current) {
                const auto low =
                    static_cast<std::int64_t>(
                        std::numeric_limits<std::int32_t>::min())
                    - current;
                const auto high =
                    static_cast<std::int64_t>(
                        std::numeric_limits<std::int32_t>::max())
                    - current;
                return static_cast<int>(std::clamp(
                    static_cast<std::int64_t>(requested), low, high));
            };
            effectiveX = clampAccumulatedDelta(
                effectiveX, keyboardNudgeDelta_.x);
            effectiveY = clampAccumulatedDelta(
                effectiveY, keyboardNudgeDelta_.y);
        }
        if (effectiveX == 0 && effectiveY == 0)
        {
            onStatus_("Selected nodes are at the canvas boundary");
            return true;
        }
        if (!keyboardNudgeActive_)
        {
            keyboardNudgeActive_ = true;
            keyboardNudgeChanged_ = false;
            keyboardNudgeNodeIds_.assign(
                selected.begin(), selected.end());
            keyboardNudgeDefinitionId_ = soundDefinitionContext_;
            keyboardNudgeDocumentSnapshot_ =
                talas::project::serializeProjectDocument(
                    bridge_.editor().document());
            keyboardNudgeDelta_ = {};
        }
        keyboardNudgeDelta_.x += effectiveX;
        keyboardNudgeDelta_.y += effectiveY;
        keyboardNudgeChanged_ = true;
        for (const auto& nodeId : selected)
        {
            const auto found = layoutIndexByNodeId_.find(nodeId);
            if (found == layoutIndexByNodeId_.end())
                continue;
            auto position = layoutCache_[found->second].bounds.getPosition();
            position += juce::Point<int>{effectiveX, effectiveY};
            updateCachedNodePosition(nodeId, position);
        }
        onStatus_(
            "Moved " + juce::String(static_cast<int>(selected.size()))
            + (selected.size() == 1 ? " selected node" : " selected nodes"));
        repaint();
        return true;
    }

    void finishKeyboardNudge()
    {
        if (!keyboardNudgeActive_)
            return;
        keyboardNudgeActive_ = false;
        const auto changed = std::exchange(keyboardNudgeChanged_, false);
        if (!changed)
        {
            clearKeyboardNudgeState();
            return;
        }
        const auto documentUnchanged =
            talas::project::serializeProjectDocument(
                bridge_.editor().document())
            == keyboardNudgeDocumentSnapshot_;
        const auto contextUnchanged =
            activeContext_
            && soundDefinitionContext_ == keyboardNudgeDefinitionId_
            && selectedNodesInDocumentOrder() == keyboardNudgeNodeIds_;
        if (!documentUnchanged || !contextUnchanged)
        {
            clearKeyboardNudgeState();
            scheduleModelRefreshAfterKeyEvent();
            onStatus_("Keyboard nudge cancelled after graph authority changed");
            repaint();
            return;
        }
        const auto result = keyboardNudgeDefinitionId_.has_value()
            ? bridge_.editor().moveNodesInSoundDefinition(
                  *keyboardNudgeDefinitionId_,
                  keyboardNudgeNodeIds_,
                  keyboardNudgeDelta_)
            : bridge_.editor().moveNodes(
                  keyboardNudgeNodeIds_,
                  keyboardNudgeDelta_);
        clearKeyboardNudgeState();
        if (!result.ok())
        {
            scheduleModelRefreshAfterKeyEvent();
            onStatus_(juceString(result.message));
            repaint();
            return;
        }
        rebuildViewportIndices();
        scheduleDocumentChangedFromKeyboard();
    }

    void clearKeyboardNudgeState()
    {
        keyboardNudgeNodeIds_.clear();
        keyboardNudgeDefinitionId_.reset();
        keyboardNudgeDocumentSnapshot_.clear();
        keyboardNudgeDelta_ = {};
    }

    void cancelKeyboardNudge()
    {
        keyboardNudgeActive_ = false;
        keyboardNudgeChanged_ = false;
        clearKeyboardNudgeState();
        scheduleModelRefreshAfterKeyEvent();
        repaint();
    }

    void cancelKeyboardInteraction(std::string_view focusedNodeId)
    {
        if (keyboardNudgeActive_)
        {
            cancelKeyboardNudge();
            onStatus_("Keyboard nudge cancelled");
            return;
        }
        if (pendingConnection_.has_value())
        {
            cancelPendingConnection();
            return;
        }
        if (!draggingNodeId_.empty())
        {
            bridge_.editor().cancelOwnedHistoryTransaction(
                dragTransactionToken_);
            dragTransactionToken_ = 0;
            draggingNodeId_.clear();
            draggedNodeMoved_ = false;
            scheduleModelRefreshAfterKeyEvent();
            onStatus_("Node drag cancelled");
            repaint();
            return;
        }
        if (selectedNodeIds_.size() > 1
            && selectedNodeIds_.contains(std::string{focusedNodeId}))
        {
            selectAccessibilityNode(focusedNodeId);
            onStatus_("Selection extension cancelled");
            return;
        }
        onStatus_("No pending graph interaction");
    }

    void cancelPendingConnection()
    {
        if (!pendingConnection_.has_value())
            return;
        pendingConnection_.reset();
        onStatus_("Connection cancelled");
        repaint();
    }

    void scheduleModelRefreshAfterKeyEvent()
    {
        juce::Component::SafePointer<GraphCanvasImpl> safeThis(this);
        juce::MessageManager::callAsync([safeThis] {
            if (safeThis != nullptr)
                safeThis->refreshModel();
        });
    }

    [[nodiscard]] talas::project::EditResult moveNode(
        std::string_view nodeId,
        talas::project::CanvasPosition position,
        talas::project::ProjectEditor::HistoryTransactionToken
            transactionToken = 0)
    {
        return soundDefinitionContext_.has_value()
            ? bridge_.editor().moveNodeInSoundDefinition(
                  *soundDefinitionContext_,
                  nodeId,
                  position,
                  transactionToken)
            : bridge_.editor().moveNode(
                  nodeId, position, transactionToken);
    }

    [[nodiscard]] talas::project::EditResult connect(
        talas::core::Connection connection)
    {
        return soundDefinitionContext_.has_value()
            ? bridge_.editor().connectInSoundDefinition(
                  *soundDefinitionContext_,
                  std::move(connection))
            : bridge_.editor().connect(std::move(connection));
    }

    [[nodiscard]] juce::String connectionFailureStatus(
        const talas::project::EditResult& result,
        const PortHit& source,
        const PortHit& destination) const
    {
        if (result.error
            != talas::project::EditError::IncompatiblePortTypes)
        {
            return juceString(result.message);
        }
        auto message = juceString(
            talas::app::descriptorConnectionCompatibilitySummary(
                source.descriptor,
                destination.descriptor));
        const auto adapters = talas::app::descriptorAdapterSuggestions(
            source.descriptor,
            destination.descriptor,
            registry_);
        if (adapters.empty())
            return message + " No compatible adapter is registered.";
        message += adapters.size() == 1
            ? " Suggested adapter: "
            : " Suggested adapters: ";
        for (std::size_t index = 0; index < adapters.size(); ++index)
        {
            if (index != 0)
                message += ", ";
            message += juceString(adapters[index].displayName)
                + " (" + juceString(adapters[index].typeId) + ")";
        }
        return message + ".";
    }

    [[nodiscard]] talas::project::EditResult disconnect(
        std::string_view toNode,
        std::string_view toPort)
    {
        return soundDefinitionContext_.has_value()
            ? bridge_.editor().disconnectInSoundDefinition(
                  *soundDefinitionContext_,
                  toNode,
                  toPort)
            : bridge_.editor().disconnect(toNode, toPort);
    }

    void scheduleDocumentChangedFromKeyboard()
    {
        if (keyboardRefreshPending_)
            return;
        keyboardRefreshPending_ = true;
        juce::Component::SafePointer<GraphCanvasImpl> safeThis(this);
        juce::Timer::callAfterDelay(75, [safeThis] {
            if (safeThis == nullptr)
                return;
            safeThis->keyboardRefreshPending_ = false;
            safeThis->onDocumentChange_();
        });
    }

    // Faint grid lines, with a brighter dot where every fourth meets.
    void drawGrid(juce::Graphics& graphics) const
    {
        const auto spacing = 32.0F * viewScale_;
        if (spacing < 6.0F)
            return;
        const auto first = [](float offset, float pitch) {
            return static_cast<long>(std::floor(-offset / pitch)) + 1;
        };
        const auto width = static_cast<float>(getWidth());
        const auto height = static_cast<float>(getHeight());
        graphics.setColour(workbenchTheme.graphGrid.withAlpha(0.55F));
        for (auto column = first(viewOffset_.x, spacing);; ++column)
        {
            const auto x = viewOffset_.x + static_cast<float>(column) * spacing;
            if (x > width)
                break;
            graphics.drawVerticalLine(juce::roundToInt(x), 0.0F, height);
        }
        for (auto row = first(viewOffset_.y, spacing);; ++row)
        {
            const auto y = viewOffset_.y + static_cast<float>(row) * spacing;
            if (y > height)
                break;
            graphics.drawHorizontalLine(juce::roundToInt(y), 0.0F, width);
        }
        const auto major = spacing * 4.0F;
        graphics.setColour(workbenchTheme.graphGrid.brighter(1.2F));
        for (auto row = first(viewOffset_.y, major);; ++row)
        {
            const auto y = juce::roundToInt(viewOffset_.y + static_cast<float>(row) * major);
            if (static_cast<float>(y) > height)
                break;
            for (auto column = first(viewOffset_.x, major);; ++column)
            {
                const auto x = juce::roundToInt(viewOffset_.x + static_cast<float>(column) * major);
                if (static_cast<float>(x) > width)
                    break;
                graphics.fillRect(x - 1, y - 1, 3, 3);
            }
        }
    }

    void drawViewStatus(juce::Graphics& graphics) const
    {
        if (std::abs(viewScale_ - 1.0F) < 0.01F
            && std::abs(viewOffset_.x) < 1.0F
            && std::abs(viewOffset_.y) < 1.0F)
        {
            return;
        }
        const auto label = juce::String(
                               static_cast<int>(std::round(viewScale_ * 100.0F)))
            + "%  |  wheel pan  |  Cmd-wheel zoom  |  Cmd-0 fit";
        const auto bounds = getLocalBounds()
            .removeFromBottom(24)
            .removeFromRight(284)
            .reduced(8, 3);
        WorkbenchStyle::paintTile(graphics, bounds.toFloat(), bounds.toFloat().getHeight() * 0.5F,
            workbenchTheme.surfaceRaised.withAlpha(0.94F));
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(
            label,
            bounds.reduced(6, 0),
            juce::Justification::centredRight);
    }

    void drawContextBadge(juce::Graphics& graphics) const
    {
        const auto label = contextPresentation_
            + (activeContext_ ? "  /  EDIT" : "  /  VIEW");
        const auto badgeWidth = contextPresentation_.startsWith("SOUND")
            ? 184
            : 122;
        const auto bounds = getLocalBounds()
            .removeFromTop(32)
            .removeFromLeft(std::min(getWidth(), badgeWidth))
            .reduced(8, 5);
        WorkbenchStyle::paintTile(graphics, bounds.toFloat(), bounds.toFloat().getHeight() * 0.5F,
            workbenchTheme.surfaceRaised.withAlpha(0.96F));
        if (activeContext_)
        {
            graphics.setGradientFill(WorkbenchStyle::accentGradient(bounds.toFloat()));
            graphics.drawRoundedRectangle(bounds.toFloat().reduced(0.5F), bounds.toFloat().getHeight() * 0.5F, 1.5F);
        }
        graphics.setColour(
            activeContext_ ? textColour : secondaryTextColour);
        graphics.setFont(WorkbenchType::microStrong());
        graphics.drawText(
            label,
            bounds.reduced(7, 0),
            juce::Justification::centredLeft);
    }

    static std::optional<juce::Point<float>> portAnchor(const NodeLayout& layout,
                                                         std::string_view portId,
                                                         bool output)
    {
        const auto& ports = output
            ? layout.descriptor.outputs
            : layout.descriptor.inputs;
        const auto found = std::ranges::find_if(ports, [&](const talas::core::PortDescriptor& port) {
            return port.id == portId;
        });
        if (found == ports.end() || found->compilerPopulated)
            return std::nullopt;

        const auto index = static_cast<int>(std::ranges::count_if(
            ports.begin(),
            found,
            [](const auto& port) { return !port.compilerPopulated; }));
        const auto y = static_cast<float>(layout.bounds.getY() + layout.headerHeight + index * portRowHeight + portRowHeight / 2);
        const auto x = static_cast<float>(output ? layout.bounds.getRight() : layout.bounds.getX());
        return juce::Point<float>{x, y};
    }

    std::optional<PortHit> findPortAt(
        juce::Point<int> point,
        bool output,
        float hitRadius)
    {
        const auto pointAsFloat = point.toFloat();
        queryNodeHits(point, hitRadius);
        for (auto iterator = nodeHitIndices_.rbegin();
             iterator != nodeHitIndices_.rend(); ++iterator)
        {
            const auto& layout = layoutCache_[*iterator];
            const auto& ports = output
                ? layout.descriptor.outputs
                : layout.descriptor.inputs;
            for (const auto& port : ports)
            {
                if (port.compilerPopulated)
                    continue;
                const auto anchor = portAnchor(layout, port.id, output);
                if (!anchor.has_value()
                    || anchor->getDistanceFrom(pointAsFloat) > hitRadius)
                    continue;
                return PortHit{layout.node.instanceId, port.id, port, *anchor};
            }
        }
        return std::nullopt;
    }

    void drawConnections(juce::Graphics& graphics) const
    {
        for (const auto index : visibleConnectionIndices_)
        {
            const auto& connection = connectionCache_[index];

            const auto path = linePath(connection.start, connection.end);
            const auto hasSelection = !selectedNodeIds_.empty();
            const auto incident = selectedNodeIds_.contains(
                connection.fromNode)
                || selectedNodeIds_.contains(connection.toNode);
            const auto emphasis = !hasSelection
                ? talas::app::DescriptorConnectionEmphasis::Normal
                : incident
                ? talas::app::DescriptorConnectionEmphasis::SelectedIncident
                : talas::app::DescriptorConnectionEmphasis::ContextDimmed;
            auto presentation = talas::app::descriptorWirePresentation(
                connection.sourcePort,
                emphasis);
            // A signal used more than once: a thick line with the number.
            if (connection.useCount > 1)
                presentation.width *= 2.5F;
            // Two lines for a class's two
            // independent channels, one for mono.
            const auto lines = talas::app::soundClassConnectionLines(
                classIconOfNode(connection.fromNode), classIconOfNode(connection.toNode));
            if (lines == std::optional<unsigned>{2U})
            {
                const auto apart = std::max(2.0F, presentation.width);
                for (const auto offset : {-apart, apart})
                {
                    auto shifted = path;
                    shifted.applyTransform(juce::AffineTransform::translation(0.0F, offset));
                    drawSemanticPath(graphics, shifted, connection.start.translated(0.0F, offset),
                        connection.end.translated(0.0F, offset), presentation, decorated_);
                }
            }
            else
                drawSemanticPath(
                    graphics,
                    path,
                    connection.start,
                    connection.end,
                    presentation,
                    decorated_);
            if (connection.labelled)
                drawUseCount(graphics, connection, presentation.opacity);
        }
    }

    void drawUseCount(
        juce::Graphics& graphics,
        const ConnectionLayout& connection,
        float opacity) const
    {
        const auto text = juce::String{static_cast<juce::int64>(connection.useCount)};
        const auto width = 8.0F + 7.0F * static_cast<float>(text.length());
        const auto box = juce::Rectangle<float>{width, 15.0F}.withCentre(
            connectionPoint(connection, 0.5F));
        graphics.setColour(workbenchTheme.well.withAlpha(opacity));
        graphics.fillRoundedRectangle(box, 4.0F);
        graphics.setColour(juce::Colours::white.withAlpha(opacity));
        graphics.setFont(WorkbenchType::labelStrong());
        graphics.drawText(text, box, juce::Justification::centred, false);
    }

    void drawPendingConnection(juce::Graphics& graphics) const
    {
        if (!pendingConnection_.has_value())
            return;

        const auto path = linePath(pendingConnection_->anchor, pendingConnectionPointer_);
        drawSemanticPath(
            graphics,
            path,
            pendingConnection_->anchor,
            pendingConnectionPointer_,
            talas::app::descriptorWirePresentation(
                pendingConnection_->descriptor,
                talas::app::DescriptorConnectionEmphasis::SelectedIncident));
    }

    // The Sound of the edited Sound that is its
    // replaceable input, shown with a cyan halo around its name.
    [[nodiscard]] std::string replaceableInputNodeId() const
    {
        if (!soundDefinitionContext_.has_value())
            return {};
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        const auto definition = std::ranges::find(definitions, *soundDefinitionContext_,
            &talas::project::ProjectSoundDefinition::definitionId);
        if (definition == definitions.end())
            return {};
        const auto* replaceable = talas::project::replaceableSoundInput(*definition);
        return replaceable != nullptr && replaceable->replacesSound ? replaceable->nodeId : std::string{};
    }

    [[nodiscard]] std::string replaceableInputNodeIdForSmoke() const override
    {
        return replaceableInputNodeId();
    }

    [[nodiscard]] const talas::project::ProjectSoundIcon* classIconOfNode(const std::string& nodeId) const
    {
        const auto layout = std::ranges::find(layoutCache_, nodeId, [](const NodeLayout& candidate) -> const std::string& {
            return candidate.node.instanceId;
        });
        return layout == layoutCache_.end() ? nullptr : classIconOf(*layout);
    }

    [[nodiscard]] const talas::project::ProjectSoundIcon* classIconOf(const NodeLayout& layout) const
    {
        if (!layout.node.soundDefinitionId.has_value())
            return nullptr;
        const auto& definitions = bridge_.editor().document().soundDefinitions;
        const auto definition = std::ranges::find(definitions, *layout.node.soundDefinitionId,
            &talas::project::ProjectSoundDefinition::definitionId);
        return definition == definitions.end() || !definition->icon.has_value() ? nullptr : &*definition->icon;
    }

    // A Sound class's own icon, in the row where
    // its inner icon goes: its PNG picture, its drawn dots (the large icon,
    // the one shown by default, where it has one), or another Sound's inner
    // icon; else its kind's.
    void drawClassIconOr(juce::Graphics& graphics, juce::Rectangle<float> row, const NodeLayout& layout) const
    {
        const auto& document = bridge_.editor().document();
        const auto definition = !layout.node.soundDefinitionId.has_value()
            ? document.soundDefinitions.end()
            : std::ranges::find(document.soundDefinitions, *layout.node.soundDefinitionId,
                  &talas::project::ProjectSoundDefinition::definitionId);
        if (definition != document.soundDefinitions.end() && definition->icon.has_value())
        {
            const auto& icon = *definition->icon;
            const auto picture = row.withTrimmedLeft(row.getWidth() - 2.0F * row.getHeight());
            if (icon.png && classIconImages_)
            {
                if (const auto image = classIconImages_(definition->definitionId); image.isValid())
                {
                    // A PNG of any size, fitted to its place.
                    const auto placed = talas::app::soundIconPngPlacement(image.getWidth(), image.getHeight(),
                        {picture.getX(), picture.getY(), picture.getWidth(), picture.getHeight()});
                    graphics.drawImage(image,
                        juce::Rectangle<double>{placed.x, placed.y, placed.width, placed.height}.toFloat(),
                        juce::RectanglePlacement::stretchToFit);
                    return;
                }
            }
            if (drawSoundIconPicture(graphics, picture, icon, !icon.large.empty(), secondaryTextColour))
                return;
            if (!icon.sameAs.empty())
            {
                talas::project::ProjectNode other;
                other.instanceId = "classIcon";
                if (registry_.findNodeType(icon.sameAs) != nullptr)
                    other.typeId = icon.sameAs;
                else
                    other.soundDefinitionId = icon.sameAs;
                if (const auto described = talas::project::describeProjectNode(document, other, registry_))
                {
                    drawSoundIconGlyphs(graphics, row, talas::app::soundIconGlyphs(*described));
                    return;
                }
            }
        }
        drawSoundIconGlyphs(graphics, row, talas::app::soundIconGlyphs(layout.descriptor));
    }

    void setClassIconImages(std::function<juce::Image(const std::string&)> images) override
    {
        classIconImages_ = std::move(images);
        repaint();
    }

    [[nodiscard]] bool parametersOpenOn(std::string_view nodeId) const
    {
        return !selectedNodeId_.empty() && selectedNodeId_ == nodeId && selectedNodeIds_.contains(selectedNodeId_);
    }

    void drawNode(juce::Graphics& graphics, const NodeLayout& layout, std::string_view replaceableNodeId) const
    {
        const auto selected = selectedNodeIds_.contains(
            layout.node.instanceId);
        const auto hasDiagnostic = std::ranges::find(
            diagnosticNodeIds_, layout.node.instanceId) != diagnosticNodeIds_.end();
        const auto bounds = layout.bounds;
        // The Sound's category colours its card; its ports keep their kinds'.
        const auto accent = WorkbenchStyle::categoryColour(layout.descriptor.category);
        const auto preservedUnavailable = layout.preserved.has_value()
            && !layout.preserved->parameterEditingEnabled;
        const auto cardAccent = layout.preserved.has_value()
            ? (preservedUnavailable
                   ? workbenchTheme.placeholderSupport
                   : workbenchTheme.fullSupport)
            : accent;

        // A class's chosen types shape it.
        const auto* classIcon = classIconOf(layout);
        const auto shape = classIcon == nullptr ? talas::app::soundIconShape(layout.descriptor)
                                                : talas::app::soundClassIconShape(talas::app::soundIconShape(layout.descriptor), *classIcon);
        const auto outline = soundIconOutline(bounds.toFloat(), shape);
        const auto card = bounds.toFloat();
        if (decorated_)
        {
            graphics.setColour(juce::Colours::black.withAlpha(0.38F));
            graphics.fillPath(outline, juce::AffineTransform::translation(0.0F, 4.0F));
        }
        // One gradient: the header lit with the category's colour, the body
        // falling to a darker glass.
        if (decorated_)
        {
            juce::ColourGradient body{panelColour.interpolatedWith(cardAccent, 0.24F).brighter(0.06F), card.getX(),
                card.getY(), panelColour.interpolatedWith(juce::Colours::black, 0.18F), card.getX(), card.getBottom(),
                false};
            body.addColour(std::clamp((static_cast<double>(layout.headerHeight) + 6.0)
                                   / std::max(1.0, static_cast<double>(card.getHeight())),
                               0.05, 0.95),
                panelColour.brighter(0.04F));
            graphics.setGradientFill(body);
        }
        else
        {
            graphics.setColour(panelColour.interpolatedWith(cardAccent, 0.08F));
        }
        graphics.fillPath(outline);
        // The Sound whose parameters are open
        // below has its icon patterned gray.
        if (parametersOpenOn(layout.node.instanceId))
        {
            juce::Graphics::ScopedSaveState clipped(graphics);
            graphics.reduceClipRegion(outline);
            graphics.setColour(juce::Colours::grey.withAlpha(0.17F));
            for (auto x = bounds.getX() - bounds.getHeight(); x < bounds.getRight(); x += 6)
                graphics.drawLine(static_cast<float>(x), static_cast<float>(bounds.getBottom()),
                    static_cast<float>(x + bounds.getHeight()), static_cast<float>(bounds.getY()), 1.5F);
        }
        if ((selected || hasDiagnostic) && decorated_ && !WorkbenchStyle::enhancedContrast())
        {
            graphics.setColour((hasDiagnostic ? errorColour : selectionColour).withAlpha(0.22F));
            graphics.strokePath(outline, juce::PathStrokeType{6.0F});
        }
        graphics.setColour(
            hasDiagnostic
                ? errorColour
                : (selected
                       ? selectionColour
                       : (WorkbenchStyle::enhancedContrast() ? workbenchTheme.interactiveOutline
                                                             : juce::Colours::white.withAlpha(0.13F))));
        graphics.strokePath(outline, juce::PathStrokeType{hasDiagnostic || selected ? 2.0F : 1.0F});
        if (hasDiagnostic && selected)
        {
            graphics.setColour(selectionColour);
            graphics.drawRoundedRectangle(bounds.reduced(10, 9).toFloat(), 4.0F, 1.0F);
        }
        {
            juce::Graphics::ScopedSaveState clipped(graphics);
            graphics.reduceClipRegion(outline);
            const auto strip = card.withHeight(shape.pointedTop ? 10.0F : 3.0F);
            if (decorated_)
                graphics.setGradientFill(juce::ColourGradient{cardAccent.brighter(0.25F), strip.getX(), strip.getY(),
                    cardAccent.withRotatedHue(0.06F), strip.getRight(), strip.getY(), false});
            else
                graphics.setColour(cardAccent);
            graphics.fillRect(strip);
        }
        if (layout.hasInputs)
        {
            const auto marker = branchMarkerBounds(layout);
            if (layout.branchHidden)
            {
                graphics.setColour(panelColour);
                graphics.fillRoundedRectangle(marker, 4.0F);
                graphics.setColour(textColour);
                graphics.drawRoundedRectangle(marker, 4.0F, 1.0F);
                graphics.setFont(WorkbenchType::microStrong());
                graphics.drawText(juce::String::fromUTF8("…"), marker,
                    juce::Justification::centred);
            }
            else
            {
                graphics.setColour(textColour.withAlpha(0.8F));
                graphics.fillEllipse(marker);
            }
        }

        auto content = bounds.reduced(10, 7);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::panelTitle());
        graphics.drawText(juceString(layout.descriptor.displayName), content.removeFromTop(18), juce::Justification::centredLeft);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        const auto nameBounds = content.removeFromTop(13);
        if (!replaceableNodeId.empty() && layout.node.instanceId == replaceableNodeId)
        {
            const auto nameWidth = juce::GlyphArrangement::getStringWidthInt(WorkbenchType::micro(), juceString(layout.node.instanceId));
            graphics.setColour(juce::Colours::cyan.withAlpha(0.35F));
            graphics.fillRoundedRectangle(nameBounds.withWidth(std::min(nameBounds.getWidth(), nameWidth + 6)).translated(-3, 0).toFloat(), 4.0F);
            graphics.setColour(juce::Colours::cyan);
            graphics.drawRoundedRectangle(nameBounds.withWidth(std::min(nameBounds.getWidth(), nameWidth + 6)).translated(-3, 0).toFloat(), 4.0F, 1.2F);
            graphics.setColour(textColour);
        }
        graphics.drawText(juceString(layout.node.instanceId), nameBounds, juce::Justification::centredLeft);
        if (!layout.preserved.has_value())
        {
            const auto row = juce::Rectangle<float>{static_cast<float>(bounds.getX()), static_cast<float>(bounds.getY() + 21),
                static_cast<float>(bounds.getWidth() - 12), 13.0F};
            drawClassIconOr(graphics, row, layout);
        }
        if (layout.preserved.has_value())
        {
            auto statusBounds = content.removeFromTop(15);
            const auto iconBounds = statusBounds.removeFromLeft(13)
                .reduced(1);
            graphics.setColour(cardAccent);
            graphics.fillEllipse(iconBounds.toFloat());
            graphics.setColour(backgroundColour);
            graphics.setFont(WorkbenchType::microStrong());
            graphics.drawText(
                preservedUnavailable ? "!" : juce::String::fromUTF8("✓"),
                iconBounds,
                juce::Justification::centred);
            graphics.setColour(
                preservedUnavailable ? errorColour : secondaryTextColour);
            graphics.setFont(WorkbenchType::microStrong());
            graphics.drawFittedText(
                juceString(layout.preserved->statusLabel)
                    .toUpperCase(),
                statusBounds.withTrimmedLeft(3),
                juce::Justification::centredLeft,
                1,
                0.8F);
        }
        if (hasDiagnostic)
        {
            const auto badge = juce::Rectangle<float>{
                static_cast<float>(bounds.getRight() - 22),
                static_cast<float>(bounds.getY() + 8),
                14.0F,
                14.0F,
            };
            graphics.setColour(errorColour);
            graphics.fillEllipse(badge);
            graphics.setColour(backgroundColour);
            graphics.setFont(WorkbenchType::microStrong());
            graphics.drawText("!", badge, juce::Justification::centred);
        }

        const auto rowCount = graphViewProjection_.showPorts
            ? std::max(
                  presentedPortCount(layout.descriptor.inputs),
                  presentedPortCount(layout.descriptor.outputs))
            : std::size_t{};
        for (std::size_t index = 0; index < rowCount; ++index)
        {
            const auto y = bounds.getY() + layout.headerHeight + static_cast<int>(index) * portRowHeight + portRowHeight / 2;
            if (const auto* input = presentedPortAt(
                    layout.descriptor.inputs, index))
            {
                drawPort(
                    graphics, *input, {bounds.getX(), y}, false, bounds, decorated_);
            }
            if (const auto* output = presentedPortAt(
                    layout.descriptor.outputs, index))
            {
                drawPort(
                    graphics, *output, {bounds.getRight(), y}, true, bounds, decorated_);
            }
        }
    }

    static void drawPort(juce::Graphics& graphics,
                         const talas::core::PortDescriptor& port,
                         juce::Point<int> anchor,
                         bool output,
                         const juce::Rectangle<int>& nodeBounds,
                         bool ringed)
    {
        const auto presentation =
            talas::app::descriptorPortPresentation(port);
        const auto colour = colourForWireRole(presentation.colour);
        if (ringed)
        {
            // A dark ring parts the port from its card and its line.
            graphics.setColour(workbenchTheme.well);
            drawPortShape(
                graphics,
                anchor.toFloat(),
                presentation.shape,
                11.0F,
                true);
        }
        graphics.setColour(colour);
        drawPortShape(
            graphics,
            anchor.toFloat(),
            presentation.shape,
            8.0F,
            true);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        const auto labelBounds = output
            ? juce::Rectangle<int>{nodeBounds.getX() + 38, anchor.y - 9, nodeBounds.getWidth() - 48, 18}
            : juce::Rectangle<int>{nodeBounds.getX() + 10, anchor.y - 9, nodeBounds.getWidth() - 48, 18};
        graphics.drawText(
            juceString(presentation.code) + juceString(" · ")
                + juceString(port.id),
            labelBounds,
            output
                ? juce::Justification::centredRight
                : juce::Justification::centredLeft);
    }

    // The signal flow of the Sound being edited.
    [[nodiscard]] const talas::project::ProjectGraph* displayedGraph(
        const talas::project::ProjectDocument& document) const
    {
        return talas::app::soundEditorSignalFlow(document, soundDefinitionContext_);
    }

    talas::runtime::ProjectRuntimeBridge& bridge_;
    const talas::core::NodeRegistry& registry_;
    SelectionCallback onSelection_;
    DocumentChangeCallback onDocumentChange_;
    StatusCallback onStatus_;
    PrototypeDropCallback onPrototypeDrop_;
    OpenChildCallback onOpenChild_;
    ViewChangeCallback onViewChange_;
    KeyboardCommandCallback onKeyboardCommand_;
    std::vector<GraphKeyboardCommandRequest> pendingKeyboardCommands_;
    std::function<juce::Image(const std::string&)> classIconImages_;
    std::vector<NodeLayout> layoutCache_;
    std::vector<ConnectionLayout> connectionCache_;
    std::unordered_map<
        std::string,
        std::size_t,
        TransparentStringHash,
        TransparentStringEqual>
        layoutIndexByNodeId_;
    std::unordered_map<
        std::string,
        std::size_t,
        TransparentStringHash,
        TransparentStringEqual>
        definitionIndexById_;
    std::unordered_map<
        std::string,
        std::vector<std::size_t>,
        TransparentStringHash,
        TransparentStringEqual>
        connectionIndicesByNodeId_;
    std::unordered_set<std::string> connectedInputKeys_;
    std::unordered_set<std::string> connectedOutputKeys_;
    talas::app::WorkbenchGraphViewportIndex nodeViewportIndex_;
    talas::app::WorkbenchGraphViewportIndex connectionViewportIndex_;
    std::vector<talas::app::WorkbenchGraphViewportRect> nodeViewportRects_;
    std::vector<talas::app::WorkbenchGraphViewportRect>
        connectionViewportRects_;
    std::vector<std::size_t> visibleLayoutIndices_;
    std::vector<std::size_t> visibleConnectionIndices_;
    std::vector<std::size_t> nodeHitIndices_;
    AccessibilityContainer accessibilityContainer_;
    std::optional<talas::app::WorkbenchGraphScroll> horizontalScroll_;
    std::function<void()> onHorizontalScrollChange_;
    juce::OwnedArray<AccessibilityItem> accessibilityItems_;
    std::unordered_map<const juce::Component*, std::size_t>
        accessibilityOrderByItem_;
    std::unordered_map<
        std::string,
        std::vector<AccessibilityItem*>,
        TransparentStringHash,
        TransparentStringEqual>
        accessibilityItemsByNodeId_;
    AccessibilityItem* focusedAccessibilityItem_ = nullptr;
    std::string selectedNodeId_;
    std::unordered_set<std::string> selectedNodeIds_;
    // Hidden branches per graph scope, for this session's view only.
    std::map<std::string, talas::app::GraphNodeSet, std::less<>> collapsedBranches_;
    std::vector<std::string> diagnosticNodeIds_;
    std::string draggingNodeId_;
    std::vector<std::pair<std::string, juce::Point<int>>> branchDragStarts_;
    juce::Point<int> branchDragOrigin_;
    bool draggedNodeMoved_ = false;
    juce::Point<int> dragOffset_;
    std::optional<PortHit> pendingConnection_;
    juce::Point<float> pendingConnectionPointer_;
    std::optional<juce::Point<int>> prototypeDropPosition_;
    GraphDropTarget prototypeDropTarget_;
    std::optional<std::string> soundDefinitionContext_;
    juce::String contextPresentation_{"ROOT"};
    float viewScale_ = 1.0F;
    juce::Point<float> viewOffset_;
    juce::Point<int> panStart_;
    juce::Point<float> panStartOffset_;
    bool compactGraphProtection_ = false;
    talas::app::GraphLineContour lineContour_ = talas::app::GraphLineContour::Curved;
    bool activeContext_ = true;
    bool autoFitOnResize_ = false;
    bool panning_ = false;
    bool keyboardRefreshPending_ = false;
    bool keyboardCommandDispatchScheduled_ = false;
    bool keyboardNudgeActive_ = false;
    bool keyboardNudgeChanged_ = false;
    talas::project::ProjectEditor::HistoryTransactionToken
        dragTransactionToken_ = 0;
    std::vector<std::string> keyboardNudgeNodeIds_;
    std::optional<std::string> keyboardNudgeDefinitionId_;
    std::string keyboardNudgeDocumentSnapshot_;
    talas::project::CanvasPosition keyboardNudgeDelta_{};
    talas::project::WorkspaceGraphViewMode graphViewMode_ =
        talas::project::WorkspaceGraphViewMode::PortsAndWires;
    talas::app::WorkbenchGraphViewProjection graphViewProjection_{};
    // Card shadows and wire glows, painted while the view is uncrowded.
    mutable bool decorated_ = true;
    std::uint64_t structuralGraphRebuildCount_ = 0;
    std::uint64_t accessibilityGraphRebuildCount_ = 0;
};

}

std::unique_ptr<GraphCanvas> makeGraphCanvas(
    talas::runtime::ProjectRuntimeBridge& bridge,
    const talas::core::NodeRegistry& registry,
    GraphCanvas::SelectionCallback onSelection,
    GraphCanvas::DocumentChangeCallback onDocumentChange,
    GraphCanvas::StatusCallback onStatus,
    GraphCanvas::PrototypeDropCallback onPrototypeDrop,
    GraphCanvas::OpenChildCallback onOpenChild,
    GraphCanvas::ViewChangeCallback onViewChange,
    GraphCanvas::KeyboardCommandCallback onKeyboardCommand)
{
    return std::make_unique<GraphCanvasImpl>(
        bridge,
        registry,
        std::move(onSelection),
        std::move(onDocumentChange),
        std::move(onStatus),
        std::move(onPrototypeDrop),
        std::move(onOpenChild),
        std::move(onViewChange),
        std::move(onKeyboardCommand));
}
}
