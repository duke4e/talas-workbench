#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace talas::app::workbench
{
struct WorkbenchHostOptions
{
    bool enableHardware = true;
    bool enablePersistentState = true;
    std::string applicationName = "Talas Workbench";
    std::filesystem::path isolatedStateDirectory;
    std::filesystem::path soundLibraryRoot;
    // A user's own preferences file, read and kept
    // instead of the shared one.
    std::filesystem::path preferencesFile;
};

using WorkbenchTitleCallback =
    std::function<void(const juce::String&)>;

class WorkbenchHost : public juce::Component,
                      public juce::MenuBarModel
{
public:
    ~WorkbenchHost() override = default;

    virtual void beginHardwareInitialisation() = 0;
    virtual void requestQuit() = 0;
    // A user's preferences file opened while the Workbench runs: its
    // preferences taken and kept there from then on.
    virtual void usePreferencesFile(const std::filesystem::path&) {}
    [[nodiscard]] virtual std::optional<std::string>
    runAutomatedSmoke(
        const std::filesystem::path& smokeDirectory) = 0;
};

[[nodiscard]] std::unique_ptr<WorkbenchHost> makeWorkbenchHost(
    WorkbenchTitleCallback onTitleChange,
    WorkbenchHostOptions options = {});

[[nodiscard]] juce::Colour
workbenchWindowBackgroundColour() noexcept;
}
