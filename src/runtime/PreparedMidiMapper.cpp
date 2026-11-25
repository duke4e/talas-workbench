#include "talas/runtime/PreparedMidiMapper.h"

#include "talas/expr/Expression.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

namespace talas::runtime
{
namespace
{
constexpr std::uint8_t minimumMidiChannel = 1;
constexpr std::uint8_t maximumMidiChannel = 16;
constexpr std::uint8_t maximumMidiValue = 127;

[[nodiscard]] std::optional<std::string> decimalText(double value)
{
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{})
        return std::nullopt;
    return std::string(buffer.data(), result.ptr);
}

[[nodiscard]] bool isKnownMode(
    PreparedMidiMapperMode mode) noexcept
{
    return mode == PreparedMidiMapperMode::Passthrough
        || mode == PreparedMidiMapperMode::Gate
        || mode == PreparedMidiMapperMode::Velocity;
}
}

PreparedMidiMapperParseResult parsePreparedMidiMapperMode(
    std::string_view text) noexcept
{
    return talas::project::parseProjectMidiMapperMap(text);
}

std::optional<std::string> resolvePreparedMidiMapperLocalSource(
    std::string_view source,
    std::optional<std::uint8_t> channel)
{
    constexpr std::string_view prefix = "MIDIController";
    if (!source.starts_with(prefix))
    {
        if (channel) return std::nullopt;
        return std::string(source);
    }
    const auto digits = source.substr(prefix.size());
    unsigned controller = 0;
    const auto parsed = std::from_chars(
        digits.data(), digits.data() + digits.size(), controller);
    if (digits.empty() || parsed.ec != std::errc{}
        || parsed.ptr != digits.data() + digits.size()
        || controller > 127)
        return std::nullopt;
    const auto canonical = std::string(prefix) + std::to_string(controller);
    const auto padded = controller < 10
        ? std::string(prefix) + "0" + std::to_string(controller)
        : canonical;
    if (source != canonical && source != padded)
        return std::nullopt;
    auto name = std::string("cc");
    if (controller < 10) name += '0';
    name += std::to_string(controller);
    if (channel)
    {
        if (*channel < 1 || *channel > 16) return std::nullopt;
        name += "_ch";
        if (*channel < 10) name += '0';
        name += std::to_string(*channel);
    }
    return name;
}

double normalizeMidiPitchBend14(int value) noexcept
{
    const auto centered = value - 8192;
    return static_cast<double>(centered)
        / (centered < 0 ? 8192.0 : 8191.0);
}

std::optional<std::string> preparedMidiMapperProjectionExpression(
    const PreparedMidiMapperAssociation& association,
    std::string_view sourceName,
    talas::expr::ValueDomain targetDomain)
{
    if (association.pitchBendSemitones.has_value())
    {
        if (sourceName != "MIDIKeyNumber"
            || targetDomain != talas::expr::ValueDomain::NoteNumber
            || association.hasRange() || association.channel.has_value())
            return std::nullopt;
        const auto amount = decimalText(*association.pitchBendSemitones);
        if (!amount) return std::nullopt;
        return "(!MIDIKeyNumber + (!MIDIPitchBend * ("
            + *amount + " nn)))";
    }
    std::string_view unit;
    switch (targetDomain)
    {
    case talas::expr::ValueDomain::Scalar: break;
    case talas::expr::ValueDomain::Hertz: unit = " hz"; break;
    case talas::expr::ValueDomain::Seconds: unit = " s"; break;
    case talas::expr::ValueDomain::NoteNumber: unit = " nn"; break;
    case talas::expr::ValueDomain::Samples: unit = " samples"; break;
    default: return std::nullopt;
    }
    if (association.taper == talas::project::MidiMapperMapTaper::Log)
    {
        double minimum = 0.0;
        double maximum = 1.0;
        if (association.hasRange()
            && (!talas::project::parseProjectMidiMapperMapBound(
                    association.minimumText, minimum)
                || !talas::project::parseProjectMidiMapperMapBound(
                    association.maximumText, maximum)))
            return std::nullopt;
        if (minimum < 0.0 || !(maximum > minimum))
            return std::nullopt;
        const auto base = decimalText(minimum);
        const auto top = decimalText(maximum);
        if (!base || !top) return std::nullopt;
        std::string mapped;
        if (minimum == 0.0)
        {
            const auto exponent = decimalText(std::log(1000.0));
            if (!exponent) return std::nullopt;
            mapped = "((exp(!" + std::string(sourceName)
                + " * (" + *exponent + ")) - 1) / 999 * ("
                + *top + "))";
        }
        else
        {
            const auto logBase = decimalText(std::log(minimum));
            const auto logSpan = decimalText(
                std::log(maximum) - std::log(minimum));
            if (!logBase || !logSpan) return std::nullopt;
            mapped = "exp((" + *logBase + ") + (!"
                + std::string(sourceName) + " * (" + *logSpan + ")))";
        }
        if (association.hasGrid())
        {
            double grid = 0.0;
            if (!talas::project::parseProjectMidiMapperMapBound(
                    association.gridText, grid)
                || !(grid > 0.0))
                return std::nullopt;
            const auto step = decimalText(grid);
            const auto halfStep = decimalText(grid * 0.5);
            if (!step || !halfStep) return std::nullopt;
            const auto shifted = "(" + mapped + " - (" + *base
                + ") + (" + *halfStep + "))";
            mapped = "(" + shifted + " - (" + shifted
                + " rem: (" + *step + ")) + (" + *base + "))";
        }
        if (!unit.empty())
            mapped += " * (1" + std::string(unit) + ")";
        return mapped;
    }
    if (!association.hasRange())
        return "!" + std::string(sourceName);

    double minimum = 0.0;
    double maximum = 0.0;
    if (!talas::project::parseProjectMidiMapperMapBound(association.minimumText, minimum)
        || !talas::project::parseProjectMidiMapperMapBound(association.maximumText, maximum)
        || !std::isfinite(maximum - minimum))
        return std::nullopt;
    const auto difference = decimalText(maximum - minimum);
    const auto base = decimalText(minimum);
    if (!difference || !base)
        return std::nullopt;

    if (association.hasGrid())
    {
        double grid = 0.0;
        if (!talas::project::parseProjectMidiMapperMapBound(
                association.gridText, grid)
            || !(grid > 0.0))
            return std::nullopt;
        const auto step = decimalText(grid);
        const auto halfStep = decimalText(grid * 0.5);
        if (!step || !halfStep)
            return std::nullopt;
        const auto shifted = "(!" + std::string(sourceName)
            + " * (" + *difference + ") + (" + *halfStep + "))";
        auto expression = "(" + shifted + " - (" + shifted
            + " rem: (" + *step + ")) + (" + *base + "))";
        if (!unit.empty())
            expression += " * (1" + std::string(unit) + ")";
        return expression;
    }
    return "!" + std::string(sourceName) + " * ("
        + *difference + std::string(unit) + ") + ("
        + *base + std::string(unit) + ")";
}

PreparedMidiMapperState::PreparedMidiMapperState(
    PreparedMidiMapperConfiguration configuration) noexcept
    : configuration_(configuration)
    , ready_(configurationIsValid())
{
    updateGain();
}

bool PreparedMidiMapperState::configurationIsValid() const noexcept
{
    return isKnownMode(configuration_.mode)
        && configuration_.channel <= maximumMidiChannel
        && configuration_.lowPitch <= maximumMidiValue
        && configuration_.highPitch <= maximumMidiValue
        && configuration_.lowPitch <= configuration_.highPitch
        && configuration_.polyphony > 0
        && configuration_.polyphony
            <= maximumPreparedMidiMapperPolyphony;
}

bool PreparedMidiMapperState::ready() const noexcept
{
    return ready_;
}

bool PreparedMidiMapperState::matches(
    std::uint8_t channel,
    std::uint8_t note) const noexcept
{
    return ready_
        && configuration_.mode
            != PreparedMidiMapperMode::Passthrough
        && channel >= minimumMidiChannel
        && channel <= maximumMidiChannel
        && note <= maximumMidiValue
        && (configuration_.channel == 0
            || configuration_.channel == channel)
        && note >= configuration_.lowPitch
        && note <= configuration_.highPitch;
}

PreparedMidiMapperMode PreparedMidiMapperState::mode() const noexcept
{
    return configuration_.mode;
}

double PreparedMidiMapperState::currentGain() const noexcept
{
    return currentGain_;
}

std::size_t PreparedMidiMapperState::heldVoiceCount() const noexcept
{
    return heldVoiceCount_;
}

PreparedMidiMapperEventResult PreparedMidiMapperState::apply(
    const PreparedMidiMapperEvent& event) noexcept
{
    if (!ready_
        || (event.kind != PreparedMidiMapperEventKind::NoteOn
            && event.kind != PreparedMidiMapperEventKind::NoteOff)
        || event.channel < minimumMidiChannel
        || event.channel > maximumMidiChannel
        || event.note > maximumMidiValue
        || event.velocity > maximumMidiValue)
    {
        return {};
    }
    if (!matches(event.channel, event.note))
    {
        return {
            PreparedMidiMapperEventStatus::Filtered,
            {},
        };
    }

    const auto isNoteOff =
        event.kind == PreparedMidiMapperEventKind::NoteOff
        || event.velocity == 0;
    std::size_t selectedIndex = configuration_.polyphony;
    auto oldestSequence =
        std::numeric_limits<std::uint64_t>::max();

    if (isNoteOff)
    {
        for (std::size_t index = 0;
             index < configuration_.polyphony;
             ++index)
        {
            const auto& slot = slots_[index];
            if (slot.held
                && slot.channel == event.channel
                && slot.note == event.note
                && slot.activationSequence < oldestSequence)
            {
                selectedIndex = index;
                oldestSequence = slot.activationSequence;
            }
        }
        if (selectedIndex == configuration_.polyphony)
        {
            return {
                PreparedMidiMapperEventStatus::Handled,
                {},
            };
        }
    }
    else
    {
        if (nextActivationSequence_
                == std::numeric_limits<std::uint64_t>::max()
            || mutationSequence_
                == std::numeric_limits<std::uint64_t>::max())
        {
            return {};
        }
        for (std::size_t index = 0;
             index < configuration_.polyphony;
             ++index)
        {
            const auto& slot = slots_[index];
            if (!slot.held
                && slot.generation
                    != std::numeric_limits<std::uint64_t>::max()
                && slot.activationSequence < oldestSequence)
            {
                selectedIndex = index;
                oldestSequence = slot.activationSequence;
            }
        }
        if (selectedIndex == configuration_.polyphony)
        {
            for (std::size_t index = 0;
                 index < configuration_.polyphony;
                 ++index)
            {
                const auto& slot = slots_[index];
                if (slot.held
                    && slot.generation
                        != std::numeric_limits<std::uint64_t>::max()
                    && slot.activationSequence < oldestSequence)
                {
                    selectedIndex = index;
                    oldestSequence = slot.activationSequence;
                }
            }
        }
        if (selectedIndex == configuration_.polyphony)
            return {};
    }

    if (mutationSequence_
        == std::numeric_limits<std::uint64_t>::max())
    {
        return {};
    }

    auto& slot = slots_[selectedIndex];
    PreparedMidiMapperAction action;
    action.valid = true;
    action.previousHeld = slot.held;
    action.voiceIndex = static_cast<std::uint8_t>(selectedIndex);
    action.previousChannel = slot.channel;
    action.previousNote = slot.note;
    action.previousVelocity = slot.velocity;
    action.previousActivationSequence = slot.activationSequence;
    action.previousGeneration = slot.generation;
    action.previousNextActivationSequence = nextActivationSequence_;
    action.previousMutationSequence = mutationSequence_;
    action.previousHeldVoiceCount = heldVoiceCount_;
    action.previousGain = currentGain_;

    if (isNoteOff)
    {
        slot.held = false;
        --heldVoiceCount_;
    }
    else
    {
        if (!slot.held)
            ++heldVoiceCount_;
        slot.channel = event.channel;
        slot.note = event.note;
        slot.velocity = event.velocity;
        slot.held = true;
        slot.activationSequence = nextActivationSequence_++;
        ++slot.generation;
    }
    ++mutationSequence_;
    action.appliedGeneration = slot.generation;
    action.committedMutationSequence = mutationSequence_;
    updateGain();
    return {
        PreparedMidiMapperEventStatus::Handled,
        action,
    };
}

bool PreparedMidiMapperState::rollback(
    const PreparedMidiMapperAction& action) noexcept
{
    if (!ready_
        || !action.valid
        || action.voiceIndex >= configuration_.polyphony
        || mutationSequence_ != action.committedMutationSequence)
    {
        return false;
    }
    auto& slot = slots_[action.voiceIndex];
    if (slot.generation != action.appliedGeneration)
        return false;

    slot.channel = action.previousChannel;
    slot.note = action.previousNote;
    slot.velocity = action.previousVelocity;
    slot.activationSequence = action.previousActivationSequence;
    slot.generation = action.previousGeneration;
    slot.held = action.previousHeld;
    nextActivationSequence_ = action.previousNextActivationSequence;
    mutationSequence_ = action.previousMutationSequence;
    heldVoiceCount_ = action.previousHeldVoiceCount;
    currentGain_ = action.previousGain;
    return true;
}

void PreparedMidiMapperState::reset() noexcept
{
    slots_ = {};
    heldVoiceCount_ = 0;
    nextActivationSequence_ = 1;
    mutationSequence_ = 0;
    updateGain();
}

void PreparedMidiMapperState::updateGain() noexcept
{
    if (!ready_)
    {
        currentGain_ = 0.0;
        return;
    }
    if (configuration_.mode
        == PreparedMidiMapperMode::Passthrough)
    {
        currentGain_ = 1.0;
        return;
    }
    if (configuration_.mode == PreparedMidiMapperMode::Gate)
    {
        currentGain_ = heldVoiceCount_ == 0 ? 0.0 : 1.0;
        return;
    }

    std::uint8_t maximumVelocity = 0;
    for (std::size_t index = 0;
         index < configuration_.polyphony;
         ++index)
    {
        if (slots_[index].held)
        {
            maximumVelocity = std::max(
                maximumVelocity,
                slots_[index].velocity);
        }
    }
    currentGain_ = static_cast<double>(maximumVelocity) / 127.0;
}
}
