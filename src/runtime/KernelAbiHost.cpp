#include "talas/runtime/KernelAbiHost.h"
#include "KernelAbiCheckpointValidation.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace talas::runtime
{
namespace
{
[[nodiscard]] KernelAbiRegistrationResult rejected(
    KernelAbiValidationCode code,
    std::string path,
    std::string message)
{
    KernelAbiRegistrationResult result;
    result.diagnostics.push_back({
        code,
        std::move(path),
        std::move(message),
    });
    return result;
}

[[nodiscard]] bool validUtf8(std::string_view value)
{
    std::size_t index = 0;
    while (index < value.size())
    {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7fU)
        {
            if (first < 0x20U || first == 0x7fU)
                return false;
            ++index;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U)
        {
            continuationCount = 1;
            codePoint = first & 0x1fU;
        }
        else if ((first & 0xf0U) == 0xe0U)
        {
            continuationCount = 2;
            codePoint = first & 0x0fU;
        }
        else if ((first & 0xf8U) == 0xf0U)
        {
            continuationCount = 3;
            codePoint = first & 0x07U;
        }
        else
        {
            return false;
        }
        if (index + continuationCount >= value.size())
            return false;
        for (std::size_t continuation = 0;
             continuation < continuationCount;
             ++continuation)
        {
            const auto byte = static_cast<unsigned char>(
                value[index + continuation + 1U]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            codePoint = (codePoint << 6U) | (byte & 0x3fU);
        }
        const auto minimum = continuationCount == 1 ? 0x80U
            : continuationCount == 2                  ? 0x800U
                                                       : 0x10000U;
        if (codePoint < minimum || codePoint > 0x10ffffU
            || (codePoint >= 0xd800U && codePoint <= 0xdfffU))
        {
            return false;
        }
        index += continuationCount + 1U;
    }
    return true;
}

[[nodiscard]] std::optional<std::string> copiedText(
    TalasKernelAbiStringViewV1 view,
    std::size_t maximumBytes)
{
    if (view.data == nullptr || view.byteCount == 0
        || view.byteCount > maximumBytes
        || view.byteCount
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()))
    {
        return std::nullopt;
    }
    std::string value(
        view.data,
        static_cast<std::size_t>(view.byteCount));
    if (!validUtf8(value))
        return std::nullopt;
    return value;
}

[[nodiscard]] TalasKernelAbiCapabilityFlagsV1 descriptorCapabilities(
    const talas::core::NodeTypeDescriptor& descriptor)
{
    TalasKernelAbiCapabilityFlagsV1 result = 0;
    if (descriptor.capabilities.supportsReset)
        result |= TALAS_KERNEL_ABI_CAPABILITY_RESET;
    if (descriptor.capabilities.state
        == talas::core::NodeStateModel::SessionStateful)
    {
        result |= TALAS_KERNEL_ABI_CAPABILITY_STATEFUL;
    }
    if (descriptor.capabilities.transportAware)
        result |= TALAS_KERNEL_ABI_CAPABILITY_TRANSPORT;
    if (descriptor.capabilities.typedEvents)
        result |= TALAS_KERNEL_ABI_CAPABILITY_TYPED_EVENTS;
    return result;
}

[[nodiscard]] bool validHostServices(
    const TalasKernelAbiHostServicesV1& services)
{
    return services.structSize >= sizeof(TalasKernelAbiHostServicesV1)
        && services.servicesVersion
            == TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V1
        && services.allocate != nullptr
        && services.deallocate != nullptr
        && services.diagnostic != nullptr
        && services.reserved0 == 0
        && services.reserved1 == 0;
}

[[nodiscard]] bool sameCompatibility(
    const TalasKernelAbiCompatibilityV1& candidate,
    const TalasKernelAbiCompatibilityV1& expected)
{
    return candidate.structSize == sizeof(TalasKernelAbiCompatibilityV1)
        && candidate.abiVersion == expected.abiVersion
        && candidate.pointerBits == expected.pointerBits
        && candidate.littleEndian == expected.littleEndian
        && candidate.doubleBits == expected.doubleBits
        && candidate.doubleIec559 == expected.doubleIec559
        && candidate.maximumAlignment == expected.maximumAlignment;
}

[[nodiscard]] bool knownStatus(TalasKernelAbiStatusV1 status) noexcept
{
    return status <= TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
}

[[nodiscard]] TalasKernelAbiStatusV1 normalizedStatus(
    TalasKernelAbiStatusV1 status) noexcept
{
    return knownStatus(status)
        ? status
        : TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
}

[[nodiscard]] bool validOptionalText(
    TalasKernelAbiStringViewV1 view,
    std::size_t maximumBytes)
{
    if (view.byteCount == 0)
        return true;
    if (view.data == nullptr || view.byteCount > maximumBytes
        || view.byteCount
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()))
    {
        return false;
    }
    return validUtf8(std::string_view(
        view.data,
        static_cast<std::size_t>(view.byteCount)));
}

[[nodiscard]] bool validTransportRecord(
    const TalasKernelAbiTransportV1& transport,
    double sampleRate) noexcept
{
    return transport.structSize == sizeof(TalasKernelAbiTransportV1)
        && transport.reserved0 == 0 && transport.reserved1 == 0
        && transport.playing <= 1 && transport.loopEnabled <= 1
        && std::isfinite(transport.sampleRate)
        && transport.sampleRate == sampleRate
        && (!transport.loopEnabled
            || transport.loopStartFrame < transport.loopEndFrame);
}

[[nodiscard]] bool validPrepareRecord(
    const TalasKernelAbiPrepareV1& prepare,
    const KernelAbiHostPolicy& policy)
{
    if (prepare.structSize < sizeof(TalasKernelAbiPrepareV1)
        || prepare.apiVersion != TALAS_KERNEL_API_VERSION_V1
        || !std::isfinite(prepare.sampleRate) || prepare.sampleRate <= 0.0
        || prepare.maximumBlockFrames == 0
        || prepare.reserved0 != 0 || prepare.reserved1 != 0
        || prepare.parameterCount > policy.maximumPrepareParameters
        || (prepare.parameterCount != 0 && prepare.parameters == nullptr))
    {
        return false;
    }
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(prepare.parameterCount);
         ++index)
    {
        const auto& parameter = prepare.parameters[index];
        if (parameter.structSize != sizeof(TalasKernelAbiParameterV1)
            || parameter.reserved0 != 0
            || parameter.kind < TALAS_KERNEL_ABI_PARAMETER_SCALAR
            || parameter.kind > TALAS_KERNEL_ABI_PARAMETER_TEXT
            || !copiedText(
                    parameter.id,
                    policy.maximumIdentityBytes).has_value())
        {
            return false;
        }
        if (parameter.kind == TALAS_KERNEL_ABI_PARAMETER_SCALAR
            && !std::isfinite(parameter.scalarValue))
        {
            return false;
        }
        if (parameter.kind == TALAS_KERNEL_ABI_PARAMETER_TEXT
            && !validOptionalText(
                parameter.textValue,
                policy.maximumIdentityBytes))
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validResetRecord(
    const TalasKernelAbiResetV1& resetRecord,
    double sampleRate) noexcept
{
    return resetRecord.structSize >= sizeof(TalasKernelAbiResetV1)
        && resetRecord.reason >= TALAS_KERNEL_ABI_RESET_EXPLICIT
        && resetRecord.reason <= TALAS_KERNEL_ABI_RESET_GRAPH_ACTIVATION
        && resetRecord.reserved0 == 0
        && validTransportRecord(resetRecord.transport, sampleRate);
}

[[nodiscard]] bool validProcessRecord(
    const TalasKernelAbiProcessV1& processRecord,
    double sampleRate,
    std::uint64_t maximumBlockFrames,
    const KernelAbiHostPolicy& policy) noexcept
{
    if (processRecord.structSize < sizeof(TalasKernelAbiProcessV1)
        || (processRecord.flags & ~TALAS_KERNEL_ABI_PROCESS_KNOWN_MASK) != 0
        || processRecord.reserved0 != 0
        || processRecord.reserved1 != 0 || processRecord.frameCount == 0
        || processRecord.frameCount > maximumBlockFrames
        || processRecord.inputCount > policy.maximumProcessSignals
        || processRecord.outputCount > policy.maximumProcessSignals
        || processRecord.eventCount > policy.maximumProcessEvents
        || processRecord.eventCount
            > TALAS_KERNEL_ABI_MAX_PROCESS_EVENTS_V1
        || (processRecord.inputCount != 0
            && processRecord.inputs == nullptr)
        || processRecord.outputCount == 0
        || processRecord.outputs == nullptr
        || (processRecord.eventCount != 0
            && processRecord.events == nullptr)
        || !validTransportRecord(processRecord.transport, sampleRate))
    {
        return false;
    }

    for (std::size_t index = 0;
         index < static_cast<std::size_t>(processRecord.inputCount);
         ++index)
    {
        const auto& signal = processRecord.inputs[index];
        if (signal.structSize != sizeof(TalasKernelAbiSignalV1)
            || signal.reserved0 != 0
            || (signal.kind != TALAS_KERNEL_ABI_SIGNAL_CONSTANT
                && signal.kind != TALAS_KERNEL_ABI_SIGNAL_PER_FRAME))
        {
            return false;
        }
        if (signal.kind == TALAS_KERNEL_ABI_SIGNAL_CONSTANT)
        {
            if (signal.samples != nullptr || signal.sampleCount != 0)
            {
                return false;
            }
        }
        else if (signal.samples == nullptr
            || signal.sampleCount != processRecord.frameCount)
        {
            return false;
        }
    }

    for (std::size_t index = 0;
         index < static_cast<std::size_t>(processRecord.outputCount);
         ++index)
    {
        const auto& output = processRecord.outputs[index];
        if (output.structSize != sizeof(TalasKernelAbiOutputV1)
            || output.reserved0 != 0 || output.reserved1 != 0
            || output.samples == nullptr
            || output.sampleCount != processRecord.frameCount)
        {
            return false;
        }
    }

    for (std::size_t index = 0;
         index < static_cast<std::size_t>(processRecord.eventCount);
         ++index)
    {
        const auto& event = processRecord.events[index];
        if (event.structSize != sizeof(TalasKernelAbiEventV1)
            || event.kind != TALAS_KERNEL_ABI_EVENT_GATE
            || event.reserved0 != 0 || event.reserved1 != 0
            || (event.payloadBits
                & ~TALAS_KERNEL_ABI_GATE_EVENT_HIGH) != 0
            || event.frameOffset >= processRecord.frameCount)
        {
            return false;
        }
    }
    return true;
}

void destroyOpaque(
    const TalasKernelAbiDescriptorV1& descriptor,
    void*& opaqueInstance,
    const TalasKernelAbiHostServicesV1& services) noexcept
{
    if (opaqueInstance == nullptr)
        return;
    auto* instance = opaqueInstance;
    opaqueInstance = nullptr;
    try
    {
        descriptor.destroy(instance, &services);
    }
    catch (...)
    {
    }
}
}

const KernelAbiModuleMetadata&
RegisteredKernelAbiModule::metadata() const noexcept
{
    return metadata_;
}

const std::vector<KernelAbiKernelMetadata>&
RegisteredKernelAbiModule::kernels() const noexcept
{
    return kernels_;
}

bool RegisteredKernelAbiModule::isRetiring() const noexcept
{
    return retiring_.load(std::memory_order_acquire);
}

std::size_t RegisteredKernelAbiModule::activeInstanceCount() const noexcept
{
    return activeInstances_.load(std::memory_order_acquire);
}

bool RegisteredKernelAbiModule::tryAcquireLease() const noexcept
{
    if (retiring_.load(std::memory_order_acquire))
        return false;
    activeInstances_.fetch_add(1, std::memory_order_acq_rel);
    if (retiring_.load(std::memory_order_acquire))
    {
        activeInstances_.fetch_sub(1, std::memory_order_acq_rel);
        return false;
    }
    return true;
}

void RegisteredKernelAbiModule::releaseLease() const noexcept
{
    activeInstances_.fetch_sub(1, std::memory_order_acq_rel);
}

TalasKernelAbiCompatibilityV1 currentKernelAbiCompatibility() noexcept
{
    TalasKernelAbiCompatibilityV1 result{};
    result.structSize = sizeof(result);
    result.abiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    result.pointerBits = sizeof(void*) * 8U;
    result.littleEndian =
        std::endian::native == std::endian::little ? 1U : 0U;
    result.doubleBits = sizeof(double) * 8U;
    result.doubleIec559 =
        std::numeric_limits<double>::is_iec559 ? 1U : 0U;
    result.maximumAlignment = alignof(std::max_align_t);
    return result;
}

KernelAbiInstance::KernelAbiInstance(
    std::shared_ptr<const RegisteredKernelAbiModule> module,
    TalasKernelAbiDescriptorV1 descriptor,
    TalasKernelAbiCheckpointDescriptorV1 checkpointDescriptor,
    void* opaqueInstance,
    std::uint64_t generation,
    double sampleRate,
    std::uint64_t maximumBlockFrames,
    KernelAbiHostPolicy policy) noexcept
    : module_(std::move(module)),
      descriptor_(descriptor),
      checkpointDescriptor_(checkpointDescriptor),
      opaqueInstance_(opaqueInstance),
      generation_(generation),
      sampleRate_(sampleRate),
      maximumBlockFrames_(maximumBlockFrames),
      policy_(policy)
{
}

KernelAbiInstance::~KernelAbiInstance() noexcept
{
    static_cast<void>(destroyOfflineCheckpoint());
    destroyOpaque(
        descriptor_,
        opaqueInstance_,
        module_->hostServices_);
    module_->releaseLease();
}

void KernelAbiInstanceDeleter::operator()(
    KernelAbiInstance* instance) const noexcept
{
    delete instance;
}

TalasKernelAbiStatusV1 KernelAbiInstance::reset(
    const TalasKernelAbiResetV1& resetRecord) noexcept
{
    if (quarantined_)
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    if ((descriptor_.capabilityFlags & TALAS_KERNEL_ABI_CAPABILITY_RESET) == 0
        || !validResetRecord(resetRecord, sampleRate_))
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    }
    try
    {
        const auto status = normalizedStatus(
            descriptor_.reset(opaqueInstance_, &resetRecord));
        if (status == TALAS_KERNEL_ABI_STATUS_FATAL
            || status == TALAS_KERNEL_ABI_STATUS_QUARANTINED
            || status == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR)
        {
            quarantined_ = true;
        }
        return status;
    }
    catch (...)
    {
        quarantined_ = true;
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
}

TalasKernelAbiStatusV1 KernelAbiInstance::process(
    const TalasKernelAbiProcessV1& processRecord) noexcept
{
    if (quarantined_)
        return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    if (!validProcessRecord(
            processRecord,
            sampleRate_,
            maximumBlockFrames_,
            policy_))
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    }
    try
    {
        const auto status = normalizedStatus(
            descriptor_.process(opaqueInstance_, &processRecord));
        if (status == TALAS_KERNEL_ABI_STATUS_FATAL
            || status == TALAS_KERNEL_ABI_STATUS_QUARANTINED
            || status == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR)
        {
            quarantined_ = true;
        }
        return status;
    }
    catch (...)
    {
        quarantined_ = true;
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
}

bool KernelAbiInstance::isQuarantined() const noexcept
{
    return quarantined_;
}

bool KernelAbiInstance::supportsOfflineCheckpoint() const noexcept
{
    return checkpointDescriptor_.capture != nullptr;
}

bool KernelAbiInstance::destroyOfflineCheckpoint() noexcept
{
    auto* checkpoint = std::exchange(offlineCheckpoint_, nullptr);
    if (checkpoint == nullptr) return true;
    try
    {
        checkpointDescriptor_.destroyCheckpoint(checkpoint, &module_->hostServices_);
        return true;
    }
    catch (...)
    {
        quarantined_ = true;
        return false;
    }
}

TalasKernelAbiStatusV1 KernelAbiInstance::beginOfflineCheckpoint() noexcept
{
    if (quarantined_) return TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    if (!supportsOfflineCheckpoint()) return TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE;
    if (offlineCheckpoint_ != nullptr) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    TalasKernelAbiStatusV1 status = TALAS_KERNEL_ABI_STATUS_FATAL;
    try
    {
        const auto raw = checkpointDescriptor_.capture(
            opaqueInstance_, &module_->hostServices_, &offlineCheckpoint_);
        status = raw == normalizedStatus(raw) ? raw : TALAS_KERNEL_ABI_STATUS_FATAL;
    }
    catch (...)
    {
        status = TALAS_KERNEL_ABI_STATUS_FATAL;
    }
    if (status == TALAS_KERNEL_ABI_STATUS_OK && offlineCheckpoint_ != nullptr)
        return status;
    if (status == TALAS_KERNEL_ABI_STATUS_OK)
        status = TALAS_KERNEL_ABI_STATUS_FATAL;
    if (!destroyOfflineCheckpoint()) status = TALAS_KERNEL_ABI_STATUS_FATAL;
    if (status == TALAS_KERNEL_ABI_STATUS_FATAL || status == TALAS_KERNEL_ABI_STATUS_QUARANTINED)
        quarantined_ = true;
    // INTERNAL_ERROR can report a failed capture allocation. Unlike a failed
    // processing callback, capture must have preserved the original DSP state.
    return status;
}

TalasKernelAbiStatusV1 KernelAbiInstance::restoreOfflineCheckpoint() noexcept
{
    if (offlineCheckpoint_ == nullptr) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    auto status = TALAS_KERNEL_ABI_STATUS_QUARANTINED;
    if (!quarantined_)
    {
        try
        {
            status = normalizedStatus(checkpointDescriptor_.restore(
                opaqueInstance_, offlineCheckpoint_));
        }
        catch (...)
        {
            status = TALAS_KERNEL_ABI_STATUS_FATAL;
        }
        if (status != TALAS_KERNEL_ABI_STATUS_OK)
        {
            quarantined_ = true;
            status = TALAS_KERNEL_ABI_STATUS_FATAL;
        }
    }
    if (!destroyOfflineCheckpoint()) status = TALAS_KERNEL_ABI_STATUS_FATAL;
    return status;
}

TalasKernelAbiStatusV1 KernelAbiInstance::commitOfflineCheckpoint() noexcept
{
    if (offlineCheckpoint_ == nullptr) return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    if (!destroyOfflineCheckpoint()) return TALAS_KERNEL_ABI_STATUS_FATAL;
    return quarantined_ ? TALAS_KERNEL_ABI_STATUS_QUARANTINED : TALAS_KERNEL_ABI_STATUS_OK;
}

KernelAbiHost::KernelAbiHost(
    talas::core::NodeRegistry registry,
    TalasKernelAbiHostServicesV1 hostServices,
    KernelAbiHostPolicy policy)
    : registry_(std::move(registry)),
      hostServices_(hostServices),
      policy_(policy)
{
    if (hostServices_.structSize >= sizeof(TalasKernelAbiHostServicesV1))
        hostServices_.structSize = sizeof(TalasKernelAbiHostServicesV1);
    retiringModules_.reserve(policy_.maximumRetiringModules);
    retiredInstances_.reserve(policy_.maximumRetiredInstances);
}

KernelAbiHost::~KernelAbiHost() noexcept
{
    retiredInstances_.clear();
    activeInstancesOwned_.clear();
    for (auto& [moduleId, module] : modules_)
    {
        static_cast<void>(moduleId);
        unloadRegisteredModule(*module);
    }
    retiringModules_.clear();
    kernelOwners_.clear();
    modules_.clear();
}

void KernelAbiHost::unloadRegisteredModule(
    RegisteredKernelAbiModule& module) noexcept
{
    if (module.unloaded_.exchange(true, std::memory_order_acq_rel))
        return;
    try
    {
        module.unload_(
            module.moduleContext_,
            &module.hostServices_);
    }
    catch (...)
    {
    }
}

KernelAbiRegistrationResult KernelAbiHost::registerModule(
    const TalasKernelAbiModuleV1& module,
    const TalasKernelAbiCheckpointExtensionV1* checkpoints)
{
    if (!validHostServices(hostServices_))
    {
        return rejected(
            KernelAbiValidationCode::InvalidHostServices,
            "hostServices",
            "Host services are incomplete or incompatible");
    }
    if (module.structSize < sizeof(TalasKernelAbiModuleV1))
    {
        return rejected(
            KernelAbiValidationCode::InvalidStructure,
            "module.structSize",
            "Module record is smaller than the ABI v1 required prefix");
    }
    if (module.descriptorVersion
        != TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1)
    {
        return rejected(
            KernelAbiValidationCode::UnsupportedVersion,
            "module.descriptorVersion",
            "Unsupported module descriptor version");
    }
    if (module.minimumApiVersion > TALAS_KERNEL_API_VERSION_V1
        || module.maximumApiVersion < TALAS_KERNEL_API_VERSION_V1
        || module.minimumApiVersion > module.maximumApiVersion)
    {
        return rejected(
            KernelAbiValidationCode::UnsupportedVersion,
            "module.apiRange",
            "The host API version is outside the module range");
    }
    if (module.compatibility.structSize
        != sizeof(TalasKernelAbiCompatibilityV1))
    {
        return rejected(
            KernelAbiValidationCode::InvalidStructure,
            "module.compatibility.structSize",
            "Embedded compatibility record must use the exact ABI v1 layout");
    }
    if (module.reserved0 != 0 || module.reserved1 != 0
        || module.reserved2 != 0
        || module.compatibility.reserved0 != 0)
    {
        return rejected(
            KernelAbiValidationCode::InvalidReservedField,
            "module.reserved",
            "ABI v1 reserved fields must be zero");
    }
    if (!sameCompatibility(
            module.compatibility,
            currentKernelAbiCompatibility()))
    {
        return rejected(
            KernelAbiValidationCode::IncompatiblePlatform,
            "module.compatibility",
            "The module ABI/platform tag is incompatible with this host");
    }
    if (module.sampleFormats != TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64)
    {
        return rejected(
            KernelAbiValidationCode::UnsupportedSampleFormat,
            "module.sampleFormats",
            "ABI v1 requires exactly native f64 sample support");
    }
    const auto trustAccepted =
        module.trustClass == TALAS_KERNEL_ABI_TRUST_BUILTIN
        || (module.trustClass == TALAS_KERNEL_ABI_TRUST_OWNER_APPROVED_TEST
            && policy_.allowOwnerApprovedTestModules);
    if (!trustAccepted)
    {
        return rejected(
            KernelAbiValidationCode::UnsupportedTrust,
            "module.trustClass",
            "The module trust class is not accepted by this host policy");
    }
    if ((module.capabilityFlags
         & ~TALAS_KERNEL_ABI_CAPABILITY_KNOWN_MASK)
        != 0)
    {
        return rejected(
            KernelAbiValidationCode::CapabilityMismatch,
            "module.capabilityFlags",
            "Module declares an unknown capability bit");
    }
    if (module.unload == nullptr)
    {
        return rejected(
            KernelAbiValidationCode::MissingCallback,
            "module.unload",
            "Module unload callback is required");
    }
    const auto moduleId = copiedText(
        module.moduleId,
        policy_.maximumIdentityBytes);
    const auto moduleVersion = copiedText(
        module.moduleVersion,
        policy_.maximumIdentityBytes);
    if (!moduleId.has_value() || !moduleVersion.has_value())
    {
        return rejected(
            KernelAbiValidationCode::InvalidMetadata,
            "module.identity",
            "Module identity/version must be bounded valid UTF-8");
    }
    if (modules_.contains(*moduleId))
    {
        return rejected(
            KernelAbiValidationCode::DuplicateIdentity,
            "module.moduleId",
            "Module identity is already registered");
    }
    if (module.kernels == nullptr || module.kernelCount == 0
        || module.kernelCount > policy_.maximumModuleKernels
        || module.kernelCount
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max()))
    {
        return rejected(
            KernelAbiValidationCode::InvalidStructure,
            "module.kernels",
            "Module kernel table is empty or exceeds the supported bound");
    }

    auto registered = std::make_shared<RegisteredKernelAbiModule>();
    registered->metadata_ = {
        *moduleId,
        *moduleVersion,
        module.trustClass,
        module.capabilityFlags,
    };
    registered->hostServices_ = hostServices_;
    registered->moduleContext_ = module.moduleContext;
    registered->unload_ = module.unload;
    registered->kernels_.reserve(
        static_cast<std::size_t>(module.kernelCount));
    registered->lifecycleDescriptors_.reserve(
        static_cast<std::size_t>(module.kernelCount));

    std::unordered_set<std::string> localTypeIds;
    TalasKernelAbiCapabilityFlagsV1 combinedCapabilities = 0;
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(module.kernelCount);
         ++index)
    {
        const auto& kernel = module.kernels[index];
        const auto path = "module.kernels[" + std::to_string(index) + "]";
        if (kernel.structSize != sizeof(TalasKernelAbiDescriptorV1))
        {
            return rejected(
                KernelAbiValidationCode::InvalidStructure,
                path + ".structSize",
                "Repeated kernel record must use the exact ABI v1 stride");
        }
        if (kernel.descriptorVersion
            != TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1)
        {
            return rejected(
                KernelAbiValidationCode::UnsupportedVersion,
                path + ".descriptorVersion",
                "Unsupported kernel descriptor version");
        }
        if (kernel.reserved0 != 0 || kernel.reserved1 != 0)
        {
            return rejected(
                KernelAbiValidationCode::InvalidReservedField,
                path + ".reserved",
                "Kernel reserved fields must be zero");
        }
        if (kernel.create == nullptr || kernel.prepare == nullptr
            || kernel.reset == nullptr || kernel.process == nullptr
            || kernel.destroy == nullptr)
        {
            return rejected(
                KernelAbiValidationCode::MissingCallback,
                path + ".callbacks",
                "Kernel lifecycle callbacks are incomplete");
        }
        if ((kernel.capabilityFlags
             & ~TALAS_KERNEL_ABI_CAPABILITY_KNOWN_MASK)
            != 0)
        {
            return rejected(
                KernelAbiValidationCode::CapabilityMismatch,
                path + ".capabilityFlags",
                "Kernel declares an unknown capability bit");
        }
        const auto typeId = copiedText(
            kernel.typeId,
            policy_.maximumIdentityBytes);
        if (!typeId.has_value())
        {
            return rejected(
                KernelAbiValidationCode::InvalidMetadata,
                path + ".typeId",
                "Kernel type identity must be bounded valid UTF-8");
        }
        if (!localTypeIds.insert(*typeId).second
            || kernelOwners_.contains(*typeId))
        {
            return rejected(
                KernelAbiValidationCode::DuplicateIdentity,
                path + ".typeId",
                "Kernel type identity is duplicated");
        }
        const auto* descriptor = registry_.findNodeType(*typeId);
        if (descriptor == nullptr)
        {
            return rejected(
                KernelAbiValidationCode::UnknownNodeType,
                path + ".typeId",
                "Kernel type is absent from NodeRegistry authority");
        }
        if (descriptor->capabilities.offlineBinding
                    != talas::core::ExecutionBinding::Kernel
                && descriptor->capabilities.preparedRealtimeBinding
                    != talas::core::ExecutionBinding::Kernel)
        {
            return rejected(
                KernelAbiValidationCode::CapabilityMismatch,
                path + ".capabilityFlags",
                "NodeRegistry does not declare a kernel execution binding");
        }
        if (kernel.capabilityFlags
            != descriptorCapabilities(*descriptor))
        {
            return rejected(
                KernelAbiValidationCode::CapabilityMismatch,
                path + ".capabilityFlags",
                "Kernel capabilities differ from NodeRegistry authority");
        }
        combinedCapabilities |= kernel.capabilityFlags;
        auto descriptorCopy = kernel;
        descriptorCopy.typeId = {};
        registered->kernels_.push_back({
            *typeId,
            kernel.capabilityFlags,
        });
        registered->lifecycleDescriptors_.push_back(descriptorCopy);
    }
    if (module.capabilityFlags != combinedCapabilities)
    {
        return rejected(
            KernelAbiValidationCode::CapabilityMismatch,
            "module.capabilityFlags",
            "Module capabilities must equal the union of kernel capabilities");
    }

    std::vector<std::string_view> checkpointTypes;
    checkpointTypes.reserve(registered->kernels_.size());
    for (const auto& kernel : registered->kernels_) checkpointTypes.push_back(kernel.typeId);
    auto checked = kernel_abi_checkpoint::validateAndCopy(checkpoints, *moduleId, *moduleVersion,
        checkpointTypes, policy_.maximumIdentityBytes);
    if (!checked.ok())
    {
        using Code = kernel_abi_checkpoint::ValidationCode;
        auto code = KernelAbiValidationCode::InvalidStructure;
        switch (checked.code)
        {
        case Code::Valid: break;
        case Code::InvalidStructure: code = KernelAbiValidationCode::InvalidStructure; break;
        case Code::UnsupportedVersion: code = KernelAbiValidationCode::UnsupportedVersion; break;
        case Code::InvalidReservedField: code = KernelAbiValidationCode::InvalidReservedField; break;
        case Code::InvalidMetadata: code = KernelAbiValidationCode::InvalidMetadata; break;
        case Code::MissingCallback: code = KernelAbiValidationCode::MissingCallback; break;
        case Code::UnknownNodeType: code = KernelAbiValidationCode::UnknownNodeType; break;
        case Code::DuplicateIdentity: code = KernelAbiValidationCode::DuplicateIdentity; break;
        }
        return rejected(code, std::move(checked.path), "Checkpoint extension failed identity/layout/callback validation");
    }
    registered->checkpointDescriptors_ = std::move(checked.descriptors);

    // Build both publication indexes off to the side. Any allocation failure
    // leaves the live host maps untouched; the two noexcept swaps are the only
    // publication point.
    auto stagedModules = modules_;
    auto stagedKernelOwners = kernelOwners_;
    const auto [modulePosition, moduleInserted] =
        stagedModules.emplace(*moduleId, registered);
    static_cast<void>(modulePosition);
    if (!moduleInserted)
    {
        return rejected(
            KernelAbiValidationCode::DuplicateIdentity,
            "module.moduleId",
            "Module identity became unavailable before publication");
    }
    for (const auto& kernel : registered->kernels_)
    {
        const auto [ownerPosition, ownerInserted] =
            stagedKernelOwners.emplace(kernel.typeId, *moduleId);
        static_cast<void>(ownerPosition);
        if (!ownerInserted)
        {
            return rejected(
                KernelAbiValidationCode::DuplicateIdentity,
                "module.kernels.typeId",
                "Kernel identity became unavailable before publication");
        }
    }
    modules_.swap(stagedModules);
    kernelOwners_.swap(stagedKernelOwners);
    return {registered, {}};
}

std::shared_ptr<const RegisteredKernelAbiModule>
KernelAbiHost::findModule(std::string_view moduleId) const
{
    const auto found = modules_.find(std::string(moduleId));
    return found == modules_.end() ? nullptr : found->second;
}

KernelAbiInstanceResult KernelAbiHost::createPreparedInstance(
    std::string_view moduleId,
    std::string_view typeId,
    const TalasKernelAbiPrepareV1& prepareRecord)
{
    const auto module = findModule(moduleId);
    if (module == nullptr)
        return {nullptr, TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT};
    const auto kernel = std::find_if(
        module->kernels_.begin(),
        module->kernels_.end(),
        [typeId](const auto& candidate) {
            return candidate.typeId == typeId;
        });
    if (kernel == module->kernels_.end())
        return {nullptr, TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT};
    const auto& lifecycle = module->lifecycleDescriptors_[
        static_cast<std::size_t>(
            std::distance(module->kernels_.begin(), kernel))];
    if (!validPrepareRecord(prepareRecord, policy_))
        return {nullptr, TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION};
    if (nextInstanceGeneration_ == 0)
        return {nullptr, TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
    if (!module->tryAcquireLease())
        return {nullptr, TALAS_KERNEL_ABI_STATUS_RETIRING};

    void* opaqueInstance = nullptr;
    TalasKernelAbiStatusV1 createStatus = TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    try
    {
        createStatus = normalizedStatus(lifecycle.create(
            &module->hostServices_,
            &opaqueInstance));
    }
    catch (...)
    {
        createStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
    }
    if (createStatus != TALAS_KERNEL_ABI_STATUS_OK)
    {
        destroyOpaque(
            lifecycle,
            opaqueInstance,
            module->hostServices_);
        module->releaseLease();
        return {nullptr, createStatus};
    }
    if (opaqueInstance == nullptr)
    {
        module->releaseLease();
        return {nullptr, TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
    }

    auto prepared = prepareRecord;
    prepared.structSize = sizeof(TalasKernelAbiPrepareV1);
    prepared.host = &module->hostServices_;
    TalasKernelAbiStatusV1 prepareStatus = TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    try
    {
        prepareStatus = normalizedStatus(
            lifecycle.prepare(opaqueInstance, &prepared));
    }
    catch (...)
    {
        prepareStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
    }
    if (prepareStatus != TALAS_KERNEL_ABI_STATUS_OK)
    {
        destroyOpaque(
            lifecycle,
            opaqueInstance,
            module->hostServices_);
        module->releaseLease();
        return {nullptr, prepareStatus};
    }

    try
    {
        const auto generation = nextInstanceGeneration_++;
        auto owned = std::unique_ptr<
            KernelAbiInstance,
            KernelAbiInstanceDeleter>(
            new KernelAbiInstance(
                module,
                lifecycle,
                module->checkpointDescriptors_[static_cast<std::size_t>(
                    std::distance(module->kernels_.begin(), kernel))],
                opaqueInstance,
                generation,
                prepareRecord.sampleRate,
                prepareRecord.maximumBlockFrames,
                policy_));
        opaqueInstance = nullptr;
        auto* instance = owned.get();
        const auto [position, inserted] = activeInstancesOwned_.emplace(
            instance,
            std::move(owned));
        if (!inserted)
            return {nullptr, TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
        return {
            KernelAbiInstanceHandle{position->first, generation},
            TALAS_KERNEL_ABI_STATUS_OK,
        };
    }
    catch (...)
    {
        if (opaqueInstance != nullptr)
        {
            destroyOpaque(
                lifecycle,
                opaqueInstance,
                module->hostServices_);
            module->releaseLease();
        }
        return {nullptr, TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR};
    }
}

KernelAbiRetirementStatus KernelAbiHost::requestModuleRetirement(
    std::string_view moduleId)
{
    const auto found = modules_.find(std::string(moduleId));
    if (found == modules_.end())
        return KernelAbiRetirementStatus::UnknownModule;
    auto& module = found->second;
    if (module->retiring_.load(std::memory_order_acquire))
        return KernelAbiRetirementStatus::AlreadyRetiring;
    if (retiringModules_.size() >= policy_.maximumRetiringModules)
        return KernelAbiRetirementStatus::QueueFull;
    bool expected = false;
    if (!module->retiring_.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return KernelAbiRetirementStatus::AlreadyRetiring;
    }
    retiringModules_.push_back(module);
    return KernelAbiRetirementStatus::Accepted;
}

KernelAbiInstanceRetirementStatus KernelAbiHost::retireInstance(
    KernelAbiInstanceHandle instance) noexcept
{
    const auto found = activeInstancesOwned_.find(instance.get());
    if (found == activeInstancesOwned_.end()
        || found->second->generation_ != instance.generation())
        return KernelAbiInstanceRetirementStatus::UnknownInstance;
    if (retiredInstances_.size() >= policy_.maximumRetiredInstances)
        return KernelAbiInstanceRetirementStatus::QueueFull;
    auto owned = std::move(found->second);
    activeInstancesOwned_.erase(found);
    retiredInstances_.push_back(std::move(owned));
    return KernelAbiInstanceRetirementStatus::Accepted;
}

std::size_t KernelAbiHost::collectRetiredModules() noexcept
{
    retiredInstances_.clear();
    std::size_t collected = 0;
    auto current = retiringModules_.begin();
    while (current != retiringModules_.end())
    {
        const auto& module = *current;
        if (module->activeInstances_.load(std::memory_order_acquire) != 0)
        {
            ++current;
            continue;
        }
        unloadRegisteredModule(*module);
        for (const auto& kernel : module->kernels_)
            kernelOwners_.erase(kernel.typeId);
        modules_.erase(module->metadata_.moduleId);
        current = retiringModules_.erase(current);
        ++collected;
    }
    return collected;
}

std::size_t KernelAbiHost::retiringModuleCount() const noexcept
{
    return retiringModules_.size();
}

std::size_t KernelAbiHost::retiredInstanceCount() const noexcept
{
    return retiredInstances_.size();
}

std::size_t KernelAbiHost::moduleCount() const noexcept
{
    return modules_.size();
}
}
