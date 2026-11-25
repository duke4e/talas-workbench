#pragma once

// ADR-0029: JUCE's global module settings header for the plugin templates,
// named by JUCE_APP_CONFIG_HEADER. juce_TargetPlatform.h includes it before
// any JUCE code, in every translation unit of the plugin and its VST3 and AU
// wrappers. It points JUCE's identity macros, which JUCE's CMake defines as
// literals, at functions that return the identity embedded in the plugin's
// own bundle. JUCE uses these macros only in ordinary expressions (the VST3
// factory, its class IDs, the AU view's description), never in #if or
// string concatenation, so a function call can stand in for each; JUCE is not
// patched. The rest (IsSynth, WantsMidiInput, AU main type, VST3 category)
// stay compile-time and differ between the instrument and effect templates.

#ifdef __cplusplus
#include <cstdint>

namespace talas::plugin::runtime_identity
{
// Cached on first use in a function-local static; safe during static
// initialisation, before JUCE has started.
[[nodiscard]] const char* name() noexcept;
[[nodiscard]] const char* manufacturer() noexcept;
[[nodiscard]] const char* description() noexcept;
[[nodiscard]] const char* versionString() noexcept;
[[nodiscard]] std::uint32_t versionCode() noexcept;
[[nodiscard]] std::uint32_t manufacturerCode() noexcept;
[[nodiscard]] std::uint32_t pluginCode() noexcept;
}

#undef JucePlugin_Name
#define JucePlugin_Name ::talas::plugin::runtime_identity::name()
#undef JucePlugin_Desc
#define JucePlugin_Desc ::talas::plugin::runtime_identity::description()
#undef JucePlugin_Manufacturer
#define JucePlugin_Manufacturer ::talas::plugin::runtime_identity::manufacturer()
#undef JucePlugin_VersionString
#define JucePlugin_VersionString ::talas::plugin::runtime_identity::versionString()
#undef JucePlugin_VersionCode
#define JucePlugin_VersionCode ::talas::plugin::runtime_identity::versionCode()
#undef JucePlugin_ManufacturerCode
#define JucePlugin_ManufacturerCode ::talas::plugin::runtime_identity::manufacturerCode()
#undef JucePlugin_PluginCode
#define JucePlugin_PluginCode ::talas::plugin::runtime_identity::pluginCode()
// JucePlugin_AUSubType, JucePlugin_AUManufacturerCode and
// JucePlugin_VSTUniqueID are defined as the two codes above and follow them.
#endif
