#pragma once

#include "talas/app/TalasFiles.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace talas::app::workbench
{
// The File Organizer: folders and the files
// Talas uses, colour-coded by kind, a selected file's information below.
class FileOrganizerPanel : public juce::Component
{
public:
    ~FileOrganizerPanel() override = default;

    virtual void setRoots(std::vector<std::filesystem::path> roots) = 0;
    virtual void refresh() = 0;
    // Edit > View...: folders first, or among the
    // files by name.
    virtual void setFoldersFirst(bool foldersFirst) = 0;
    [[nodiscard]] virtual bool foldersFirst() const = 0;
    [[nodiscard]] virtual std::vector<talas::app::FileOrganizerRow> rowsForSmoke() const = 0;
    virtual bool activateRowForSmoke(std::size_t row) = 0;
    [[nodiscard]] virtual juce::String informationForSmoke(std::size_t row) = 0;
    // Ctrl+Space on a row: whether it played.
    [[nodiscard]] virtual bool playRowForSmoke(std::size_t row) = 0;
    // A click on a row, then a key pressed in the list; the row selected
    // after it.
    [[nodiscard]] virtual int selectThenPressForSmoke(std::size_t row, const juce::KeyPress& key) = 0;
};

[[nodiscard]] std::unique_ptr<FileOrganizerPanel> makeFileOrganizerPanel(
    std::function<void(const std::filesystem::path&)> open,
    std::function<bool(const std::filesystem::path&)> play);
}
