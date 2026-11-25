#include "runtime/macos/ComponentIsolationXpcEnvelope.h"

#include <xpc/xpc.h>

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace
{
using namespace talas::runtime;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ComponentIsolationHandshakeV1 handshake()
{
    ComponentIsolationHandshakeV1 value;
    value.header.magic = componentIsolationProtocolMagicV1;
    value.header.protocolVersion = currentComponentIsolationProtocolVersion;
    value.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::HandshakeRequest);
    value.header.recordBytes = sizeof(value);
    value.header.sessionGeneration = 3;
    value.header.validationNonce.fill(1);
    value.candidateFingerprintDigest = componentIsolationProductFingerprintV1(
        ComponentIsolationProductServiceV1::GainFixture);
    value.sharedStorageIdentity = 5;
    value.profileVersion = currentComponentIsolationProfileVersion;
    value.requestedPermissions = componentPermission(
        ComponentExecutionPermission::RealtimeAudio);
    value.inputChannelCount = 2;
    value.outputChannelCount = 2;
    value.maximumFrames = 64;
    value.controlPayloadCapacityBytes = 128;
    return value;
}

ComponentIsolationControlRecordV1 control()
{
    const auto source = handshake();
    ComponentIsolationControlRecordV1 value;
    value.header = source.header;
    value.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::Heartbeat);
    value.header.recordBytes = sizeof(value);
    value.sharedStorageIdentity = source.sharedStorageIdentity;
    value.controlSequence = 1;
    return value;
}

void testExactHandshakeAndControlRecordsRoundTrip()
{
    const auto handshakeValue = handshake();
    void* region = mmap(
        nullptr,
        4'096,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_SHARED,
        -1,
        0);
    require(region != MAP_FAILED, "shared test page should map");
    auto sharedMemory = xpc_shmem_create(region, 4'096);
    require(sharedMemory != nullptr, "mapped test page should box as XPC shmem");
    auto message = createComponentIsolationXpcMessageV1(
        handshakeValue,
        sharedMemory);
    require(message != nullptr, "handshake envelope allocation should succeed");
    ComponentIsolationXpcEnvelopeV1 decoded;
    require(
        decodeComponentIsolationXpcEnvelopeV1(message, decoded)
                == ComponentIsolationXpcEnvelopeDiagnosticV1::Valid
            && decoded.kind == ComponentIsolationXpcEnvelopeKindV1::Handshake
            && decoded.sharedMemory != nullptr
            && xpc_get_type(decoded.sharedMemory) == XPC_TYPE_SHMEM
            && std::memcmp(
                &decoded.handshake,
                &handshakeValue,
                sizeof(handshakeValue)) == 0,
        "exact handshake bytes should round-trip without interpretation");
    xpc_release(message);
    xpc_release(sharedMemory);
    require(munmap(region, 4'096) == 0, "shared test page should unmap");

    const auto controlValue = control();
    message = createComponentIsolationXpcMessageV1(controlValue);
    require(
        decodeComponentIsolationXpcEnvelopeV1(message, decoded)
                == ComponentIsolationXpcEnvelopeDiagnosticV1::Valid
            && decoded.kind == ComponentIsolationXpcEnvelopeKindV1::Control
            && std::memcmp(
                &decoded.control,
                &controlValue,
                sizeof(controlValue)) == 0,
        "exact control bytes should round-trip without interpretation");
    xpc_release(message);
}

void testWrongObjectKeysCountsAndSizesReject()
{
    ComponentIsolationXpcEnvelopeV1 decoded;
    auto text = xpc_string_create("not a dictionary");
    require(
        decodeComponentIsolationXpcEnvelopeV1(text, decoded)
            == ComponentIsolationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid,
        "non-dictionary XPC objects should reject");
    xpc_release(text);

    auto dictionary = xpc_dictionary_create_empty();
    require(
        decodeComponentIsolationXpcEnvelopeV1(dictionary, decoded)
            == ComponentIsolationXpcEnvelopeDiagnosticV1::FieldCountInvalid,
        "empty dictionaries should reject");
    xpc_release(dictionary);

    dictionary = xpc_dictionary_create_empty();
    const std::uint32_t marker = 7;
    xpc_dictionary_set_data(dictionary, "unknown", &marker, sizeof(marker));
    require(
        decodeComponentIsolationXpcEnvelopeV1(dictionary, decoded)
            == ComponentIsolationXpcEnvelopeDiagnosticV1::RecordMissing,
        "unknown dictionary keys should not become records");
    xpc_release(dictionary);

    void* region = mmap(
        nullptr,
        4'096,
        PROT_READ | PROT_WRITE,
        MAP_ANON | MAP_SHARED,
        -1,
        0);
    require(region != MAP_FAILED, "malformed-envelope shared page should map");
    auto sharedMemory = xpc_shmem_create(region, 4'096);
    auto message = createComponentIsolationXpcMessageV1(
        handshake(),
        sharedMemory);
    xpc_dictionary_set_data(message, "extra", &marker, sizeof(marker));
    require(
        decodeComponentIsolationXpcEnvelopeV1(message, decoded)
            == ComponentIsolationXpcEnvelopeDiagnosticV1::FieldCountInvalid,
        "extra fields should reject even with one valid record");
    xpc_release(message);
    xpc_release(sharedMemory);
    require(munmap(region, 4'096) == 0, "malformed-envelope page should unmap");

    dictionary = xpc_dictionary_create_empty();
    const auto handshakeValue = handshake();
    xpc_dictionary_set_data(
        dictionary,
        "talas.component.record.v1",
        &handshakeValue,
        sizeof(handshakeValue));
    require(
        decodeComponentIsolationXpcEnvelopeV1(dictionary, decoded)
            == ComponentIsolationXpcEnvelopeDiagnosticV1::SharedMemoryInvalid,
        "a handshake request without explicit shared storage should reject");
    xpc_release(dictionary);

    dictionary = xpc_dictionary_create_empty();
    xpc_dictionary_set_data(dictionary, "talas.component.record.v1", &marker, 3);
    require(
        decodeComponentIsolationXpcEnvelopeV1(dictionary, decoded)
            == ComponentIsolationXpcEnvelopeDiagnosticV1::RecordSizeInvalid,
        "truncated record blobs should reject before memcpy");
    xpc_release(dictionary);
}

void testDiagnosticBlobMustExactlyMatchTheValidatedRecord()
{
    ComponentIsolationProductHelperResultV1 result;
    result.action = ComponentIsolationProductHelperActionV1::Respond;
    result.responseKind =
        ComponentIsolationProductHelperResponseKindV1::Control;
    result.control = control();
    result.control.header.recordKind = static_cast<std::uint16_t>(
        ComponentIsolationRecordKind::TeardownResponse);
    result.control.outcome = static_cast<std::uint32_t>(
        ComponentIsolationControlOutcome::ControlledFailure);
    result.control.diagnosticBytes = 3;
    result.diagnosticBytes = 3;
    result.diagnosticStorage[0] = std::byte{1};
    result.diagnosticStorage[1] = std::byte{2};
    result.diagnosticStorage[2] = std::byte{3};

    auto dictionary = xpc_dictionary_create_empty();
    require(
        writeComponentIsolationXpcResponseV1(dictionary, result),
        "bounded typed response should encode");
    ComponentIsolationXpcEnvelopeV1 decoded;
    require(
        decodeComponentIsolationXpcEnvelopeV1(dictionary, decoded)
                == ComponentIsolationXpcEnvelopeDiagnosticV1::Valid
            && decoded.diagnosticBytes == 3
            && decoded.diagnosticStorage[2] == std::byte{3},
        "bounded diagnostics should decode only at their exact declared size");
    xpc_release(dictionary);

    result.control.diagnosticBytes = 4;
    dictionary = xpc_dictionary_create_empty();
    require(
        !writeComponentIsolationXpcResponseV1(dictionary, result),
        "contradictory result-side diagnostic lengths should not encode");
    xpc_release(dictionary);

    auto message = createComponentIsolationXpcMessageV1(control());
    const std::byte unexpected{1};
    xpc_dictionary_set_data(
        message,
        "talas.component.diagnostic.v1",
        &unexpected,
        1);
    require(
        decodeComponentIsolationXpcEnvelopeV1(message, decoded)
            == ComponentIsolationXpcEnvelopeDiagnosticV1::FieldCountInvalid,
        "a diagnostic cannot be smuggled beside a zero-length record claim");
    xpc_release(message);
}
}

int main()
{
    testExactHandshakeAndControlRecordsRoundTrip();
    testWrongObjectKeysCountsAndSizesReject();
    testDiagnosticBlobMustExactlyMatchTheValidatedRecord();
    return 0;
}
