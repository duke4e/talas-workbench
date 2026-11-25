#pragma once

#include <array>
#include <span>
#include <string_view>

namespace talas::core
{
enum class BuiltinKernelKind
{
    Oscillator,
    Filter,
    Adsr,
    Clock,
    ClockDivider,
    Quantizer,
    Gate,
    Logic,
    Slew,
};

struct BuiltinKernelDescriptor
{
    std::string_view typeId;
    BuiltinKernelKind kind;
};

inline constexpr std::array builtinKernelRegistryEntries = {
    BuiltinKernelDescriptor{"talas.oscillator", BuiltinKernelKind::Oscillator},
    BuiltinKernelDescriptor{"talas.filter", BuiltinKernelKind::Filter},
    BuiltinKernelDescriptor{"talas.adsr", BuiltinKernelKind::Adsr},
    BuiltinKernelDescriptor{"talas.clock", BuiltinKernelKind::Clock},
    BuiltinKernelDescriptor{"talas.clockDivider", BuiltinKernelKind::ClockDivider},
    BuiltinKernelDescriptor{"talas.quantizer", BuiltinKernelKind::Quantizer},
    BuiltinKernelDescriptor{"talas.gate", BuiltinKernelKind::Gate},
    BuiltinKernelDescriptor{"talas.logic", BuiltinKernelKind::Logic},
    BuiltinKernelDescriptor{"talas.slew", BuiltinKernelKind::Slew},
};

[[nodiscard]] inline std::span<const BuiltinKernelDescriptor>
builtinKernelRegistry() noexcept
{
    return builtinKernelRegistryEntries;
}

[[nodiscard]] inline const BuiltinKernelDescriptor*
findBuiltinKernelDescriptor(std::string_view typeId) noexcept
{
    for (const auto& descriptor : builtinKernelRegistryEntries)
        if (descriptor.typeId == typeId)
            return &descriptor;
    return nullptr;
}
}
