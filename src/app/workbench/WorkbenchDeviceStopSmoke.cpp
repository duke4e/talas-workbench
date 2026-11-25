#include "app/workbench/WorkbenchComponentInternal.h"

namespace talas::app::workbench
{
std::optional<std::string>
WorkbenchComponent::runDeviceStopOffMessageThreadSmoke()
{
    // A lost device can be reported on a CoreAudio notification thread; the
    // stop must not run its message-thread body there.
    const auto generationBeforeStop =
        deviceGeneration_.load(std::memory_order_acquire);
    std::thread notification([this] { audioDeviceStopped(); });
    notification.join();
    const auto stopDeferred =
        deviceGeneration_.load(std::memory_order_acquire)
        == generationBeforeStop;
    // Retire the deferred stop as a newer device start would.
    deviceGeneration_.fetch_add(1, std::memory_order_acq_rel);
    if (!stopDeferred)
        return "Device stop reported off the message thread ran there";
    return std::nullopt;
}
}
