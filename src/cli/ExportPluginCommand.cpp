#include "cli/ExportPluginCommand.h"

#include "talas/plugin/PluginExport.h"

#include <ostream>
#include <string>

namespace talas::cli
{
namespace
{
constexpr int successExitCode = 0;
constexpr int failureExitCode = 1;
constexpr int usageExitCode = 2;

[[nodiscard]] std::string domainName(talas::expr::ValueDomain domain)
{
    switch (domain)
    {
    case talas::expr::ValueDomain::Hertz:
        return "hz";
    case talas::expr::ValueDomain::Seconds:
        return "seconds";
    case talas::expr::ValueDomain::NoteNumber:
        return "note number";
    case talas::expr::ValueDomain::Samples:
        return "samples";
    case talas::expr::ValueDomain::Scalar:
        break;
    }
    return "scalar";
}
}

int runExportPluginCommand(
    std::span<const std::string_view> args,
    std::ostream& output,
    std::ostream& error)
{
    using namespace talas::plugin;
    PluginExportRequest request;
    bool kindGiven = false;
    bool formatsGiven = false;
    for (std::size_t index = 1; index < args.size(); ++index)
    {
        const auto option = args[index];
        if (option == "--no-sign")
        {
            request.sign = false;
            continue;
        }
        if (index + 1 >= args.size())
        {
            error << option << " requires a value\n";
            return usageExitCode;
        }
        const auto value = args[++index];
        if (option == "--project")
            request.projectPath = std::filesystem::path{std::string{value}};
        else if (option == "--name")
            request.name = std::string{value};
        else if (option == "--manufacturer")
            request.manufacturer = std::string{value};
        else if (option == "--kind")
        {
            const auto kind = parsePluginKind(value);
            if (!kind.has_value())
            {
                error << "--kind must be instrument or effect\n";
                return usageExitCode;
            }
            request.kind = *kind;
            kindGiven = true;
        }
        else if (option == "--format")
        {
            std::string_view rest = value;
            while (!rest.empty())
            {
                const auto comma = rest.find(',');
                const auto part = rest.substr(0, comma);
                const auto format = parsePluginFormat(part);
                if (!format.has_value())
                {
                    error << "--format takes vst3, au or both separated by a comma, not '" << part << "'\n";
                    return usageExitCode;
                }
                request.formats.push_back(*format);
                if (comma == std::string_view::npos)
                    break;
                rest.remove_prefix(comma + 1);
            }
            formatsGiven = true;
        }
        else if (option == "--output")
            request.outputDirectory = std::filesystem::path{std::string{value}};
        else if (option == "--template-dir")
            request.templateDirectories.emplace_back(std::string{value});
        else if (option == "--manufacturer-code")
            request.manufacturerCode = std::string{value};
        else if (option == "--plugin-code")
            request.pluginCode = std::string{value};
        else if (option == "--version")
            request.version = std::string{value};
        else
        {
            error << "Unknown export-plugin option: " << option << '\n';
            return usageExitCode;
        }
    }
    if (!request.projectPath.has_value() || request.name.empty() || !kindGiven || request.outputDirectory.empty())
    {
        error << "export-plugin requires --project, --name, --kind and --output\n";
        return usageExitCode;
    }
    if (!formatsGiven)
    {
        request.formats.push_back(PluginFormat::Vst3);
#if defined(__APPLE__)
        request.formats.push_back(PluginFormat::AudioUnit);
#endif
    }
    if (!request.templateDirectories.empty())
    {
        // Named folders come first; the usual places stay as fallbacks.
        for (auto& directory : defaultPluginTemplateDirectories())
            request.templateDirectories.push_back(std::move(directory));
    }

    const auto result = exportPlugin(request);
    for (const auto& warning : result.warnings)
        error << "Warning: " << warning << '\n';
    if (!result.ok())
    {
        error << "Plugin export failed: " << result.error << '\n';
        return failureExitCode;
    }
    const auto& identity = *result.identity;
    output << "Exported " << pluginKindName(identity.kind) << " \"" << identity.name << "\" by "
           << identity.manufacturer << " (manufacturer code " << identity.manufacturerCode << ", plugin code "
           << identity.pluginCode << ", version " << identity.version << ")\n";
    for (const auto& bundle : result.bundles)
        output << "  " << pluginFormatName(bundle.format) << ": " << bundle.path.string() << '\n';
    if (result.latencyFrames > 0)
        output << "Latency: " << result.latencyFrames << " frames at 48 kHz\n";
    output << "Parameters: " << result.parameters.size() << '\n';
    for (const auto& parameter : result.parameters)
    {
        output << "  !" << parameter.eventName << " \"" << parameter.displayName << "\" " << parameter.minimum
               << " to " << parameter.maximum << ", default " << parameter.defaultValue;
        if (!parameter.unit.empty())
            output << ' ' << parameter.unit;
        output << " (" << domainName(parameter.domain)
               << (parameter.kind == PluginParameterKind::Continuous
                          ? (parameter.logarithmic ? ", log" : "")
                          : (parameter.kind == PluginParameterKind::Toggle ? ", toggle" : ", gate"))
               << (parameter.declared ? "" : ", inferred") << ")\n";
    }
    output << pluginExportLicenseNotice() << '\n';
    return successExitCode;
}
}
