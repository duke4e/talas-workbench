#include "app/workbench/WorkbenchComponentInternal.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <tuple>
#include <unordered_set>

namespace talas::app::workbench
{
namespace
{
struct FocusBinding
{
    WorkbenchFocusCandidate candidate;
    juce::Component* component = nullptr;
};

constexpr std::size_t maximumFocusTreeComponents = 65536;

class WorkbenchKeyboardFocusTraverser final
    : public juce::ComponentTraverser
{
public:
    using OrderProvider =
        std::function<std::vector<juce::Component*>()>;

    explicit WorkbenchKeyboardFocusTraverser(OrderProvider orderProvider)
        : orderProvider_(std::move(orderProvider))
    {
    }

    juce::Component* getDefaultComponent(
        juce::Component* parentComponent) override
    {
        const auto order = getAllComponents(parentComponent);
        return order.empty() ? nullptr : order.front();
    }

    juce::Component* getNextComponent(
        juce::Component* current) override
    {
        return adjacent(current, true);
    }

    juce::Component* getPreviousComponent(
        juce::Component* current) override
    {
        return adjacent(current, false);
    }

    std::vector<juce::Component*> getAllComponents(
        juce::Component* parentComponent) override
    {
        auto order = orderProvider_();
        if (parentComponent == nullptr)
            return {};
        std::erase_if(order, [&](const auto* component) {
            return component == nullptr
                || (component != parentComponent
                    && !parentComponent->isParentOf(component));
        });
        return order;
    }

private:
    juce::Component* adjacent(
        juce::Component* current,
        bool forwards)
    {
        if (current == nullptr)
            return nullptr;
        const auto order = orderProvider_();
        auto* anchor = current;
        auto found = order.end();
        while (anchor != nullptr && found == order.end())
        {
            found = std::ranges::find(order, anchor);
            anchor = anchor->getParentComponent();
        }
        if (found == order.end())
            return nullptr;
        if (forwards)
        {
            ++found;
            return found == order.end() ? nullptr : *found;
        }
        return found == order.begin() ? nullptr : *std::prev(found);
    }

    OrderProvider orderProvider_;
};

[[nodiscard]] bool containsFocus(
    const juce::Component& root,
    const juce::Component* focused) noexcept
{
    return focused != nullptr
        && (&root == focused || root.isParentOf(focused));
}

[[nodiscard]] bool isUsableFocusRoot(
    const juce::Component& component) noexcept
{
    return component.isVisible() && component.isEnabled()
        && !component.getBounds().isEmpty();
}

[[nodiscard]] std::string focusItemIdentity(
    const juce::Component& root,
    const juce::Component& focused,
    const char* fallback)
{
    for (auto* current = &focused; current != nullptr;
         current = current->getParentComponent())
    {
        const auto identity = current->getComponentID();
        if (identity.isNotEmpty())
            return identity.toStdString();
        if (current == &root)
            break;
    }
    return fallback;
}

struct UniqueIdentityLookup
{
    juce::Component* component = nullptr;
    bool ambiguous = false;
    bool exhausted = false;
};

void findUniqueDescendantByIdentity(
    juce::Component& root,
    const juce::String& identity,
    std::size_t& remaining,
    UniqueIdentityLookup& result)
{
    std::vector<juce::Component*> pending{&root};
    while (!pending.empty())
    {
        if (remaining == 0)
        {
            result.exhausted = true;
            return;
        }
        auto* current = pending.back();
        pending.pop_back();
        --remaining;
        if (current->getComponentID() == identity)
        {
            if (result.component != nullptr && result.component != current)
            {
                result.ambiguous = true;
                return;
            }
            result.component = current;
        }
        const auto children = current->getChildren();
        for (auto index = children.size(); index > 0; --index)
            pending.push_back(children.getUnchecked(index - 1));
    }
}

[[nodiscard]] int focusOrder(const juce::Component& component) noexcept
{
    const auto explicitOrder = component.getExplicitFocusOrder();
    return explicitOrder > 0
        ? explicitOrder
        : std::numeric_limits<int>::max();
}

void appendFocusableTree(
    juce::Component& root,
    std::vector<juce::Component*>& order,
    std::unordered_set<juce::Component*>& visited)
{
    std::vector<juce::Component*> pending{&root};
    while (!pending.empty())
    {
        auto* current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second)
            continue;
        if (!current->isVisible() || !current->isEnabled()
            || current->getBounds().isEmpty())
            continue;
        if (current->getWantsKeyboardFocus())
            order.push_back(current);
        auto children = current->getChildren();
        std::ranges::stable_sort(
            children, [](const auto* left, const auto* right) {
                return std::tuple{
                           focusOrder(*left),
                           left->isAlwaysOnTop() ? 0 : 1,
                           left->getY(),
                           left->getX(),
                       }
                    < std::tuple{
                           focusOrder(*right),
                           right->isAlwaysOnTop() ? 0 : 1,
                           right->getY(),
                           right->getX(),
                       };
            });
        for (auto index = children.size(); index > 0; --index)
            pending.push_back(children.getUnchecked(index - 1));
    }
}

[[nodiscard]] bool focusComponentOrFirstChild(juce::Component& component)
{
    if (!isUsableFocusRoot(component)
        || (!component.isShowing() && !component.isOnDesktop()))
        return false;
    if (component.getWantsKeyboardFocus())
    {
        component.grabKeyboardFocus();
        return component.hasKeyboardFocus(true);
    }
    const auto traverser = component.createKeyboardFocusTraverser();
    if (traverser == nullptr)
        return false;
    auto* target = traverser->getDefaultComponent(&component);
    if (target == nullptr || !target->isVisible() || !target->isEnabled())
        return false;
    target->grabKeyboardFocus();
    return target->hasKeyboardFocus(true);
}
}

void WorkbenchComponent::configureWorkbenchFocusTopology()
{
    setFocusContainerType(
        juce::Component::FocusContainerType::keyboardFocusContainer);
    const auto configure = [](juce::Component& component,
                               const char* identity,
                               int order) {
        component.setComponentID(identity);
        component.setExplicitFocusOrder(order);
    };

    configure(playButton_, "toolbar.play", 1);
    configure(pauseButton_, "toolbar.pause", 2);
    configure(stopButton_, "toolbar.stop", 3);
    configure(undoButton_, "toolbar.undo", 4);
    configure(redoButton_, "toolbar.redo", 5);
    configure(deleteButton_, "toolbar.delete", 6);
    configure(saveButton_, "toolbar.save", 7);
    configure(moreButton_, "toolbar.more", 8);
    configure(workspacePatchButton_, "toolbar.workspace.patch", 9);
    configure(workspaceAutomationButton_, "toolbar.workspace.automation", 10);
    configure(workspaceAnalysisButton_, "toolbar.workspace.analysis", 11);
    configure(workspacePerformanceButton_, "toolbar.workspace.performance", 12);
    configure(workspaceSamplesButton_, "toolbar.workspace.samples", 13);

    configure(*prototypePanel_, "library.prototypes", 100);
    configure(*soundDefinitionsPanel_, "library.sounds", 110);
    configure(*graphCanvas_, "graph.primary", 200);
    configure(*secondaryGraphCanvas_, "graph.secondary", 210);
    configure(*workspaceSplitHandle_, "graph.split", 220);
    configure(*performanceCanvas_, "graph.performance", 230);
    configure(*sampleMapPanel_, "graph.samples.map", 240);
    configure(*partialTrackEditorPanel_, "graph.partial-tracks", 245);
    configure(*waveformPanel_, "graph.samples.waveform", 250);

    configure(*assetRecoveryPanel_, "workspace.asset-recovery", 300);
    configure(*inspectorPanel_, "workspace.inspector", 310);
    configure(*timelinePanel_, "workspace.timeline", 320);
    configure(*analysisPanel_, "workspace.analysis", 330);
    configure(*performancePanel_, "workspace.performance", 340);
    configure(*sampleRegionInspector_, "workspace.sample-region", 350);

    configure(*virtualControlSurface_, "lower.virtual-controls", 400);
    configure(*diagnosticsPanel_, "lower.diagnostics", 410);
    configure(projectStateLabel_, "status.project-state", 500);
    configure(statusLabel_, "status.message", 510);
    projectStateLabel_.setWantsKeyboardFocus(true);
    statusLabel_.setWantsKeyboardFocus(true);
}

std::unique_ptr<juce::ComponentTraverser>
WorkbenchComponent::createKeyboardFocusTraverser()
{
    return std::make_unique<WorkbenchKeyboardFocusTraverser>(
        [this] { return visibleWorkbenchKeyboardFocusOrder(); });
}

std::vector<juce::Component*>
WorkbenchComponent::visibleWorkbenchKeyboardFocusOrder()
{
    std::vector<juce::Component*> order;
    order.reserve(128);
    std::unordered_set<juce::Component*> visited;
    visited.reserve(128);
    const auto append = [&](juce::Component* component) {
        if (component != nullptr)
            appendFocusableTree(*component, order, visited);
    };

    for (auto* component : std::array<juce::Component*, 13>{
             &playButton_, &pauseButton_, &stopButton_, &undoButton_,
             &redoButton_, &deleteButton_, &saveButton_, &moreButton_,
             &workspacePatchButton_, &workspaceAutomationButton_,
             &workspaceAnalysisButton_, &workspacePerformanceButton_,
             &workspaceSamplesButton_,
         })
    {
        append(component);
    }

    for (auto* component : std::array<juce::Component*, 17>{
             prototypePanel_.get(),
             soundDefinitionsPanel_.get(),
             graphCanvas_.get(),
             performanceCanvas_.get(),
             sampleMapPanel_.get(),
             partialTrackEditorPanel_.get(),
             secondaryGraphCanvas_.get(),
             workspaceSplitHandle_.get(),
             waveformPanel_.get(),
             assetRecoveryPanel_.get(),
             inspectorPanel_.get(),
             timelinePanel_.get(),
             analysisPanel_.get(),
             performancePanel_.get(),
             sampleRegionInspector_.get(),
             virtualControlSurface_.get(),
             diagnosticsPanel_.get(),
         })
    {
        append(component);
    }
    append(&projectStateLabel_);
    append(&statusLabel_);
    return order;
}

std::optional<WorkbenchFocusIdentity>
WorkbenchComponent::currentWorkbenchFocusIdentity() const
{
    return workbenchFocusIdentityForComponent(
        juce::Component::getCurrentlyFocusedComponent());
}

std::optional<WorkbenchFocusIdentity>
WorkbenchComponent::workbenchFocusIdentityForComponent(
    const juce::Component* focused) const
{
    const auto locallyShowing = [&](const juce::Component* component) {
        for (auto* current = component; current != nullptr;
             current = current->getParentComponent())
        {
            if (current == this)
                return true;
            if (!current->isVisible() || !current->isEnabled())
                return false;
        }
        return false;
    };
    if (focused == nullptr || !containsFocus(*this, focused)
        || !locallyShowing(focused) || !focused->isEnabled())
        return std::nullopt;

    const auto identify = [&](const juce::Component& component,
                              WorkbenchFocusRegion region,
                              const char* itemId)
        -> std::optional<WorkbenchFocusIdentity> {
        if (!containsFocus(component, focused)
            || !isUsableFocusRoot(component))
            return std::nullopt;
        return WorkbenchFocusIdentity{
            region,
            focusItemIdentity(component, *focused, itemId),
        };
    };

    const std::array<std::pair<const juce::Component*, const char*>, 13> toolbar{{
        {&playButton_, "toolbar.play"},
        {&pauseButton_, "toolbar.pause"},
        {&stopButton_, "toolbar.stop"},
        {&undoButton_, "toolbar.undo"},
        {&redoButton_, "toolbar.redo"},
        {&deleteButton_, "toolbar.delete"},
        {&saveButton_, "toolbar.save"},
        {&moreButton_, "toolbar.more"},
        {&workspacePatchButton_, "toolbar.workspace.patch"},
        {&workspaceAutomationButton_, "toolbar.workspace.automation"},
        {&workspaceAnalysisButton_, "toolbar.workspace.analysis"},
        {&workspacePerformanceButton_, "toolbar.workspace.performance"},
        {&workspaceSamplesButton_, "toolbar.workspace.samples"},
    }};
    for (const auto& [component, itemId] : toolbar)
    {
        if (const auto identity = identify(
                *component, WorkbenchFocusRegion::Toolbar, itemId))
        {
            return identity;
        }
    }

    const std::array<
        std::tuple<const juce::Component*, WorkbenchFocusRegion, const char*>,
        17> bindings{{
        {prototypePanel_.get(), WorkbenchFocusRegion::Library,
            "library.prototypes"},
        {soundDefinitionsPanel_.get(), WorkbenchFocusRegion::Library,
            "library.sounds"},
        {graphCanvas_.get(), WorkbenchFocusRegion::PrimaryGraph,
            "graph.primary"},
        {performanceCanvas_.get(), WorkbenchFocusRegion::PrimaryGraph,
            "graph.performance"},
        {sampleMapPanel_.get(), WorkbenchFocusRegion::PrimaryGraph,
            "graph.samples.map"},
        {partialTrackEditorPanel_.get(), WorkbenchFocusRegion::PrimaryGraph,
            "graph.partial-tracks"},
        {waveformPanel_.get(), WorkbenchFocusRegion::SecondaryGraph,
            "graph.samples.waveform"},
        {secondaryGraphCanvas_.get(), WorkbenchFocusRegion::SecondaryGraph,
            "graph.secondary"},
        {workspaceSplitHandle_.get(), WorkbenchFocusRegion::SecondaryGraph,
            "graph.split"},
        {assetRecoveryPanel_.get(), WorkbenchFocusRegion::RightWorkspace,
            "workspace.asset-recovery"},
        {inspectorPanel_.get(), WorkbenchFocusRegion::RightWorkspace,
            "workspace.inspector"},
        {timelinePanel_.get(), WorkbenchFocusRegion::RightWorkspace,
            "workspace.timeline"},
        {analysisPanel_.get(), WorkbenchFocusRegion::RightWorkspace,
            "workspace.analysis"},
        {performancePanel_.get(), WorkbenchFocusRegion::RightWorkspace,
            "workspace.performance"},
        {sampleRegionInspector_.get(), WorkbenchFocusRegion::RightWorkspace,
            "workspace.sample-region"},
        {virtualControlSurface_.get(), WorkbenchFocusRegion::LowerWorkspace,
            "lower.virtual-controls"},
        {diagnosticsPanel_.get(), WorkbenchFocusRegion::LowerWorkspace,
            "lower.diagnostics"},
    }};
    for (const auto& [component, region, itemId] : bindings)
    {
        if (const auto identity = identify(*component, region, itemId))
            return identity;
    }
    if (const auto identity = identify(
            projectStateLabel_, WorkbenchFocusRegion::StatusAndHelp,
            "status.project-state"))
    {
        return identity;
    }
    if (const auto identity = identify(
            statusLabel_, WorkbenchFocusRegion::StatusAndHelp,
            "status.message"))
    {
        return identity;
    }
    return std::nullopt;
}

void WorkbenchComponent::restoreWorkbenchFocusAfterLayout()
{
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    if ((focused != nullptr && !containsFocus(*this, focused))
        || fileChooserActive_ || destructivePromptActive_
        || recoveryDialog_ != nullptr || soundDialog_ != nullptr
        || exportDialog_ != nullptr)
    {
        return;
    }
    const auto current = currentWorkbenchFocusIdentity();
    if (current.has_value())
    {
        focusCoordinator_.capture(current);
        lastFocusRestorationTarget_ = current;
        return;
    }
    if (!focusCoordinator_.captured().has_value())
    {
        lastFocusRestorationTarget_.reset();
        return;
    }

    const auto binding = [](WorkbenchFocusRegion region,
                            const char* itemId,
                            std::uint32_t order,
                            juce::Component* component) {
        return FocusBinding{
            {{region, itemId}, order,
                component != nullptr && isUsableFocusRoot(*component),
                component != nullptr && component->isEnabled()},
            component,
        };
    };
    std::vector bindings{
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.play", 1, &playButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.pause", 2, &pauseButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.stop", 3, &stopButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.undo", 4, &undoButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.redo", 5, &redoButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.delete", 6, &deleteButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.save", 7, &saveButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.more", 8, &moreButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.workspace.patch", 9, &workspacePatchButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.workspace.automation", 10, &workspaceAutomationButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.workspace.analysis", 11, &workspaceAnalysisButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.workspace.performance", 12, &workspacePerformanceButton_),
        binding(WorkbenchFocusRegion::Toolbar, "toolbar.workspace.samples", 13, &workspaceSamplesButton_),
        binding(WorkbenchFocusRegion::Library, "library.prototypes", 1, prototypePanel_.get()),
        binding(WorkbenchFocusRegion::Library, "library.sounds", 2, soundDefinitionsPanel_.get()),
        binding(WorkbenchFocusRegion::PrimaryGraph, "graph.primary", 1, graphCanvas_.get()),
        binding(WorkbenchFocusRegion::PrimaryGraph, "graph.performance", 2, performanceCanvas_.get()),
        binding(WorkbenchFocusRegion::PrimaryGraph, "graph.samples.map", 3, sampleMapPanel_.get()),
        binding(WorkbenchFocusRegion::PrimaryGraph, "graph.partial-tracks", 4, partialTrackEditorPanel_.get()),
        binding(WorkbenchFocusRegion::SecondaryGraph, "graph.secondary", 1, secondaryGraphCanvas_.get()),
        binding(WorkbenchFocusRegion::SecondaryGraph, "graph.split", 2, workspaceSplitHandle_.get()),
        binding(WorkbenchFocusRegion::SecondaryGraph, "graph.samples.waveform", 3, waveformPanel_.get()),
        binding(WorkbenchFocusRegion::RightWorkspace, "workspace.asset-recovery", 1, assetRecoveryPanel_.get()),
        binding(WorkbenchFocusRegion::RightWorkspace, "workspace.inspector", 2, inspectorPanel_.get()),
        binding(WorkbenchFocusRegion::RightWorkspace, "workspace.timeline", 3, timelinePanel_.get()),
        binding(WorkbenchFocusRegion::RightWorkspace, "workspace.analysis", 4, analysisPanel_.get()),
        binding(WorkbenchFocusRegion::RightWorkspace, "workspace.performance", 5, performancePanel_.get()),
        binding(WorkbenchFocusRegion::RightWorkspace, "workspace.sample-region", 6, sampleRegionInspector_.get()),
        binding(WorkbenchFocusRegion::LowerWorkspace, "lower.virtual-controls", 1, virtualControlSurface_.get()),
        binding(WorkbenchFocusRegion::LowerWorkspace, "lower.diagnostics", 2, diagnosticsPanel_.get()),
        binding(WorkbenchFocusRegion::StatusAndHelp, "status.project-state", 1, &projectStateLabel_),
        binding(WorkbenchFocusRegion::StatusAndHelp, "status.message", 2, &statusLabel_),
    };
    if (const auto& captured = focusCoordinator_.captured();
        captured.has_value())
    {
        const auto exactAlreadyPresent = std::ranges::any_of(
            bindings,
            [&](const auto& candidate) {
                return candidate.candidate.identity == *captured;
            });
        if (!exactAlreadyPresent)
        {
            std::optional<FocusBinding> exactBinding;
            auto remaining = maximumFocusTreeComponents;
            UniqueIdentityLookup lookup;
            for (const auto& candidate : bindings)
            {
                if (candidate.component == nullptr
                    || candidate.candidate.identity.region
                        != captured->region
                    || !candidate.candidate.visible)
                {
                    continue;
                }
                findUniqueDescendantByIdentity(
                    *candidate.component,
                    juceString(captured->itemId),
                    remaining,
                    lookup);
                if (lookup.ambiguous || lookup.exhausted)
                    break;
            }
            if (!lookup.ambiguous && !lookup.exhausted
                && lookup.component != nullptr
                && lookup.component->isVisible()
                && lookup.component->isEnabled())
            {
                exactBinding = FocusBinding{
                    {{captured->region, captured->itemId}, 0, true, true},
                    lookup.component,
                };
            }
            if (exactBinding.has_value())
                bindings.push_back(std::move(*exactBinding));
        }
    }
    std::vector<WorkbenchFocusCandidate> candidates;
    candidates.reserve(bindings.size());
    for (const auto& candidate : bindings)
        candidates.push_back(candidate.candidate);
    const auto target = focusCoordinator_.restorationTarget(candidates);
    lastFocusRestorationTarget_ = target;
    if (!target.has_value())
        return;
    const auto found = std::ranges::find_if(
        bindings,
        [&](const auto& candidate) {
            return candidate.candidate.identity == *target;
        });
    if (found != bindings.end() && found->component != nullptr
        && getPeer() != nullptr && getPeer()->isFocused())
        (void)focusComponentOrFirstChild(*found->component);
}

void WorkbenchComponent::scheduleWorkbenchFocusRestoration()
{
    juce::Component::SafePointer<WorkbenchComponent> safeThis(this);
    juce::MessageManager::callAsync([safeThis] {
        if (safeThis != nullptr)
            safeThis->restoreWorkbenchFocusAfterLayout();
    });
}
}
