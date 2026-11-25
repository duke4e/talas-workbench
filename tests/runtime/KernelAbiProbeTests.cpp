#include "talas/runtime/KernelAbiProbe.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireOutcome(
    talas::runtime::KernelAbiProbeCase probeCase,
    talas::runtime::KernelAbiProbeOutcome expected,
    std::string_view diagnosticTag,
    std::chrono::milliseconds timeout = std::chrono::seconds(2))
{
    const auto result = talas::runtime::runKernelAbiProbe(probeCase, timeout);
    require(result.outcome == expected, "probe should return the typed outcome");
    require(
        result.diagnostics.find(diagnosticTag) != std::string::npos,
        "probe should retain the expected child diagnostic tag");
    require(
        result.diagnostics.size() <= talas::runtime::maximumKernelAbiProbeDiagnosticBytes,
        "probe diagnostics must remain within the fixed parent bound");
    require(!result.confersTrust,
        "probe execution must never confer trust on a candidate");
}

void testClosedProbeCasesRemainContainedAndTyped()
{
    requireOutcome(
        talas::runtime::KernelAbiProbeCase::CleanExit,
        talas::runtime::KernelAbiProbeOutcome::CleanExit,
        "clean_exit");
    requireOutcome(
        talas::runtime::KernelAbiProbeCase::MetadataRejected,
        talas::runtime::KernelAbiProbeOutcome::MetadataRejected,
        "metadata_rejected");
    requireOutcome(
        talas::runtime::KernelAbiProbeCase::ControlledFatal,
        talas::runtime::KernelAbiProbeOutcome::ControlledFatal,
        "controlled_fatal");
    requireOutcome(
        talas::runtime::KernelAbiProbeCase::AbnormalTermination,
        talas::runtime::KernelAbiProbeOutcome::AbnormalTermination,
        "abnormal_termination");
    requireOutcome(
        talas::runtime::KernelAbiProbeCase::AbnormalTerminationNormalExit,
        talas::runtime::KernelAbiProbeOutcome::ProtocolError,
        "abnormal_normal_exit");
    requireOutcome(
        talas::runtime::KernelAbiProbeCase::BoundedTimeout,
        talas::runtime::KernelAbiProbeOutcome::TimedOut,
        "bounded_timeout",
        std::chrono::milliseconds(100));

    requireOutcome(
        talas::runtime::KernelAbiProbeCase::CleanExit,
        talas::runtime::KernelAbiProbeOutcome::CleanExit,
        "clean_exit");
}

void testTimeoutDiagnosticsAreTruncatedWithoutBlockingParent()
{
    const auto result = talas::runtime::runKernelAbiProbe(
        talas::runtime::KernelAbiProbeCase::BoundedTimeout,
        std::chrono::milliseconds(100));
    require(
        result.outcome == talas::runtime::KernelAbiProbeOutcome::TimedOut
            && result.diagnosticsTruncated
            && result.diagnostics.size()
                == talas::runtime::maximumKernelAbiProbeDiagnosticBytes,
        "timeout output should be drained, bounded, and classified");
}

static_assert(std::is_same_v<
    decltype(talas::runtime::runKernelAbiProbe(
        talas::runtime::KernelAbiProbeCase::CleanExit,
        std::chrono::milliseconds(1))),
    talas::runtime::KernelAbiProbeResult>);
}

int main()
{
    testClosedProbeCasesRemainContainedAndTyped();
    testTimeoutDiagnosticsAreTruncatedWithoutBlockingParent();
    return 0;
}
