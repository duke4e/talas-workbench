#ifndef TALAS_RUNTIME_KERNEL_ABI_H
#define TALAS_RUNTIME_KERNEL_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TALAS_KERNEL_ABI_VERSION_V1 UINT32_C(1)
#define TALAS_KERNEL_API_VERSION_V1 UINT32_C(1)
#define TALAS_KERNEL_API_VERSION_V2 UINT32_C(2)
#define TALAS_KERNEL_API_VERSION_V3 UINT32_C(3)
#define TALAS_KERNEL_API_VERSION_V4 UINT32_C(4)
#define TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1 UINT32_C(1)
#define TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V1 UINT32_C(1)
#define TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V2 UINT32_C(2)
#define TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V3 UINT32_C(3)
#define TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V4 UINT32_C(4)

typedef uint32_t TalasKernelAbiStatusV1;
#define TALAS_KERNEL_ABI_STATUS_OK UINT32_C(0)
#define TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT UINT32_C(1)
#define TALAS_KERNEL_ABI_STATUS_INCOMPATIBLE UINT32_C(2)
#define TALAS_KERNEL_ABI_STATUS_NOT_PREPARED UINT32_C(3)
#define TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT UINT32_C(4)
#define TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION UINT32_C(5)
#define TALAS_KERNEL_ABI_STATUS_INVALID_VALUE UINT32_C(6)
#define TALAS_KERNEL_ABI_STATUS_FATAL UINT32_C(7)
#define TALAS_KERNEL_ABI_STATUS_QUARANTINED UINT32_C(8)
#define TALAS_KERNEL_ABI_STATUS_RETIRING UINT32_C(9)
#define TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR UINT32_C(10)

typedef uint64_t TalasKernelAbiSampleFormatMaskV1;
#define TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64 UINT64_C(1)

typedef uint32_t TalasKernelAbiTrustClassV1;
#define TALAS_KERNEL_ABI_TRUST_BUILTIN UINT32_C(1)
#define TALAS_KERNEL_ABI_TRUST_OWNER_APPROVED_TEST UINT32_C(2)
#define TALAS_KERNEL_ABI_TRUST_UNTRUSTED UINT32_C(3)

typedef uint64_t TalasKernelAbiCapabilityFlagsV1;
#define TALAS_KERNEL_ABI_CAPABILITY_RESET UINT64_C(1)
#define TALAS_KERNEL_ABI_CAPABILITY_STATEFUL UINT64_C(2)
#define TALAS_KERNEL_ABI_CAPABILITY_TRANSPORT UINT64_C(4)
#define TALAS_KERNEL_ABI_CAPABILITY_TYPED_EVENTS UINT64_C(8)
#define TALAS_KERNEL_ABI_CAPABILITY_KNOWN_MASK UINT64_C(15)

typedef uint32_t TalasKernelAbiDiagnosticSeverityV1;
#define TALAS_KERNEL_ABI_DIAGNOSTIC_INFO UINT32_C(1)
#define TALAS_KERNEL_ABI_DIAGNOSTIC_WARNING UINT32_C(2)
#define TALAS_KERNEL_ABI_DIAGNOSTIC_ERROR UINT32_C(3)

typedef uint32_t TalasKernelAbiParameterKindV1;
#define TALAS_KERNEL_ABI_PARAMETER_SCALAR UINT32_C(1)
#define TALAS_KERNEL_ABI_PARAMETER_INTEGER UINT32_C(2)
#define TALAS_KERNEL_ABI_PARAMETER_TEXT UINT32_C(3)
#define TALAS_KERNEL_ABI_PARAMETER_RESOURCE UINT32_C(4)

typedef uint32_t TalasKernelAbiImmutableResourceKindV2;
#define TALAS_KERNEL_ABI_RESOURCE_TABLE UINT32_C(1)
#define TALAS_KERNEL_ABI_RESOURCE_BINARY UINT32_C(2)

typedef uint32_t TalasKernelAbiImmutableResourceElementFormatV2;
#define TALAS_KERNEL_ABI_RESOURCE_ELEMENT_F64_LITTLE_ENDIAN UINT32_C(1)
#define TALAS_KERNEL_ABI_RESOURCE_ELEMENT_RAW_BYTES UINT32_C(2)
#define TALAS_KERNEL_ABI_RESOURCE_DIGEST_BYTES_V2 UINT32_C(32)

typedef uint64_t TalasKernelAbiAudioStreamTokenV3;

typedef uint32_t TalasKernelAbiAudioStreamReadOutcomeV3;
#define TALAS_KERNEL_ABI_AUDIO_STREAM_READ_COMPLETE UINT32_C(1)
#define TALAS_KERNEL_ABI_AUDIO_STREAM_READ_UNDERRUN UINT32_C(2)
#define TALAS_KERNEL_ABI_AUDIO_STREAM_READ_END_OF_STREAM UINT32_C(3)

typedef uint32_t TalasKernelAbiSignalKindV1;
#define TALAS_KERNEL_ABI_SIGNAL_CONSTANT UINT32_C(1)
#define TALAS_KERNEL_ABI_SIGNAL_PER_FRAME UINT32_C(2)

typedef uint32_t TalasKernelAbiEventKindV1;
#define TALAS_KERNEL_ABI_EVENT_GATE UINT32_C(1)
#define TALAS_KERNEL_ABI_GATE_EVENT_HIGH UINT64_C(1)
#define TALAS_KERNEL_ABI_MAX_PROCESS_EVENTS_V1 UINT64_C(65536)

typedef uint32_t TalasKernelAbiProcessFlagsV1;
#define TALAS_KERNEL_ABI_PROCESS_TYPED_EVENTS UINT32_C(1)
#define TALAS_KERNEL_ABI_PROCESS_KNOWN_MASK UINT32_C(1)

typedef uint32_t TalasKernelAbiResetReasonV1;
#define TALAS_KERNEL_ABI_RESET_EXPLICIT UINT32_C(1)
#define TALAS_KERNEL_ABI_RESET_TRANSPORT UINT32_C(2)
#define TALAS_KERNEL_ABI_RESET_GRAPH_ACTIVATION UINT32_C(3)

typedef struct TalasKernelAbiStringViewV1
{
    const char* data;
    uint64_t byteCount;
} TalasKernelAbiStringViewV1;

typedef struct TalasKernelAbiByteViewV1
{
    const uint8_t* data;
    uint64_t byteCount;
} TalasKernelAbiByteViewV1;

typedef struct TalasKernelAbiCompatibilityV1
{
    uint32_t structSize;
    uint32_t abiVersion;
    uint32_t pointerBits;
    uint32_t littleEndian;
    uint32_t doubleBits;
    uint32_t doubleIec559;
    uint32_t maximumAlignment;
    uint32_t reserved0;
} TalasKernelAbiCompatibilityV1;

typedef void* (*TalasKernelAbiAllocateV1)(
    void* hostContext,
    uint64_t byteCount,
    uint64_t alignment);

typedef void (*TalasKernelAbiDeallocateV1)(
    void* hostContext,
    void* allocation,
    uint64_t byteCount,
    uint64_t alignment);

typedef void (*TalasKernelAbiDiagnosticV1)(
    void* hostContext,
    TalasKernelAbiDiagnosticSeverityV1 severity,
    uint32_t code,
    TalasKernelAbiStringViewV1 message);

typedef struct TalasKernelAbiImmutableResourceViewV2
{
    uint32_t structSize;
    TalasKernelAbiImmutableResourceKindV2 kind;
    TalasKernelAbiImmutableResourceElementFormatV2 elementFormat;
    uint32_t reserved0;
    TalasKernelAbiStringViewV1 id;
    uint64_t generation;
    uint8_t sourceContentDigest[TALAS_KERNEL_ABI_RESOURCE_DIGEST_BYTES_V2];
    uint8_t mediatedPayloadDigest[TALAS_KERNEL_ABI_RESOURCE_DIGEST_BYTES_V2];
    TalasKernelAbiByteViewV1 bytes;
    uint64_t elementCount;
    uint64_t reserved1[2];
} TalasKernelAbiImmutableResourceViewV2;

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiResolveImmutableResourceV2)(
    void* hostContext,
    TalasKernelAbiStringViewV1 id,
    TalasKernelAbiImmutableResourceViewV2* resource);

typedef struct TalasKernelAbiImmutableAudioStreamPrepareV3
{
    uint32_t structSize;
    uint32_t apiVersion;
    TalasKernelAbiStringViewV1 id;
    uint64_t generation;
    uint8_t sourceContentDigest[TALAS_KERNEL_ABI_RESOURCE_DIGEST_BYTES_V2];
    uint32_t channelCount;
    uint32_t sampleRate;
    uint64_t frameCount;
    double initialSourceFrame;
    double maximumSourceFrameIncrement;
    uint64_t maximumReadFrames;
    uint64_t reserved0[2];
} TalasKernelAbiImmutableAudioStreamPrepareV3;

typedef struct TalasKernelAbiImmutableAudioStreamViewV3
{
    uint32_t structSize;
    uint32_t apiVersion;
    TalasKernelAbiStringViewV1 id;
    uint64_t generation;
    uint8_t sourceContentDigest[TALAS_KERNEL_ABI_RESOURCE_DIGEST_BYTES_V2];
    uint32_t channelCount;
    uint32_t sampleRate;
    uint64_t frameCount;
    TalasKernelAbiAudioStreamTokenV3 streamToken;
    double maximumSourceFrameIncrement;
    uint64_t maximumReadFrames;
    uint64_t reserved0[2];
} TalasKernelAbiImmutableAudioStreamViewV3;

typedef struct TalasKernelAbiImmutableAudioStreamReadV3
{
    uint32_t structSize;
    uint32_t apiVersion;
    TalasKernelAbiAudioStreamTokenV3 streamToken;
    double absoluteSourceFrame;
    double sourceFrameIncrement;
    uint64_t frameCount;
    uint32_t outputChannelCount;
    uint32_t reserved0;
    double* interleavedOutput;
    uint64_t outputSampleCapacity;
    uint64_t reserved1[2];
} TalasKernelAbiImmutableAudioStreamReadV3;

typedef struct TalasKernelAbiImmutableAudioStreamReadResultV3
{
    uint32_t structSize;
    TalasKernelAbiAudioStreamReadOutcomeV3 outcome;
    uint64_t frameCount;
    uint64_t missingFrameCount;
    uint64_t endOfStreamFrameCount;
    uint64_t seekGeneration;
    double expectedNextSourceFrame;
    uint64_t reserved0[2];
} TalasKernelAbiImmutableAudioStreamReadResultV3;

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiResolveImmutableAudioStreamV3)(
    void* hostContext,
    const TalasKernelAbiImmutableAudioStreamPrepareV3* prepare,
    TalasKernelAbiImmutableAudioStreamViewV3* stream);

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiReadImmutableAudioStreamV3)(
    void* hostContext,
    const TalasKernelAbiImmutableAudioStreamReadV3* read,
    TalasKernelAbiImmutableAudioStreamReadResultV3* result);

typedef struct TalasKernelAbiHostServicesV1
{
    uint32_t structSize;
    uint32_t servicesVersion;
    void* hostContext;
    TalasKernelAbiAllocateV1 allocate;
    TalasKernelAbiDeallocateV1 deallocate;
    TalasKernelAbiDiagnosticV1 diagnostic;
    uint64_t reserved0;
    uint64_t reserved1;
} TalasKernelAbiHostServicesV1;

typedef struct TalasKernelAbiHostServicesV2
{
    TalasKernelAbiHostServicesV1 v1;
    TalasKernelAbiResolveImmutableResourceV2 resolveImmutableResource;
    uint64_t reserved2;
    uint64_t reserved3;
} TalasKernelAbiHostServicesV2;

typedef struct TalasKernelAbiHostServicesV3
{
    TalasKernelAbiHostServicesV2 v2;
    TalasKernelAbiResolveImmutableAudioStreamV3 resolveImmutableAudioStream;
    TalasKernelAbiReadImmutableAudioStreamV3 readImmutableAudioStream;
    uint64_t reserved4;
    uint64_t reserved5;
} TalasKernelAbiHostServicesV3;

// V4 is a closed profile marker for several manifest-declared immutable
// resources. It adds no enumeration or arbitrary resource service; exact-ID
// v2/v3 functions remain the complete callable surface.
typedef struct TalasKernelAbiHostServicesV4
{
    TalasKernelAbiHostServicesV3 v3;
    uint64_t reserved6;
    uint64_t reserved7;
} TalasKernelAbiHostServicesV4;

typedef struct TalasKernelAbiParameterV1
{
    uint32_t structSize;
    TalasKernelAbiParameterKindV1 kind;
    TalasKernelAbiStringViewV1 id;
    double scalarValue;
    int64_t integerValue;
    TalasKernelAbiStringViewV1 textValue;
    uint64_t reserved0;
} TalasKernelAbiParameterV1;

typedef struct TalasKernelAbiPrepareV1
{
    uint32_t structSize;
    uint32_t apiVersion;
    double sampleRate;
    uint64_t maximumBlockFrames;
    const TalasKernelAbiParameterV1* parameters;
    uint64_t parameterCount;
    const TalasKernelAbiHostServicesV1* host;
    uint64_t reserved0;
    uint64_t reserved1;
} TalasKernelAbiPrepareV1;

typedef struct TalasKernelAbiSignalV1
{
    uint32_t structSize;
    TalasKernelAbiSignalKindV1 kind;
    double constantValue;
    const double* samples;
    uint64_t sampleCount;
    uint64_t reserved0;
} TalasKernelAbiSignalV1;

typedef struct TalasKernelAbiOutputV1
{
    uint32_t structSize;
    uint32_t reserved0;
    double* samples;
    uint64_t sampleCount;
    uint64_t reserved1;
} TalasKernelAbiOutputV1;

typedef struct TalasKernelAbiEventV1
{
    uint32_t structSize;
    TalasKernelAbiEventKindV1 kind;
    uint32_t targetIndex;
    uint32_t reserved0;
    uint64_t frameOffset;
    uint64_t sequence;
    uint64_t payloadBits;
    uint64_t reserved1;
} TalasKernelAbiEventV1;

typedef struct TalasKernelAbiTransportV1
{
    uint32_t structSize;
    uint32_t playing;
    uint32_t loopEnabled;
    uint32_t reserved0;
    uint64_t epoch;
    uint64_t absoluteFrame;
    uint64_t playheadFrame;
    uint64_t loopStartFrame;
    uint64_t loopEndFrame;
    uint64_t discontinuitySequence;
    double sampleRate;
    uint64_t reserved1;
} TalasKernelAbiTransportV1;

typedef struct TalasKernelAbiResetV1
{
    uint32_t structSize;
    TalasKernelAbiResetReasonV1 reason;
    TalasKernelAbiTransportV1 transport;
    uint64_t reserved0;
} TalasKernelAbiResetV1;

typedef struct TalasKernelAbiProcessV1
{
    uint32_t structSize;
    uint32_t flags;
    uint64_t frameCount;
    const TalasKernelAbiSignalV1* inputs;
    uint64_t inputCount;
    const TalasKernelAbiOutputV1* outputs;
    uint64_t outputCount;
    const TalasKernelAbiEventV1* events;
    uint64_t eventCount;
    TalasKernelAbiTransportV1 transport;
    uint64_t reserved0;
    uint64_t reserved1;
} TalasKernelAbiProcessV1;

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiCreateV1)(
    const TalasKernelAbiHostServicesV1* host,
    void** instance);

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiPrepareInstanceV1)(
    void* instance,
    const TalasKernelAbiPrepareV1* prepare);

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiResetInstanceV1)(
    void* instance,
    const TalasKernelAbiResetV1* reset);

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiProcessInstanceV1)(
    void* instance,
    const TalasKernelAbiProcessV1* process);

typedef void (*TalasKernelAbiDestroyV1)(
    void* instance,
    const TalasKernelAbiHostServicesV1* host);

typedef struct TalasKernelAbiDescriptorV1
{
    uint32_t structSize;
    uint32_t descriptorVersion;
    TalasKernelAbiStringViewV1 typeId;
    TalasKernelAbiCapabilityFlagsV1 capabilityFlags;
    TalasKernelAbiCreateV1 create;
    TalasKernelAbiPrepareInstanceV1 prepare;
    TalasKernelAbiResetInstanceV1 reset;
    TalasKernelAbiProcessInstanceV1 process;
    TalasKernelAbiDestroyV1 destroy;
    uint64_t reserved0;
    uint64_t reserved1;
} TalasKernelAbiDescriptorV1;

typedef void (*TalasKernelAbiModuleUnloadV1)(
    void* moduleContext,
    const TalasKernelAbiHostServicesV1* host);

typedef struct TalasKernelAbiModuleV1
{
    uint32_t structSize;
    uint32_t descriptorVersion;
    TalasKernelAbiStringViewV1 moduleId;
    TalasKernelAbiStringViewV1 moduleVersion;
    uint32_t minimumApiVersion;
    uint32_t maximumApiVersion;
    TalasKernelAbiCompatibilityV1 compatibility;
    TalasKernelAbiSampleFormatMaskV1 sampleFormats;
    TalasKernelAbiTrustClassV1 trustClass;
    uint32_t reserved0;
    TalasKernelAbiCapabilityFlagsV1 capabilityFlags;
    const TalasKernelAbiDescriptorV1* kernels;
    uint64_t kernelCount;
    void* moduleContext;
    TalasKernelAbiModuleUnloadV1 unload;
    uint64_t reserved1;
    uint64_t reserved2;
} TalasKernelAbiModuleV1;

/* Optional exclusive offline/control-thread processing checkpoint extension.
 * Passed separately from the frozen V1 module and descriptor array. Payloads
 * remain in the kernel's process and never represent serialized pointer data.
 * Capture must preserve processing state on failure. A non-null payload is
 * owned by the host even on failure and is released through destroyCheckpoint.
 * Restore and destroyCheckpoint must not allocate or throw. Restore changes
 * processing state only; authorization/quarantine is host-owned authority.
 * Host services, instance and module code outlive every checkpoint callback.
 */
#define TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1 UINT32_C(1)

typedef TalasKernelAbiStatusV1 (*TalasKernelAbiCaptureCheckpointV1)(
    void* instance,
    const TalasKernelAbiHostServicesV1* host,
    void** checkpoint);
typedef TalasKernelAbiStatusV1 (*TalasKernelAbiRestoreCheckpointV1)(
    void* instance,
    const void* checkpoint);
typedef void (*TalasKernelAbiDestroyCheckpointV1)(
    void* checkpoint,
    const TalasKernelAbiHostServicesV1* host);

typedef struct TalasKernelAbiCheckpointDescriptorV1
{
    uint32_t structSize;
    uint32_t checkpointVersion;
    TalasKernelAbiStringViewV1 typeId;
    TalasKernelAbiCaptureCheckpointV1 capture;
    TalasKernelAbiRestoreCheckpointV1 restore;
    TalasKernelAbiDestroyCheckpointV1 destroyCheckpoint;
    uint64_t reserved0;
    uint64_t reserved1;
} TalasKernelAbiCheckpointDescriptorV1;

typedef struct TalasKernelAbiCheckpointExtensionV1
{
    uint32_t structSize;
    uint32_t checkpointVersion;
    TalasKernelAbiStringViewV1 moduleId;
    TalasKernelAbiStringViewV1 moduleVersion;
    const TalasKernelAbiCheckpointDescriptorV1* kernels;
    uint64_t kernelCount;
    uint64_t reserved0;
    uint64_t reserved1;
} TalasKernelAbiCheckpointExtensionV1;

/* Optional metadata-only entry in an already authenticated component image.
 * No host callbacks, allocations or lifecycle operations are allowed here. */
#define TALAS_KERNEL_ABI_CHECKPOINT_ENTRY_SYMBOL_V1 "talas_component_checkpoint_entry_v1"
typedef TalasKernelAbiStatusV1 (*TalasKernelAbiCheckpointEntryPointV1)(
    const TalasKernelAbiCheckpointExtensionV1** extension);

#ifdef __cplusplus
}
#endif

#endif
