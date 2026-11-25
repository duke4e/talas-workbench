#include "runtime/component_probe/ComponentBundleProbeProtocol.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace
{
using namespace talas::runtime;
using namespace talas::runtime::component_probe;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

ComponentBundleProbeRequestV1 validRequest()
{
    ComponentBundleProbeRequestV1 request;
    request.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::Request,
        sizeof(request));
    constexpr std::string_view bundleId = "org.talas.probe-fixture";
    constexpr std::string_view version = "1.2.3";
    request.bundleIdBytes = bundleId.size();
    request.semanticVersionBytes = version.size();
    std::copy(bundleId.begin(), bundleId.end(), request.bundleId.begin());
    std::copy(version.begin(), version.end(), request.semanticVersion.begin());
    request.contentDigest.fill(0x31);
    request.signerIdentityDigest.fill(0x73);
    request.requestedPermissions =
        componentPermission(ComponentExecutionPermission::RealtimeAudio);
    request.declaredRoles =
        componentRole(ComponentExecutionRole::RealtimeProcessor);
    request.policyVersion = currentComponentTrustPolicyVersion;
    request.entryByteCount = 4096;
    request.artifactGeneration = 19;
    request.validationNonce.fill(0x5c);
    return request;
}

ComponentBundleProbeResultV1 validCleanResult(
    const ComponentBundleProbeRequestV1& request)
{
    ComponentBundleProbeResultV1 result;
    result.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::Result,
        sizeof(result));
    result.state = static_cast<std::uint32_t>(ComponentProbeState::Clean);
    result.diagnostic = static_cast<std::uint32_t>(
        ComponentBundleProbeResultDiagnosticV1::Ready);
    result.contentDigest = request.contentDigest;
    result.artifactGeneration = request.artifactGeneration;
    result.validationNonce = request.validationNonce;
    return result;
}

ComponentBundleProbeStartedV1 validStarted(
    const ComponentBundleProbeRequestV1& request)
{
    ComponentBundleProbeStartedV1 started;
    started.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::Started,
        sizeof(started));
    started.contentDigest = request.contentDigest;
    started.artifactGeneration = request.artifactGeneration;
    started.validationNonce = request.validationNonce;
    started.processIdentifier = 1234;
    started.effectiveUser = 501;
    started.startSeconds = 1'234'567;
    started.startMicroseconds = 123'456;
    return started;
}

ComponentBundleProbeAcknowledgementV1 validAcknowledgement(
    const ComponentBundleProbeRequestV1& request)
{
    ComponentBundleProbeAcknowledgementV1 acknowledgement;
    acknowledgement.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::Acknowledgement,
        sizeof(acknowledgement));
    acknowledgement.contentDigest = request.contentDigest;
    acknowledgement.artifactGeneration = request.artifactGeneration;
    acknowledgement.validationNonce = request.validationNonce;
    return acknowledgement;
}

ComponentBundleProbeFinalizationV1 validFinalization(
    const ComponentBundleProbeRequestV1& request)
{
    ComponentBundleProbeFinalizationV1 finalization;
    finalization.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::Finalization,
        sizeof(finalization));
    finalization.contentDigest = request.contentDigest;
    finalization.artifactGeneration = request.artifactGeneration;
    finalization.validationNonce = request.validationNonce;
    return finalization;
}

ComponentBundleProbeTerminationReadyV1 validTerminationReady(
    const ComponentBundleProbeRequestV1& request)
{
    ComponentBundleProbeTerminationReadyV1 ready;
    ready.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::TerminationReady,
        sizeof(ready));
    ready.contentDigest = request.contentDigest;
    ready.artifactGeneration = request.artifactGeneration;
    ready.validationNonce = request.validationNonce;
    return ready;
}

ComponentBundleProbeExpectedAbiV1 validExpectedAbi(
    const ComponentBundleProbeRequestV1& request)
{
    ComponentBundleProbeExpectedAbiV1 expected;
    expected.header = componentBundleProbeHeaderV1(
        ComponentBundleProbeRecordKindV1::ExpectedAbi,
        sizeof(expected));
    expected.entryDigest.fill(0xa5);
    expected.entryByteCount = request.entryByteCount;
    expected.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    expected.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    expected.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    expected.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    constexpr std::string_view nodeId =
        "org.talas.probe-fixture.oscillator";
    expected.nodeCount = 1;
    expected.nodeIdBytes[0] = nodeId.size();
    std::copy(
        nodeId.begin(),
        nodeId.end(),
        expected.nodeIds[0].begin());
    return expected;
}

void testExactRequestAndResultPass()
{
    const auto request = validRequest();
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "the exact bounded request should pass");
    const auto result = validCleanResult(request);
    require(
        validateComponentBundleProbeResultV1(result, request)
            == ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "the exact clean reply should pass");
    const auto started = validStarted(request);
    require(
        validateComponentBundleProbeStartedV1(started, request)
            == ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "the exact helper-start record should pass");
    const auto acknowledgement = validAcknowledgement(request);
    require(
        validateComponentBundleProbeAcknowledgementV1(
            acknowledgement,
            request) == ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "the exact parent acknowledgement should pass");
    require(
        validateComponentBundleProbeFinalizationV1(
            validFinalization(request),
            request) == ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "the exact parent finalization should pass");
    require(
        validateComponentBundleProbeTerminationReadyV1(
            validTerminationReady(request),
            request) == ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "the exact helper termination-ready record should pass");
    const auto expected = validExpectedAbi(request);
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::Valid,
        "the exact entry digest and manifest ABI projection should pass");
}

void testHelperStartIdentityAndReplayFailClosed()
{
    const auto request = validRequest();
    auto started = validStarted(request);
    started.processIdentifier = 0;
    require(
        validateComponentBundleProbeStartedV1(started, request)
            == ComponentBundleProbeProtocolDiagnosticV1::ProcessIdentityInvalid,
        "a helper-start record without a process identity should reject");

    started = validStarted(request);
    started.contentDigest.front() ^= 0xffU;
    require(
        validateComponentBundleProbeStartedV1(started, request)
            == ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch,
        "a helper-start record for another candidate should reject");

    started = validStarted(request);
    ++started.artifactGeneration;
    require(
        validateComponentBundleProbeStartedV1(started, request)
            == ComponentBundleProbeProtocolDiagnosticV1::SessionBindingMismatch,
        "a helper-start record from another generation should reject");

    started = validStarted(request);
    started.reserved0 = 1;
    require(
        validateComponentBundleProbeStartedV1(started, request)
            == ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero,
        "reserved helper-start data should reject");

    started = validStarted(request);
    started.startSeconds = 0;
    require(
        validateComponentBundleProbeStartedV1(started, request)
            == ComponentBundleProbeProtocolDiagnosticV1::ProcessIdentityInvalid,
        "a helper-start record without a process start time should reject");

    started = validStarted(request);
    started.startMicroseconds = 1'000'000;
    require(
        validateComponentBundleProbeStartedV1(started, request)
            == ComponentBundleProbeProtocolDiagnosticV1::ProcessIdentityInvalid,
        "an out-of-range helper process start fraction should reject");

    auto acknowledgement = validAcknowledgement(request);
    acknowledgement.validationNonce.front() ^= 0xffU;
    require(
        validateComponentBundleProbeAcknowledgementV1(
            acknowledgement,
            request)
            == ComponentBundleProbeProtocolDiagnosticV1::
                SessionBindingMismatch,
        "an acknowledgement for another validation session should reject");

    auto finalization = validFinalization(request);
    ++finalization.artifactGeneration;
    require(
        validateComponentBundleProbeFinalizationV1(finalization, request)
            == ComponentBundleProbeProtocolDiagnosticV1::
                SessionBindingMismatch,
        "a finalization for another generation should reject");

    auto ready = validTerminationReady(request);
    ready.contentDigest.front() ^= 0xffU;
    require(
        validateComponentBundleProbeTerminationReadyV1(ready, request)
            == ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch,
        "termination readiness for another candidate should reject");
}

void testHeadersAndReservedFieldsFailClosed()
{
    auto request = validRequest();
    --request.header.recordBytes;
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::RecordSizeMismatch,
        "a truncated request declaration should reject");
    request = validRequest();
    ++request.header.protocolVersion;
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::ProtocolMismatch,
        "a future protocol should reject");
    request = validRequest();
    request.header.recordKind = 0xff;
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::RecordKindMismatch,
        "an unknown record kind should reject");
    request = validRequest();
    request.reserved0 = 1;
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero,
        "nonzero reserved request data should reject");
}

void testCandidateAndEntryBudgetsFailClosed()
{
    auto request = validRequest();
    request.bundleIdBytes = 0;
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::CandidateInvalid,
        "an empty bundle identity should reject");
    request = validRequest();
    request.bundleId[request.bundleIdBytes] = 'x';
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::CandidateInvalid,
        "noncanonical trailing identity bytes should reject");
    request = validRequest();
    request.contentDigest.fill(0);
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::CandidateInvalid,
        "missing authenticated content identity should reject");
    request = validRequest();
    request.entryByteCount = maximumComponentBundleProbeEntryBytesV1 + 1;
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::EntryBudgetInvalid,
        "an oversized entry should reject");
    request = validRequest();
    request.validationNonce.fill(0);
    require(
        validateComponentBundleProbeRequestV1(request)
            == ComponentBundleProbeProtocolDiagnosticV1::SessionBindingInvalid,
        "a zero nonce should reject");
}

void testStatePairsAndReplayFailClosed()
{
    const auto request = validRequest();
    auto result = validCleanResult(request);
    result.diagnostic = static_cast<std::uint32_t>(
        ComponentBundleProbeResultDiagnosticV1::MetadataRejected);
    require(
        validateComponentBundleProbeResultV1(result, request)
            == ComponentBundleProbeProtocolDiagnosticV1::StateDiagnosticMismatch,
        "clean cannot pair with a rejection diagnostic");
    result = validCleanResult(request);
    result.state = 0xff;
    require(
        validateComponentBundleProbeResultV1(result, request)
            == ComponentBundleProbeProtocolDiagnosticV1::StateUnknown,
        "an unknown result state should reject");
    result = validCleanResult(request);
    ++result.artifactGeneration;
    require(
        validateComponentBundleProbeResultV1(result, request)
            == ComponentBundleProbeProtocolDiagnosticV1::SessionBindingMismatch,
        "a result from another generation should reject");
    result = validCleanResult(request);
    result.validationNonce[3] ^= 0xff;
    require(
        validateComponentBundleProbeResultV1(result, request)
            == ComponentBundleProbeProtocolDiagnosticV1::SessionBindingMismatch,
        "a result from another validation session should reject");
    result = validCleanResult(request);
    result.contentDigest[7] ^= 0xff;
    require(
        validateComponentBundleProbeResultV1(result, request)
            == ComponentBundleProbeProtocolDiagnosticV1::CandidateMismatch,
        "a result for different bytes should reject");
}

void testExpectedAbiEntryBindingFailsClosed()
{
    const auto request = validRequest();
    auto expected = validExpectedAbi(request);
    expected.entryDigest.fill(0);
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::EntryBindingInvalid,
        "a missing entry digest should reject");

    expected = validExpectedAbi(request);
    ++expected.entryByteCount;
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::EntryBindingMismatch,
        "entry ABI evidence for different bytes should reject");

    expected = validExpectedAbi(request);
    expected.header.recordKind = 0xff;
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::RecordKindMismatch,
        "a different fixed-record kind should reject");

    expected = validExpectedAbi(request);
    expected.reserved1[2] = 1;
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::ReservedNonZero,
        "reserved expected-ABI data should reject");
}

void testExpectedAbiRangesAndNodeSetFailClosed()
{
    const auto request = validRequest();
    auto expected = validExpectedAbi(request);
    expected.minimumEngineAbiVersion = 0;
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::ExpectedAbiInvalid,
        "a zero ABI range should reject");

    expected = validExpectedAbi(request);
    expected.maximumEngineApiVersion =
        expected.minimumEngineApiVersion - 1U;
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::ExpectedAbiInvalid,
        "a reversed API range should reject");

    expected = validExpectedAbi(request);
    expected.nodeCount = 0;
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid,
        "an empty node set should reject");

    expected = validExpectedAbi(request);
    expected.nodeIdBytes[0] = 3;
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid,
        "a truncated node identity should reject");

    expected = validExpectedAbi(request);
    expected.nodeCount = 2;
    expected.nodeIdBytes[1] = expected.nodeIdBytes[0];
    expected.nodeIds[1] = expected.nodeIds[0];
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid,
        "duplicate node identities should reject");

    expected = validExpectedAbi(request);
    expected.nodeIds[1][0] = 'x';
    require(
        validateComponentBundleProbeExpectedAbiV1(expected, request)
            == ComponentBundleProbeProtocolDiagnosticV1::NodeSetInvalid,
        "unused node storage must remain zero");
}
}

int main()
{
    testExactRequestAndResultPass();
    testHeadersAndReservedFieldsFailClosed();
    testHelperStartIdentityAndReplayFailClosed();
    testCandidateAndEntryBudgetsFailClosed();
    testStatePairsAndReplayFailClosed();
    testExpectedAbiEntryBindingFailsClosed();
    testExpectedAbiRangesAndNodeSetFailClosed();
    return 0;
}
