#include "runtime/macos/ComponentIsolationXpcEnvelope.h"

#include <cstring>

namespace talas::runtime
{
namespace
{
constexpr char recordKey[] = "talas.component.record.v1";
constexpr char diagnosticKey[] = "talas.component.diagnostic.v1";
constexpr char sharedMemoryKey[] = "talas.component.audio-shmem.v1";

template <typename Record>
[[nodiscard]] xpc_object_t createRecordMessage(const Record& record) noexcept
{
    auto message = xpc_dictionary_create_empty();
    if (message == nullptr)
        return nullptr;
    xpc_dictionary_set_data(message, recordKey, &record, sizeof(record));
    return message;
}
}

xpc_object_t createComponentIsolationXpcMessageV1(
    const ComponentIsolationHandshakeV1& request,
    xpc_object_t sharedMemory) noexcept
{
    if (sharedMemory == nullptr
        || xpc_get_type(sharedMemory) != XPC_TYPE_SHMEM)
    {
        return nullptr;
    }
    auto message = createRecordMessage(request);
    if (message != nullptr)
        xpc_dictionary_set_value(message, sharedMemoryKey, sharedMemory);
    return message;
}

xpc_object_t createComponentIsolationXpcMessageV1(
    const ComponentIsolationControlRecordV1& request) noexcept
{
    return createRecordMessage(request);
}

bool writeComponentIsolationXpcResponseV1(
    xpc_object_t dictionary,
    const ComponentIsolationProductHelperResultV1& result) noexcept
{
    if (dictionary == nullptr
        || xpc_get_type(dictionary) != XPC_TYPE_DICTIONARY
        || xpc_dictionary_get_count(dictionary) != 0
        || result.action != ComponentIsolationProductHelperActionV1::Respond)
    {
        return false;
    }
    if (result.responseKind
        == ComponentIsolationProductHelperResponseKindV1::Handshake)
    {
        if (result.diagnosticBytes != 0)
            return false;
        xpc_dictionary_set_data(
            dictionary,
            recordKey,
            &result.handshake,
            sizeof(result.handshake));
        return true;
    }
    if (result.responseKind
        != ComponentIsolationProductHelperResponseKindV1::Control)
    {
        return false;
    }
    if (result.diagnosticBytes > result.diagnosticStorage.size()
        || result.control.diagnosticBytes != result.diagnosticBytes)
    {
        return false;
    }
    xpc_dictionary_set_data(
        dictionary,
        recordKey,
        &result.control,
        sizeof(result.control));
    if (result.diagnosticBytes != 0)
    {
        xpc_dictionary_set_data(
            dictionary,
            diagnosticKey,
            result.diagnosticStorage.data(),
            result.diagnosticBytes);
    }
    return true;
}

ComponentIsolationXpcEnvelopeDiagnosticV1
decodeComponentIsolationXpcEnvelopeV1(
    xpc_object_t object,
    ComponentIsolationXpcEnvelopeV1& envelope) noexcept
{
    envelope = {};
    if (object == nullptr || xpc_get_type(object) != XPC_TYPE_DICTIONARY)
    {
        return ComponentIsolationXpcEnvelopeDiagnosticV1::ObjectTypeInvalid;
    }
    const auto fieldCount = xpc_dictionary_get_count(object);
    if (fieldCount == 0 || fieldCount > 2)
        return ComponentIsolationXpcEnvelopeDiagnosticV1::FieldCountInvalid;

    std::size_t recordBytes = 0;
    const auto* record = xpc_dictionary_get_data(
        object,
        recordKey,
        &recordBytes);
    if (record == nullptr)
        return ComponentIsolationXpcEnvelopeDiagnosticV1::RecordMissing;
    if (recordBytes == sizeof(ComponentIsolationHandshakeV1))
    {
        std::memcpy(&envelope.handshake, record, recordBytes);
        envelope.kind = ComponentIsolationXpcEnvelopeKindV1::Handshake;
        const auto kind = static_cast<ComponentIsolationRecordKind>(
            envelope.handshake.header.recordKind);
        if (kind == ComponentIsolationRecordKind::HandshakeRequest)
        {
            if (fieldCount != 2)
            {
                return ComponentIsolationXpcEnvelopeDiagnosticV1::
                    SharedMemoryInvalid;
            }
            envelope.sharedMemory = xpc_dictionary_get_value(
                object,
                sharedMemoryKey);
            if (envelope.sharedMemory == nullptr
                || xpc_get_type(envelope.sharedMemory) != XPC_TYPE_SHMEM)
            {
                envelope.sharedMemory = nullptr;
                return ComponentIsolationXpcEnvelopeDiagnosticV1::
                    SharedMemoryInvalid;
            }
        }
        else if (fieldCount != 1)
        {
            return ComponentIsolationXpcEnvelopeDiagnosticV1::
                FieldCountInvalid;
        }
        return ComponentIsolationXpcEnvelopeDiagnosticV1::Valid;
    }
    if (recordBytes != sizeof(ComponentIsolationControlRecordV1))
        return ComponentIsolationXpcEnvelopeDiagnosticV1::RecordSizeInvalid;

    std::memcpy(&envelope.control, record, recordBytes);
    envelope.kind = ComponentIsolationXpcEnvelopeKindV1::Control;
    if (envelope.control.diagnosticBytes == 0)
    {
        return fieldCount == 1
            ? ComponentIsolationXpcEnvelopeDiagnosticV1::Valid
            : ComponentIsolationXpcEnvelopeDiagnosticV1::FieldCountInvalid;
    }
    if (fieldCount != 2
        || envelope.control.diagnosticBytes
            > maximumComponentIsolationControlPayloadBytesV1)
    {
        return ComponentIsolationXpcEnvelopeDiagnosticV1::DiagnosticInvalid;
    }
    std::size_t diagnosticBytes = 0;
    const auto* diagnostic = xpc_dictionary_get_data(
        object,
        diagnosticKey,
        &diagnosticBytes);
    if (diagnostic == nullptr
        || diagnosticBytes != envelope.control.diagnosticBytes)
    {
        return ComponentIsolationXpcEnvelopeDiagnosticV1::DiagnosticInvalid;
    }
    std::memcpy(
        envelope.diagnosticStorage.data(),
        diagnostic,
        diagnosticBytes);
    envelope.diagnosticBytes = static_cast<std::uint32_t>(diagnosticBytes);
    return ComponentIsolationXpcEnvelopeDiagnosticV1::Valid;
}
}
