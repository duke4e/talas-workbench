#pragma once

#include "plugin/TalasPluginJuceConfig.h"
#include "talas/plugin/PluginIdentity.h"

#include <filesystem>
#include <string>

// ADR-0029: what a template finds in its own bundle at load time.
namespace talas::plugin::runtime_identity
{
// The kind the template was built as (JucePlugin_IsSynth).
[[nodiscard]] PluginKind templateKind() noexcept;
// The embedded identity, or the template's own when the bundle has none.
[[nodiscard]] const PluginIdentity& identity() noexcept;
[[nodiscard]] bool hasEmbeddedIdentity() noexcept;
// Why an embedded talas-plugin.json was not used; empty when it was, or
// there is none.
[[nodiscard]] const std::string& identityProblem() noexcept;
// The plugin bundle's Contents/Resources; empty when the plugin binary's
// path is unknown.
[[nodiscard]] const std::filesystem::path& resourcesDirectory() noexcept;
}
