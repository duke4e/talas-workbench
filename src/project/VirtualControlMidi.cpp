#include "talas/project/VirtualControlMidi.h"

#include "talas/project/ProjectSnapshots.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <thread>
#include <tuple>

namespace talas::project
{
namespace
{
bool isValidUtf8(std::string_view value) noexcept
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
        for (std::size_t offset = 1; offset <= continuationCount; ++offset)
        {
            const auto byte = static_cast<unsigned char>(value[index + offset]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            codePoint = (codePoint << 6U) | (byte & 0x3fU);
        }
        const auto minimum = continuationCount == 1 ? 0x80U
            : continuationCount == 2 ? 0x800U : 0x10000U;
        if (codePoint < minimum || codePoint > 0x10ffffU
            || (codePoint >= 0xd800U && codePoint <= 0xdfffU))
        {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

bool isValidMidiControllerMessage(MidiControllerMessage message)
{
    return message.channel >= 1 && message.channel <= 16
        && message.controller >= 0 && message.controller <= 127
        && message.value >= 0 && message.value <= 127
        && message.sourceSlot <= 16
        && (message.sourceSlot == 0 || message.sourceGeneration != 0);
}

bool isValidVirtualControlKind(VirtualControlKind kind)
{
    switch (kind)
    {
    case VirtualControlKind::Fader:
    case VirtualControlKind::Gate:
    case VirtualControlKind::Toggle:
        return true;
    }
    return false;
}

bool isValidMidiControlMappingImpl(const MidiControlMapping& mapping) noexcept
{
    if (!isValidMidiControllerMessage(
            {mapping.channel, mapping.controller, 0}))
    {
        return false;
    }
    switch (mapping.sourceMode)
    {
    case MidiControlSourceMode::LegacyAnyInput:
        return mapping.sourceIdentifier.empty();
    case MidiControlSourceMode::ExactInput:
        return isValidMidiSourceIdentifier(mapping.sourceIdentifier);
    }
    return false;
}

bool midiControlMappingsConflictImpl(
    const MidiControlMapping& left,
    const MidiControlMapping& right) noexcept
{
    if (left.channel != right.channel
        || left.controller != right.controller)
    {
        return false;
    }
    return left.sourceMode == MidiControlSourceMode::LegacyAnyInput
        || right.sourceMode == MidiControlSourceMode::LegacyAnyInput
        || left.sourceIdentifier == right.sourceIdentifier;
}

bool isValidValueDomain(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Scalar:
    case talas::expr::ValueDomain::Hertz:
    case talas::expr::ValueDomain::Seconds:
    case talas::expr::ValueDomain::NoteNumber:
    case talas::expr::ValueDomain::Samples:
        return true;
    }
    return false;
}

MidiControlRouteSetDiagnosticCode validatePreparedRoute(
    const PreparedMidiControlRoute& route)
{
    if (!isValidMidiControlMappingImpl(route.mapping))
    {
        return MidiControlRouteSetDiagnosticCode::InvalidSource;
    }
    if ((route.mapping.sourceMode == MidiControlSourceMode::LegacyAnyInput
            && route.sourceSlot != 0)
        || (route.mapping.sourceMode == MidiControlSourceMode::ExactInput
            && (route.sourceSlot == 0 || route.sourceSlot > 16
                || route.sourceGeneration == 0)))
    {
        return MidiControlRouteSetDiagnosticCode::InvalidSource;
    }
    if (route.eventId == 0)
        return MidiControlRouteSetDiagnosticCode::InvalidEventId;
    if (!isValidVirtualControlKind(route.kind))
        return MidiControlRouteSetDiagnosticCode::InvalidKind;
    if (!isValidValueDomain(route.domain))
        return MidiControlRouteSetDiagnosticCode::InvalidDomain;
    if (!std::isfinite(route.minimum) || !std::isfinite(route.maximum)
        || route.maximum < route.minimum)
    {
        return MidiControlRouteSetDiagnosticCode::InvalidRange;
    }
    return MidiControlRouteSetDiagnosticCode::None;
}

double mappedValue(VirtualControlKind kind, double minimum, double maximum, int midiValue)
{
    switch (kind)
    {
    case VirtualControlKind::Fader:
        return std::lerp(
            minimum,
            maximum,
            static_cast<double>(midiValue) / 127.0);
    case VirtualControlKind::Gate:
        return midiValue == 0 ? minimum : maximum;
    case VirtualControlKind::Toggle:
        return midiValue < 64 ? minimum : maximum;
    }
    return minimum;
}

std::size_t routeIndex(
    std::uint8_t sourceSlot,
    int channel,
    int controller)
{
    return static_cast<std::size_t>(sourceSlot) * 16 * 128
        + static_cast<std::size_t>(channel - 1) * 128
        + static_cast<std::size_t>(controller);
}
}

std::optional<BuiltInMidiController>
parseBuiltInMidiControllerName(std::string_view name) noexcept
{
    if (name.size() != 4 && name.size() != 5)
        return std::nullopt;
    VirtualControlKind kind = VirtualControlKind::Fader;
    if (name.starts_with("bt"))
        kind = VirtualControlKind::Gate;
    else if (name.starts_with("sw"))
        kind = VirtualControlKind::Toggle;
    else if (!name.starts_with("cc"))
        return std::nullopt;

    unsigned number = 0;
    for (std::size_t index = 2; index < name.size(); ++index)
    {
        if (name[index] < '0' || name[index] > '9')
            return std::nullopt;
        number = number * 10U + static_cast<unsigned>(name[index] - '0');
    }
    if (number > 127 || (name.size() == 5 && number < 100))
        return std::nullopt;
    return BuiltInMidiController{static_cast<std::uint8_t>(number), kind};
}

bool isValidMidiSourceIdentifier(std::string_view identifier) noexcept
{
    return !identifier.empty()
        && identifier.size() <= maximumMidiSourceIdentifierBytes
        && isValidUtf8(identifier);
}

bool isValidMidiControlMapping(const MidiControlMapping& mapping) noexcept
{
    return isValidMidiControlMappingImpl(mapping);
}

bool midiControlMappingsConflict(
    const MidiControlMapping& left,
    const MidiControlMapping& right) noexcept
{
    return midiControlMappingsConflictImpl(left, right);
}

std::string virtualControlTargetIdentity(const VirtualControl& control)
{
    SnapshotTargetIdentity target;
    target.kind = SnapshotTargetKind::EventValue;
    target.eventName = control.eventName;
    target.domain = control.domain;
    return snapshotTargetIdentityString(target);
}

bool RealtimeMidiRouteAdmissionGate::tryBegin() noexcept
{
    for (std::size_t attempt = 0; attempt < 8; ++attempt)
    {
        auto state = state_.load(std::memory_order_acquire);
        if ((state & closedBit) != 0 || (state & activeMask) == activeMask)
            return false;
        if (state_.compare_exchange_weak(
                state,
                state + 1,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

void RealtimeMidiRouteAdmissionGate::end() noexcept
{
    state_.fetch_sub(1, std::memory_order_release);
}

void RealtimeMidiRouteAdmissionGate::closeAndWait() noexcept
{
    auto state = state_.load(std::memory_order_acquire);
    while (!state_.compare_exchange_weak(
        state,
        state | closedBit,
        std::memory_order_acq_rel,
        std::memory_order_acquire))
    {
    }
    while ((state_.load(std::memory_order_acquire) & activeMask) != 0)
        std::this_thread::yield();
}

void RealtimeMidiRouteAdmissionGate::open() noexcept
{
    state_.store(0, std::memory_order_release);
}

MidiLearnPlan planVirtualControlMidiLearn(
    const ProjectDocument& document,
    std::string_view eventName,
    MidiControlMapping mapping)
{
    MidiLearnPlan result;
    result.mapping = mapping;
    if (!isValidMidiControlMapping(mapping))
    {
        result.disposition = MidiLearnDisposition::InvalidSource;
        return result;
    }

    const auto requested = std::ranges::find_if(
        document.virtualControls,
        [&](const auto& control) {
            return control.eventName == eventName;
        });
    if (requested == document.virtualControls.end())
    {
        result.disposition = MidiLearnDisposition::UnknownTarget;
        return result;
    }

    result.requestedEventName = requested->eventName;
    result.requestedTargetIdentity =
        virtualControlTargetIdentity(*requested);
    if (!isCanonicalSnapshotTargetIdentityString(
            result.requestedTargetIdentity))
    {
        result.disposition = MidiLearnDisposition::UnknownTarget;
        return result;
    }

    const auto conflict = std::ranges::find_if(
        document.virtualControls,
        [&](const auto& control) {
            return control.eventName != requested->eventName
                && control.midiMapping.has_value()
                && midiControlMappingsConflict(*control.midiMapping, mapping);
        });
    if (conflict != document.virtualControls.end())
    {
        result.disposition = MidiLearnDisposition::Conflict;
        result.conflictingEventName = conflict->eventName;
        result.conflictingTargetIdentity =
            virtualControlTargetIdentity(*conflict);
        return result;
    }

    if (requested->midiMapping.has_value()
        && *requested->midiMapping == mapping)
    {
        result.disposition = MidiLearnDisposition::AlreadyMapped;
        return result;
    }

    result.disposition = MidiLearnDisposition::Ready;
    return result;
}

std::optional<VirtualControlMidiValue> mapMidiController(
    const ProjectDocument& document,
    MidiControllerMessage message,
    std::string_view sourceIdentifier)
{
    if (!isValidMidiControllerMessage(message))
        return std::nullopt;

    const auto found = std::ranges::find_if(document.virtualControls, [&](const VirtualControl& control) {
        return control.midiMapping.has_value()
            && control.midiMapping->channel == message.channel
            && control.midiMapping->controller == message.controller
            && (control.midiMapping->sourceMode
                    == MidiControlSourceMode::LegacyAnyInput
                || (control.midiMapping->sourceMode
                        == MidiControlSourceMode::ExactInput
                    && control.midiMapping->sourceIdentifier
                        == sourceIdentifier));
    });
    if (found == document.virtualControls.end())
        return std::nullopt;

    return VirtualControlMidiValue{
        found->eventName,
        mappedValue(found->kind, found->minimum, found->maximum, message.value),
        found->domain};
}

bool RealtimeMidiControlRoutes::publish(
    MidiControlMapping mapping,
    std::uint32_t eventId,
    VirtualControlKind kind,
    double minimum,
    double maximum,
    talas::expr::ValueDomain domain,
    std::uint8_t sourceSlot,
    std::uint64_t sourceGeneration) noexcept
{
    const PreparedMidiControlRoute prepared{
        mapping,
        eventId,
        kind,
        minimum,
        maximum,
        domain,
        sourceSlot,
        sourceGeneration,
    };
    if (validatePreparedRoute(prepared)
        != MidiControlRouteSetDiagnosticCode::None)
    {
        return false;
    }

    if (mapping.sourceMode == MidiControlSourceMode::ExactInput)
    {
        for (std::uint8_t slot = 1; slot <= 16; ++slot)
        {
            const std::string_view boundIdentifier{
                sourceIdentifierBytes_[slot].data(),
                sourceIdentifierLengths_[slot]};
            if ((slot == sourceSlot && !boundIdentifier.empty()
                    && boundIdentifier != mapping.sourceIdentifier)
                || (slot != sourceSlot
                    && boundIdentifier == mapping.sourceIdentifier))
            {
                return false;
            }
        }
    }

    const auto hasConflictingAuthority = [&] {
        if (mapping.sourceMode == MidiControlSourceMode::LegacyAnyInput)
        {
            for (std::uint8_t slot = 1; slot <= 16; ++slot)
            {
                const auto index = routeIndex(
                    slot, mapping.channel, mapping.controller);
                if (routes_[index].eventId.load(std::memory_order_seq_cst)
                    != 0)
                {
                    return true;
                }
            }
            return false;
        }
        const auto legacyIndex = routeIndex(
            0, mapping.channel, mapping.controller);
        return routes_[legacyIndex].eventId.load(
                   std::memory_order_seq_cst)
            != 0;
    };
    if (hasConflictingAuthority())
        return false;

    const auto storageIndex = routeIndex(
        sourceSlot, mapping.channel, mapping.controller);
    const auto alreadyActive = std::ranges::find(
        std::span{
            activeRouteIndices_.data(), activeRouteCount_},
        storageIndex) != std::span{
            activeRouteIndices_.data(), activeRouteCount_}.end();
    if (!alreadyActive && activeRouteCount_ == activeRouteIndices_.size())
        return false;

    publicationSequence_.fetch_add(1, std::memory_order_seq_cst);
    auto& route = routes_[storageIndex];
    route.minimum.store(minimum, std::memory_order_seq_cst);
    route.maximum.store(maximum, std::memory_order_seq_cst);
    route.kind.store(kind, std::memory_order_seq_cst);
    route.domain.store(domain, std::memory_order_seq_cst);
    route.sourceGeneration.store(
        sourceGeneration, std::memory_order_seq_cst);
    route.eventId.store(eventId, std::memory_order_seq_cst);
    if (!alreadyActive)
        activeRouteIndices_[activeRouteCount_++] = storageIndex;
    if (mapping.sourceMode == MidiControlSourceMode::ExactInput
        && sourceIdentifierLengths_[sourceSlot] == 0)
    {
        std::ranges::copy(
            mapping.sourceIdentifier,
            sourceIdentifierBytes_[sourceSlot].begin());
        sourceIdentifierLengths_[sourceSlot] =
            static_cast<std::uint16_t>(mapping.sourceIdentifier.size());
    }
    publicationSequence_.fetch_add(1, std::memory_order_seq_cst);
    return true;
}

MidiControlRouteSetResult RealtimeMidiControlRoutes::replace(
    std::span<const PreparedMidiControlRoute> routes) noexcept
{
    MidiControlRouteSetResult result;
    if (routes.size() > routeCount)
    {
        result.code = MidiControlRouteSetDiagnosticCode::TooManyRoutes;
        result.routeIndex = routeCount;
        return result;
    }

    std::array<std::size_t, routeStorageCount> sourceOwners;
    sourceOwners.fill(std::numeric_limits<std::size_t>::max());
    std::array<std::string_view, maximumSourceSlots + 1>
        preparedSourceIdentifiers{};
    for (std::size_t index = 0; index < routes.size(); ++index)
    {
        result.code = validatePreparedRoute(routes[index]);
        result.routeIndex = index;
        if (result.code != MidiControlRouteSetDiagnosticCode::None)
            return result;

        if (routes[index].mapping.sourceMode
            == MidiControlSourceMode::ExactInput)
        {
            const auto slot = routes[index].sourceSlot;
            auto& boundIdentifier = preparedSourceIdentifiers[slot];
            bool identifierUsesAnotherSlot = false;
            for (std::uint8_t otherSlot = 1;
                 otherSlot <= maximumSourceSlots;
                 ++otherSlot)
            {
                identifierUsesAnotherSlot = otherSlot != slot
                    && preparedSourceIdentifiers[otherSlot]
                        == routes[index].mapping.sourceIdentifier;
                if (identifierUsesAnotherSlot)
                    break;
            }
            if ((!boundIdentifier.empty()
                    && boundIdentifier
                        != routes[index].mapping.sourceIdentifier)
                || identifierUsesAnotherSlot)
            {
                result.code =
                    MidiControlRouteSetDiagnosticCode::DuplicateSource;
                return result;
            }
            boundIdentifier = routes[index].mapping.sourceIdentifier;
        }

        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (!midiControlMappingsConflict(
                    routes[previous].mapping,
                    routes[index].mapping))
            {
                continue;
            }
            result.code = MidiControlRouteSetDiagnosticCode::DuplicateSource;
            result.conflictingRouteIndex = previous;
            return result;
        }

        const auto sourceIndex = routeIndex(
            routes[index].sourceSlot,
            routes[index].mapping.channel,
            routes[index].mapping.controller);
        if (sourceOwners[sourceIndex]
            != std::numeric_limits<std::size_t>::max())
        {
            result.code = MidiControlRouteSetDiagnosticCode::DuplicateSource;
            result.conflictingRouteIndex = sourceOwners[sourceIndex];
            return result;
        }
        sourceOwners[sourceIndex] = index;
    }

    publicationSequence_.fetch_add(1, std::memory_order_seq_cst);
    for (std::size_t index = 0; index < activeRouteCount_; ++index)
    {
        routes_[activeRouteIndices_[index]].eventId.store(
            0, std::memory_order_seq_cst);
    }
    activeRouteCount_ = 0;
    sourceIdentifierLengths_.fill(0);
    for (const auto& prepared : routes)
    {
        const auto storageIndex = routeIndex(
            prepared.sourceSlot,
            prepared.mapping.channel,
            prepared.mapping.controller);
        auto& route = routes_[storageIndex];
        route.minimum.store(prepared.minimum, std::memory_order_seq_cst);
        route.maximum.store(prepared.maximum, std::memory_order_seq_cst);
        route.kind.store(prepared.kind, std::memory_order_seq_cst);
        route.domain.store(prepared.domain, std::memory_order_seq_cst);
        route.sourceGeneration.store(
            prepared.sourceGeneration, std::memory_order_seq_cst);
        route.eventId.store(prepared.eventId, std::memory_order_seq_cst);
        activeRouteIndices_[activeRouteCount_++] = storageIndex;
        if (prepared.mapping.sourceMode
                == MidiControlSourceMode::ExactInput
            && sourceIdentifierLengths_[prepared.sourceSlot] == 0)
        {
            std::ranges::copy(
                prepared.mapping.sourceIdentifier,
                sourceIdentifierBytes_[prepared.sourceSlot].begin());
            sourceIdentifierLengths_[prepared.sourceSlot] =
                static_cast<std::uint16_t>(
                    prepared.mapping.sourceIdentifier.size());
        }
    }
    publicationSequence_.fetch_add(1, std::memory_order_seq_cst);
    result.routeIndex = routes.size();
    return result;
}

void RealtimeMidiControlRoutes::clear() noexcept
{
    (void)replace({});
}

std::optional<RealtimeMidiControlValue> RealtimeMidiControlRoutes::map(MidiControllerMessage message) const noexcept
{
    if (!isValidMidiControllerMessage(message))
        return std::nullopt;

    const auto before = publicationSequence_.load(std::memory_order_seq_cst);
    if ((before & 1U) != 0)
        return std::nullopt;
    const auto readRoute = [&](std::uint8_t sourceSlot) {
        const auto& route = routes_[routeIndex(
            sourceSlot, message.channel, message.controller)];
        return std::tuple{
            route.eventId.load(std::memory_order_seq_cst),
            route.minimum.load(std::memory_order_seq_cst),
            route.maximum.load(std::memory_order_seq_cst),
            route.kind.load(std::memory_order_seq_cst),
            route.domain.load(std::memory_order_seq_cst),
            route.sourceGeneration.load(std::memory_order_seq_cst)};
    };
    auto [eventId, minimum, maximum, kind, domain, sourceGeneration] =
        message.sourceSlot == 0 ? readRoute(0)
                                : readRoute(message.sourceSlot);
    if ((eventId == 0 || sourceGeneration != message.sourceGeneration)
        && message.sourceSlot != 0)
    {
        std::tie(
            eventId,
            minimum,
            maximum,
            kind,
            domain,
            sourceGeneration) = readRoute(0);
    }
    const auto after = publicationSequence_.load(std::memory_order_seq_cst);
    if (before != after || eventId == 0
        || sourceGeneration != message.sourceGeneration)
        return std::nullopt;

    return RealtimeMidiControlValue{
        eventId,
        mappedValue(kind, minimum, maximum, message.value),
        domain};
}
}
