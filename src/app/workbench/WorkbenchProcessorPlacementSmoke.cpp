#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runProcessorPlacementSmoke()
{
    const auto previousDocument = projectBridge_.editor().document();
    const auto finish = [&](std::string error = {})
        -> std::optional<std::string> {
        const auto restored =
            projectBridge_.editor().replaceDocument(previousDocument);
        documentChanged();
        if (!restored.ok() && error.empty())
            error = "processor placement smoke could not restore the prior project";
        if (error.empty())
            return std::nullopt;
        return error;
    };

    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "outer", "talas.forcedProcessorAssignment", {{"processor", "7"}}});
    document.graph.nodes.push_back({
        "inner", "talas.forcedProcessorAssignment", {{"processor", "3"}}});
    document.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    document.graph.connections.push_back(
        {"osc", "audioOut", "inner", "audioIn"});
    document.graph.connections.push_back(
        {"inner", "audioOut", "outer", "audioIn"});
    document.graph.connections.push_back(
        {"outer", "audioOut", "out", "audioIn"});

    if (!projectBridge_.editor().replaceDocument(document).ok())
        return finish("processor placement smoke project was rejected");
    documentChanged();
    talas::runtime::RenderSettings settings{48'000.0, 64};
    if (!stageProject(settings, "processor placement smoke"))
        return finish("processor placement smoke could not stage its graph");
    std::array<float, 64> output{};
    std::array<float*, 1> channels{output.data()};
    projectBridge_.setPlaying(true);
    const auto adopted = projectBridge_.render(
        {}, 1.0, channels.data(), channels.size(), output.size());
    projectBridge_.setPlaying(false);
    if (!adopted)
        return finish("processor placement smoke could not adopt its staged graph");
    selectNode("inner");
    refreshPreparedGraphStatus();
    if (!projectStateLabel_.getText().contains("Logical domains 3")
        || !projectStateLabel_.getTitle().contains(
            "3 logical processor domains")
        || !projectStateLabel_.getTitle().contains(
            "portable serial host execution")
        || !inspectorPanel_->preparedContractContainsForSmoke(
            "Logical processor 3")
        || !inspectorPanel_->preparedContractContainsForSmoke(
            "placement boundary")
        || !inspectorPanel_->preparedContractContainsForSmoke(
            "Processor transfer"))
    {
        return finish(
            "processor placement smoke omitted visible or accessible authority");
    }

    auto replacement = document;
    replacement.graph.nodes.erase(
        replacement.graph.nodes.begin() + 1,
        replacement.graph.nodes.begin() + 3);
    replacement.graph.connections = {
        {"osc", "audioOut", "out", "audioIn"},
    };
    if (!projectBridge_.editor().replaceDocument(replacement).ok())
        return finish("processor placement replacement project was rejected");
    documentChanged();
    if (!stageProject(settings, "processor placement replacement smoke"))
        return finish("processor placement replacement graph did not stage");
    projectBridge_.setPlaying(true);
    const auto replacementAdopted = projectBridge_.render(
        {}, 2.0, channels.data(), channels.size(), output.size());
    projectBridge_.setPlaying(false);
    if (!replacementAdopted)
        return finish("processor placement replacement graph was not adopted");
    selectNode("osc");
    refreshPreparedGraphStatus();
    if (!projectStateLabel_.getText().contains("Logical domains 1")
        || projectStateLabel_.getText().contains("Logical domains 3")
        || inspectorPanel_->preparedContractContainsForSmoke(
            "Logical processor 3")
        || inspectorPanel_->preparedContractContainsForSmoke(
            "Processor transfer"))
    {
        return finish(
            "processor placement replacement retained stale prepared authority");
    }
    return finish();
}
}
