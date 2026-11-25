#include <JuceHeader.h>

#include "app/workbench/WorkbenchHost.h"
#include "talas/audio/WavReader.h"
#include "talas/app/WorkbenchLaunch.h"
#include "talas/app/WorkbenchUiPolicy.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
using talas::app::workbench::WorkbenchHost;
using talas::app::workbench::WorkbenchHostOptions;
using talas::app::workbench::makeWorkbenchHost;
using talas::app::workbench::workbenchWindowBackgroundColour;

std::vector<std::string> workbenchCommandLineArguments()
{
    std::vector<std::string> arguments;
    const auto juceArguments =
        juce::JUCEApplicationBase::getCommandLineParameterArray();
    arguments.reserve(
        static_cast<std::size_t>(juceArguments.size()));
    for (const auto& argument : juceArguments)
        arguments.push_back(argument.toStdString());
    return arguments;
}

std::filesystem::path soundLibraryRoot()
{
#if defined(TALAS_SOUND_LIBRARY_SOURCE_ROOT)
    return TALAS_SOUND_LIBRARY_SOURCE_ROOT;
#else
    const auto executable = juce::File::getSpecialLocation(
        juce::File::currentExecutableFile);
#if JUCE_MAC
    return executable.getParentDirectory()
        .getParentDirectory()
        .getChildFile("Resources")
        .getChildFile("TalasSoundLibrary")
        .getFullPathName()
        .toStdString();
#else
    return executable.getParentDirectory()
        .getChildFile("resources")
        .getChildFile("TalasSoundLibrary")
        .getFullPathName()
        .toStdString();
#endif
#endif
}

class WorkbenchApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override
    {
        return JUCE_APPLICATION_NAME_STRING;
    }

    const juce::String getApplicationVersion() override
    {
        return JUCE_APPLICATION_VERSION_STRING;
    }

    bool moreThanOneInstanceAllowed() override
    {
        return talas::app::requiresIndependentWorkbenchInstance(
            workbenchCommandLineArguments());
    }

    void anotherInstanceStarted(
        const juce::String& commandLine) override
    {
        if (mainWindow_ != nullptr)
        {
            // A user's preferences file opened, as
            // by double-clicking it, gives its preferences.
            if (const auto preferences = talas::app::preferencesFileOpened(commandLine.toStdString()))
                mainWindow_->usePreferencesFile(*preferences);
            mainWindow_->setVisible(true);
            mainWindow_->toFront(true);
        }
    }

    void initialise(const juce::String& commandLine) override
    {
        juce::ignoreUnused(commandLine);
        const auto parsed = talas::app::parseWorkbenchLaunch(
            workbenchCommandLineArguments());
        if (!parsed.ok())
        {
            std::cerr << "WORKBENCH_SMOKE_FAILED: "
                      << parsed.error << '\n';
            setApplicationReturnValue(2);
            juce::MessageManager::callAsync([] {
                juce::JUCEApplication::getInstance()->quit();
            });
            return;
        }
        if (parsed.options->mode
            == talas::app::WorkbenchLaunchMode::SmokeTest)
        {
            WorkbenchHostOptions options;
            options.enableHardware = false;
            options.enablePersistentState = false;
            options.applicationName =
                getApplicationName().toStdString();
            options.isolatedStateDirectory =
                parsed.options->smokeDirectory;
            options.soundLibraryRoot = soundLibraryRoot();
            smokeWorkbench_ = makeWorkbenchHost(
                [](const juce::String&) {},
                std::move(options));
            const auto smokeDirectory =
                parsed.options->smokeDirectory;
            juce::MessageManager::callAsync(
                [this, smokeDirectory] {
                    auto error =
                        smokeWorkbench_->runAutomatedSmoke(
                            smokeDirectory);
                    smokeWorkbench_.reset();
                    if (!error.has_value())
                    {
                        const auto shutdownTake =
                            talas::audio::readPcm16Wav(
                                smokeDirectory
                                / "workbench-analysis-shutdown-take.wav");
                        if (!shutdownTake.ok()
                            || shutdownTake.sampleRate != 8000
                            || shutdownTake.channels.size() != 1
                            || shutdownTake.channels.front().size() != 128
                            || !std::ranges::all_of(
                                shutdownTake.channels.front(),
                                [](double sample) {
                                    return std::abs(sample - 0.25)
                                        <= 1.0 / 32767.0 + 1.0e-9;
                                }))
                        {
                            error = "Workbench destruction did not stop, join, and publish the active E6 output take";
                        }
                    }
                    if (error.has_value())
                    {
                        std::cerr
                            << "WORKBENCH_SMOKE_FAILED: "
                            << *error << '\n';
                        setApplicationReturnValue(1);
                    }
                    else
                    {
                        std::cout
                            << "WORKBENCH_SMOKE_OK\n";
                        setApplicationReturnValue(0);
                    }
                    quit();
                });
            return;
        }
        if (parsed.options->mode
            == talas::app::WorkbenchLaunchMode::RenderScreenshots)
        {
            renderScreenshots(parsed.options->screenshotDirectory);
            return;
        }
        mainWindow_ =
            std::make_unique<MainWindow>(getApplicationName(), parsed.options->preferencesFile);
    }

    // The Workbench built off screen without devices or user state, its
    // own state kept in a scratch folder removed afterwards.
    void renderScreenshots(const std::filesystem::path& directory)
    {
        // A short, generic scratch path, since the Samples view shows where
        // its sample files are.
#if JUCE_WINDOWS
        const auto scratchRoot = juce::File::getSpecialLocation(juce::File::tempDirectory);
#else
        const auto scratchRoot = juce::File{"/tmp"}.isDirectory()
            ? juce::File{"/tmp"}
            : juce::File::getSpecialLocation(juce::File::tempDirectory);
#endif
        screenshotState_ = scratchRoot.getNonexistentChildFile("talas-showcase", {}, false);
        if (!screenshotState_.createDirectory())
        {
            std::cerr << "WORKBENCH_SCREENSHOTS_FAILED: scratch state folder could not be created\n";
            setApplicationReturnValue(1);
            juce::MessageManager::callAsync([] { juce::JUCEApplication::getInstance()->quit(); });
            return;
        }
        WorkbenchHostOptions options;
        options.enableHardware = false;
        options.enablePersistentState = false;
        options.applicationName = getApplicationName().toStdString();
        options.isolatedStateDirectory = screenshotState_.getFullPathName().toStdString();
        options.soundLibraryRoot = soundLibraryRoot();
        smokeWorkbench_ = makeWorkbenchHost([](const juce::String&) {}, std::move(options));
        juce::MessageManager::callAsync([this, directory] {
            const auto error = smokeWorkbench_->renderScreenshots(directory);
            smokeWorkbench_.reset();
            screenshotState_.deleteRecursively();
            if (error.has_value())
            {
                std::cerr << "WORKBENCH_SCREENSHOTS_FAILED: " << *error << '\n';
                setApplicationReturnValue(1);
            }
            else
            {
                std::cout << "WORKBENCH_SCREENSHOTS_OK\n";
                setApplicationReturnValue(0);
            }
            quit();
        });
    }

    void shutdown() override
    {
        smokeWorkbench_.reset();
        mainWindow_.reset();
    }

    void systemRequestedQuit() override
    {
        if (mainWindow_ != nullptr)
            mainWindow_->requestClose();
        else
            quit();
    }

private:
    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow(const juce::String& name, std::filesystem::path preferencesFile)
            : juce::DocumentWindow(
                  name,
                  workbenchWindowBackgroundColour(),
                  juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar(true);
            WorkbenchHostOptions options;
            options.applicationName = name.toStdString();
            options.soundLibraryRoot = soundLibraryRoot();
            options.preferencesFile = std::move(preferencesFile);
            auto workbench = makeWorkbenchHost(
                [this](const juce::String& title) {
                    setName(title);
                },
                std::move(options));
            workbench_ = workbench.get();
            setContentOwned(workbench.release(), true);
#if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu(workbench_);
            constexpr int windowMenuBarHeight = 0;
#else
            setMenuBar(workbench_);
            const auto windowMenuBarHeight =
                getLookAndFeel().getDefaultMenuBarHeight();
#endif
            setResizable(true, true);
            setResizeLimits(
                talas::app::workbenchMinimumWidth,
                talas::app::workbenchMinimumHeight
                    + windowMenuBarHeight,
                4096,
                2160 + windowMenuBarHeight);
            centreWithSize(
                talas::app::workbenchDefaultWidth,
                talas::app::workbenchDefaultHeight
                    + windowMenuBarHeight);
            setVisible(true);
            workbench_->beginHardwareInitialisation();
        }

        ~MainWindow() override
        {
#if JUCE_MAC
            juce::MenuBarModel::setMacMainMenu(nullptr);
#else
            setMenuBar(nullptr);
#endif
            workbench_ = nullptr;
        }

        void usePreferencesFile(const std::filesystem::path& file)
        {
            if (workbench_ != nullptr)
                workbench_->usePreferencesFile(file);
        }

        void requestClose()
        {
            if (workbench_ != nullptr)
                workbench_->requestQuit();
            else
                juce::JUCEApplication::getInstance()->quit();
        }

        void closeButtonPressed() override
        {
            requestClose();
        }

    private:
        WorkbenchHost* workbench_ = nullptr;
    };

    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<WorkbenchHost> smokeWorkbench_;
    juce::File screenshotState_;
};
}

START_JUCE_APPLICATION(WorkbenchApplication)
