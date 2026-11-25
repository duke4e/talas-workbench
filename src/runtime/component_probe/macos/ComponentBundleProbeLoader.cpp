#include "runtime/component_probe/macos/ComponentBundleProbeLoader.h"

#include "runtime/component_bundle/ComponentBundleManifest.h"
#include "runtime/component_probe/ComponentBundleMachOValidation.h"
#include "runtime/component_probe/ComponentBundleProbeAbiValidation.h"
#include "runtime/component_probe/macos/ComponentBundleProbeMaterialization.h"

#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <os/lock.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace talas::runtime::component_probe
{
namespace
{
constexpr char materializedEntryName[] = "entry.bundle";

[[nodiscard]] ComponentBundleProbeResultV1 resultFor(
    const ComponentBundleProbeRequestV1& request,
    ComponentProbeState state,
    ComponentBundleProbeResultDiagnosticV1 diagnostic) noexcept
{
    ComponentBundleProbeResultV1 result;
    result.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::Result,
        sizeof(result));
    result.state = static_cast<std::uint32_t>(state);
    result.diagnostic = static_cast<std::uint32_t>(diagnostic);
    result.contentDigest = request.contentDigest;
    result.artifactGeneration = request.artifactGeneration;
    result.validationNonce = request.validationNonce;
    return result;
}

[[nodiscard]] bool digestBytes(
    std::span<const std::byte> bytes,
    std::array<std::uint8_t, componentTrustDigestBytes>& digest) noexcept
{
    if (bytes.empty()
        || bytes.size() > std::numeric_limits<CC_LONG>::max())
    {
        return false;
    }
    return CC_SHA256(
               bytes.data(),
               static_cast<CC_LONG>(bytes.size()),
               digest.data())
        != nullptr;
}

[[nodiscard]] bool sameFileIdentity(
    const struct stat& left,
    const struct stat& right) noexcept
{
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino
        && left.st_uid == right.st_uid && left.st_nlink == right.st_nlink
        && left.st_size == right.st_size
        && (left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
}

class PrivateMaterializedEntry final
{
public:
    PrivateMaterializedEntry() = default;
    PrivateMaterializedEntry(const PrivateMaterializedEntry&) = delete;
    PrivateMaterializedEntry& operator=(const PrivateMaterializedEntry&) = delete;

    ~PrivateMaterializedEntry()
    {
        static_cast<void>(cleanup());
    }

    [[nodiscard]] bool create(
        const ComponentBundleProbeRequestV1& request,
        std::span<const std::byte> bytes,
        const std::array<std::uint8_t, componentTrustDigestBytes>& expectedDigest)
        noexcept
    {
        if (directoryDescriptor_ >= 0 || bytes.empty())
            return false;
        if (!componentBundleProbeMaterializationPathV1(
                request,
                directoryPath_)
            || mkdir(directoryPath_.data(), 0700) != 0)
        {
            return false;
        }
        directoryCreated_ = true;
        directoryDescriptor_ = open(
            directoryPath_.data(),
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directoryDescriptor_ < 0)
            return false;
        struct stat directoryStatus{};
        if (fstat(directoryDescriptor_, &directoryStatus) != 0
            || !S_ISDIR(directoryStatus.st_mode)
            || directoryStatus.st_uid != getuid()
            || (directoryStatus.st_mode & 0777) != 0700)
        {
            return false;
        }

        fileDescriptor_ = openat(
            directoryDescriptor_,
            materializedEntryName,
            O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            0600);
        if (fileDescriptor_ < 0)
            return false;
        std::size_t written = 0;
        while (written < bytes.size())
        {
            const auto amount = write(
                fileDescriptor_,
                bytes.data() + written,
                bytes.size() - written);
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount <= 0)
                return false;
            written += static_cast<std::size_t>(amount);
        }
        if (fsync(fileDescriptor_) != 0 || fchmod(fileDescriptor_, 0500) != 0)
            return false;

        struct stat beforeDigest{};
        if (fstat(fileDescriptor_, &beforeDigest) != 0
            || !S_ISREG(beforeDigest.st_mode)
            || beforeDigest.st_uid != getuid()
            || beforeDigest.st_nlink != 1
            || beforeDigest.st_size < 0
            || static_cast<std::uint64_t>(beforeDigest.st_size) != bytes.size()
            || (beforeDigest.st_mode & 0777) != 0500)
        {
            return false;
        }
        std::array<std::uint8_t, componentTrustDigestBytes> actualDigest{};
        if (!digestFile(actualDigest))
            return false;
        struct stat afterDigest{};
        if (fstat(fileDescriptor_, &afterDigest) != 0
            || !sameFileIdentity(beforeDigest, afterDigest)
            || actualDigest != expectedDigest)
        {
            return false;
        }
        verifiedIdentity_ = afterDigest;
        if (close(fileDescriptor_) != 0)
        {
            fileDescriptor_ = -1;
            return false;
        }
        fileDescriptor_ = -1;
        const auto pathBytes = std::snprintf(
            entryPath_.data(),
            entryPath_.size(),
            "%s/%s",
            directoryPath_.data(),
            materializedEntryName);
        return pathBytes > 0
            && static_cast<std::size_t>(pathBytes) < entryPath_.size();
    }

    [[nodiscard]] const char* path() const noexcept
    {
        return entryPath_.data();
    }

    // dlopen() takes a path, so check that the path still names the file
    // whose digest was verified before relying on the loaded image.
    [[nodiscard]] bool pathStillNamesVerifiedFile() const noexcept
    {
        struct stat current{};
        return entryPath_.front() != '\0'
            && lstat(entryPath_.data(), &current) == 0
            && S_ISREG(current.st_mode)
            && sameFileIdentity(verifiedIdentity_, current)
            && (current.st_mode & 0777) == 0500;
    }

    [[nodiscard]] bool cleanup() noexcept
    {
        bool clean = true;
        if (fileDescriptor_ >= 0)
        {
            if (close(fileDescriptor_) != 0)
                clean = false;
            fileDescriptor_ = -1;
        }
        if (directoryDescriptor_ >= 0)
        {
            if (unlinkat(
                    directoryDescriptor_,
                    materializedEntryName,
                    0)
                    != 0
                && errno != ENOENT)
            {
                clean = false;
            }
            if (close(directoryDescriptor_) != 0)
                clean = false;
            directoryDescriptor_ = -1;
        }
        if (directoryCreated_)
        {
            if (rmdir(directoryPath_.data()) != 0 && errno != ENOENT)
                clean = false;
            directoryCreated_ = false;
        }
        entryPath_.fill(0);
        directoryPath_.fill(0);
        return clean;
    }

private:
    [[nodiscard]] bool digestFile(
        std::array<std::uint8_t, componentTrustDigestBytes>& digest) noexcept
    {
        CC_SHA256_CTX context{};
        if (CC_SHA256_Init(&context) != 1)
            return false;
        std::array<std::byte, 16U * 1024U> buffer{};
        std::uint64_t offset = 0;
        while (true)
        {
            const auto amount = pread(
                fileDescriptor_,
                buffer.data(),
                buffer.size(),
                static_cast<off_t>(offset));
            if (amount < 0 && errno == EINTR)
                continue;
            if (amount < 0)
                return false;
            if (amount == 0)
                break;
            if (CC_SHA256_Update(
                    &context,
                    buffer.data(),
                    static_cast<CC_LONG>(amount))
                != 1)
            {
                return false;
            }
            offset += static_cast<std::uint64_t>(amount);
        }
        return CC_SHA256_Final(digest.data(), &context) == 1;
    }

    std::array<char, PATH_MAX> directoryPath_{};
    std::array<char, PATH_MAX> entryPath_{};
    struct stat verifiedIdentity_{};
    int directoryDescriptor_ = -1;
    int fileDescriptor_ = -1;
    bool directoryCreated_ = false;
};

struct HostAllocationV1
{
    void* pointer = nullptr;
    std::uint64_t byteCount = 0;
    std::uint64_t alignment = 0;
};

class BoundedProbeHost final
{
public:
    BoundedProbeHost()
    {
        services_.structSize = sizeof(services_);
        services_.servicesVersion = TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V1;
        services_.hostContext = this;
        services_.allocate = allocate;
        services_.deallocate = deallocate;
        services_.diagnostic = diagnostic;
    }

    BoundedProbeHost(const BoundedProbeHost&) = delete;
    BoundedProbeHost& operator=(const BoundedProbeHost&) = delete;

    ~BoundedProbeHost()
    {
        static_cast<void>(finish());
    }

    [[nodiscard]] const TalasKernelAbiHostServicesV1& services() const noexcept
    {
        return services_;
    }

    [[nodiscard]] ComponentBundleProbeHostObservationV1 finish() noexcept
    {
        os_unfair_lock_lock(&lock_);
        for (auto& allocation : allocations_)
        {
            if (allocation.pointer != nullptr)
            {
                observation_.violationObserved = true;
                std::free(allocation.pointer);
                allocation = {};
            }
        }
        const auto result = observation_;
        os_unfair_lock_unlock(&lock_);
        return result;
    }

private:
    [[nodiscard]] static bool validAlignment(std::uint64_t alignment) noexcept
    {
        return alignment != 0 && std::has_single_bit(alignment)
            && alignment <= maximumComponentBundleProbeHostAllocationBytesV1;
    }

    [[nodiscard]] static void* allocate(
        void* context,
        std::uint64_t byteCount,
        std::uint64_t alignment) noexcept
    {
        auto* self = static_cast<BoundedProbeHost*>(context);
        if (self == nullptr)
            return nullptr;
        os_unfair_lock_lock(&self->lock_);
        const auto nextCalls = self->observation_.allocationCalls + 1U;
        const auto budgetAvailable = byteCount != 0
            && byteCount <= maximumComponentBundleProbeHostAllocationBytesV1
            && self->observation_.allocationBytes
                <= maximumComponentBundleProbeHostAllocationBytesV1 - byteCount;
        if (!validAlignment(alignment)
            || nextCalls > maximumComponentBundleProbeHostAllocationsV1
            || !budgetAvailable)
        {
            self->observation_.violationObserved = true;
            self->observation_.allocationCalls = nextCalls;
            os_unfair_lock_unlock(&self->lock_);
            return nullptr;
        }
        auto slot = self->allocations_.end();
        for (auto iterator = self->allocations_.begin();
             iterator != self->allocations_.end();
             ++iterator)
        {
            if (iterator->pointer == nullptr)
            {
                slot = iterator;
                break;
            }
        }
        if (slot == self->allocations_.end())
        {
            self->observation_.violationObserved = true;
            self->observation_.allocationCalls = nextCalls;
            os_unfair_lock_unlock(&self->lock_);
            return nullptr;
        }
        const auto nativeAlignment = std::max<std::uint64_t>(
            alignment,
            sizeof(void*));
        void* allocation = nullptr;
        const auto allocated = posix_memalign(
            &allocation,
            static_cast<std::size_t>(nativeAlignment),
            static_cast<std::size_t>(byteCount)) == 0;
        self->observation_.allocationCalls = nextCalls;
        if (!allocated || allocation == nullptr)
        {
            self->observation_.violationObserved = true;
            os_unfair_lock_unlock(&self->lock_);
            return nullptr;
        }
        self->observation_.allocationBytes += byteCount;
        *slot = {allocation, byteCount, alignment};
        os_unfair_lock_unlock(&self->lock_);
        return allocation;
    }

    static void deallocate(
        void* context,
        void* allocation,
        std::uint64_t byteCount,
        std::uint64_t alignment) noexcept
    {
        auto* self = static_cast<BoundedProbeHost*>(context);
        if (self == nullptr || allocation == nullptr)
            return;
        os_unfair_lock_lock(&self->lock_);
        for (auto& record : self->allocations_)
        {
            if (record.pointer == allocation)
            {
                if (record.byteCount != byteCount
                    || record.alignment != alignment)
                {
                    self->observation_.violationObserved = true;
                    os_unfair_lock_unlock(&self->lock_);
                    return;
                }
                record = {};
                os_unfair_lock_unlock(&self->lock_);
                std::free(allocation);
                return;
            }
        }
        self->observation_.violationObserved = true;
        os_unfair_lock_unlock(&self->lock_);
    }

    static void diagnostic(
        void* context,
        TalasKernelAbiDiagnosticSeverityV1 severity,
        std::uint32_t,
        TalasKernelAbiStringViewV1 message) noexcept
    {
        auto* self = static_cast<BoundedProbeHost*>(context);
        if (self == nullptr)
            return;
        os_unfair_lock_lock(&self->lock_);
        ++self->observation_.diagnosticCalls;
        if (severity < TALAS_KERNEL_ABI_DIAGNOSTIC_INFO
            || severity > TALAS_KERNEL_ABI_DIAGNOSTIC_ERROR
            || (message.byteCount != 0 && message.data == nullptr)
            || message.byteCount
                > maximumComponentBundleProbeHostDiagnosticBytesV1
            || self->observation_.diagnosticBytes
                > maximumComponentBundleProbeHostDiagnosticBytesV1
                    - message.byteCount)
        {
            self->observation_.violationObserved = true;
        }
        else
        {
            self->observation_.diagnosticBytes += message.byteCount;
        }
        if (self->observation_.diagnosticCalls
            > maximumComponentBundleProbeHostDiagnosticsV1)
        {
            self->observation_.violationObserved = true;
        }
        os_unfair_lock_unlock(&self->lock_);
    }

    os_unfair_lock lock_ = OS_UNFAIR_LOCK_INIT;
    TalasKernelAbiHostServicesV1 services_{};
    ComponentBundleProbeHostObservationV1 observation_{};
    std::array<
        HostAllocationV1,
        maximumComponentBundleProbeHostAllocationsV1>
        allocations_{};
};

[[nodiscard]] bool copyText(
    TalasKernelAbiStringViewV1 source,
    std::size_t maximumBytes,
    std::string& destination)
{
    if (source.data == nullptr || source.byteCount == 0
        || source.byteCount > maximumBytes)
    {
        return false;
    }
    destination.assign(
        source.data,
        static_cast<std::size_t>(source.byteCount));
    return true;
}

[[nodiscard]] bool copyModuleMetadata(
    const TalasKernelAbiModuleV1* source,
    ComponentBundleProbeCopiedModuleMetadataV1& destination)
{
    if (source == nullptr
        || source->kernelCount == 0
        || source->kernelCount > maximumComponentBundleProbeNodesV1
        || source->kernels == nullptr
        || !copyText(
            source->moduleId,
            maximumComponentBundleIdBytes,
            destination.moduleId)
        || !copyText(
            source->moduleVersion,
            maximumComponentSemanticVersionBytes,
            destination.moduleVersion))
    {
        return false;
    }
    destination.structSize = source->structSize;
    destination.descriptorVersion = source->descriptorVersion;
    destination.minimumApiVersion = source->minimumApiVersion;
    destination.maximumApiVersion = source->maximumApiVersion;
    destination.compatibility = source->compatibility;
    destination.sampleFormats = source->sampleFormats;
    destination.trustClass = source->trustClass;
    destination.reserved0 = source->reserved0;
    destination.capabilityFlags = source->capabilityFlags;
    destination.hasUnload = source->unload != nullptr;
    destination.reserved1 = source->reserved1;
    destination.reserved2 = source->reserved2;
    destination.kernels.reserve(static_cast<std::size_t>(source->kernelCount));
    for (std::size_t index = 0; index < source->kernelCount; ++index)
    {
        const auto& sourceKernel = source->kernels[index];
        ComponentBundleProbeCopiedKernelMetadataV1 kernel;
        kernel.structSize = sourceKernel.structSize;
        kernel.descriptorVersion = sourceKernel.descriptorVersion;
        if (!copyText(
                sourceKernel.typeId,
                maximumComponentBundleProbeNodeIdBytesV1,
                kernel.typeId))
        {
            return false;
        }
        kernel.capabilityFlags = sourceKernel.capabilityFlags;
        kernel.hasCreate = sourceKernel.create != nullptr;
        kernel.hasPrepare = sourceKernel.prepare != nullptr;
        kernel.hasReset = sourceKernel.reset != nullptr;
        kernel.hasProcess = sourceKernel.process != nullptr;
        kernel.hasDestroy = sourceKernel.destroy != nullptr;
        kernel.reserved0 = sourceKernel.reserved0;
        kernel.reserved1 = sourceKernel.reserved1;
        destination.kernels.push_back(std::move(kernel));
    }
    return true;
}

[[nodiscard]] component_bundle::ComponentBundleManifest expectedManifest(
    const ComponentBundleProbeRequestV1& request,
    const ComponentBundleProbeExpectedAbiV1& expectedAbi)
{
    component_bundle::ComponentBundleManifest manifest;
    manifest.bundleId.assign(
        request.bundleId.data(),
        request.bundleIdBytes);
    manifest.semanticVersion.assign(
        request.semanticVersion.data(),
        request.semanticVersionBytes);
    manifest.minimumEngineAbiVersion = expectedAbi.minimumEngineAbiVersion;
    manifest.maximumEngineAbiVersion = expectedAbi.maximumEngineAbiVersion;
    manifest.minimumEngineApiVersion = expectedAbi.minimumEngineApiVersion;
    manifest.maximumEngineApiVersion = expectedAbi.maximumEngineApiVersion;
    manifest.nodes.reserve(expectedAbi.nodeCount);
    for (std::size_t index = 0; index < expectedAbi.nodeCount; ++index)
    {
        component_bundle::ComponentBundleNode node;
        node.typeId.assign(
            expectedAbi.nodeIds[index].data(),
            expectedAbi.nodeIdBytes[index]);
        manifest.nodes.push_back(std::move(node));
    }
    return manifest;
}

[[nodiscard]] TalasKernelAbiCompatibilityV1 expectedCompatibility() noexcept
{
    TalasKernelAbiCompatibilityV1 value{};
    value.structSize = sizeof(value);
    value.abiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.pointerBits = sizeof(void*) * 8U;
    value.littleEndian = std::endian::native == std::endian::little ? 1U : 0U;
    value.doubleBits = sizeof(double) * 8U;
    value.doubleIec559 = std::numeric_limits<double>::is_iec559 ? 1U : 0U;
    value.maximumAlignment = alignof(std::max_align_t);
    return value;
}
}

ComponentBundleProbeResultV1 runComponentBundleProbeLoaderV1(
    const ComponentBundleProbeRequestV1& request,
    const ComponentBundleProbeExpectedAbiV1& expectedAbi,
    std::span<const std::byte> entryBytes) noexcept
{
    if (validateComponentBundleProbeExpectedAbiV1(expectedAbi, request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid
        || entryBytes.size() != request.entryByteCount)
    {
        return resultFor(
            request,
            ComponentProbeState::Rejected,
            ComponentBundleProbeResultDiagnosticV1::RequestRejected);
    }
    std::array<std::uint8_t, componentTrustDigestBytes> inputDigest{};
    if (!digestBytes(entryBytes, inputDigest)
        || inputDigest != expectedAbi.entryDigest)
    {
        return resultFor(
            request,
            ComponentProbeState::Rejected,
            ComponentBundleProbeResultDiagnosticV1::EntryRejected);
    }
    const auto image = std::span(
        reinterpret_cast<const std::uint8_t*>(entryBytes.data()),
        entryBytes.size());
    if (!validateComponentBundleMachOV1(
             image,
             currentComponentBundleMachOArchitectureV1())
             .ok())
    {
        return resultFor(
            request,
            ComponentProbeState::Rejected,
            ComponentBundleProbeResultDiagnosticV1::EntryRejected);
    }

    try
    {
        PrivateMaterializedEntry materialized;
        if (!materialized.create(
                request,
                entryBytes,
                expectedAbi.entryDigest))
        {
            return resultFor(
                request,
                ComponentProbeState::Rejected,
                ComponentBundleProbeResultDiagnosticV1::LoadRejected);
        }
        void* imageHandle = dlopen(
            materialized.path(),
            RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
        if (imageHandle == nullptr)
        {
            static_cast<void>(materialized.cleanup());
            return resultFor(
                request,
                ComponentProbeState::Rejected,
                ComponentBundleProbeResultDiagnosticV1::LoadRejected);
        }
        // The mapped image no longer needs its file. Remove it before any
        // module code runs, so a hang that ends in SIGKILL leaves no copy.
        const auto verifiedImage = materialized.pathStillNamesVerifiedFile();
        if (!materialized.cleanup() || !verifiedImage)
        {
            static_cast<void>(dlclose(imageHandle));
            return resultFor(
                request,
                ComponentProbeState::Rejected,
                ComponentBundleProbeResultDiagnosticV1::LoadRejected);
        }

        auto* symbol = dlsym(
            imageHandle,
            componentBundleProbeEntrySymbolV1.data());
        if (symbol == nullptr)
        {
            static_cast<void>(dlclose(imageHandle));
            static_cast<void>(materialized.cleanup());
            return resultFor(
                request,
                ComponentProbeState::Rejected,
                ComponentBundleProbeResultDiagnosticV1::EntrySymbolMissing);
        }
        ComponentBundleProbeEntryPointV1 entry = nullptr;
        static_assert(sizeof(entry) == sizeof(symbol));
        std::memcpy(&entry, &symbol, sizeof(entry));

        BoundedProbeHost host;
        const TalasKernelAbiModuleV1* module = nullptr;
        TalasKernelAbiStatusV1 entryStatus = TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
        try
        {
            entryStatus = entry(&host.services(), &module);
        }
        catch (...)
        {
            entryStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
        }

        ComponentBundleProbeResultV1 terminal;
        if (entryStatus != TALAS_KERNEL_ABI_STATUS_OK)
        {
            terminal = resultFor(
                request,
                ComponentProbeState::ControlledFatal,
                ComponentBundleProbeResultDiagnosticV1::ModuleControlledFatal);
        }
        else
        {
            ComponentBundleProbeCopiedModuleMetadataV1 copied;
            if (!copyModuleMetadata(module, copied))
            {
                terminal = resultFor(
                    request,
                    ComponentProbeState::Rejected,
                    ComponentBundleProbeResultDiagnosticV1::MetadataRejected);
            }
            else
            {
                const auto manifest = expectedManifest(request, expectedAbi);
                const auto validation = validateComponentBundleProbeAbiV1(
                    copied,
                    manifest,
                    expectedCompatibility(),
                    {});
                terminal = validation.ok()
                    ? resultFor(
                        request,
                        ComponentProbeState::Clean,
                        ComponentBundleProbeResultDiagnosticV1::Ready)
                    : resultFor(
                        request,
                        ComponentProbeState::Rejected,
                        ComponentBundleProbeResultDiagnosticV1::AbiRejected);
            }
        }

        bool unloadClean = true;
        if (module != nullptr && module->unload != nullptr)
        {
            try
            {
                module->unload(module->moduleContext, &host.services());
            }
            catch (...)
            {
                unloadClean = false;
            }
        }
        const auto observation = host.finish();
        if (observation.violationObserved
            || observation.allocationCalls
                > maximumComponentBundleProbeHostAllocationsV1
            || observation.allocationBytes
                > maximumComponentBundleProbeHostAllocationBytesV1
            || observation.diagnosticCalls
                > maximumComponentBundleProbeHostDiagnosticsV1
            || observation.diagnosticBytes
                > maximumComponentBundleProbeHostDiagnosticBytesV1)
        {
            if (terminal.state
                == static_cast<std::uint32_t>(ComponentProbeState::Clean))
            {
                terminal = resultFor(
                    request,
                    ComponentProbeState::Rejected,
                    ComponentBundleProbeResultDiagnosticV1::MetadataRejected);
            }
        }
        if (dlclose(imageHandle) != 0)
            unloadClean = false;
        if (!materialized.cleanup())
            unloadClean = false;
        if (!unloadClean)
        {
            return resultFor(
                request,
                ComponentProbeState::Rejected,
                ComponentBundleProbeResultDiagnosticV1::UnloadRejected);
        }
        return terminal;
    }
    catch (...)
    {
        return resultFor(
            request,
            ComponentProbeState::ControlledFatal,
            ComponentBundleProbeResultDiagnosticV1::ModuleControlledFatal);
    }
}
}
