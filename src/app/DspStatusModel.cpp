#include "talas/app/DspStatusModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace talas::app
{
std::vector<double> talasSampleRates()
{
    return {48000.0, 44100.0, 32000.0, 24000.0, 22050.0, 12000.0, 11025.0};
}

std::vector<double> dspStatusSampleRateChoices(std::span<const double> deviceRates)
{
    auto rates = talasSampleRates();
    std::erase_if(rates, [&](double rate) {
        return std::ranges::none_of(deviceRates, [rate](double offered) { return std::abs(offered - rate) < 0.5; });
    });
    return rates;
}

std::string dspStatusElapsedText(double seconds)
{
    const auto whole = static_cast<long long>(std::floor(std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0));
    char text[32];
    std::snprintf(text, sizeof text, "%lld:%02lld:%02lld", whole / 3600, whole / 60 % 60, whole % 60);
    return text;
}

std::string dspStatusTimeCodeText(unsigned hour, unsigned minute, unsigned second, unsigned frame)
{
    char text[32];
    std::snprintf(text, sizeof text, "%02u:%02u:%02u.%02u", hour, minute, second, frame);
    return text;
}

double dspStatusMeterFraction(double peak)
{
    if (!std::isfinite(peak) || peak <= 0.0)
        return 0.0;
    const auto decibels = 20.0 * std::log10(peak);
    return std::clamp((decibels - dspStatusMeterFloorDb) / -dspStatusMeterFloorDb, 0.0, 1.0);
}

std::vector<double> dspStatusMeterMarksDb()
{
    std::vector<double> marks;
    for (double decibels = 0.0; decibels >= dspStatusMeterFloorDb; decibels -= 10.0)
        marks.push_back(decibels);
    return marks;
}

void DspStatusClock::start(double nowSeconds) noexcept
{
    startedAt_ = nowSeconds;
    heldElapsed_ = 0.0;
    running_ = true;
}

void DspStatusClock::stop(double nowSeconds) noexcept
{
    if (!running_)
        return;
    heldElapsed_ = std::max(0.0, nowSeconds - startedAt_);
    running_ = false;
}

double DspStatusClock::elapsed(double nowSeconds) const noexcept
{
    return running_ ? std::max(0.0, nowSeconds - startedAt_) : heldElapsed_;
}

bool DspStatusClock::running() const noexcept
{
    return running_;
}

void DspPeakIndicator::observe(double peak) noexcept
{
    if (std::isfinite(peak))
        held_ = std::max(held_, std::abs(peak));
}

void DspPeakIndicator::reset() noexcept
{
    held_ = 0.0;
}

double DspPeakIndicator::held() const noexcept
{
    return held_;
}

bool DspPeakIndicator::clipped() const noexcept
{
    return held_ >= 1.0;
}

std::string DspPeakIndicator::text() const
{
    if (clipped())
        return ">0<";
    if (held_ <= 0.0)
        return "-inf";
    char text[16];
    std::snprintf(text, sizeof text, "%.1f", 20.0 * std::log10(held_));
    return text;
}

MemoryUseColour memoryUseColour(double fraction) noexcept
{
    if (!(fraction >= 0.25))
        return MemoryUseColour::Cyan;
    if (fraction < 0.5)
        return MemoryUseColour::Green;
    return fraction < 0.75 ? MemoryUseColour::Yellow : MemoryUseColour::Red;
}

std::string dspStatusProcessorText(double load)
{
    const auto percent = std::isfinite(load) ? std::clamp(load, 0.0, 1.0) * 100.0 : 0.0;
    auto text = "Processor " + std::to_string(static_cast<int>(std::lround(percent))) + " %";
    // A processor that cannot keep up says so.
    if (std::isfinite(load) && load >= 1.0)
        text += "  Out of Real Time";
    return text;
}

bool shouldRecycleAutomatically(double fraction, double secondsSinceRecycled) noexcept
{
    return memoryUseColour(fraction) == MemoryUseColour::Red && secondsSinceRecycled >= automaticRecycleSeconds;
}

LoadedSoundInputs loadedSoundInputs(const talas::project::ProjectDocument& document)
{
    LoadedSoundInputs inputs;
    // NONE SELECTED when the Default map is used.
    inputs.globalMap = document.globalMap.empty() ? "NONE SELECTED (Default)" : "the project's own";
    const auto parameter = [](const talas::project::ProjectNode& node, std::string_view id) -> const talas::project::ProjectParameterExpression* {
        const auto found = std::ranges::find(node.parameters, id, &talas::project::ProjectParameterExpression::parameterId);
        return found == node.parameters.end() ? nullptr : &*found;
    };
    // A GenericSource with no source, or a live one, reads the A/D, as the
    // runtime has it.
    const auto live = [](std::string source) {
        std::ranges::transform(source, source.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        while (!source.empty() && std::isspace(static_cast<unsigned char>(source.back())) != 0)
            source.pop_back();
        while (!source.empty() && std::isspace(static_cast<unsigned char>(source.front())) != 0)
            source.erase(source.begin());
        return source.empty() || source == "live" || source == "live input" || source == "input" || source == "audio input";
    };
    std::set<std::string> channels;
    const auto visit = [&](const talas::project::ProjectGraph& graph) {
        for (const auto& node : graph.nodes)
        {
            if (node.typeId == "talas.audioInput")
                inputs.audioInput = true;
            else if (node.typeId == "talas.genericSource")
            {
                const auto* source = parameter(node, "source");
                const auto isLive = live(source == nullptr ? std::string{} : source->text.value_or(""));
                inputs.audioInput = inputs.audioInput || isLive;
                inputs.usesDisk = inputs.usesDisk || !isLive;
            }
            else if (node.typeId == "talas.diskPlayer" || node.typeId == "talas.diskSplicer" || node.typeId == "talas.diskCache"
                || node.typeId == "talas.diskRecorder")
                inputs.usesDisk = true;
            else if (node.typeId == "talas.midiVoice" || node.typeId == "talas.midiMapper")
            {
                const auto* channel = parameter(node, "channel");
                channels.insert(channel != nullptr && channel->expression.has_value() ? *channel->expression : "0");
            }
        }
    };
    visit(document.graph);
    for (const auto& definition : document.soundDefinitions)
        visit(definition.graph);
    inputs.midiChannels.assign(channels.begin(), channels.end());
    for (const auto& control : document.virtualControls)
        inputs.eventValues.push_back(control.eventName);
    std::ranges::sort(inputs.eventValues);
    return inputs;
}

bool takesLiveInput(const LoadedSoundInputs& inputs) noexcept
{
    return inputs.audioInput || !inputs.midiChannels.empty();
}

RecordingCountOff recordingCountOff(unsigned beats, double beatsPerMinute)
{
    RecordingCountOff countOff;
    if (beats == 0 || !std::isfinite(beatsPerMinute) || beatsPerMinute <= 0.0)
        return countOff;
    const auto beat = 60.0 / beatsPerMinute;
    for (unsigned index = 0; index < beats; ++index)
        countOff.beepSeconds.push_back(static_cast<double>(index) * beat);
    countOff.recordingStartSeconds = static_cast<double>(beats) * beat;
    return countOff;
}

std::vector<std::string> loadedSoundInputLines(const LoadedSoundInputs& inputs)
{
    const auto joined = [](const std::vector<std::string>& names) {
        std::string text;
        for (const auto& name : names)
            text += (text.empty() ? "" : ", ") + name;
        return text;
    };
    std::vector<std::string> eventValues;
    for (const auto& name : inputs.eventValues)
        eventValues.push_back("!" + name);
    return {
        "Global map: " + inputs.globalMap,
        inputs.audioInput ? std::string{"Input from the A/D"} : std::string{"No input from the A/D"},
        inputs.midiChannels.empty() ? std::string{"No MIDI keyboard input"} : "MIDI keyboard on channel " + joined(inputs.midiChannels),
        inputs.eventValues.empty() ? std::string{"No Event Values"} : "Event Values: " + joined(eventValues),
    };
}

std::string midiMessageDescription(std::span<const std::uint8_t> bytes)
{
    if (bytes.empty())
        return {};
    const auto status = bytes[0];
    const auto channel = std::to_string((status & 0x0F) + 1);
    const auto data = [&](std::size_t index) { return index < bytes.size() ? std::to_string(bytes[index]) : std::string{"?"}; };
    switch (status & 0xF0)
    {
    case 0x90:
        if (bytes.size() > 2 && bytes[2] > 0)
            return "Key down " + data(1) + " velocity " + data(2) + " channel " + channel;
        [[fallthrough]];
    case 0x80:
        return "Key up " + data(1) + " channel " + channel;
    case 0xA0:
        return "Key pressure " + data(1) + " value " + data(2) + " channel " + channel;
    case 0xB0:
        return "Controller " + data(1) + " value " + data(2) + " channel " + channel;
    case 0xC0:
        return "Program " + data(1) + " channel " + channel;
    case 0xD0:
        return "Channel pressure " + data(1) + " channel " + channel;
    case 0xE0:
        if (bytes.size() > 2)
            return "Pitch bend " + std::to_string((bytes[2] << 7) | bytes[1]) + " channel " + channel;
        return "Pitch bend channel " + channel;
    default:
        break;
    }
    std::string hex;
    static constexpr char digits[] = "0123456789ABCDEF";
    for (const auto byte : bytes)
    {
        if (!hex.empty())
            hex += ' ';
        hex += digits[byte >> 4];
        hex += digits[byte & 0x0F];
    }
    return hex;
}

void MidiMessageLog::add(std::string line)
{
    lines_.push_back(std::move(line));
    while (lines_.size() > capacity)
        lines_.pop_front();
}

std::string MidiMessageLog::text() const
{
    std::string joined;
    for (const auto& line : lines_)
        joined += line + '\n';
    return joined;
}
}
