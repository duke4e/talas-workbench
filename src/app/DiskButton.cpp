#include "talas/app/DiskButton.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace talas::app
{
namespace
{
std::string lowerExtension(std::string_view fileName)
{
    const auto slash = fileName.find_last_of("/\\");
    if (slash != std::string_view::npos)
        fileName.remove_prefix(slash + 1);
    const auto dot = fileName.find_last_of('.');
    std::string extension{dot == std::string_view::npos ? std::string_view{} : fileName.substr(dot)};
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

std::string number(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.6g", value);
    return text;
}
}

DiskButtonAction diskButtonAction(bool commandOrControl, bool shift) noexcept
{
    if (commandOrControl)
        return DiskButtonAction::OpenEditor;
    return shift ? DiskButtonAction::ShowInformation : DiskButtonAction::ChooseFile;
}

FileEditor fileEditorFor(std::string_view fileName, bool wavetable) noexcept
{
    if (wavetable)
        return FileEditor::Sample;
    const auto extension = lowerExtension(fileName);
    if (extension == ".wav" || extension == ".wave" || extension == ".aif" || extension == ".aiff"
        || extension == ".aifc" || extension == ".sf" || extension == ".sd1" || extension == ".sd2")
        return FileEditor::Sample;
    if (extension == ".talaspartial" || extension == ".spc")
        return FileEditor::Spectrum;
    return FileEditor::None;
}

std::string sampleFileInformationText(const SampleFileInformation& file)
{
    const auto extension = lowerExtension(file.name);
    const auto format = extension == ".wav" || extension == ".wave" ? "WAV"
        : extension == ".aif" || extension == ".aiff" || extension == ".aifc" ? "AIFF"
        : extension == ".sf"                                                   ? "IRCAM"
        : extension == ".sd1"                                                  ? "SD-I"
        : extension == ".sd2"                                                  ? "SD-II"
                                                                               : "Sample file";
    std::string text = file.name + "\n" + format + ", " + std::to_string(file.channels)
        + (file.channels == 1 ? " channel" : " channels") + ", " + number(file.sampleRate) + " Hz";
    text += "\nDuration: " + std::to_string(file.frames) + " samp ("
        + number(file.sampleRate > 0.0 ? static_cast<double>(file.frames) / file.sampleRate : 0.0) + " s)";
    if (file.loop)
        text += "\nLoop: " + std::to_string(file.loop->first) + " - " + std::to_string(file.loop->second) + " samp";
    if (file.pitch)
        text += "\nPitch: " + number(*file.pitch) + " nn";
    return text;
}

std::string spectrumFileInformationText(std::string_view name, const talas::project::PartialTrackAsset& spectrum)
{
    std::size_t partials = 0;
    for (const auto& frame : spectrum.frames)
        partials = std::max(partials, frame.points.size());
    const auto rate = spectrum.sampleRate > 0.0 ? spectrum.sampleRate : 1.0;
    std::string text{name};
    text += "\nSpectrum file, " + std::to_string(spectrum.frames.size()) + " frames of up to "
        + std::to_string(partials) + " partials";
    if (spectrum.frames.size() > 1)
    {
        const auto hop = static_cast<double>(spectrum.frames[1].sourceFrame - spectrum.frames[0].sourceFrame) / rate;
        text += "\nFrame: " + number(hop * 1000.0) + " ms";
    }
    text += "\nDuration: " + number(static_cast<double>(spectrum.sourceFrameCount) / rate) + " s, analysed at "
        + number(spectrum.sampleRate) + " Hz";
    return text;
}

std::string wavetableInformationText(std::string_view name, std::size_t samples)
{
    return std::string{name} + "\nOne of the built-in wavetables, " + std::to_string(samples) + " samples";
}

std::optional<std::string> fromMemoryWriterCheckBox(
    const talas::core::NodeTypeDescriptor& descriptor, std::string_view fileParameterId)
{
    const auto booleanNamed = [&](std::string_view id) {
        const auto box = std::ranges::find(descriptor.parameters, id, &talas::core::ParameterDescriptor::id);
        return box != descriptor.parameters.end() && box->kind == talas::core::ParameterKind::Boolean;
    };
    const auto file = std::ranges::find(descriptor.parameters, talas::core::ParameterKind::Resource,
        &talas::core::ParameterDescriptor::kind);
    // The Sound's first file field: its FromMemoryWriter.
    if (file != descriptor.parameters.end() && file->id == fileParameterId && booleanNamed("fromMemoryWriter"))
        return std::string{"fromMemoryWriter"};
    // Another file field: a box of its own named for it.
    const auto field = std::ranges::find(descriptor.parameters, fileParameterId, &talas::core::ParameterDescriptor::id);
    const auto own = std::string{fileParameterId} + "FromMemoryWriter";
    if (field != descriptor.parameters.end() && field->kind == talas::core::ParameterKind::Resource && booleanNamed(own))
        return own;
    return std::nullopt;
}
}
