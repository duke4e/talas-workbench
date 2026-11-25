#include "runtime/component_probe/macos/ComponentBundleProbeXpcEnvelope.h"

#include <cstring>

namespace talas::runtime::component_probe
{
namespace
{
constexpr char requestKey[] = "talas.component-probe.request.v1";
constexpr char expectedAbiKey[] = "talas.component-probe.expected-abi.v1";
constexpr char entryKey[] = "talas.component-probe.entry.v1";
constexpr char startedKey[] = "talas.component-probe.started.v1";
constexpr char acknowledgementKey[] =
    "talas.component-probe.acknowledgement.v1";
constexpr char finalizationKey[] = "talas.component-probe.finalization.v1";
constexpr char terminationReadyKey[] =
    "talas.component-probe.termination-ready.v1";
constexpr char resultKey[] = "talas.component-probe.result.v1";
}

xpc_object_t createComponentBundleProbeXpcRequestV1(
    const ComponentBundleProbeRequestV1& request,
    const ComponentBundleProbeExpectedAbiV1& expectedAbi,
    std::span<const std::byte> entryBytes) noexcept
{
    if (validateComponentBundleProbeExpectedAbiV1(expectedAbi, request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid
        || entryBytes.empty()
        || entryBytes.size() != request.entryByteCount)
    {
        return nullptr;
    }
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr)
        return nullptr;
    xpc_dictionary_set_data(message, requestKey, &request, sizeof(request));
    xpc_dictionary_set_data(
        message,
        expectedAbiKey,
        &expectedAbi,
        sizeof(expectedAbi));
    xpc_dictionary_set_data(
        message,
        entryKey,
        entryBytes.data(),
        entryBytes.size());
    return message;
}

ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcRequestV1(
    xpc_object_t object,
    ComponentBundleProbeXpcRequestEnvelopeV1& envelope) noexcept
{
    envelope = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    }
    if (xpc_dictionary_get_count(object) != 3)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::FieldCountInvalid;

    std::size_t requestBytes = 0;
    const auto* requestData = xpc_dictionary_get_data(
        object,
        requestKey,
        &requestBytes);
    if (requestData == nullptr)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::RequestMissing;
    if (requestBytes != sizeof(envelope.request))
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::RequestSizeInvalid;
    }

    std::size_t expectedAbiBytes = 0;
    const auto* expectedAbiData = xpc_dictionary_get_data(
        object,
        expectedAbiKey,
        &expectedAbiBytes);
    if (expectedAbiData == nullptr)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ExpectedAbiMissing;
    if (expectedAbiBytes != sizeof(envelope.expectedAbi))
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ExpectedAbiSizeInvalid;
    }

    std::size_t entryBytes = 0;
    const auto* entryData = xpc_dictionary_get_data(
        object,
        entryKey,
        &entryBytes);
    if (entryData == nullptr)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::EntryMissing;

    std::memcpy(&envelope.request, requestData, requestBytes);
    std::memcpy(&envelope.expectedAbi, expectedAbiData, expectedAbiBytes);
    if (validateComponentBundleProbeExpectedAbiV1(
            envelope.expectedAbi,
            envelope.request)
        != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        envelope = {};
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    if (entryBytes != envelope.request.entryByteCount)
    {
        envelope = {};
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::EntrySizeInvalid;
    }
    envelope.entryBytes = {
        static_cast<const std::byte*>(entryData),
        entryBytes,
    };
    return ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid;
}

bool writeComponentBundleProbeXpcStartedV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeStartedV1& started,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || validateComponentBundleProbeStartedV1(started, request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    xpc_dictionary_set_data(
        dictionary,
        startedKey,
        &started,
        sizeof(started));
    return true;
}

ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcStartedV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeStartedV1& started) noexcept
{
    started = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    }
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::FieldCountInvalid;

    std::size_t startedBytes = 0;
    const auto* startedData = xpc_dictionary_get_data(
        object,
        startedKey,
        &startedBytes);
    if (startedData == nullptr)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::StartedMissing;
    if (startedBytes != sizeof(started))
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::StartedSizeInvalid;
    std::memcpy(&started, startedData, startedBytes);
    if (validateComponentBundleProbeStartedV1(started, request)
        != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        started = {};
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid;
}

bool writeComponentBundleProbeXpcAcknowledgementV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeAcknowledgementV1& acknowledgement,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || validateComponentBundleProbeAcknowledgementV1(
               acknowledgement,
               request) != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    xpc_dictionary_set_data(
        dictionary,
        acknowledgementKey,
        &acknowledgement,
        sizeof(acknowledgement));
    return true;
}

ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcAcknowledgementV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeAcknowledgementV1& acknowledgement) noexcept
{
    acknowledgement = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    }
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::FieldCountInvalid;

    std::size_t acknowledgementBytes = 0;
    const auto* acknowledgementData = xpc_dictionary_get_data(
        object,
        acknowledgementKey,
        &acknowledgementBytes);
    if (acknowledgementData == nullptr)
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::
            AcknowledgementMissing;
    }
    if (acknowledgementBytes != sizeof(acknowledgement))
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::
            AcknowledgementSizeInvalid;
    }
    std::memcpy(
        &acknowledgement,
        acknowledgementData,
        acknowledgementBytes);
    if (validateComponentBundleProbeAcknowledgementV1(
            acknowledgement,
            request) != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        acknowledgement = {};
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid;
}

bool writeComponentBundleProbeXpcFinalizationV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeFinalizationV1& finalization,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || validateComponentBundleProbeFinalizationV1(finalization, request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    xpc_dictionary_set_data(
        dictionary,
        finalizationKey,
        &finalization,
        sizeof(finalization));
    return true;
}

ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcFinalizationV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeFinalizationV1& finalization) noexcept
{
    finalization = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(
        object,
        finalizationKey,
        &bytes);
    if (data == nullptr)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::FinalizationMissing;
    if (bytes != sizeof(finalization))
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::
            FinalizationSizeInvalid;
    }
    std::memcpy(&finalization, data, bytes);
    if (validateComponentBundleProbeFinalizationV1(finalization, request)
        != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        finalization = {};
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid;
}

bool writeComponentBundleProbeXpcTerminationReadyV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeTerminationReadyV1& ready,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || validateComponentBundleProbeTerminationReadyV1(ready, request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    xpc_dictionary_set_data(
        dictionary,
        terminationReadyKey,
        &ready,
        sizeof(ready));
    return true;
}

ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcTerminationReadyV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeTerminationReadyV1& ready) noexcept
{
    ready = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    std::size_t bytes = 0;
    const auto* data = xpc_dictionary_get_data(
        object,
        terminationReadyKey,
        &bytes);
    if (data == nullptr)
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::
            TerminationReadyMissing;
    }
    if (bytes != sizeof(ready))
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::
            TerminationReadySizeInvalid;
    }
    std::memcpy(&ready, data, bytes);
    if (validateComponentBundleProbeTerminationReadyV1(ready, request)
        != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        ready = {};
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid;
}

bool writeComponentBundleProbeXpcReplyV1(
    xpc_object_t dictionary,
    const ComponentBundleProbeResultV1& result,
    const ComponentBundleProbeRequestV1& request) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || validateComponentBundleProbeResultV1(result, request)
            != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        return false;
    }
    xpc_dictionary_set_data(dictionary, resultKey, &result, sizeof(result));
    return true;
}

ComponentBundleProbeXpcEnvelopeDiagnosticV1
decodeComponentBundleProbeXpcReplyV1(
    xpc_object_t object,
    const ComponentBundleProbeRequestV1& request,
    ComponentBundleProbeResultV1& result) noexcept
{
    result = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
    {
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    }
    if (xpc_dictionary_get_count(object) != 1)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::FieldCountInvalid;

    std::size_t resultBytes = 0;
    const auto* resultData = xpc_dictionary_get_data(
        object,
        resultKey,
        &resultBytes);
    if (resultData == nullptr)
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ResultMissing;
    if (resultBytes != sizeof(result))
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ResultSizeInvalid;
    std::memcpy(&result, resultData, resultBytes);
    if (validateComponentBundleProbeResultV1(result, request)
        != ComponentBundleProbeProtocolDiagnosticV1::Valid)
    {
        result = {};
        return ComponentBundleProbeXpcEnvelopeDiagnosticV1::ProtocolInvalid;
    }
    return ComponentBundleProbeXpcEnvelopeDiagnosticV1::Valid;
}
}
