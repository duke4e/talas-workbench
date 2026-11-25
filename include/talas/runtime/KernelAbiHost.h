#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/runtime/KernelAbi.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace talas::runtime
{
enum class KernelAbiValidationCode
{
    InvalidHostServices,
    InvalidStructure,
    UnsupportedVersion,
    IncompatiblePlatform,
    UnsupportedSampleFormat,
    UnsupportedTrust,
    InvalidReservedField,
    InvalidMetadata,
    DuplicateIdentity,
    MissingCallback,
    UnknownNodeType,
    CapabilityMismatch,
};

struct KernelAbiDiagnostic
{
    KernelAbiValidationCode code =
        KernelAbiValidationCode::InvalidStructure;
    std::string path;
    std::string message;
};

struct KernelAbiKernelMetadata
{
    std::string typeId;
    TalasKernelAbiCapabilityFlagsV1 capabilityFlags = 0;
};

struct KernelAbiModuleMetadata
{
    std::string moduleId;
    std::string moduleVersion;
    TalasKernelAbiTrustClassV1 trustClass = 0;
    TalasKernelAbiCapabilityFlagsV1 capabilityFlags = 0;
};

class RegisteredKernelAbiModule
{
public:
    [[nodiscard]] const KernelAbiModuleMetadata& metadata() const noexcept;
    [[nodiscard]] const std::vector<KernelAbiKernelMetadata>&
    kernels() const noexcept;
    [[nodiscard]] bool isRetiring() const noexcept;
    [[nodiscard]] std::size_t activeInstanceCount() const noexcept;

private:
    friend class KernelAbiHost;
    friend class KernelAbiInstance;

    [[nodiscard]] bool tryAcquireLease() const noexcept;
    void releaseLease() const noexcept;

    KernelAbiModuleMetadata metadata_;
    std::vector<KernelAbiKernelMetadata> kernels_;
    std::vector<TalasKernelAbiDescriptorV1> lifecycleDescriptors_;
    std::vector<TalasKernelAbiCheckpointDescriptorV1> checkpointDescriptors_;
    TalasKernelAbiHostServicesV1 hostServices_{};
    void* moduleContext_ = nullptr;
    TalasKernelAbiModuleUnloadV1 unload_ = nullptr;
    mutable std::atomic<std::size_t> activeInstances_{0};
    mutable std::atomic<bool> retiring_{false};
    mutable std::atomic<bool> unloaded_{false};
};

struct KernelAbiRegistrationResult
{
    std::shared_ptr<const RegisteredKernelAbiModule> module;
    std::vector<KernelAbiDiagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept
    {
        return module != nullptr && diagnostics.empty();
    }
};

struct KernelAbiHostPolicy
{
    bool allowOwnerApprovedTestModules = false;
    std::size_t maximumIdentityBytes = 256;
    std::size_t maximumModuleKernels = 256;
    std::size_t maximumPrepareParameters = 1'024;
    std::size_t maximumProcessSignals = 1'024;
    std::size_t maximumProcessEvents =
        TALAS_KERNEL_ABI_MAX_PROCESS_EVENTS_V1;
    std::size_t maximumRetiringModules = 64;
    std::size_t maximumRetiredInstances = 4'096;
};

enum class KernelAbiRetirementStatus
{
    Accepted,
    AlreadyRetiring,
    UnknownModule,
    QueueFull,
};

enum class KernelAbiInstanceRetirementStatus
{
    Accepted,
    UnknownInstance,
    QueueFull,
};

[[nodiscard]] TalasKernelAbiCompatibilityV1
currentKernelAbiCompatibility() noexcept;

class KernelAbiInstance;

struct KernelAbiInstanceDeleter
{
    void operator()(KernelAbiInstance* instance) const noexcept;
};

class KernelAbiInstance
{
public:
    KernelAbiInstance(const KernelAbiInstance&) = delete;
    KernelAbiInstance& operator=(const KernelAbiInstance&) = delete;
    KernelAbiInstance(KernelAbiInstance&&) = delete;
    KernelAbiInstance& operator=(KernelAbiInstance&&) = delete;

    [[nodiscard]] TalasKernelAbiStatusV1 reset(
        const TalasKernelAbiResetV1& resetRecord) noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 process(
        const TalasKernelAbiProcessV1& processRecord) noexcept;
    [[nodiscard]] bool isQuarantined() const noexcept;
    // Exclusive offline/control-thread operations. One baseline belongs to the
    // instance until restore/commit or destruction; no lifetime escapes it.
    [[nodiscard]] bool supportsOfflineCheckpoint() const noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 beginOfflineCheckpoint() noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 restoreOfflineCheckpoint() noexcept;
    [[nodiscard]] TalasKernelAbiStatusV1 commitOfflineCheckpoint() noexcept;

private:
    friend class KernelAbiHost;
    friend struct KernelAbiInstanceDeleter;

    ~KernelAbiInstance() noexcept;

    KernelAbiInstance(
        std::shared_ptr<const RegisteredKernelAbiModule> module,
        TalasKernelAbiDescriptorV1 descriptor,
        TalasKernelAbiCheckpointDescriptorV1 checkpointDescriptor,
        void* opaqueInstance,
        std::uint64_t generation,
        double sampleRate,
        std::uint64_t maximumBlockFrames,
        KernelAbiHostPolicy policy) noexcept;

    std::shared_ptr<const RegisteredKernelAbiModule> module_;
    TalasKernelAbiDescriptorV1 descriptor_{};
    TalasKernelAbiCheckpointDescriptorV1 checkpointDescriptor_{};
    void* offlineCheckpoint_ = nullptr;
    [[nodiscard]] bool destroyOfflineCheckpoint() noexcept;
    void* opaqueInstance_ = nullptr;
    std::uint64_t generation_ = 0;
    double sampleRate_ = 0.0;
    std::uint64_t maximumBlockFrames_ = 0;
    KernelAbiHostPolicy policy_;
    bool quarantined_ = false;
};

class KernelAbiInstanceHandle
{
public:
    constexpr KernelAbiInstanceHandle() noexcept = default;
    constexpr KernelAbiInstanceHandle(std::nullptr_t) noexcept
    {
    }
    constexpr KernelAbiInstanceHandle(
        KernelAbiInstance* instance,
        std::uint64_t generation) noexcept
        : instance_(instance), generation_(generation)
    {
    }

    [[nodiscard]] constexpr KernelAbiInstance* get() const noexcept
    {
        return instance_;
    }

    [[nodiscard]] constexpr std::uint64_t generation() const noexcept
    {
        return generation_;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept
    {
        return instance_ != nullptr && generation_ != 0;
    }

    [[nodiscard]] constexpr KernelAbiInstance* operator->() const noexcept
    {
        return instance_;
    }

private:
    KernelAbiInstance* instance_ = nullptr;
    std::uint64_t generation_ = 0;
};

struct KernelAbiInstanceResult
{
    KernelAbiInstanceHandle instance;
    TalasKernelAbiStatusV1 status = TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;

    [[nodiscard]] bool ok() const noexcept
    {
        return static_cast<bool>(instance)
            && status == TALAS_KERNEL_ABI_STATUS_OK;
    }
};

class KernelAbiHost
{
public:
    KernelAbiHost(
        talas::core::NodeRegistry registry,
        TalasKernelAbiHostServicesV1 hostServices,
        KernelAbiHostPolicy policy = {});
    ~KernelAbiHost() noexcept;

    [[nodiscard]] KernelAbiRegistrationResult registerModule(
        const TalasKernelAbiModuleV1& module,
        const TalasKernelAbiCheckpointExtensionV1* checkpoints = nullptr);
    [[nodiscard]] std::shared_ptr<const RegisteredKernelAbiModule>
    findModule(std::string_view moduleId) const;
    [[nodiscard]] KernelAbiInstanceResult createPreparedInstance(
        std::string_view moduleId,
        std::string_view typeId,
        const TalasKernelAbiPrepareV1& prepareRecord);
    [[nodiscard]] KernelAbiRetirementStatus requestModuleRetirement(
        std::string_view moduleId);
    [[nodiscard]] KernelAbiInstanceRetirementStatus retireInstance(
        KernelAbiInstanceHandle instance) noexcept;
    [[nodiscard]] std::size_t collectRetiredModules() noexcept;
    [[nodiscard]] std::size_t retiringModuleCount() const noexcept;
    [[nodiscard]] std::size_t retiredInstanceCount() const noexcept;
    [[nodiscard]] std::size_t moduleCount() const noexcept;

private:
    static void unloadRegisteredModule(
        RegisteredKernelAbiModule& module) noexcept;

    talas::core::NodeRegistry registry_;
    TalasKernelAbiHostServicesV1 hostServices_{};
    KernelAbiHostPolicy policy_;
    std::unordered_map<
        std::string,
        std::shared_ptr<RegisteredKernelAbiModule>> modules_;
    std::unordered_map<std::string, std::string> kernelOwners_;
    std::vector<std::shared_ptr<RegisteredKernelAbiModule>>
        retiringModules_;
    std::unordered_map<
        KernelAbiInstance*,
        std::unique_ptr<KernelAbiInstance, KernelAbiInstanceDeleter>>
        activeInstancesOwned_;
    std::vector<
        std::unique_ptr<KernelAbiInstance, KernelAbiInstanceDeleter>>
        retiredInstances_;
    std::uint64_t nextInstanceGeneration_ = 1;
};
}
