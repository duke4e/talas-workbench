#include "app/workbench/WorkbenchDiagnosticsPanel.h"

#include "app/workbench/WorkbenchTheme.h"
#include "app/workbench/WorkbenchUiHelpers.h"
#include "talas/app/WorkbenchSurfaceState.h"

#include <algorithm>
#include <ranges>
#include <utility>

namespace talas::app::workbench
{
namespace
{
class DiagnosticsPanelImpl final : public DiagnosticsPanel,
                                   private juce::ListBoxModel
{
public:
    explicit DiagnosticsPanelImpl(ActivateCallback onActivate)
        : onActivate_(std::move(onActivate))
    {
        describeControl(
            *this,
            "Diagnostics",
            "Compile and prepared-runtime diagnostics for the current Sound graph.");
        addAndMakeVisible(title_);
        addAndMakeVisible(list_);
        addAndMakeVisible(emptyState_);
        title_.setText("Diagnostics", juce::dontSendNotification);
        title_.setColour(juce::Label::textColourId, textColour);
        title_.setFont(WorkbenchType::panelTitle());
        list_.setModel(this);
        list_.setRowHeight(42);
        list_.setMultipleSelectionEnabled(false);
        list_.setWantsKeyboardFocus(true);
        list_.setColour(
            juce::ListBox::backgroundColourId,
            panelColour);
        list_.setColour(
            juce::ListBox::outlineColourId,
            panelBorderColour);
        describeControl(
            list_,
            "Compile and runtime diagnostics",
            "Select a diagnostic with the pointer or arrow keys. Press Return to open its exact graph target.");

        emptyState_.setText(
            "No compile or runtime diagnostics",
            juce::dontSendNotification);
        emptyState_.setColour(
            juce::Label::textColourId,
            secondaryTextColour);
        emptyState_.setFont(WorkbenchType::label());
        emptyState_.setJustificationType(
            juce::Justification::centred);
        emptyState_.setInterceptsMouseClicks(false, false);
        describeControl(
            emptyState_,
            "No compile or runtime diagnostics",
            "The current graph has no compile issue, latency warning, or stream-health diagnostic.");
        rebuildRows();
    }

    ~DiagnosticsPanelImpl() override
    {
        list_.setModel(nullptr);
    }

    std::unique_ptr<juce::AccessibilityHandler>
    createAccessibilityHandler() override
    {
        return std::make_unique<juce::AccessibilityHandler>(
            *this,
            juce::AccessibilityRole::group);
    }

    const std::optional<talas::app::WorkbenchSurfaceState>&
    workbenchSurfaceStateForContextHelp() const noexcept override
    {
        return surfaceState_;
    }

    void setCompileDiagnostics(
        std::span<const talas::runtime::Diagnostic> diagnostics) override
    {
        compileRows_.clear();
        compileRows_.reserve(diagnostics.size());
        for (const auto& diagnostic : diagnostics)
        {
            auto target = diagnostic.structuralSourcePath.empty()
                ? diagnostic.nodeId
                : diagnostic.structuralSourcePath;
            WorkbenchStatusDiagnostic row;
            row.title = target.empty()
                ? juce::String{"Compile issue"}
                : juceString("Compile · ") + juceString(target);
            row.detail = juceString(diagnostic.message);
            if (!diagnostic.portId.empty())
                row.detail += juceString(" · port ")
                    + juceString(diagnostic.portId);
            if (!diagnostic.parameterId.empty())
            {
                row.detail += juceString(" · parameter ")
                    + juceString(diagnostic.parameterId);
            }
            row.identity = {
                diagnostic.nodeId,
                diagnostic.structuralSourcePath,
            };
            row.severity = WorkbenchStatusDiagnosticSeverity::Error;
            row.navigation.kind = diagnostic.parameterId.empty()
                ? talas::app::AnalysisNavigationKind::GraphNode
                : talas::app::AnalysisNavigationKind::Parameter;
            row.navigation.nodeId = diagnostic.nodeId;
            row.navigation.structuralSourcePath =
                diagnostic.structuralSourcePath;
            row.navigation.parameterId = diagnostic.parameterId;
            compileRows_.push_back(std::move(row));
        }
        rebuildRows();
    }

    void setRuntimeDiagnostics(
        std::vector<WorkbenchStatusDiagnostic> diagnostics) override
    {
        if (sameDiagnostics(runtimeRows_, diagnostics))
            return;
        runtimeRows_ = std::move(diagnostics);
        rebuildRows();
    }

    [[nodiscard]] bool
    hasKeyboardAccessiblePreparedStateForSmoke() const override
    {
        return list_.getWantsKeyboardFocus()
            && list_.getTitle().isNotEmpty()
            && (rows_.empty()
                    ? emptyState_.isVisible()
                        && emptyState_.getText()
                            .startsWith("No diagnostics")
                    : list_.isVisible());
    }

    [[nodiscard]] bool runtimeDiagnosticContainsForSmoke(
        juce::StringRef titlePrefix,
        juce::StringRef detailText,
        talas::app::AnalysisNavigationKind navigationKind,
        std::uint64_t stageId) const override
    {
        return std::ranges::any_of(
            runtimeRows_,
            [&](const auto& diagnostic) {
                return diagnostic.title.startsWith(titlePrefix)
                    && diagnostic.detail.contains(detailText)
                    && diagnostic.navigation.kind == navigationKind
                    && diagnostic.navigation.stageId == stageId;
            });
    }

    [[nodiscard]] bool activateFirstTargetForSmoke() override
    {
        for (std::size_t row = 0; row < rows_.size(); ++row)
        {
            if (!hasTarget(rows_[row]))
                continue;
            list_.selectRow(static_cast<int>(row));
            returnKeyPressed(static_cast<int>(row));
            return true;
        }
        return false;
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(panelColour);
        graphics.setColour(panelBorderColour);
        graphics.drawRect(getLocalBounds(), 1);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(8);
        title_.setBounds(bounds.removeFromTop(20));
        list_.setBounds(bounds);
        emptyState_.setBounds(bounds.reduced(8));
    }

private:
    int getNumRows() override
    {
        return static_cast<int>(rows_.size());
    }

    juce::String getNameForRow(int row) override
    {
        const auto* diagnostic = diagnosticForRow(row);
        if (diagnostic == nullptr)
            return {};
        auto name = severityText(diagnostic->severity)
            + ", " + diagnostic->title
            + ", " + diagnostic->detail;
        if (hasTarget(*diagnostic))
            name += ", activate to select graph target";
        return name;
    }

    juce::String getTooltipForRow(int row) override
    {
        const auto* diagnostic = diagnosticForRow(row);
        if (diagnostic == nullptr)
            return {};
        return diagnostic->title + "\n" + diagnostic->detail;
    }

    void paintListBoxItem(
        int row,
        juce::Graphics& graphics,
        int width,
        int height,
        bool selected) override
    {
        const auto* diagnostic = diagnosticForRow(row);
        if (diagnostic == nullptr)
            return;
        if (selected)
        {
            graphics.setColour(workbenchTheme.selectedRow);
            graphics.fillRect(0, 0, width, height);
        }
        auto bounds = juce::Rectangle<int>{0, 0, width, height}
                          .reduced(8, 3);
        const auto severityBounds = bounds.removeFromLeft(42);
        graphics.setColour(
            diagnostic->severity
                    == WorkbenchStatusDiagnosticSeverity::Error
                ? errorColour
                : secondaryTextColour);
        graphics.setFont(WorkbenchType::microStrong());
        graphics.drawText(
            severityText(diagnostic->severity),
            severityBounds,
            juce::Justification::centredLeft);
        graphics.setColour(textColour);
        graphics.setFont(WorkbenchType::labelStrong());
        graphics.drawText(
            diagnostic->title,
            bounds.removeFromTop(18),
            juce::Justification::centredLeft,
            true);
        graphics.setColour(secondaryTextColour);
        graphics.setFont(WorkbenchType::micro());
        graphics.drawText(
            diagnostic->detail,
            bounds,
            juce::Justification::centredLeft,
            true);
    }

    void listBoxItemClicked(
        int row,
        const juce::MouseEvent&) override
    {
        activateRow(row);
    }

    void returnKeyPressed(int row) override
    {
        activateRow(row);
    }

    [[nodiscard]] const WorkbenchStatusDiagnostic*
    diagnosticForRow(int row) const
    {
        if (row < 0 || row >= static_cast<int>(rows_.size()))
            return nullptr;
        return &rows_[static_cast<std::size_t>(row)];
    }

    [[nodiscard]] static bool hasTarget(
        const WorkbenchStatusDiagnostic& diagnostic)
    {
        return diagnostic.navigation.kind
                != talas::app::AnalysisNavigationKind::None
            || !diagnostic.identity.nodeId.empty()
            || !diagnostic.identity.structuralSourcePath.empty();
    }

    [[nodiscard]] static bool sameDiagnostics(
        std::span<const WorkbenchStatusDiagnostic> left,
        std::span<const WorkbenchStatusDiagnostic> right)
    {
        return left.size() == right.size()
            && std::ranges::equal(
                left,
                right,
                [](const auto& a, const auto& b) {
                    return a.title == b.title
                        && a.detail == b.detail
                        && a.identity.nodeId == b.identity.nodeId
                        && a.identity.structuralSourcePath
                            == b.identity.structuralSourcePath
                        && a.severity == b.severity
                        && a.navigation == b.navigation;
                });
    }

    [[nodiscard]] static juce::String severityText(
        WorkbenchStatusDiagnosticSeverity severity)
    {
        switch (severity)
        {
        case WorkbenchStatusDiagnosticSeverity::Error:
            return "ERROR";
        case WorkbenchStatusDiagnosticSeverity::Warning:
            return "WARN";
        case WorkbenchStatusDiagnosticSeverity::Info:
            return "INFO";
        }
        return {};
    }

    void activateRow(int row)
    {
        const auto* diagnostic = diagnosticForRow(row);
        if (diagnostic != nullptr
            && hasTarget(*diagnostic)
            && onActivate_)
        {
            onActivate_(*diagnostic);
        }
    }

    void rebuildRows()
    {
        rows_.clear();
        rows_.reserve(compileRows_.size() + runtimeRows_.size());
        rows_.insert(
            rows_.end(),
            compileRows_.begin(),
            compileRows_.end());
        rows_.insert(
            rows_.end(),
            runtimeRows_.begin(),
            runtimeRows_.end());
        list_.updateContent();
        const auto empty = rows_.empty();
        const auto hasError = std::ranges::any_of(
            rows_,
            [](const auto& row) {
                return row.severity == WorkbenchStatusDiagnosticSeverity::Error;
            });
        const auto kind = empty
            ? talas::app::WorkbenchSurfaceStateKind::Empty
            : hasError
                ? talas::app::WorkbenchSurfaceStateKind::Error
                : talas::app::WorkbenchSurfaceStateKind::Content;
        surfaceState_ = talas::app::workbenchSurfaceState({
            kind,
            empty ? "No diagnostics" : "Diagnostics available",
            empty
                ? "The current graph has no compile issue, latency warning, or stream-health diagnostic."
                : hasError
                    ? "One or more diagnostics require attention; prior usable rows remain available."
                    : "Prepared graph diagnostics are available.",
            {},
            !empty,
        });
        if (surfaceState_.has_value())
        {
            emptyState_.setText(
                juceString(surfaceState_->title()) + " | "
                    + juceString(surfaceState_->explanation()),
                juce::dontSendNotification);
            setTitle(
                juceString(talas::app::workbenchSurfaceStateName(
                    surfaceState_->kind()))
                + ": " + juceString(surfaceState_->title()));
        }
        list_.setVisible(!empty);
        emptyState_.setVisible(empty);
        repaint();
    }

    ActivateCallback onActivate_;
    std::vector<WorkbenchStatusDiagnostic> compileRows_;
    std::vector<WorkbenchStatusDiagnostic> runtimeRows_;
    std::vector<WorkbenchStatusDiagnostic> rows_;
    std::optional<talas::app::WorkbenchSurfaceState> surfaceState_;
    juce::Label title_;
    juce::ListBox list_;
    juce::Label emptyState_;
};
}

std::unique_ptr<DiagnosticsPanel>
makeDiagnosticsPanel(DiagnosticsPanel::ActivateCallback onActivate)
{
    return std::make_unique<DiagnosticsPanelImpl>(
        std::move(onActivate));
}
}
