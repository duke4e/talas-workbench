#include "talas/runtime/BuiltinKernelAbiModule.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/KernelAbiHost.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace
{
void* allocate(void*, std::uint64_t bytes, std::uint64_t)
{
    return std::malloc(static_cast<std::size_t>(bytes));
}

void deallocate(
    void*,
    void* allocation,
    std::uint64_t,
    std::uint64_t)
{
    std::free(allocation);
}

void diagnostic(
    void*,
    TalasKernelAbiDiagnosticSeverityV1,
    std::uint32_t,
    TalasKernelAbiStringViewV1)
{
}

TalasKernelAbiHostServicesV1 services()
{
    TalasKernelAbiHostServicesV1 result{};
    result.structSize = sizeof(result);
    result.servicesVersion = TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V1;
    result.allocate = allocate;
    result.deallocate = deallocate;
    result.diagnostic = diagnostic;
    return result;
}

TalasKernelAbiStringViewV1 text(std::string_view value)
{
    return {value.data(), value.size()};
}

TalasKernelAbiPrepareV1 prepare()
{
    TalasKernelAbiPrepareV1 result{};
    result.structSize = sizeof(result);
    result.apiVersion = TALAS_KERNEL_API_VERSION_V1;
    result.sampleRate = 48'000.0;
    result.maximumBlockFrames = 1;
    return result;
}

int cleanExit()
{
    auto hostServices = services();
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        hostServices);
    if (!host.registerModule(talas::runtime::builtinKernelAbiModule()).ok()
        || !host.createPreparedInstance(
            "org.talas.builtin-kernels",
            "talas.oscillator",
            prepare()).ok())
    {
        return 70;
    }
    std::cout << "clean_exit\n" << std::flush;
    return 0;
}

int metadataRejected()
{
    auto hostServices = services();
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        hostServices);
    auto candidate = talas::runtime::builtinKernelAbiModule();
    candidate.compatibility.pointerBits /= 2;
    const auto result = host.registerModule(candidate);
    if (result.ok() || result.diagnostics.empty()
        || result.diagnostics.front().code
            != talas::runtime::KernelAbiValidationCode::IncompatiblePlatform)
    {
        return 71;
    }
    std::cout << "metadata_rejected\n" << std::flush;
    return 20;
}

TalasKernelAbiStatusV1 createFatal(
    const TalasKernelAbiHostServicesV1* host,
    void** instance)
{
    *instance = host->allocate(host->hostContext, 1, 1);
    return *instance == nullptr
        ? TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR
        : TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 prepareFatal(void*, const TalasKernelAbiPrepareV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 resetFatal(void*, const TalasKernelAbiResetV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 processFatal(void*, const TalasKernelAbiProcessV1*)
{
    return TALAS_KERNEL_ABI_STATUS_FATAL;
}

void destroyFatal(
    void* instance,
    const TalasKernelAbiHostServicesV1* host)
{
    host->deallocate(host->hostContext, instance, 1, 1);
}

void unloadFatal(void*, const TalasKernelAbiHostServicesV1*)
{
}

TalasKernelAbiModuleV1 fatalModule()
{
    static const TalasKernelAbiDescriptorV1 descriptor = [] {
        TalasKernelAbiDescriptorV1 result{};
        result.structSize = sizeof(result);
        result.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        result.typeId = text("talas.oscillator");
        result.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET
            | TALAS_KERNEL_ABI_CAPABILITY_STATEFUL;
        result.create = createFatal;
        result.prepare = prepareFatal;
        result.reset = resetFatal;
        result.process = processFatal;
        result.destroy = destroyFatal;
        return result;
    }();
    TalasKernelAbiModuleV1 result{};
    result.structSize = sizeof(result);
    result.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
    result.moduleId = text("org.talas.fatal-probe");
    result.moduleVersion = text("1.0.0");
    result.minimumApiVersion = TALAS_KERNEL_API_VERSION_V1;
    result.maximumApiVersion = TALAS_KERNEL_API_VERSION_V1;
    result.compatibility = talas::runtime::currentKernelAbiCompatibility();
    result.sampleFormats = TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64;
    result.trustClass = TALAS_KERNEL_ABI_TRUST_BUILTIN;
    result.capabilityFlags = descriptor.capabilityFlags;
    result.kernels = &descriptor;
    result.kernelCount = 1;
    result.unload = unloadFatal;
    return result;
}

int controlledFatal()
{
    auto hostServices = services();
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        hostServices);
    const auto module = fatalModule();
    if (!host.registerModule(module).ok())
        return 72;
    const auto created = host.createPreparedInstance(
        "org.talas.fatal-probe",
        "talas.oscillator",
        prepare());
    if (!created.ok())
        return 73;
    double sample = 0.0;
    TalasKernelAbiOutputV1 output{};
    output.structSize = sizeof(output);
    output.samples = &sample;
    output.sampleCount = 1;
    TalasKernelAbiProcessV1 block{};
    block.structSize = sizeof(block);
    block.frameCount = 1;
    block.outputs = &output;
    block.outputCount = 1;
    block.transport.structSize = sizeof(block.transport);
    block.transport.sampleRate = 48'000.0;
    if (created.instance->process(block) != TALAS_KERNEL_ABI_STATUS_FATAL
        || created.instance->process(block)
            != TALAS_KERNEL_ABI_STATUS_QUARANTINED)
    {
        return 74;
    }
    if (host.retireInstance(created.instance)
            != talas::runtime::KernelAbiInstanceRetirementStatus::Accepted
        || host.requestModuleRetirement("org.talas.fatal-probe")
            != talas::runtime::KernelAbiRetirementStatus::Accepted
        || host.collectRetiredModules() != 1)
    {
        return 75;
    }
    std::cout << "controlled_fatal\n" << std::flush;
    return 21;
}
}

int main(int argumentCount, char** arguments)
{
    if (argumentCount != 2)
        return 64;
    const std::string_view probeCase = arguments[1];
    if (probeCase == "clean")
        return cleanExit();
    if (probeCase == "metadata-rejected")
        return metadataRejected();
    if (probeCase == "controlled-fatal")
        return controlledFatal();
    if (probeCase == "abnormal-termination")
    {
        std::cout << "abnormal_termination\n" << std::flush;
        std::abort();
    }
    if (probeCase == "abnormal-termination-normal-exit")
    {
        std::cout << "abnormal_normal_exit\n" << std::flush;
        return 0;
    }
    if (probeCase == "bounded-timeout")
    {
        std::cout << "bounded_timeout\n" << std::string(8'192, 'x')
                  << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        return 76;
    }
    return 64;
}
