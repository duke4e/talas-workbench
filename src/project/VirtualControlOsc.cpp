#include "talas/project/VirtualControlOsc.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <thread>

namespace talas::project
{
namespace
{
[[nodiscard]] constexpr bool isAsciiLetter(char value) noexcept
{
    return (value >= 'A' && value <= 'Z')
        || (value >= 'a' && value <= 'z');
}

[[nodiscard]] constexpr bool isSegmentHead(char value) noexcept
{
    return isAsciiLetter(value) || value == '_';
}

[[nodiscard]] constexpr bool isSegmentTail(char value) noexcept
{
    return isSegmentHead(value)
        || (value >= '0' && value <= '9')
        || value == '-';
}
}

bool isValidOscAddress(std::string_view address) noexcept
{
    if (address.size() < minimumOscAddressBytes
        || address.size() > maximumOscAddressBytes
        || address.front() != '/')
    {
        return false;
    }

    std::size_t segmentCount = 0;
    std::size_t index = 1;
    while (index < address.size())
    {
        if (++segmentCount > maximumOscAddressSegments
            || !isSegmentHead(address[index]))
        {
            return false;
        }
        ++index;
        while (index < address.size() && address[index] != '/')
        {
            if (!isSegmentTail(address[index]))
                return false;
            ++index;
        }
        if (index < address.size())
        {
            ++index;
            if (index == address.size())
                return false;
        }
    }
    return segmentCount != 0;
}

bool isValidOscControlMapping(const OscControlMapping& mapping) noexcept
{
    return isValidOscAddress(mapping.address);
}

bool isValidOscEgressMapping(const OscEgressMapping& mapping) noexcept
{
    return isValidOscAddress(mapping.address);
}

bool isValidOscIngressSettings(
    const ProjectOscIngressSettings& settings) noexcept
{
    if (!settings.enabled)
        return settings.port == defaultOscIngressPort;
    return settings.port >= minimumOscIngressPort
        && settings.port <= maximumOscIngressPort;
}

bool isValidOscEgressSettings(
    const ProjectOscEgressSettings& settings) noexcept
{
    if (!settings.enabled)
        return settings.port == defaultOscEgressPort;
    return settings.port >= minimumOscIngressPort
        && settings.port <= maximumOscIngressPort;
}

bool isValidOscEndpointPair(
    const ProjectOscIngressSettings& ingress,
    const ProjectOscEgressSettings& egress) noexcept
{
    return isValidOscIngressSettings(ingress)
        && isValidOscEgressSettings(egress)
        && (!ingress.enabled || !egress.enabled
            || ingress.port != egress.port);
}

std::optional<double> normalizeOscEgressValue(
    VirtualControlKind kind,
    double value,
    double minimum,
    double maximum) noexcept
{
    if ((kind != VirtualControlKind::Fader
         && kind != VirtualControlKind::Gate
         && kind != VirtualControlKind::Toggle)
        || !std::isfinite(value)
        || !std::isfinite(minimum) || !std::isfinite(maximum)
        || !(minimum < maximum) || value < minimum || value > maximum)
    {
        return std::nullopt;
    }
    if (kind != VirtualControlKind::Fader)
        return value < std::midpoint(minimum, maximum) ? 0.0 : 1.0;
    if (value <= minimum)
        return 0.0;
    if (value >= maximum)
        return 1.0;

    const auto scale = std::max(
        {std::abs(minimum), std::abs(maximum), 1.0});
    const auto scaledMinimum = minimum / scale;
    const auto scaledMaximum = maximum / scale;
    const auto scaledValue = value / scale;
    const auto denominator = scaledMaximum - scaledMinimum;
    if (!std::isfinite(denominator) || !(denominator > 0.0))
        return std::nullopt;
    const auto normalized =
        (scaledValue - scaledMinimum) / denominator;
    if (!std::isfinite(normalized))
        return std::nullopt;
    return std::clamp(normalized, 0.0, 1.0);
}

namespace
{
[[nodiscard]] bool isValidKind(VirtualControlKind kind) noexcept
{
    return kind == VirtualControlKind::Fader
        || kind == VirtualControlKind::Gate
        || kind == VirtualControlKind::Toggle;
}

[[nodiscard]] bool isValidDomain(talas::expr::ValueDomain domain) noexcept
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

[[nodiscard]] OscControlRouteSetDiagnosticCode validateRoute(
    const PreparedOscControlRoute& route) noexcept
{
    if (!isValidOscControlMapping(route.mapping))
        return OscControlRouteSetDiagnosticCode::InvalidAddress;
    if (route.eventId == 0)
        return OscControlRouteSetDiagnosticCode::InvalidEventId;
    if (!isValidKind(route.kind))
        return OscControlRouteSetDiagnosticCode::InvalidKind;
    if (!isValidDomain(route.domain))
        return OscControlRouteSetDiagnosticCode::InvalidDomain;
    if (!std::isfinite(route.minimum) || !std::isfinite(route.maximum)
        || !(route.minimum < route.maximum))
    {
        return OscControlRouteSetDiagnosticCode::InvalidRange;
    }
    if (route.stageGeneration == 0)
        return OscControlRouteSetDiagnosticCode::InvalidStage;
    return OscControlRouteSetDiagnosticCode::None;
}

[[nodiscard]] double mapNormalized(
    VirtualControlKind kind,
    double minimum,
    double maximum,
    double normalized) noexcept
{
    if (kind == VirtualControlKind::Fader)
        return std::lerp(minimum, maximum, normalized);
    return normalized < 0.5 ? minimum : maximum;
}
}

bool RealtimeOscRouteAdmissionGate::tryBegin() noexcept
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

void RealtimeOscRouteAdmissionGate::end() noexcept
{
    state_.fetch_sub(1, std::memory_order_release);
}

void RealtimeOscRouteAdmissionGate::closeAndWait() noexcept
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

void RealtimeOscRouteAdmissionGate::open() noexcept
{
    state_.store(0, std::memory_order_release);
}

OscControlRouteSetResult RealtimeOscControlRoutes::replace(
    std::span<const PreparedOscControlRoute> routes) noexcept
{
    OscControlRouteSetResult result;
    if (routes.size() > routes_.size())
    {
        result.code = OscControlRouteSetDiagnosticCode::TooManyRoutes;
        result.routeIndex = routes_.size();
        return result;
    }
    for (std::size_t index = 0; index < routes.size(); ++index)
    {
        result.code = validateRoute(routes[index]);
        result.routeIndex = index;
        if (result.code != OscControlRouteSetDiagnosticCode::None)
            return result;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
        {
            if (routes[earlier].mapping.address
                == routes[index].mapping.address)
            {
                result.code =
                    OscControlRouteSetDiagnosticCode::DuplicateAddress;
                result.conflictingRouteIndex = earlier;
                return result;
            }
        }
    }

    std::array<Route, maximumOscControlMappings> candidate{};
    for (std::size_t index = 0; index < routes.size(); ++index)
    {
        const auto& source = routes[index];
        auto& target = candidate[index];
        std::memcpy(
            target.address.data(),
            source.mapping.address.data(),
            source.mapping.address.size());
        target.addressSize = static_cast<std::uint16_t>(
            source.mapping.address.size());
        target.eventId = source.eventId;
        target.kind = source.kind;
        target.minimum = source.minimum;
        target.maximum = source.maximum;
        target.domain = source.domain;
        target.stageGeneration = source.stageGeneration;
        target.performanceMacro = source.performanceMacro;
    }
    routes_ = candidate;
    routeCount_ = routes.size();
    return result;
}

void RealtimeOscControlRoutes::clear() noexcept
{
    routes_ = {};
    routeCount_ = 0;
}

std::optional<RealtimeOscControlValue> RealtimeOscControlRoutes::map(
    std::string_view address,
    double normalizedValue) const noexcept
{
    if (!isValidOscAddress(address) || !std::isfinite(normalizedValue)
        || normalizedValue < 0.0 || normalizedValue > 1.0)
    {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < routeCount_; ++index)
    {
        const auto& route = routes_[index];
        if (address != std::string_view{
                route.address.data(), route.addressSize})
        {
            continue;
        }
        return RealtimeOscControlValue{
            route.eventId,
            mapNormalized(
                route.kind,
                route.minimum,
                route.maximum,
                normalizedValue),
            route.domain,
            route.stageGeneration,
            route.performanceMacro,
        };
    }
    return std::nullopt;
}
}
