#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace talas::runtime
{
enum class KernelAbiProbeCase
{
    CleanExit,
    MetadataRejected,
    ControlledFatal,
    AbnormalTermination,
    AbnormalTerminationNormalExit,
    BoundedTimeout,
};

enum class KernelAbiProbeOutcome
{
    CleanExit,
    MetadataRejected,
    ControlledFatal,
    AbnormalTermination,
    TimedOut,
    LaunchFailed,
    ProtocolError,
};

inline constexpr std::size_t maximumKernelAbiProbeDiagnosticBytes = 4'096;

struct KernelAbiProbeResult
{
    KernelAbiProbeOutcome outcome = KernelAbiProbeOutcome::LaunchFailed;
    int exitCode = -1;
    std::string diagnostics;
    bool diagnosticsTruncated = false;
    bool confersTrust = false;
};

[[nodiscard]] KernelAbiProbeResult runKernelAbiProbe(
    KernelAbiProbeCase probeCase,
    std::chrono::milliseconds timeout);
}
