#pragma once

#include "talas/project/ProjectDocument.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// --render-screenshots: the views beyond the workspaces, each drawn off
// screen through the Workbench's look and feel exactly as the live window
// draws it: popup menus, fly-by help, alerts, the common controls, dialog
// windows and the plugin editor.
namespace talas::app::workbench::screenshot_views
{
// An image written as an opaque PNG.
[[nodiscard]] std::optional<std::string> writePng(const juce::Image& image, const std::filesystem::path& file);

// Menus side by side over the backdrop, each with the item at its index
// highlighted, as a popup menu window draws them.
[[nodiscard]] juce::Image popupMenus(const std::vector<std::pair<juce::PopupMenu, int>>& menus);

// Fly-by help beside a control, an information alert and a warning.
[[nodiscard]] juce::Image helpAndAlerts();

// The common controls in their states: tabs, progress, buttons, toggles,
// combo boxes, fields, sliders, knobs, pads and pills.
[[nodiscard]] juce::Image controlSheet();

// A dialog's content under a title bar, over the window's backdrop.
[[nodiscard]] juce::Image dialogWindow(juce::Component& content, const juce::String& title);

// The Audio & MIDI Settings dialog's content, with a stand-in device type
// so no audio hardware is opened.
[[nodiscard]] juce::Image audioMidiSettings();

// The exported plugin's editor for the project, its parameters at the
// project's values.
[[nodiscard]] juce::Image pluginEditor(const talas::project::ProjectDocument& document);
}
