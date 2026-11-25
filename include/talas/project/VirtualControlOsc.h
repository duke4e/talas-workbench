#pragma once

#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <atomic>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace talas::project
{
inline constexpr std::size_t minimumOscAddressBytes = 2;
inline constexpr std::size_t maximumOscAddressBytes = 256;
inline constexpr std::size_t maximumOscAddressSegments = 8;
inline constexpr std::size_t maximumOscControlMappings = 128;
inline constexpr int defaultOscIngressPort = 9000;
inline constexpr int defaultOscEgressPort = 9001;
inline constexpr int minimumOscIngressPort = 1024;
inline constexpr int maximumOscIngressPort = 65535;

[[nodiscard]] bool isValidOscAddress(std::string_view address) noexcept;
[[nodiscard]] bool isValidOscControlMapping(
    const OscControlMapping& mapping) noexcept;
[[nodiscard]] bool isValidOscEgressMapping(
    const OscEgressMapping& mapping) noexcept;
[[nodiscard]] bool isValidOscIngressSettings(
    const ProjectOscIngressSettings& settings) noexcept;
[[nodiscard]] bool isValidOscEgressSettings(
    const ProjectOscEgressSettings& settings) noexcept;
[[nodiscard]] bool isValidOscEndpointPair(
    const ProjectOscIngressSettings& ingress,
    const ProjectOscEgressSettings& egress) noexcept;
[[nodiscard]] std::optional<double> normalizeOscEgressValue(
    VirtualControlKind kind,
    double value,
    double minimum,
    double maximum) noexcept;

struct PreparedOscControlRoute
{
    OscControlMapping mapping;
    std::uint32_t eventId = 0;
    VirtualControlKind kind = VirtualControlKind::Fader;
    double minimum = 0.0;
    double maximum = 1.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::uint64_t stageGeneration = 0;
    bool performanceMacro = false;
};

struct RealtimeOscControlValue
{
    std::uint32_t eventId = 0;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::uint64_t stageGeneration = 0;
    bool performanceMacro = false;
};

enum class OscControlRouteSetDiagnosticCode : std::uint8_t
{
    None,
    TooManyRoutes,
    InvalidAddress,
    DuplicateAddress,
    InvalidEventId,
    InvalidKind,
    InvalidDomain,
    InvalidRange,
    InvalidStage,
};

struct OscControlRouteSetResult
{
    OscControlRouteSetDiagnosticCode code =
        OscControlRouteSetDiagnosticCode::None;
    std::size_t routeIndex = 0;
    std::size_t conflictingRouteIndex =
        std::numeric_limits<std::size_t>::max();

    [[nodiscard]] bool ok() const noexcept
    {
        return code == OscControlRouteSetDiagnosticCode::None;
    }
};

class RealtimeOscRouteAdmissionGate
{
public:
    [[nodiscard]] bool tryBegin() noexcept;
    void end() noexcept;
    void closeAndWait() noexcept;
    void open() noexcept;

private:
    static constexpr std::uint64_t closedBit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t activeMask = closedBit - 1;
    std::atomic<std::uint64_t> state_ = 0;
};

class RealtimeOscControlRoutes
{
public:
    // One control-thread writer. The owner must close its admission gate
    // across replace/clear and hold the same gate through map + queue enqueue.
    [[nodiscard]] OscControlRouteSetResult replace(
        std::span<const PreparedOscControlRoute> routes) noexcept;
    void clear() noexcept;
    [[nodiscard]] std::optional<RealtimeOscControlValue> map(
        std::string_view address,
        double normalizedValue) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return routeCount_; }

private:
    struct Route
    {
        std::array<char, maximumOscAddressBytes + 1> address{};
        std::uint16_t addressSize = 0;
        std::uint32_t eventId = 0;
        VirtualControlKind kind = VirtualControlKind::Fader;
        double minimum = 0.0;
        double maximum = 1.0;
        talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
        std::uint64_t stageGeneration = 0;
        bool performanceMacro = false;
    };

    std::array<Route, maximumOscControlMappings> routes_{};
    std::size_t routeCount_ = 0;
};
}
