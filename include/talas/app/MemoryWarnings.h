#pragma once

#include "talas/project/ProjectDocument.h"

#include <string>
#include <vector>

namespace talas::app
{
// Sample memory warnings: a Sound that reads a
// MemoryWriter's recording from the moment the Sound starts, before
// anything is written there, and a MemoryWriter whose recording no Sound
// reads. A reader waits, and is not warned of, when a TimeOffset or a
// SetDuration with a StartTime lies between it and the output, or when its
// own Gate or Trigger is not a constant (put a TimeOffset on the Sound that
// reads the memory). Each warning names the Sound and the recording.
enum class MemoryWarningKind
{
    ReadBeforeWritten,
    WrittenNeverRead,
};

struct MemoryWarning
{
    MemoryWarningKind kind = MemoryWarningKind::ReadBeforeWritten;
    std::string nodeId;
    std::string recording;
    std::string message;

    bool operator==(const MemoryWarning&) const = default;
};

[[nodiscard]] std::vector<MemoryWarning> memoryWarnings(const talas::project::ProjectGraph& graph);
// What the play status adds: the first warning and how many more, or
// nothing.
[[nodiscard]] std::string memoryWarningsStatus(const talas::project::ProjectGraph& graph);
}
