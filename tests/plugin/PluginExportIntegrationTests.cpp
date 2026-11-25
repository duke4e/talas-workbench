// ADR-0029: plugin export end to end. Exports a demo instrument and a demo
// effect with the real templates into a temporary folder, loads each VST3
// with JUCE's VST3 host and, on macOS, each AU registered in this process
// (nothing is installed), and plays them.

#include "talas/plugin/PluginExport.h"
#include "talas/plugin/PluginPlatform.h"
#include "talas/plugin/PluginPlist.h"

#include <juce_audio_processors/juce_audio_processors.h>

#if JUCE_MAC
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#endif

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using namespace talas::plugin;
namespace fs = std::filesystem;

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 512;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::string readFile(const fs::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] std::string hexOf(const juce::VST3Interface::Id& id)
{
    std::string text;
    constexpr char digits[] = "0123456789ABCDEF";
    for (const auto byte : id)
    {
        text.push_back(digits[(static_cast<unsigned>(byte) >> 4U) & 0xFU]);
        text.push_back(digits[static_cast<unsigned>(byte) & 0xFU]);
    }
    return text;
}

struct Signal
{
    double peak = 0.0;
    double rms = 0.0;
    bool finite = true;
};

[[nodiscard]] Signal measure(const juce::AudioBuffer<float>& buffer, int channels)
{
    Signal signal;
    double sum = 0.0;
    std::size_t count = 0;
    for (int channel = 0; channel < channels; ++channel)
    {
        for (int frame = 0; frame < buffer.getNumSamples(); ++frame)
        {
            const auto sample = static_cast<double>(buffer.getSample(channel, frame));
            signal.finite = signal.finite && std::isfinite(sample);
            signal.peak = std::max(signal.peak, std::abs(sample));
            sum += sample * sample;
            ++count;
        }
    }
    signal.rms = count == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(count));
    return signal;
}

[[nodiscard]] juce::AudioProcessorParameter* parameterNamed(juce::AudioPluginInstance& instance, const std::string& name)
{
    for (auto* parameter : instance.getParameters())
    {
        if (parameter->getName(128).toStdString() == name)
            return parameter;
    }
    throw std::runtime_error("the plugin has no parameter named " + name);
}

void setParameter(juce::AudioPluginInstance& instance, const PluginParameterSpec& spec, double value)
{
    auto* parameter = parameterNamed(instance, spec.displayName);
    parameter->setValueNotifyingHost(static_cast<float>(parameterValueToNormalized(spec, value)));
}

[[nodiscard]] const PluginParameterSpec& specNamed(const PluginExportResult& result, std::string_view eventName)
{
    const auto found = std::ranges::find(result.parameters, eventName, &PluginParameterSpec::eventName);
    require(found != result.parameters.end(), "the export maps !" + std::string{eventName});
    return *found;
}

// The parameter names a host sees, in order, without what JUCE's VST3
// wrapper adds: a bypass and, for an instrument, the hidden "MIDI CC"
// parameters that carry controllers, pitch bend and pressure to it.
void requireParameterNames(juce::AudioPluginInstance& instance, const PluginExportResult& result, const std::string& what)
{
    std::vector<std::string> names;
    for (auto* parameter : instance.getParameters())
    {
        const auto name = parameter->getName(128).toStdString();
        if (name != "Bypass" && name != "Program" && !name.starts_with("MIDI CC "))
            names.push_back(name);
    }
    std::vector<std::string> expected;
    for (const auto& spec : result.parameters)
        expected.push_back(spec.displayName);
    if (what.starts_with("AU"))
    {
        // JUCE's AU wrapper lists parameters in the order of their hashed
        // IDs (so the list stays stable as a plugin grows), not the project's.
        std::ranges::sort(names);
        std::ranges::sort(expected);
    }
    std::string shown;
    for (const auto& name : names)
        shown += " '" + name + "'";
    require(names == expected, what + " shows the project's controls as parameters, got" + shown);
}

[[nodiscard]] PluginExportResult exportDemo(
    const fs::path& output,
    std::string_view demo,
    std::string name,
    PluginKind kind)
{
    PluginExportRequest request;
    request.projectPath = fs::path{TALAS_SOURCE_DIR} / "examples" / "demos" / (std::string{demo} + ".talas.json");
    request.name = std::move(name);
    request.manufacturer = "Talas Tests";
    request.kind = kind;
    request.formats = {PluginFormat::Vst3};
    if (platform::supportsAudioUnits())
        request.formats.push_back(PluginFormat::AudioUnit);
    request.outputDirectory = output;
    request.templateDirectories = {fs::path{TALAS_PLUGIN_TEMPLATE_DIR}};
    auto result = exportPlugin(request);
    require(result.ok(), "exporting " + std::string{demo} + " succeeds: " + result.error);
    return result;
}

[[nodiscard]] fs::path bundleOf(const PluginExportResult& result, PluginFormat format)
{
    for (const auto& bundle : result.bundles)
    {
        if (bundle.format == format)
            return bundle.path;
    }
    throw std::runtime_error("the export has no " + std::string{pluginFormatName(format)} + " bundle");
}

void checkBundle(const PluginExportResult& result)
{
    const auto& identity = *result.identity;
    for (const auto& bundle : result.bundles)
    {
        require(platform::verifyBundleSignature(bundle.path).empty(),
            std::string{pluginFormatName(bundle.format)} + " bundle passes codesign --verify: "
                + platform::verifyBundleSignature(bundle.path));
        const auto resources = bundle.path / "Contents" / "Resources";
        require(parsePluginIdentity(readFile(resources / "talas-plugin.json")) == identity,
            "the bundle embeds the identity");
        require(fs::is_regular_file(resources / "project.talas.json"), "and the project");
    }
    const auto expectedComponent = hexOf(juce::VST3Interface::jucePluginId(
        fourCharCode(identity.manufacturerCode), fourCharCode(identity.pluginCode),
        juce::VST3Interface::Type::component));
    const auto expectedController = hexOf(juce::VST3Interface::jucePluginId(
        fourCharCode(identity.manufacturerCode), fourCharCode(identity.pluginCode),
        juce::VST3Interface::Type::controller));
    require(vst3ClassIdText(fourCharCode(identity.manufacturerCode), fourCharCode(identity.pluginCode), false)
            == expectedComponent,
        "the exporter's component class ID is JUCE's");
    const auto moduleInfo = readFile(bundleOf(result, PluginFormat::Vst3) / "Contents" / "Resources" / "moduleinfo.json");
    require(moduleInfo.find(expectedComponent) != std::string::npos
            && moduleInfo.find(expectedController) != std::string::npos,
        "moduleinfo.json lists JUCE's class IDs for the plugin's codes");
#if JUCE_MAC
    const auto au = bundleOf(result, PluginFormat::AudioUnit);
    const auto info = plist::parse(readFile(au / "Contents" / "Info.plist"));
    require(info.ok(), "the AU's Info.plist parses");
    const auto& component = info.root->find("AudioComponents")->items().front();
    require(*component.findString("name") == identity.manufacturer + ": " + identity.name, "the AU is named for the plugin");
    require(*component.findString("type") == auComponentType(identity.kind), "with its kind's component type");
    require(*component.findString("subtype") == identity.pluginCode
            && *component.findString("manufacturer") == identity.manufacturerCode,
        "and its codes");
    require(fs::is_regular_file(au / "Contents" / "MacOS" / identity.name), "the AU binary is named for the plugin");
#endif
}

[[nodiscard]] juce::PluginDescription describeVst3(const fs::path& bundle)
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile(found, juce::String{bundle.string()});
    require(found.size() == 1, "the VST3 " + bundle.string() + " holds one plugin, found " + std::to_string(found.size()));
    return *found[0];
}

[[nodiscard]] std::unique_ptr<juce::AudioPluginInstance> instantiate(
    juce::AudioPluginFormat& format,
    const juce::PluginDescription& description)
{
    juce::String error;
    auto instance = format.createInstanceFromDescription(description, sampleRate, blockSize, error);
    require(instance != nullptr, "the plugin instantiates: " + error.toStdString());
    return instance;
}

// Render `blocks` blocks; `midiAt(block)` gives each block's MIDI.
Signal play(
    juce::AudioPluginInstance& instance,
    int blocks,
    const std::function<juce::MidiBuffer(int)>& midiAt = {},
    const std::function<void(juce::AudioBuffer<float>&, int)>& inputAt = {},
    juce::AudioBuffer<float>* last = nullptr)
{
    juce::AudioBuffer<float> buffer(2, blockSize);
    Signal total;
    double sum = 0.0;
    for (int block = 0; block < blocks; ++block)
    {
        buffer.clear();
        if (inputAt)
            inputAt(buffer, block);
        auto midi = midiAt ? midiAt(block) : juce::MidiBuffer{};
        instance.processBlock(buffer, midi);
        const auto signal = measure(buffer, 2);
        total.finite = total.finite && signal.finite;
        total.peak = std::max(total.peak, signal.peak);
        sum += signal.rms * signal.rms;
    }
    total.rms = blocks == 0 ? 0.0 : std::sqrt(sum / blocks);
    if (last != nullptr)
        *last = buffer;
    return total;
}

void checkInstrument(juce::AudioPluginInstance& instance, const PluginExportResult& result, const std::string& format)
{
    require(instance.getName().toStdString() == result.identity->name, format + " instrument reports its name");
    require(instance.acceptsMidi(), format + " instrument takes MIDI");
    requireParameterNames(instance, result, format + " instrument");
    const auto& demo = specNamed(result, "Demo");
    // The demo phrase off: only the host's notes sound.
    setParameter(instance, demo, 0.0);
    instance.setPlayConfigDetails(0, 2, sampleRate, blockSize);
    instance.prepareToPlay(sampleRate, blockSize);
    const auto quiet = play(instance, 24);
    require(quiet.finite && quiet.peak < 1.0e-4, format + " instrument is silent without notes: peak "
            + std::to_string(quiet.peak));
    const auto notes = play(instance, 48, [](int block) {
        juce::MidiBuffer midi;
        if (block == 0)
        {
            midi.addEvent(juce::MidiMessage::noteOn(1, 36, static_cast<juce::uint8>(110)), 64);
            midi.addEvent(juce::MidiMessage::noteOn(1, 43, static_cast<juce::uint8>(90)), 200);
        }
        if (block == 40)
        {
            midi.addEvent(juce::MidiMessage::noteOff(1, 36), 0);
            midi.addEvent(juce::MidiMessage::noteOff(1, 43), 0);
        }
        return midi;
    });
    require(notes.finite, format + " instrument output is finite");
    require(notes.rms > 1.0e-3 && notes.peak > 1.0e-2,
        format + " instrument plays the host's notes: rms " + std::to_string(notes.rms));
    // Let the release end, then the demo phrase on: a parameter change is heard.
    (void)play(instance, 48);
    const auto released = play(instance, 8);
    require(released.peak < 1.0e-3, format + " instrument falls silent after note-off: peak "
            + std::to_string(released.peak));
    setParameter(instance, demo, 1.0);
    const auto phrase = play(instance, 140);
    require(phrase.finite && phrase.rms > 1.0e-3,
        format + " instrument's Demo parameter turns the phrase on: rms " + std::to_string(phrase.rms));
    instance.releaseResources();
}

void fillSine(juce::AudioBuffer<float>& buffer, int block)
{
    for (int frame = 0; frame < buffer.getNumSamples(); ++frame)
    {
        const auto time = static_cast<double>(block * blockSize + frame) / sampleRate;
        const auto sample = static_cast<float>(0.25 * std::sin(2.0 * juce::MathConstants<double>::pi * 220.0 * time));
        buffer.setSample(0, frame, sample);
        buffer.setSample(1, frame, sample);
    }
}

// How far the output is from the input it was given, relative to the input.
[[nodiscard]] double changeFromInput(juce::AudioPluginInstance& instance, int blocks)
{
    juce::AudioBuffer<float> buffer(2, blockSize);
    double difference = 0.0;
    double input = 0.0;
    for (int block = 0; block < blocks; ++block)
    {
        fillSine(buffer, block);
        juce::AudioBuffer<float> dry;
        dry.makeCopyOf(buffer);
        juce::MidiBuffer midi;
        instance.processBlock(buffer, midi);
        if (block < blocks / 2)
            continue;
        for (int channel = 0; channel < 2; ++channel)
        {
            for (int frame = 0; frame < blockSize; ++frame)
            {
                const auto delta = buffer.getSample(channel, frame) - dry.getSample(channel, frame);
                require(std::isfinite(buffer.getSample(channel, frame)), "the effect's output is finite");
                difference += delta * delta;
                input += dry.getSample(channel, frame) * dry.getSample(channel, frame);
            }
        }
    }
    return std::sqrt(difference / std::max(input, 1.0e-12));
}

void checkEffect(juce::AudioPluginInstance& instance, const PluginExportResult& result, const std::string& format)
{
    require(instance.getName().toStdString() == result.identity->name, format + " effect reports its name");
    requireParameterNames(instance, result, format + " effect");
    instance.setPlayConfigDetails(2, 2, sampleRate, blockSize);
    instance.prepareToPlay(sampleRate, blockSize);
    require(instance.getLatencySamples() == static_cast<int>(result.latencyFrames), format + " effect reports its latency");
    const auto& mix = specNamed(result, "Mix");
    setParameter(instance, mix, 1.0);
    const auto wet = changeFromInput(instance, 32);
    require(wet > 0.3, format + " effect changes its input: relative difference " + std::to_string(wet));
    setParameter(instance, mix, 0.0);
    const auto dry = changeFromInput(instance, 32);
    require(dry < 0.05, format + " effect's Mix parameter takes it back to the dry input: relative difference "
            + std::to_string(dry));
    instance.releaseResources();
}

void checkStateRoundTrip(
    juce::AudioPluginFormat& format,
    const juce::PluginDescription& description,
    const PluginExportResult& result,
    const std::string& what)
{
    auto first = instantiate(format, description);
    std::vector<float> chosen;
    for (std::size_t index = 0; index < result.parameters.size(); ++index)
    {
        const auto& spec = result.parameters[index];
        const auto target = spec.kind == PluginParameterKind::Continuous
            ? spec.minimum + (spec.maximum - spec.minimum) * (0.2 + 0.05 * static_cast<double>(index))
            : (spec.defaultValue >= spec.maximum ? spec.minimum : spec.maximum);
        setParameter(*first, spec, target);
        chosen.push_back(parameterNamed(*first, spec.displayName)->getValue());
    }
    juce::MemoryBlock state;
    first->getStateInformation(state);
    require(state.getSize() > 0, what + " saves a state");
    auto second = instantiate(format, description);
    second->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    for (std::size_t index = 0; index < result.parameters.size(); ++index)
    {
        const auto restored = parameterNamed(*second, result.parameters[index].displayName)->getValue();
        require(std::abs(restored - chosen[index]) < 1.0e-4F,
            what + " restores !" + result.parameters[index].eventName + ": " + std::to_string(restored) + " vs "
                + std::to_string(chosen[index]));
    }
}

void checkVst3(const PluginExportResult& result, const fs::path& scratch, bool instrument)
{
    const auto bundle = bundleOf(result, PluginFormat::Vst3);
    const auto fast = describeVst3(bundle);
    require(fast.name.toStdString() == result.identity->name, "the VST3's moduleinfo names the plugin");
    require(fast.manufacturerName.toStdString() == result.identity->manufacturer, "and its manufacturer");
    require(fast.isInstrument == instrument, "and its kind");
    // Without moduleinfo.json a host asks the binary itself: the identity it
    // finds in its bundle at load time must be the same.
    const auto copy = scratch / ("slow-" + result.identity->name + ".vst3");
    fs::copy(bundle, copy, fs::copy_options::recursive);
    fs::remove(copy / "Contents" / "Resources" / "moduleinfo.json");
    const auto slow = describeVst3(copy);
    require(slow.name == fast.name && slow.manufacturerName == fast.manufacturerName,
        "the VST3 binary reports the embedded name and manufacturer: " + slow.name.toStdString() + " / "
            + slow.manufacturerName.toStdString());
    require(slow.uniqueId == fast.uniqueId && slow.deprecatedUid == fast.deprecatedUid,
        "the VST3 binary's class ID is moduleinfo.json's");
    require(slow.version.toStdString() == result.identity->version, "and its version");

    juce::VST3PluginFormat format;
    auto instance = instantiate(format, fast);
    if (instrument)
        checkInstrument(*instance, result, "VST3");
    else
        checkEffect(*instance, result, "VST3");
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(instance->createEditorAndMakeActive());
        require(editor != nullptr && editor->getWidth() > 0 && editor->getHeight() > 0, "the VST3 has its editor");
    }
    instance.reset();
    checkStateRoundTrip(format, fast, result, "the VST3");
}

void checkRawTemplate()
{
    // A raw template, with nothing embedded, passes its input through.
    const auto bundle = fs::path{TALAS_PLUGIN_TEMPLATE_DIR} / "Talas Effect.vst3";
    const auto description = describeVst3(bundle);
    require(description.name == "Talas Effect" && description.manufacturerName == "Talas",
        "the raw effect template has the template identity");
    juce::VST3PluginFormat format;
    auto instance = instantiate(format, description);
    instance->setPlayConfigDetails(2, 2, sampleRate, blockSize);
    instance->prepareToPlay(sampleRate, blockSize);
    require(instance->getParameters().size() <= 1, "the raw effect has no parameters of its own");
    const auto change = changeFromInput(*instance, 8);
    require(change < 1.0e-5, "the raw effect passes its input through: " + std::to_string(change));
    instance->releaseResources();
}

#if JUCE_MAC
// Registers the AU in this process alone: AudioComponentRegister with the
// factory from the bundle's binary, so no Components folder is touched.
[[nodiscard]] juce::String registerAudioUnitInProcess(const PluginExportResult& result, void*& handle)
{
    const auto bundle = bundleOf(result, PluginFormat::AudioUnit);
    const auto info = plist::parse(readFile(bundle / "Contents" / "Info.plist"));
    const auto& component = info.root->find("AudioComponents")->items().front();
    const auto binary = bundle / "Contents" / "MacOS" / *info.root->findString("CFBundleExecutable");
    handle = dlopen(binary.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
    {
        const auto* reason = dlerror();
        throw std::runtime_error(std::string{"the AU binary loads: "} + (reason != nullptr ? reason : "unknown"));
    }
    const auto factoryName = *component.findString("factoryFunction");
    auto* factory = reinterpret_cast<AudioComponentFactoryFunction>(dlsym(handle, factoryName.c_str()));
    require(factory != nullptr, "the AU exports " + factoryName);
    AudioComponentDescription description{};
    description.componentType = fourCharCode(*component.findString("type"));
    description.componentSubType = fourCharCode(*component.findString("subtype"));
    description.componentManufacturer = fourCharCode(*component.findString("manufacturer"));
    const auto name = juce::String{*component.findString("name")};
    const auto registered = AudioComponentRegister(&description, name.toCFString(),
        static_cast<UInt32>(*component.find("version")->integerValue()), factory);
    require(registered != nullptr, "the AU registers in this process");
    juce::String identifier = "AudioUnit:";
    identifier << (result.identity->kind == PluginKind::Instrument ? "Synths/" : "Effects/")
               << *component.findString("type") << "," << *component.findString("subtype") << ","
               << *component.findString("manufacturer");
    return identifier;
}

void checkAudioUnit(const PluginExportResult& result, bool instrument)
{
    void* handle = nullptr;
    const auto identifier = registerAudioUnitInProcess(result, handle);
    juce::AudioUnitPluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile(found, identifier);
    require(found.size() == 1, "JUCE's AU host finds the registered AU " + identifier.toStdString());
    const auto description = *found[0];
    require(description.name.toStdString() == result.identity->name, "the AU's name is the plugin's, got "
            + description.name.toStdString());
    require(description.manufacturerName.toStdString() == result.identity->manufacturer, "and its manufacturer");
    auto instance = instantiate(format, description);
    if (instrument)
        checkInstrument(*instance, result, "AU");
    else
        checkEffect(*instance, result, "AU");
    instance.reset();
    checkStateRoundTrip(format, description, result, "the AU");
    // The binary stays loaded: the AU's registration names its factory.
}
#endif
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    const auto scratch = fs::temp_directory_path()
        / ("talas-plugin-export-integration-"
            + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    int status = 0;
    try
    {
        fs::create_directories(scratch);
        const auto instrument = exportDemo(scratch / "out", "sub_bass", "Talas Test Bass", PluginKind::Instrument);
        const auto effect = exportDemo(scratch / "out", "ring_modulator", "Talas Test Ring", PluginKind::Effect);
        require(instrument.identity->pluginCode != effect.identity->pluginCode, "two exports get two plugin codes");
        checkBundle(instrument);
        checkBundle(effect);
        checkVst3(instrument, scratch, true);
        checkVst3(effect, scratch, false);
        checkRawTemplate();
#if JUCE_MAC
        checkAudioUnit(instrument, true);
        checkAudioUnit(effect, false);
        std::cout << "AU: loaded in process with AudioComponentRegister and played\n";
#endif
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        status = 1;
    }
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    if (status == 0)
        std::cout << "talas_plugin_export_integration_tests passed\n";
    return status;
}
