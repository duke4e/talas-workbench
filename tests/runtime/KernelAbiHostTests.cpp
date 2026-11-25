#include "talas/runtime/KernelAbiHost.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/BuiltinKernelAbiModule.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{
static_assert(
    !std::is_pointer_v<decltype(
        talas::runtime::KernelAbiInstanceResult{}.instance)>,
    "retirement handles need an identity token in addition to an address");
template <typename Type>
concept ExposesLifecycleDescriptor = requires(const Type& value) {
    value.descriptor;
};
static_assert(
    !ExposesLifecycleDescriptor<talas::runtime::KernelAbiKernelMetadata>,
    "validated public metadata must not expose callable lifecycle pointers");

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct CallbackCounts
{
    std::size_t allocations = 0;
    std::size_t deallocations = 0;
    std::size_t diagnostics = 0;
    std::size_t creates = 0;
    std::size_t prepares = 0;
    std::size_t resets = 0;
    std::size_t processes = 0;
    std::size_t destroys = 0;
    std::size_t unloads = 0;
    std::size_t captures = 0;
    std::size_t restores = 0;
    std::size_t checkpointDestroys = 0;
    std::size_t liveCheckpoints = 0;
    bool checkpointLifetimeValid = true;
    TalasKernelAbiStatusV1 captureStatus = TALAS_KERNEL_ABI_STATUS_OK;
    TalasKernelAbiStatusV1 restoreStatus = TALAS_KERNEL_ABI_STATUS_OK;
    bool nullCheckpoint = false;
    bool throwCapture = false;
    bool throwRestore = false;
    bool throwCheckpointDestroy = false;
    TalasKernelAbiStatusV1 createStatus = TALAS_KERNEL_ABI_STATUS_OK;
    TalasKernelAbiStatusV1 prepareStatus = TALAS_KERNEL_ABI_STATUS_OK;
    TalasKernelAbiStatusV1 resetStatus = TALAS_KERNEL_ABI_STATUS_OK;
    TalasKernelAbiStatusV1 processStatus = TALAS_KERNEL_ABI_STATUS_OK;
    bool nullCreateResult = false;
    bool diagnoseDuringPrepare = false;
    std::uint32_t createHostStructSize = 0;
    std::uint32_t prepareStructSize = 0;
    std::uint32_t prepareHostStructSize = 0;
    std::uint32_t destroyHostStructSize = 0;
    std::uint32_t unloadHostStructSize = 0;
};

struct TestInstance
{
    CallbackCounts* counts = nullptr;
    const TalasKernelAbiHostServicesV1* host = nullptr;
};

void* allocate(void* context, std::uint64_t bytes, std::uint64_t)
{
    ++static_cast<CallbackCounts*>(context)->allocations;
    return std::malloc(static_cast<std::size_t>(bytes));
}

void deallocate(
    void* context,
    void* allocation,
    std::uint64_t,
    std::uint64_t)
{
    ++static_cast<CallbackCounts*>(context)->deallocations;
    std::free(allocation);
}

void diagnostic(
    void* context,
    TalasKernelAbiDiagnosticSeverityV1,
    std::uint32_t,
    TalasKernelAbiStringViewV1)
{
    ++static_cast<CallbackCounts*>(context)->diagnostics;
}

TalasKernelAbiStatusV1 create(
    const TalasKernelAbiHostServicesV1* host,
    void** instance)
{
    auto* counts = static_cast<CallbackCounts*>(host->hostContext);
    ++counts->creates;
    counts->createHostStructSize = host->structSize;
    if (counts->nullCreateResult)
    {
        *instance = nullptr;
        return counts->createStatus;
    }
    auto* created = static_cast<TestInstance*>(host->allocate(
        host->hostContext,
        sizeof(TestInstance),
        alignof(TestInstance)));
    if (created == nullptr)
    {
        *instance = nullptr;
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    }
    created->counts = counts;
    created->host = host;
    *instance = created;
    return counts->createStatus;
}

TalasKernelAbiStatusV1 prepare(
    void* instance,
    const TalasKernelAbiPrepareV1* prepareRecord)
{
    auto* state = static_cast<TestInstance*>(instance);
    ++state->counts->prepares;
    state->counts->prepareStructSize = prepareRecord->structSize;
    state->counts->prepareHostStructSize =
        prepareRecord->host->structSize;
    if (state->counts->diagnoseDuringPrepare)
    {
        static constexpr char message[] = "prepare diagnostic";
        state->host->diagnostic(
            state->host->hostContext,
            TALAS_KERNEL_ABI_DIAGNOSTIC_INFO,
            1,
            {message, sizeof(message) - 1});
    }
    return state->counts->prepareStatus;
}

TalasKernelAbiStatusV1 reset(
    void* instance,
    const TalasKernelAbiResetV1*)
{
    auto* state = static_cast<TestInstance*>(instance);
    ++state->counts->resets;
    return state->counts->resetStatus;
}

TalasKernelAbiStatusV1 process(
    void* instance,
    const TalasKernelAbiProcessV1* block)
{
    auto* state = static_cast<TestInstance*>(instance);
    ++state->counts->processes;
    if (state->counts->processStatus == TALAS_KERNEL_ABI_STATUS_OK
        && block->outputCount != 0 && block->outputs != nullptr
        && block->outputs[0].samples != nullptr)
    {
        block->outputs[0].samples[0] = 0.25;
    }
    return state->counts->processStatus;
}

void destroy(
    void* instance,
    const TalasKernelAbiHostServicesV1* host)
{
    auto* state = static_cast<TestInstance*>(instance);
    state->counts->checkpointLifetimeValid &= state->counts->liveCheckpoints == 0;
    ++state->counts->destroys;
    state->counts->destroyHostStructSize = host->structSize;
    host->deallocate(
        host->hostContext,
        state,
        sizeof(TestInstance),
        alignof(TestInstance));
}

void unload(
    void* context,
    const TalasKernelAbiHostServicesV1* host)
{
    auto* counts = static_cast<CallbackCounts*>(context);
    ++counts->unloads;
    counts->unloadHostStructSize = host->structSize;
}

TalasKernelAbiStringViewV1 text(const char* value)
{
    return {value, std::char_traits<char>::length(value)};
}

struct CheckpointPayload { CallbackCounts* counts; };

TalasKernelAbiStatusV1 captureCheckpoint(void* opaque,
    const TalasKernelAbiHostServicesV1* host, void** checkpoint)
{
    auto* counts = static_cast<TestInstance*>(opaque)->counts;
    ++counts->captures;
    *checkpoint = nullptr;
    if (!counts->nullCheckpoint)
    {
        auto* value = static_cast<CheckpointPayload*>(host->allocate(
            host->hostContext, sizeof(CheckpointPayload), alignof(CheckpointPayload)));
        if (value == nullptr) return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
        value->counts = counts;
        *checkpoint = value;
        ++counts->liveCheckpoints;
    }
    if (counts->throwCapture) throw std::runtime_error("capture fixture exception");
    return counts->captureStatus;
}

TalasKernelAbiStatusV1 restoreCheckpoint(void* opaque, const void* checkpoint)
{
    auto* counts = static_cast<TestInstance*>(opaque)->counts;
    require(static_cast<const CheckpointPayload*>(checkpoint)->counts == counts,
        "checkpoint payload must belong to the original instance");
    ++counts->restores;
    if (counts->throwRestore) throw std::runtime_error("restore fixture exception");
    return counts->restoreStatus;
}

void destroyCheckpoint(void* opaque, const TalasKernelAbiHostServicesV1* host)
{
    auto* value = static_cast<CheckpointPayload*>(opaque);
    auto* counts = value->counts;
    counts->checkpointLifetimeValid &= counts->destroys == 0 && counts->unloads == 0;
    ++counts->checkpointDestroys;
    --counts->liveCheckpoints;
    host->deallocate(host->hostContext, value, sizeof(CheckpointPayload), alignof(CheckpointPayload));
    if (counts->throwCheckpointDestroy) throw std::runtime_error("destroy fixture exception");
}

struct ModuleFixture
{
    CallbackCounts counts;
    TalasKernelAbiHostServicesV1 services{};
    TalasKernelAbiDescriptorV1 kernel{};
    TalasKernelAbiModuleV1 module{};
    TalasKernelAbiCheckpointDescriptorV1 checkpoint{};
    TalasKernelAbiCheckpointExtensionV1 extension{};

    ModuleFixture()
    {
        services.structSize = sizeof(services);
        services.servicesVersion =
            TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V1;
        services.hostContext = &counts;
        services.allocate = allocate;
        services.deallocate = deallocate;
        services.diagnostic = diagnostic;

        kernel.structSize = sizeof(kernel);
        kernel.descriptorVersion =
            TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        kernel.typeId = text("talas.oscillator");
        kernel.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET
            | TALAS_KERNEL_ABI_CAPABILITY_STATEFUL;
        kernel.create = create;
        kernel.prepare = prepare;
        kernel.reset = reset;
        kernel.process = process;
        kernel.destroy = destroy;

        module.structSize = sizeof(module);
        module.descriptorVersion =
            TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        module.moduleId = text("org.talas.builtin.abi-tests");
        module.moduleVersion = text("1.0.0");
        module.minimumApiVersion = TALAS_KERNEL_API_VERSION_V1;
        module.maximumApiVersion = TALAS_KERNEL_API_VERSION_V1;
        module.compatibility =
            talas::runtime::currentKernelAbiCompatibility();
        module.sampleFormats = TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64;
        module.trustClass = TALAS_KERNEL_ABI_TRUST_BUILTIN;
        module.capabilityFlags = kernel.capabilityFlags;
        module.kernels = &kernel;
        module.kernelCount = 1;
        module.moduleContext = &counts;
        module.unload = unload;
        checkpoint = {sizeof(checkpoint), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
            kernel.typeId, captureCheckpoint, restoreCheckpoint, destroyCheckpoint, 0, 0};
        extension = {sizeof(extension), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
            module.moduleId, module.moduleVersion, &checkpoint, 1, 0, 0};
    }
};

void requireNoCallbacks(
    const CallbackCounts& counts,
    const std::string& message)
{
    require(
        counts.allocations == 0 && counts.deallocations == 0
            && counts.diagnostics == 0 && counts.creates == 0
            && counts.prepares == 0 && counts.resets == 0
            && counts.processes == 0 && counts.destroys == 0
            && counts.unloads == 0 && counts.captures == 0
            && counts.restores == 0 && counts.checkpointDestroys == 0,
        message);
}

TalasKernelAbiPrepareV1 validPrepare();

template <typename Mutate>
void expectRejected(
    talas::runtime::KernelAbiValidationCode code,
    Mutate mutate,
    std::string message)
{
    ModuleFixture fixture;
    mutate(fixture);
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    const auto result = host.registerModule(fixture.module);
    require(
        !result.ok() && !result.diagnostics.empty()
            && result.diagnostics.front().code == code,
        message);
    requireNoCallbacks(
        fixture.counts,
        message + " must reject before callbacks");
}

void testExactModuleRegistersWithoutLifecycleCalls()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    const auto result = host.registerModule(fixture.module);
    require(result.ok(), "an exact built-in ABI module should register");
    require(result.module->metadata().moduleId
                == "org.talas.builtin.abi-tests"
            && result.module->kernels().size() == 1
            && result.module->kernels().front().typeId
                == "talas.oscillator",
        "registration should publish copied immutable metadata");
    requireNoCallbacks(
        fixture.counts,
        "registration must not invoke lifecycle or host callbacks");

    ModuleFixture additiveModuleTail;
    additiveModuleTail.module.structSize += 64;
    talas::runtime::KernelAbiHost additiveHost(
        talas::core::makeBuiltinNodeRegistry(),
        additiveModuleTail.services);
    require(
        additiveHost.registerModule(additiveModuleTail.module).ok(),
        "a standalone module record may carry an ignored additive tail");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidStructure,
        [](auto& fixture) { fixture.kernel.structSize += 64; },
        "a repeated kernel record needs the exact v1 stride");
}

void testHostDestructionDrainsInstancesBeforeUnloadingModules()
{
    ModuleFixture idle;
    {
        talas::runtime::KernelAbiHost host(
            talas::core::makeBuiltinNodeRegistry(),
            idle.services);
        require(host.registerModule(idle.module).ok(),
            "idle teardown fixture should register");
    }
    require(
        idle.counts.unloads == 1 && idle.counts.destroys == 0,
        "host teardown should unload an idle registered module exactly once");

    ModuleFixture active;
    {
        talas::runtime::KernelAbiHost host(
            talas::core::makeBuiltinNodeRegistry(),
            active.services);
        require(host.registerModule(active.module).ok(),
            "active teardown fixture should register");
        auto first = host.createPreparedInstance(
            "org.talas.builtin.abi-tests",
            "talas.oscillator",
            validPrepare());
        auto second = host.createPreparedInstance(
            "org.talas.builtin.abi-tests",
            "talas.oscillator",
            validPrepare());
        require(first.ok() && second.ok(),
            "active teardown fixtures should prepare");
        require(
            host.retireInstance(first.instance)
                == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted,
            "host teardown should cover an already queued instance");
    }
    require(
        active.counts.destroys == 2
            && active.counts.allocations == active.counts.deallocations
            && active.counts.unloads == 1,
        "host teardown must destroy queued and active instances before one module unload");
}

void testCopiedStandaloneRecordsAdvertiseOnlyRetainedPrefix()
{
    ModuleFixture fixture;
    fixture.services.structSize += 64;
    {
        talas::runtime::KernelAbiHost host(
            talas::core::makeBuiltinNodeRegistry(),
            fixture.services);
        require(host.registerModule(fixture.module).ok(),
            "additive host-services fixture should register");
        auto prepareRecord = validPrepare();
        prepareRecord.structSize += 64;
        const auto created = host.createPreparedInstance(
            "org.talas.builtin.abi-tests",
            "talas.oscillator",
            prepareRecord);
        require(created.ok(),
            "standalone prepare tail should retain its v1 prefix");
        require(
            fixture.counts.createHostStructSize
                    == sizeof(TalasKernelAbiHostServicesV1)
                && fixture.counts.prepareStructSize
                    == sizeof(TalasKernelAbiPrepareV1)
                && fixture.counts.prepareHostStructSize
                    == sizeof(TalasKernelAbiHostServicesV1),
            "callback copies must not advertise tails that the host did not retain");
    }
    require(
        fixture.counts.destroyHostStructSize
                == sizeof(TalasKernelAbiHostServicesV1)
            && fixture.counts.unloadHostStructSize
                == sizeof(TalasKernelAbiHostServicesV1),
        "destroy and unload must receive the normalized retained host-services prefix");
}

void testStructureVersionAndPlatformMismatchesReject()
{
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidStructure,
        [](auto& fixture) { --fixture.module.structSize; },
        "a truncated module record should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidStructure,
        [](auto& fixture) { --fixture.kernel.structSize; },
        "a truncated kernel record should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidStructure,
        [](auto& fixture) {
            ++fixture.module.compatibility.structSize;
        },
        "an embedded compatibility record needs the exact v1 layout");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::UnsupportedVersion,
        [](auto& fixture) { ++fixture.module.descriptorVersion; },
        "a future module descriptor version should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::UnsupportedVersion,
        [](auto& fixture) {
            ++fixture.module.minimumApiVersion;
            ++fixture.module.maximumApiVersion;
        },
        "an API range miss should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::IncompatiblePlatform,
        [](auto& fixture) { ++fixture.module.compatibility.abiVersion; },
        "an ABI revision mismatch should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::IncompatiblePlatform,
        [](auto& fixture) { fixture.module.compatibility.pointerBits /= 2; },
        "a pointer-width mismatch should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidReservedField,
        [](auto& fixture) {
            fixture.module.compatibility.reserved0 = 1;
        },
        "a nonzero compatibility reserved field should reject distinctly");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::UnsupportedSampleFormat,
        [](auto& fixture) { fixture.module.sampleFormats = UINT64_C(2); },
        "an f32-only or unknown sample-format bit should reject");
}

void testTrustMetadataCallbacksAndCapabilitiesRejectBeforeFactory()
{
    expectRejected(
        talas::runtime::KernelAbiValidationCode::UnsupportedTrust,
        [](auto& fixture) {
            fixture.module.trustClass = TALAS_KERNEL_ABI_TRUST_UNTRUSTED;
        },
        "an untrusted module should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidReservedField,
        [](auto& fixture) { fixture.module.reserved1 = 1; },
        "a nonzero reserved field should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::MissingCallback,
        [](auto& fixture) { fixture.kernel.create = nullptr; },
        "a missing create callback should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::MissingCallback,
        [](auto& fixture) { fixture.module.unload = nullptr; },
        "a missing unload callback should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::CapabilityMismatch,
        [](auto& fixture) {
            fixture.kernel.capabilityFlags |=
                TALAS_KERNEL_ABI_CAPABILITY_TRANSPORT;
            fixture.module.capabilityFlags =
                fixture.kernel.capabilityFlags;
        },
        "kernel flags must match NodeRegistry authority");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::CapabilityMismatch,
        [](auto& fixture) { fixture.module.capabilityFlags = 0; },
        "module flags must equal the union of its kernels");
}

void testIdentityAndHostValidationRejectsDeterministically()
{
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidMetadata,
        [](auto& fixture) {
            static constexpr char invalidUtf8[] = {'x', '\xC3', '(', 0};
            fixture.module.moduleId = {invalidUtf8, 3};
        },
        "invalid UTF-8 metadata should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::InvalidMetadata,
        [](auto& fixture) {
            static constexpr char controlIdentity[] = {'x', '\n', 'y'};
            fixture.module.moduleId = {controlIdentity, 3};
        },
        "control characters in metadata should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::UnknownNodeType,
        [](auto& fixture) { fixture.kernel.typeId = text("talas.unknown"); },
        "an unknown kernel type should reject");
    expectRejected(
        talas::runtime::KernelAbiValidationCode::DuplicateIdentity,
        [](auto& fixture) {
            static TalasKernelAbiDescriptorV1 duplicate[2];
            duplicate[0] = fixture.kernel;
            duplicate[1] = fixture.kernel;
            fixture.module.kernels = duplicate;
            fixture.module.kernelCount = 2;
        },
        "duplicate type identities should reject");

    ModuleFixture fixture;
    fixture.services.allocate = nullptr;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    const auto invalidHost = host.registerModule(fixture.module);
    require(
        !invalidHost.ok()
            && invalidHost.diagnostics.front().code
                == talas::runtime::KernelAbiValidationCode::InvalidHostServices,
        "invalid host services should reject registration");
    requireNoCallbacks(
        fixture.counts,
        "invalid host services must not invoke callbacks");

    ModuleFixture duplicateModule;
    talas::runtime::KernelAbiHost duplicateHost(
        talas::core::makeBuiltinNodeRegistry(),
        duplicateModule.services);
    require(
        duplicateHost.registerModule(duplicateModule.module).ok(),
        "duplicate-module fixture should register once");
    const auto duplicateResult =
        duplicateHost.registerModule(duplicateModule.module);
    require(
        !duplicateResult.ok()
            && duplicateResult.diagnostics.front().code
                == talas::runtime::KernelAbiValidationCode::DuplicateIdentity,
        "a duplicate module identity should reject atomically");
    requireNoCallbacks(
        duplicateModule.counts,
        "duplicate registration must not invoke callbacks");
}

TalasKernelAbiPrepareV1 validPrepare()
{
    TalasKernelAbiPrepareV1 prepareRecord{};
    prepareRecord.structSize = sizeof(prepareRecord);
    prepareRecord.apiVersion = TALAS_KERNEL_API_VERSION_V1;
    prepareRecord.sampleRate = 48'000.0;
    prepareRecord.maximumBlockFrames = 64;
    return prepareRecord;
}

TalasKernelAbiTransportV1 validTransport()
{
    TalasKernelAbiTransportV1 transport{};
    transport.structSize = sizeof(transport);
    transport.sampleRate = 48'000.0;
    return transport;
}

void testPreparedLifecycleOwnsMemoryAndKeepsProcessCallbackClean()
{
    ModuleFixture fixture;
    fixture.counts.diagnoseDuringPrepare = true;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    require(host.registerModule(fixture.module).ok(),
        "lifecycle fixture should register");

    auto created = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(created.ok(), "create and prepare should publish one instance");
    require(
        fixture.counts.creates == 1 && fixture.counts.prepares == 1
            && fixture.counts.allocations == 1
            && fixture.counts.diagnostics == 1
            && fixture.counts.destroys == 0
            && fixture.counts.deallocations == 0,
        "create/prepare should use host-owned memory and off-callback diagnostics");

    TalasKernelAbiResetV1 resetRecord{};
    resetRecord.structSize = sizeof(resetRecord);
    resetRecord.reason = TALAS_KERNEL_ABI_RESET_EXPLICIT;
    resetRecord.transport = validTransport();
    require(
        created.instance->reset(resetRecord) == TALAS_KERNEL_ABI_STATUS_OK
            && fixture.counts.resets == 1,
        "reset status should map through the prepared instance");

    double sample = -1.0;
    TalasKernelAbiOutputV1 output{};
    output.structSize = sizeof(output);
    output.samples = &sample;
    output.sampleCount = 1;
    TalasKernelAbiProcessV1 block{};
    block.structSize = sizeof(block);
    block.frameCount = 1;
    block.outputs = &output;
    block.outputCount = 1;
    block.transport = validTransport();
    const auto allocationsBeforeProcess = fixture.counts.allocations;
    const auto deallocationsBeforeProcess = fixture.counts.deallocations;
    const auto diagnosticsBeforeProcess = fixture.counts.diagnostics;
    require(
        created.instance->process(block) == TALAS_KERNEL_ABI_STATUS_OK
            && sample == 0.25 && fixture.counts.processes == 1,
        "process should enter the module once and map success");
    require(
        fixture.counts.allocations == allocationsBeforeProcess
            && fixture.counts.deallocations == deallocationsBeforeProcess
            && fixture.counts.diagnostics == diagnosticsBeforeProcess,
        "the host process path must not allocate, free, or diagnose");

    require(
        host.retireInstance(created.instance)
            == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted
            && fixture.counts.destroys == 0
            && host.retiredInstanceCount() == 1,
        "instance retirement should queue state without destroying it");
    require(
        host.retireInstance(created.instance)
            == talas::runtime::KernelAbiInstanceRetirementStatus::UnknownInstance,
        "double instance retirement should be classified without a second queue entry");
    require(
        host.collectRetiredModules() == 0
            && host.retiredInstanceCount() == 0,
        "off-callback collection should destroy queued state without retiring its active module");
    require(
        fixture.counts.destroys == 1
            && fixture.counts.deallocations == 1
            && fixture.counts.allocations == fixture.counts.deallocations,
        "destroy should return every host-owned allocation exactly once");
}

void testPartialCreateAndPrepareFailuresCleanUpWithoutPublication()
{
    ModuleFixture invalidPrepare;
    talas::runtime::KernelAbiHost invalidPrepareHost(
        talas::core::makeBuiltinNodeRegistry(),
        invalidPrepare.services);
    require(invalidPrepareHost.registerModule(invalidPrepare.module).ok(),
        "invalid-prepare fixture should register");
    auto malformedPrepare = validPrepare();
    malformedPrepare.maximumBlockFrames = 0;
    auto rejectedBeforeCreate = invalidPrepareHost.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        malformedPrepare);
    require(
        !rejectedBeforeCreate.ok()
            && rejectedBeforeCreate.status
                == TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION,
        "invalid prepare metadata should reject deterministically");
    requireNoCallbacks(
        invalidPrepare.counts,
        "prepare validation must reject before lifecycle callbacks");
    TalasKernelAbiParameterV1 oversizedParameter{};
    oversizedParameter.structSize = sizeof(oversizedParameter) + 8;
    oversizedParameter.kind = TALAS_KERNEL_ABI_PARAMETER_SCALAR;
    oversizedParameter.id = text("frequency");
    oversizedParameter.scalarValue = 440.0;
    auto oversizedParameterPrepare = validPrepare();
    oversizedParameterPrepare.parameters = &oversizedParameter;
    oversizedParameterPrepare.parameterCount = 1;
    const auto rejectedParameterStride =
        invalidPrepareHost.createPreparedInstance(
            "org.talas.builtin.abi-tests",
            "talas.oscillator",
            oversizedParameterPrepare);
    require(
        !rejectedParameterStride.ok()
            && rejectedParameterStride.status
                == TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION,
        "a repeated prepare parameter needs the exact v1 stride");
    requireNoCallbacks(
        invalidPrepare.counts,
        "parameter stride rejection must happen before lifecycle callbacks");

    ModuleFixture createFailure;
    createFailure.counts.createStatus =
        TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
    talas::runtime::KernelAbiHost createHost(
        talas::core::makeBuiltinNodeRegistry(),
        createFailure.services);
    require(createHost.registerModule(createFailure.module).ok(),
        "create-failure fixture should register");
    auto rejectedCreate = createHost.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(
        !rejectedCreate.ok()
            && rejectedCreate.status
                == TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION
            && createFailure.counts.creates == 1
            && createFailure.counts.prepares == 0
            && createFailure.counts.allocations == 1
            && createFailure.counts.destroys == 1
            && createFailure.counts.deallocations == 1,
        "a partial create failure should destroy host-owned state and publish nothing");

    ModuleFixture prepareFailure;
    prepareFailure.counts.prepareStatus = TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    talas::runtime::KernelAbiHost prepareHost(
        talas::core::makeBuiltinNodeRegistry(),
        prepareFailure.services);
    require(prepareHost.registerModule(prepareFailure.module).ok(),
        "prepare-failure fixture should register");
    auto rejectedPrepare = prepareHost.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(
        !rejectedPrepare.ok()
            && rejectedPrepare.status == TALAS_KERNEL_ABI_STATUS_INVALID_VALUE
            && prepareFailure.counts.creates == 1
            && prepareFailure.counts.prepares == 1
            && prepareFailure.counts.allocations == 1
            && prepareFailure.counts.destroys == 1
            && prepareFailure.counts.deallocations == 1,
        "a prepare failure should destroy the opaque instance and publish nothing");

    ModuleFixture nullCreate;
    nullCreate.counts.nullCreateResult = true;
    talas::runtime::KernelAbiHost nullHost(
        talas::core::makeBuiltinNodeRegistry(),
        nullCreate.services);
    require(nullHost.registerModule(nullCreate.module).ok(),
        "null-create fixture should register");
    auto nullResult = nullHost.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(
        !nullResult.ok()
            && nullResult.status == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR
            && nullCreate.counts.creates == 1
            && nullCreate.counts.prepares == 0
            && nullCreate.counts.allocations == 0
            && nullCreate.counts.destroys == 0,
        "a successful create status without an instance should fail closed");
}

void testValidationAndFatalQuarantineDoNotLeakPartialOutput()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    require(host.registerModule(fixture.module).ok(),
        "quarantine fixture should register");
    auto created = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(created.ok(), "quarantine fixture should prepare");

    double sample = -1.0;
    TalasKernelAbiOutputV1 output{};
    output.structSize = sizeof(output);
    output.samples = &sample;
    output.sampleCount = 1;
    TalasKernelAbiProcessV1 invalid{};
    invalid.structSize = sizeof(invalid) - 1;
    invalid.frameCount = 1;
    invalid.outputs = &output;
    invalid.outputCount = 1;
    invalid.transport = validTransport();
    require(
        created.instance->process(invalid)
                == TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT
            && fixture.counts.processes == 0 && sample == -1.0,
        "host-side layout rejection should not enter module code or mutate output");

    TalasKernelAbiProcessV1 fatal = invalid;
    fatal.structSize = sizeof(fatal);
    auto expectBlockRejected = [&](const std::string& message) {
        const auto processesBefore = fixture.counts.processes;
        sample = -1.0;
        require(
            created.instance->process(fatal)
                    == TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT
                && fixture.counts.processes == processesBefore
                && sample == -1.0,
            message);
    };
    output.structSize = sizeof(output) + 8;
    expectBlockRejected("a repeated output needs the exact v1 stride");
    output.structSize = sizeof(output);
    TalasKernelAbiSignalV1 oversizedSignal{};
    oversizedSignal.structSize = sizeof(oversizedSignal) + 8;
    oversizedSignal.kind = TALAS_KERNEL_ABI_SIGNAL_CONSTANT;
    fatal.inputs = &oversizedSignal;
    fatal.inputCount = 1;
    expectBlockRejected("a repeated signal needs the exact v1 stride");
    fatal.inputs = nullptr;
    fatal.inputCount = 0;
    TalasKernelAbiEventV1 oversizedEvent{};
    oversizedEvent.structSize = sizeof(oversizedEvent) + 8;
    oversizedEvent.kind = TALAS_KERNEL_ABI_EVENT_GATE;
    fatal.events = &oversizedEvent;
    fatal.eventCount = 1;
    expectBlockRejected("a repeated event needs the exact v1 stride");
    fatal.events = nullptr;
    fatal.eventCount = 0;
    ++fatal.transport.structSize;
    expectBlockRejected("an embedded transport needs the exact v1 layout");
    fatal.transport.structSize = sizeof(fatal.transport);
    TalasKernelAbiEventV1 unknownGatePayload{};
    unknownGatePayload.structSize = sizeof(unknownGatePayload);
    unknownGatePayload.kind = TALAS_KERNEL_ABI_EVENT_GATE;
    unknownGatePayload.payloadBits =
        TALAS_KERNEL_ABI_GATE_EVENT_HIGH | UINT64_C(2);
    fatal.events = &unknownGatePayload;
    fatal.eventCount = 1;
    require(
        created.instance->process(fatal)
                == TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT
            && fixture.counts.processes == 0 && sample == -1.0,
        "an unknown gate payload bit should fail closed before module entry");
    fatal.events = nullptr;
    fatal.eventCount = 0;
    fixture.counts.processStatus = TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    require(
        created.instance->process(fatal) == TALAS_KERNEL_ABI_STATUS_INVALID_VALUE
            && !created.instance->isQuarantined()
            && fixture.counts.processes == 1 && sample == -1.0,
        "a controlled nonfatal rejection should map without output mutation or quarantine");

    fixture.counts.processStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
    require(
        created.instance->process(fatal) == TALAS_KERNEL_ABI_STATUS_FATAL
            && created.instance->isQuarantined()
            && fixture.counts.processes == 2 && sample == -1.0,
        "a controlled fatal status should quarantine without partial output");
    fixture.counts.processStatus = TALAS_KERNEL_ABI_STATUS_OK;
    require(
        created.instance->process(fatal)
                == TALAS_KERNEL_ABI_STATUS_QUARANTINED
            && fixture.counts.processes == 2 && sample == -1.0,
        "quarantine should reject later blocks without re-entering the module");

    fixture.counts.resetStatus = TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    TalasKernelAbiResetV1 resetRecord{};
    resetRecord.structSize = sizeof(resetRecord);
    resetRecord.reason = TALAS_KERNEL_ABI_RESET_EXPLICIT;
    resetRecord.transport = validTransport();
    require(
        created.instance->reset(resetRecord)
                == TALAS_KERNEL_ABI_STATUS_QUARANTINED
            && fixture.counts.resets == 0,
        "quarantine should also block later reset callbacks");
}

void testZeroLeaseRetirementCollectsAndUnloadsExactlyOnce()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    const auto registered = host.registerModule(fixture.module);
    require(registered.ok(), "zero-lease retirement fixture should register");
    require(
        host.requestModuleRetirement(
            "org.talas.builtin.abi-tests")
            == talas::runtime::KernelAbiRetirementStatus::Accepted,
        "the first retirement request should be accepted");
    require(
        registered.module->isRetiring()
            && registered.module->activeInstanceCount() == 0,
        "retirement should publish the zero-lease state");

    const auto rejected = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(
        !rejected.ok()
            && rejected.status == TALAS_KERNEL_ABI_STATUS_RETIRING
            && fixture.counts.creates == 0,
        "retirement should reject new instances before factory entry");
    require(
        host.collectRetiredModules() == 1 && host.moduleCount() == 0
            && host.retiringModuleCount() == 0
            && fixture.counts.unloads == 1,
        "a zero-lease retired module should collect and unload once");
    require(
        host.collectRetiredModules() == 0 && fixture.counts.unloads == 1,
        "repeated collection must not unload twice");
    require(
        host.requestModuleRetirement(
            "org.talas.builtin.abi-tests")
            == talas::runtime::KernelAbiRetirementStatus::UnknownModule,
        "a retirement request after collection should be stale");
}

void testActiveLeasesContinueUntilFinalOffCallbackCollection()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    const auto registered = host.registerModule(fixture.module);
    require(registered.ok(), "active-lease retirement fixture should register");
    auto first = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    auto second = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(
        first.ok() && second.ok()
            && registered.module->activeInstanceCount() == 2,
        "two prepared instances should hold two module leases");
    require(
        host.requestModuleRetirement(
            "org.talas.builtin.abi-tests")
            == talas::runtime::KernelAbiRetirementStatus::Accepted,
        "active module retirement should be accepted");
    require(
        host.requestModuleRetirement(
            "org.talas.builtin.abi-tests")
            == talas::runtime::KernelAbiRetirementStatus::AlreadyRetiring,
        "a duplicate active retirement should be idempotently classified");

    double sample = -1.0;
    TalasKernelAbiOutputV1 output{};
    output.structSize = sizeof(output);
    output.samples = &sample;
    output.sampleCount = 1;
    TalasKernelAbiProcessV1 block{};
    block.structSize = sizeof(block);
    block.frameCount = 1;
    block.outputs = &output;
    block.outputCount = 1;
    block.transport = validTransport();
    require(
        first.instance->process(block) == TALAS_KERNEL_ABI_STATUS_OK
            && sample == 0.25 && fixture.counts.processes == 1
            && fixture.counts.unloads == 0,
        "an existing lease should process after retirement without unloading");
    require(
        host.collectRetiredModules() == 0
            && fixture.counts.unloads == 0,
        "collection must wait for all active leases");

    require(
        host.retireInstance(first.instance)
            == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted,
        "the first active instance should enter the retirement queue");
    require(
        host.collectRetiredModules() == 0,
        "collect should destroy the first queued instance but keep the leased module");
    require(
        registered.module->activeInstanceCount() == 1
            && fixture.counts.unloads == 0,
        "one remaining lease should keep the module resident");
    require(
        host.retireInstance(second.instance)
            == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted,
        "the final active instance should enter the retirement queue");
    require(
        registered.module->activeInstanceCount() == 1
            && host.collectRetiredModules() == 1
            && registered.module->activeInstanceCount() == 0
            && fixture.counts.unloads == 1
            && fixture.counts.allocations
                == fixture.counts.deallocations,
        "final lease release should permit one balanced off-callback collection");
}

void testRetirementQueueBoundFailsWithoutChangingModuleState()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHostPolicy policy;
    policy.maximumRetiringModules = 0;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services,
        policy);
    const auto registered = host.registerModule(fixture.module);
    require(registered.ok(), "bounded-retirement fixture should register");
    require(
        host.requestModuleRetirement(
            "org.talas.builtin.abi-tests")
            == talas::runtime::KernelAbiRetirementStatus::QueueFull
            && !registered.module->isRetiring()
            && host.retiringModuleCount() == 0,
        "a full bounded queue should reject without changing module state");
    auto stillActive = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(stillActive.ok(),
        "queue rejection should leave the module available for creation");
}

void testInstanceRetirementQueueBoundPreservesActiveHandle()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHostPolicy policy;
    policy.maximumRetiredInstances = 0;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services,
        policy);
    require(host.registerModule(fixture.module).ok(),
        "bounded-instance fixture should register");
    auto active = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(active.ok(), "bounded-instance fixture should prepare");
    require(
        host.retireInstance(active.instance)
                == talas::runtime::KernelAbiInstanceRetirementStatus::QueueFull
            && host.retiredInstanceCount() == 0
            && fixture.counts.destroys == 0,
        "a full instance queue should preserve the active handle and opaque state");
}

void testRetirementHandleRejectsStaleAndMismatchedGenerations()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    require(host.registerModule(fixture.module).ok(),
        "generation fixture should register");
    auto first = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(first.ok(), "generation fixture should prepare the first instance");
    const auto stale = first.instance;
    require(
        host.retireInstance(first.instance)
                == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted
            && host.collectRetiredModules() == 0,
        "generation fixture should collect the first instance");

    auto second = host.createPreparedInstance(
        "org.talas.builtin.abi-tests",
        "talas.oscillator",
        validPrepare());
    require(second.ok(), "generation fixture should prepare the replacement");
    const auto mismatched = talas::runtime::KernelAbiInstanceHandle{
        second.instance.get(),
        stale.generation()};
    require(
        host.retireInstance(stale)
                == talas::runtime::KernelAbiInstanceRetirementStatus::UnknownInstance
            && host.retireInstance(mismatched)
                == talas::runtime::KernelAbiInstanceRetirementStatus::UnknownInstance,
        "stale or address-matched wrong generations must not retire a live replacement");
    require(
        host.retireInstance(second.instance)
            == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted,
        "the exact replacement handle should remain retireable");
}

void testRepeatedCreateRetireCollectLoopsRemainBalanced()
{
    ModuleFixture fixture;
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        fixture.services);
    constexpr std::size_t repetitions = 128;
    constexpr std::size_t instancesPerModule = 3;
    for (std::size_t repetition = 0; repetition < repetitions; ++repetition)
    {
        const auto registered = host.registerModule(fixture.module);
        require(registered.ok(), "stress module should re-register after collection");
        std::array<talas::runtime::KernelAbiInstanceHandle, instancesPerModule>
            instances{};
        for (auto& instance : instances)
        {
            auto created = host.createPreparedInstance(
                "org.talas.builtin.abi-tests",
                "talas.oscillator",
                validPrepare());
            require(created.ok(), "stress instance should prepare");
            instance = created.instance;
        }
        require(
            host.requestModuleRetirement(
                "org.talas.builtin.abi-tests")
                == talas::runtime::KernelAbiRetirementStatus::Accepted,
            "stress module retirement should be accepted");
        for (auto instance : instances)
        {
            require(
                host.retireInstance(instance)
                    == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted,
                "stress instance retirement should be accepted");
        }
        require(
            registered.module->activeInstanceCount() == instancesPerModule
                && host.collectRetiredModules() == 1
                && registered.module->activeInstanceCount() == 0,
            "stress collection should destroy all instances before unloading");
    }
    require(
        fixture.counts.creates == repetitions * instancesPerModule
            && fixture.counts.prepares == repetitions * instancesPerModule
            && fixture.counts.destroys == repetitions * instancesPerModule
            && fixture.counts.allocations == fixture.counts.deallocations
            && fixture.counts.unloads == repetitions,
        "repeated retirement should remain allocation-balanced with one unload per module");
}
void testCheckpointExtensionValidationBeforePublication()
{
    using Code = talas::runtime::KernelAbiValidationCode;
    const auto reject = [](Code code, auto mutate) {
        ModuleFixture fixture;
        mutate(fixture);
        talas::runtime::KernelAbiHost host(talas::core::makeBuiltinNodeRegistry(), fixture.services);
        const auto result = host.registerModule(fixture.module, &fixture.extension);
        require(!result.ok() && !result.diagnostics.empty() && result.diagnostics.front().code == code,
            "malformed checkpoint extension must reject with the expected diagnostic");
        require(host.moduleCount() == 0, "invalid extension must not publish the module");
        requireNoCallbacks(fixture.counts, "extension rejection must precede every lifecycle callback");
        require(host.registerModule(fixture.module).ok(), "rejection must leave both publication indexes available");
    };
    reject(Code::InvalidStructure, [](auto& f) { --f.extension.structSize; });
    reject(Code::InvalidStructure, [](auto& f) { ++f.extension.structSize; });
    reject(Code::UnsupportedVersion, [](auto& f) { ++f.extension.checkpointVersion; });
    reject(Code::InvalidReservedField, [](auto& f) { f.extension.reserved0 = 1; });
    reject(Code::InvalidReservedField, [](auto& f) { f.extension.reserved1 = 1; });
    reject(Code::InvalidMetadata, [](auto& f) { f.extension.moduleId = text("other"); });
    reject(Code::InvalidMetadata, [](auto& f) { f.extension.moduleVersion = text("2.0.0"); });
    reject(Code::InvalidMetadata, [](auto& f) { f.extension.moduleId = {nullptr, 5}; });
    reject(Code::InvalidStructure, [](auto& f) { f.extension.kernels = nullptr; });
    reject(Code::InvalidStructure, [](auto& f) { f.extension.kernelCount = 0; });
    reject(Code::InvalidStructure, [](auto& f) { f.extension.kernelCount = UINT64_MAX; });
    reject(Code::InvalidStructure, [](auto& f) { ++f.checkpoint.structSize; });
    reject(Code::UnsupportedVersion, [](auto& f) { ++f.checkpoint.checkpointVersion; });
    reject(Code::InvalidReservedField, [](auto& f) { f.checkpoint.reserved0 = 1; });
    reject(Code::InvalidReservedField, [](auto& f) { f.checkpoint.reserved1 = 1; });
    reject(Code::MissingCallback, [](auto& f) { f.checkpoint.capture = nullptr; });
    reject(Code::MissingCallback, [](auto& f) { f.checkpoint.restore = nullptr; });
    reject(Code::MissingCallback, [](auto& f) { f.checkpoint.destroyCheckpoint = nullptr; });
    reject(Code::InvalidMetadata, [](auto& f) { f.checkpoint.typeId = {nullptr, 7}; });
    reject(Code::UnknownNodeType, [](auto& f) { f.checkpoint.typeId = text("talas.filter"); });

    ModuleFixture legacy;
    talas::runtime::KernelAbiHost host(talas::core::makeBuiltinNodeRegistry(), legacy.services);
    require(host.registerModule(legacy.module).ok(), "legacy module remains accepted");
    const auto instance = host.createPreparedInstance("org.talas.builtin.abi-tests", "talas.oscillator", validPrepare());
    require(instance.ok() && !instance.instance->supportsOfflineCheckpoint()
        && instance.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE,
        "absence must never imply checkpoint capability");
}

void testCheckpointExtensionSubsetAndDuplicateIdentity()
{
    const auto& module = talas::runtime::builtinKernelAbiModule();
    auto extension = talas::runtime::builtinKernelAbiCheckpointExtension();
    std::array descriptors{extension.kernels[0], extension.kernels[1], extension.kernels[2]};
    extension.kernels = descriptors.data();
    ModuleFixture fixture;
    talas::runtime::KernelAbiHost host(talas::core::makeBuiltinNodeRegistry(), fixture.services);
    descriptors[1] = descriptors[0];
    const auto duplicate = host.registerModule(module, &extension);
    require(!duplicate.ok() && duplicate.diagnostics.front().code == talas::runtime::KernelAbiValidationCode::DuplicateIdentity
        && host.moduleCount() == 0, "duplicate extension type must reject without publication");
    requireNoCallbacks(fixture.counts, "duplicate checkpoint identity must reject before callbacks");
    extension.kernelCount = 1;
    require(host.registerModule(module, &extension).ok(), "explicit checkpoint subset should register");
    const auto oscillator = host.createPreparedInstance("org.talas.builtin-kernels", "talas.oscillator", validPrepare());
    const auto filter = host.createPreparedInstance("org.talas.builtin-kernels", "talas.filter", validPrepare());
    require(oscillator.ok() && filter.ok() && oscillator.instance->supportsOfflineCheckpoint()
        && !filter.instance->supportsOfflineCheckpoint()
        && filter.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE,
        "extension subset must not grant capabilities to omitted kernel types");
}

void testCheckpointCaptureCleanupCommitAndRetirement()
{
    for (const bool retire : {false, true})
    {
        ModuleFixture fixture;
        {
            talas::runtime::KernelAbiHost host(talas::core::makeBuiltinNodeRegistry(), fixture.services);
            require(host.registerModule(fixture.module, &fixture.extension).ok(), "checkpoint module should register");
            // Registration must own immutable callbacks, not this borrowed table.
            fixture.checkpoint.capture = nullptr;
            fixture.checkpoint.restore = nullptr;
            fixture.checkpoint.destroyCheckpoint = nullptr;
            const auto created = host.createPreparedInstance("org.talas.builtin.abi-tests", "talas.oscillator", validPrepare());
            require(created.ok() && created.instance->supportsOfflineCheckpoint(), "copied extension should survive table mutation");
            auto& instance = *created.instance.get();
            fixture.counts.captureStatus = TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR
                && !instance.isQuarantined() && fixture.counts.liveCheckpoints == 0
                && fixture.counts.checkpointDestroys == 1,
                "capture failure must destroy partial payload and preserve retry");
            fixture.counts.captureStatus = TALAS_KERNEL_ABI_STATUS_OK;
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "capture retry should succeed");
            const auto captures = fixture.counts.captures;
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT
                && fixture.counts.captures == captures, "nested capture must not enter module code");
            const auto allocations = fixture.counts.allocations;
            require(instance.restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK
                && fixture.counts.restores == 1 && fixture.counts.allocations == allocations,
                "restore must consume payload without allocation");
            require(instance.restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                "restoration cannot consume a baseline twice");
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "commit fixture capture");
            require(instance.commitOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK && fixture.counts.restores == 1,
                "commit must only release snapshot");
            require(instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "retirement fixture capture");
            if (retire)
            {
                require(host.requestModuleRetirement("org.talas.builtin.abi-tests")
                    == talas::runtime::KernelAbiRetirementStatus::Accepted, "module may retire with held snapshot");
                require(host.collectRetiredModules() == 0 && fixture.counts.liveCheckpoints == 1,
                    "active instance lease must retain snapshot and module code");
                require(host.retireInstance(created.instance) == talas::runtime::KernelAbiInstanceRetirementStatus::Accepted,
                    "instance retirement should transfer ownership");
                require(host.collectRetiredModules() == 1, "retired instance and snapshot must drain before module unload");
            }
        }
        require(fixture.counts.checkpointLifetimeValid && fixture.counts.liveCheckpoints == 0
            && fixture.counts.destroys == 1 && fixture.counts.unloads == 1
            && fixture.counts.allocations == fixture.counts.deallocations,
            "host destruction/retirement must release snapshot before instance and module");
    }
}

void testCheckpointFatalBoundariesNeverReviveInstance()
{
    for (int scenario = 0; scenario < 8; ++scenario)
    {
        ModuleFixture fixture;
        talas::runtime::KernelAbiHost host(talas::core::makeBuiltinNodeRegistry(), fixture.services);
        require(host.registerModule(fixture.module, &fixture.extension).ok(), "fatal fixture registration");
        const auto created = host.createPreparedInstance("org.talas.builtin.abi-tests", "talas.oscillator", validPrepare());
        require(created.ok(), "fatal fixture should prepare");
        auto& instance = *created.instance.get();
        if (scenario == 0) fixture.counts.throwCapture = true;
        if (scenario == 1) fixture.counts.captureStatus = UINT32_MAX;
        if (scenario == 2) fixture.counts.nullCheckpoint = true;
        const auto capture = instance.beginOfflineCheckpoint();
        if (scenario <= 2) require(capture == TALAS_KERNEL_ABI_STATUS_FATAL, "malformed/throwing capture must quarantine");
        else
        {
            require(capture == TALAS_KERNEL_ABI_STATUS_OK, "fatal boundary needs a valid baseline");
            if (scenario == 3) fixture.counts.restoreStatus = TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
            if (scenario == 4) fixture.counts.throwRestore = true;
            if (scenario == 5 || scenario == 7) fixture.counts.throwCheckpointDestroy = true;
            if (scenario == 6)
            {
                fixture.counts.resetStatus = TALAS_KERNEL_ABI_STATUS_FATAL;
                TalasKernelAbiResetV1 reset{};
                reset.structSize = sizeof(reset); reset.reason = TALAS_KERNEL_ABI_RESET_EXPLICIT; reset.transport = validTransport();
                require(instance.reset(reset) == TALAS_KERNEL_ABI_STATUS_FATAL, "processing authority should quarantine");
            }
            const auto status = scenario == 7 ? instance.commitOfflineCheckpoint() : instance.restoreOfflineCheckpoint();
            require(status == (scenario == 6 ? TALAS_KERNEL_ABI_STATUS_QUARANTINED : TALAS_KERNEL_ABI_STATUS_FATAL),
                "restore/cleanup failure must retain fatal authority");
            if (scenario == 6) require(fixture.counts.restores == 0, "quarantined DSP must never enter restore callback");
        }
        require(instance.isQuarantined() && instance.supportsOfflineCheckpoint()
            && fixture.counts.liveCheckpoints == 0
            && instance.beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_QUARANTINED,
            "checkpoint failure must release payload without reviving authority");
    }
}

}

int main()
{
    testCheckpointExtensionValidationBeforePublication();
    testCheckpointExtensionSubsetAndDuplicateIdentity();
    testCheckpointCaptureCleanupCommitAndRetirement();
    testCheckpointFatalBoundariesNeverReviveInstance();
    testExactModuleRegistersWithoutLifecycleCalls();
    testHostDestructionDrainsInstancesBeforeUnloadingModules();
    testCopiedStandaloneRecordsAdvertiseOnlyRetainedPrefix();
    testStructureVersionAndPlatformMismatchesReject();
    testTrustMetadataCallbacksAndCapabilitiesRejectBeforeFactory();
    testIdentityAndHostValidationRejectsDeterministically();
    testPreparedLifecycleOwnsMemoryAndKeepsProcessCallbackClean();
    testPartialCreateAndPrepareFailuresCleanUpWithoutPublication();
    testValidationAndFatalQuarantineDoNotLeakPartialOutput();
    testZeroLeaseRetirementCollectsAndUnloadsExactlyOnce();
    testActiveLeasesContinueUntilFinalOffCallbackCollection();
    testRetirementQueueBoundFailsWithoutChangingModuleState();
    testInstanceRetirementQueueBoundPreservesActiveHandle();
    testRetirementHandleRejectsStaleAndMismatchedGenerations();
    testRepeatedCreateRetireCollectLoopsRemainBalanced();
    return 0;
}
