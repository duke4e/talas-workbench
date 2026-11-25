#pragma once

namespace talas::project
{
class ProjectAutosaveState
{
public:
    explicit ProjectAutosaveState(
        double debounceMilliseconds = 2000.0,
        double initialRetryMilliseconds = 5000.0,
        double maximumRetryMilliseconds = 30000.0);

    void documentChanged(bool dirty, double nowMilliseconds) noexcept;
    [[nodiscard]] bool shouldStartWrite(
        bool dirty,
        bool writerActive,
        double nowMilliseconds) const noexcept;
    void writeStarted() noexcept;
    void writeFinished(
        bool succeeded,
        bool dirty,
        double nowMilliseconds) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] bool hasWriteFailure() const noexcept;

private:
    double debounceMilliseconds_ = 2000.0;
    double initialRetryMilliseconds_ = 5000.0;
    double maximumRetryMilliseconds_ = 30000.0;
    double currentRetryMilliseconds_ = 5000.0;
    double dueMilliseconds_ = 0.0;
    bool pending_ = false;
    bool hasWriteFailure_ = false;
};
}
