#include "runtime/component_probe/macos/ComponentBundleProbeXpcEnvelope.h"

#include <xpc/xpc.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace
{
namespace probe = talas::runtime::component_probe;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

probe::ComponentBundleProbeRequestV1 request(std::size_t entryBytes)
{
    probe::ComponentBundleProbeRequestV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Request,
        sizeof(value));
    constexpr std::string_view bundleId = "org.talas.fixture";
    constexpr std::string_view version = "1.2.3";
    value.bundleIdBytes = bundleId.size();
    value.semanticVersionBytes = version.size();
    std::copy(bundleId.begin(), bundleId.end(), value.bundleId.begin());
    std::copy(version.begin(), version.end(), value.semanticVersion.begin());
    value.contentDigest.fill(0x11);
    value.signerIdentityDigest.fill(0x22);
    value.requestedPermissions = talas::runtime::componentPermission(
        talas::runtime::ComponentExecutionPermission::RealtimeAudio);
    value.declaredRoles = talas::runtime::componentRole(
        talas::runtime::ComponentExecutionRole::RealtimeProcessor);
    value.policyVersion = talas::runtime::currentComponentTrustPolicyVersion;
    value.entryByteCount = entryBytes;
    value.artifactGeneration = 17;
    value.validationNonce.fill(0x33);
    return value;
}

probe::ComponentBundleProbeExpectedAbiV1 expectedAbi(
    const probe::ComponentBundleProbeRequestV1& requestValue)
{
    probe::ComponentBundleProbeExpectedAbiV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::ExpectedAbi,
        sizeof(value));
    value.entryDigest.fill(0x44);
    value.entryByteCount = requestValue.entryByteCount;
    value.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    constexpr std::string_view nodeId =
        "org.talas.fixture.oscillator";
    value.nodeCount = 1;
    value.nodeIdBytes[0] = nodeId.size();
    std::copy(nodeId.begin(), nodeId.end(), value.nodeIds[0].begin());
    return value;
}

probe::ComponentBundleProbeResultV1 cleanResult(
    const probe::ComponentBundleProbeRequestV1& requestValue)
{
    probe::ComponentBundleProbeResultV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Result,
        sizeof(value));
    value.state = static_cast<std::uint32_t>(
        talas::runtime::ComponentProbeState::Clean);
    value.diagnostic = static_cast<std::uint32_t>(
        probe::ComponentBundleProbeResultDiagnosticV1::Ready);
    value.contentDigest = requestValue.contentDigest;
    value.artifactGeneration = requestValue.artifactGeneration;
    value.validationNonce = requestValue.validationNonce;
    return value;
}

probe::ComponentBundleProbeStartedV1 started(
    const probe::ComponentBundleProbeRequestV1& requestValue)
{
    probe::ComponentBundleProbeStartedV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Started,
        sizeof(value));
    value.contentDigest = requestValue.contentDigest;
    value.artifactGeneration = requestValue.artifactGeneration;
    value.validationNonce = requestValue.validationNonce;
    value.processIdentifier = 4321;
    value.effectiveUser = 501;
    value.startSeconds = 1'234'567;
    value.startMicroseconds = 123'456;
    return value;
}

probe::ComponentBundleProbeAcknowledgementV1 acknowledgement(
    const probe::ComponentBundleProbeRequestV1& requestValue)
{
    probe::ComponentBundleProbeAcknowledgementV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Acknowledgement,
        sizeof(value));
    value.contentDigest = requestValue.contentDigest;
    value.artifactGeneration = requestValue.artifactGeneration;
    value.validationNonce = requestValue.validationNonce;
    return value;
}

probe::ComponentBundleProbeFinalizationV1 finalization(
    const probe::ComponentBundleProbeRequestV1& requestValue)
{
    probe::ComponentBundleProbeFinalizationV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::Finalization,
        sizeof(value));
    value.contentDigest = requestValue.contentDigest;
    value.artifactGeneration = requestValue.artifactGeneration;
    value.validationNonce = requestValue.validationNonce;
    return value;
}

probe::ComponentBundleProbeTerminationReadyV1 terminationReady(
    const probe::ComponentBundleProbeRequestV1& requestValue)
{
    probe::ComponentBundleProbeTerminationReadyV1 value;
    value.header = probe::componentBundleProbeHeaderV1(
        probe::ComponentBundleProbeRecordKindV1::TerminationReady,
        sizeof(value));
    value.contentDigest = requestValue.contentDigest;
    value.artifactGeneration = requestValue.artifactGeneration;
    value.validationNonce = requestValue.validationNonce;
    return value;
}

void testExactRequestAndReplyRoundTrip()
{
    std::vector<std::byte> entry(4'096, std::byte{0x5a});
    const auto requestValue = request(entry.size());
    const auto expectedValue = expectedAbi(requestValue);
    auto message = probe::createComponentBundleProbeXpcRequestV1(
        requestValue,
        expectedValue,
        entry);
    require(message != nullptr, "exact probe request should encode");

    probe::ComponentBundleProbeXpcRequestEnvelopeV1 decoded;
    require(
        probe::decodeComponentBundleProbeXpcRequestV1(message, decoded)
                == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                   &decoded.request,
                   &requestValue,
                   sizeof(requestValue)) == 0
            && std::memcmp(
                   &decoded.expectedAbi,
                   &expectedValue,
                   sizeof(expectedValue)) == 0
            && decoded.entryBytes.size() == entry.size()
            && std::equal(
                decoded.entryBytes.begin(),
                decoded.entryBytes.end(),
                entry.begin()),
        "the immutable request records and entry bytes should round-trip exactly");
    xpc_release(message);

    auto startedMessage = xpc_dictionary_create_empty();
    const auto startedValue = started(requestValue);
    require(
        probe::writeComponentBundleProbeXpcStartedV1(
            startedMessage,
            startedValue,
            requestValue),
        "a validated helper-start record should encode once");
    probe::ComponentBundleProbeStartedV1 decodedStarted;
    require(
        probe::decodeComponentBundleProbeXpcStartedV1(
            startedMessage,
            requestValue,
            decodedStarted)
                == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                &decodedStarted,
                &startedValue,
                sizeof(startedValue)) == 0,
        "the exact typed helper-start record should round-trip");
    xpc_release(startedMessage);

    auto acknowledgementMessage = xpc_dictionary_create_empty();
    const auto acknowledgementValue = acknowledgement(requestValue);
    require(
        probe::writeComponentBundleProbeXpcAcknowledgementV1(
            acknowledgementMessage,
            acknowledgementValue,
            requestValue),
        "a validated parent acknowledgement should encode once");
    probe::ComponentBundleProbeAcknowledgementV1 decodedAcknowledgement;
    require(
        probe::decodeComponentBundleProbeXpcAcknowledgementV1(
            acknowledgementMessage,
            requestValue,
            decodedAcknowledgement)
                == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                &decodedAcknowledgement,
                &acknowledgementValue,
                sizeof(acknowledgementValue)) == 0,
        "the exact typed parent acknowledgement should round-trip");
    xpc_release(acknowledgementMessage);

    auto finalizationMessage = xpc_dictionary_create_empty();
    const auto finalizationValue = finalization(requestValue);
    require(
        probe::writeComponentBundleProbeXpcFinalizationV1(
            finalizationMessage,
            finalizationValue,
            requestValue),
        "a validated parent finalization should encode once");
    probe::ComponentBundleProbeFinalizationV1 decodedFinalization;
    require(
        probe::decodeComponentBundleProbeXpcFinalizationV1(
            finalizationMessage,
            requestValue,
            decodedFinalization)
                == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                &decodedFinalization,
                &finalizationValue,
                sizeof(finalizationValue)) == 0,
        "the exact typed parent finalization should round-trip");
    xpc_release(finalizationMessage);

    auto terminationReadyMessage = xpc_dictionary_create_empty();
    const auto readyValue = terminationReady(requestValue);
    require(
        probe::writeComponentBundleProbeXpcTerminationReadyV1(
            terminationReadyMessage,
            readyValue,
            requestValue),
        "validated termination readiness should encode once");
    probe::ComponentBundleProbeTerminationReadyV1 decodedReady;
    require(
        probe::decodeComponentBundleProbeXpcTerminationReadyV1(
            terminationReadyMessage,
            requestValue,
            decodedReady)
                == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                &decodedReady,
                &readyValue,
                sizeof(readyValue)) == 0,
        "the exact typed termination readiness should round-trip");
    xpc_release(terminationReadyMessage);

    auto reply = xpc_dictionary_create_empty();
    const auto result = cleanResult(requestValue);
    require(
        probe::writeComponentBundleProbeXpcReplyV1(
            reply,
            result,
            requestValue),
        "a validated terminal result should encode once");
    probe::ComponentBundleProbeResultV1 decodedResult;
    require(
        probe::decodeComponentBundleProbeXpcReplyV1(
            reply,
            requestValue,
            decodedResult)
                == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid
            && std::memcmp(
                   &decodedResult,
                   &result,
                   sizeof(result)) == 0,
        "the exact typed probe reply should round-trip");
    xpc_release(reply);
}

void testMalformedEnvelopeFailsClosed()
{
    probe::ComponentBundleProbeXpcRequestEnvelopeV1 decoded;
    auto text = xpc_string_create("not a request");
    require(
        probe::decodeComponentBundleProbeXpcRequestV1(text, decoded)
            == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid,
        "a non-dictionary request should reject");
    xpc_release(text);

    std::vector<std::byte> entry(64, std::byte{1});
    const auto requestValue = request(entry.size());
    const auto expectedValue = expectedAbi(requestValue);
    auto message = probe::createComponentBundleProbeXpcRequestV1(
        requestValue,
        expectedValue,
        entry);
    const std::uint32_t marker = 7;
    xpc_dictionary_set_data(message, "unexpected", &marker, sizeof(marker));
    require(
        probe::decodeComponentBundleProbeXpcRequestV1(message, decoded)
            == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::FieldCountInvalid,
        "an additional request field should reject");
    xpc_release(message);

    message = xpc_dictionary_create_empty();
    xpc_dictionary_set_data(
        message,
        "talas.component-probe.request.v1",
        &requestValue,
        sizeof(requestValue));
    xpc_dictionary_set_data(
        message,
        "talas.component-probe.expected-abi.v1",
        &expectedValue,
        sizeof(expectedValue));
    xpc_dictionary_set_data(
        message,
        "wrong-entry-key",
        entry.data(),
        entry.size());
    require(
        probe::decodeComponentBundleProbeXpcRequestV1(message, decoded)
            == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::EntryMissing,
        "a request without owned entry bytes should reject");
    xpc_release(message);

    message = xpc_dictionary_create_empty();
    xpc_dictionary_set_data(
        message,
        "talas.component-probe.request.v1",
        &requestValue,
        sizeof(requestValue) - 1U);
    xpc_dictionary_set_data(
        message,
        "talas.component-probe.expected-abi.v1",
        &expectedValue,
        sizeof(expectedValue));
    xpc_dictionary_set_data(
        message,
        "talas.component-probe.entry.v1",
        entry.data(),
        entry.size());
    require(
        probe::decodeComponentBundleProbeXpcRequestV1(message, decoded)
            == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::RequestSizeInvalid,
        "a truncated request record should reject before copying");
    xpc_release(message);

    message = probe::createComponentBundleProbeXpcRequestV1(
        requestValue,
        expectedValue,
        std::span(entry).first(entry.size() - 1U));
    require(
        message == nullptr,
        "contradictory entry length should not encode");
}

void testInvalidReplyCannotEncodeOrDecode()
{
    const auto requestValue = request(64);
    auto result = cleanResult(requestValue);
    result.validationNonce[0] ^= 0xff;
    auto reply = xpc_dictionary_create_empty();
    require(
        !probe::writeComponentBundleProbeXpcReplyV1(
            reply,
            result,
            requestValue),
        "a result from another validation session should not encode");
    xpc_release(reply);

    reply = xpc_dictionary_create_empty();
    result = cleanResult(requestValue);
    xpc_dictionary_set_data(
        reply,
        "talas.component-probe.result.v1",
        &result,
        sizeof(result) - 1U);
    probe::ComponentBundleProbeResultV1 decoded;
    require(
        probe::decodeComponentBundleProbeXpcReplyV1(
            reply,
            requestValue,
            decoded)
            == probe::ComponentBundleProbeXpcEnvelopeDiagnosticV1::ResultSizeInvalid,
        "a truncated reply should reject before copying");
    xpc_release(reply);

    auto startedMessage = xpc_dictionary_create_empty();
    auto startedValue = started(requestValue);
    startedValue.processIdentifier = 0;
    require(
        !probe::writeComponentBundleProbeXpcStartedV1(
            startedMessage,
            startedValue,
            requestValue),
        "an invalid helper-start process identity should not encode");
    xpc_release(startedMessage);
}
}

int main()
{
    testExactRequestAndReplyRoundTrip();
    testMalformedEnvelopeFailsClosed();
    testInvalidReplyCannotEncodeOrDecode();
    return 0;
}
