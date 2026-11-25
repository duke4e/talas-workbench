#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talas::expr
{
enum class Unit
{
    None,
    Hertz,
    Decibels,
    Milliseconds,
    Seconds,
    NoteNumber,
    Beats,
    Samples,
    Microseconds,
    Minutes,
    Hours,
    Days,
    BeatsPerMinute
};

enum class ValueDomain
{
    Scalar,
    Hertz,
    Seconds,
    NoteNumber,
    Samples
};

enum class ExpressionLanguageVersion
{
    LegacyV1,
    PureV2,
    PureV3,
    PureV4,
    SessionV1,
    VariablesV1,
    VariablesV2,
    VoiceV1,
    VariablesV3,
    PureV5,
    VariablesV4,
    ValV1
};

inline constexpr ExpressionLanguageVersion latestPureExpressionLanguage =
    ExpressionLanguageVersion::PureV5;
inline constexpr ExpressionLanguageVersion latestExpressionLanguage =
    ExpressionLanguageVersion::VariablesV2;

[[nodiscard]] constexpr bool isPureExpressionLanguage(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::PureV2
        || languageVersion == ExpressionLanguageVersion::PureV3
        || languageVersion == ExpressionLanguageVersion::PureV4
        || languageVersion == ExpressionLanguageVersion::PureV5;
}

[[nodiscard]] constexpr bool isCompiledExpressionLanguage(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return isPureExpressionLanguage(languageVersion)
        || languageVersion == ExpressionLanguageVersion::SessionV1
        || languageVersion == ExpressionLanguageVersion::VariablesV1
        || languageVersion == ExpressionLanguageVersion::VariablesV2
        || languageVersion == ExpressionLanguageVersion::VariablesV3
        || languageVersion == ExpressionLanguageVersion::VariablesV4
        || languageVersion == ExpressionLanguageVersion::ValV1
        || languageVersion == ExpressionLanguageVersion::VoiceV1;
}

[[nodiscard]] constexpr bool supportsPureExpressionSeries(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::PureV3
        || languageVersion == ExpressionLanguageVersion::PureV4
        || languageVersion == ExpressionLanguageVersion::PureV5
        || languageVersion == ExpressionLanguageVersion::SessionV1
        || languageVersion == ExpressionLanguageVersion::VariablesV1
        || languageVersion == ExpressionLanguageVersion::VariablesV2
        || languageVersion == ExpressionLanguageVersion::VariablesV3
        || languageVersion == ExpressionLanguageVersion::VariablesV4
        || languageVersion == ExpressionLanguageVersion::ValV1
        || languageVersion == ExpressionLanguageVersion::VoiceV1;
}

[[nodiscard]] constexpr bool supportsPureExpressionDefinitions(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::PureV4
        || languageVersion == ExpressionLanguageVersion::PureV5;
}

[[nodiscard]] constexpr bool supportsArrayMap(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::PureV5
        || languageVersion == ExpressionLanguageVersion::ValV1;
}

[[nodiscard]] constexpr bool supportsExpressionDefinitions(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::PureV4
        || languageVersion == ExpressionLanguageVersion::PureV5
        || languageVersion == ExpressionLanguageVersion::SessionV1
        || languageVersion == ExpressionLanguageVersion::VariablesV1
        || languageVersion == ExpressionLanguageVersion::VariablesV2
        || languageVersion == ExpressionLanguageVersion::VariablesV3
        || languageVersion == ExpressionLanguageVersion::VariablesV4
        || languageVersion == ExpressionLanguageVersion::ValV1
        || languageVersion == ExpressionLanguageVersion::VoiceV1;
}

[[nodiscard]] constexpr bool supportsStatefulExpressions(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::SessionV1
        || languageVersion == ExpressionLanguageVersion::VariablesV1
        || languageVersion == ExpressionLanguageVersion::VariablesV2
        || languageVersion == ExpressionLanguageVersion::VariablesV3
        || languageVersion == ExpressionLanguageVersion::VariablesV4
        || languageVersion == ExpressionLanguageVersion::ValV1
        || languageVersion == ExpressionLanguageVersion::VoiceV1;
}

[[nodiscard]] constexpr bool supportsVariableReferences(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::VariablesV1
        || languageVersion == ExpressionLanguageVersion::VariablesV2
        || languageVersion == ExpressionLanguageVersion::VariablesV3
        || languageVersion == ExpressionLanguageVersion::VariablesV4
        || languageVersion == ExpressionLanguageVersion::ValV1
        || languageVersion == ExpressionLanguageVersion::VoiceV1;
}

[[nodiscard]] constexpr bool supportsCheckedRemainder(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::VariablesV2
        || languageVersion == ExpressionLanguageVersion::VariablesV3
        || languageVersion == ExpressionLanguageVersion::VariablesV4
        || languageVersion == ExpressionLanguageVersion::ValV1
        || languageVersion == ExpressionLanguageVersion::VoiceV1;
}

[[nodiscard]] constexpr bool supportsTimedExpressions(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::VariablesV3
        || languageVersion == ExpressionLanguageVersion::VariablesV4
        || languageVersion == ExpressionLanguageVersion::ValV1;
}

[[nodiscard]] constexpr bool supportsPostfixUnits(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::VariablesV4;
}

// Val, the parameter-field language, sends messages: unary
// messages such as `3 twoExp` or `4 c hz` bind first, then binary
// operators from left to right, then keyword messages such as
// `!KeyDown ramp: 2 s` or `x vmin: y`.
[[nodiscard]] constexpr bool usesValMessages(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::ValV1;
}

[[nodiscard]] constexpr bool supportsVoiceNumberSpecialization(
    ExpressionLanguageVersion languageVersion) noexcept
{
    return languageVersion == ExpressionLanguageVersion::VoiceV1;
}

struct ExpressionLimits
{
    static constexpr std::size_t maximumSourceBytes = 4096;
    static constexpr std::size_t maximumTokens = 512;
    static constexpr std::size_t maximumNodes = 256;
    static constexpr std::size_t maximumInstructions = 256;
    static constexpr std::size_t maximumNestingDepth = 32;
    // Historical LegacyV1 sources predate the nesting budget; this larger
    // limit only keeps parsing and evaluation within small thread stacks.
    static constexpr std::size_t maximumLegacyNestingDepth = 256;
    static constexpr std::size_t maximumEvaluationStack = 32;
    static constexpr std::size_t maximumListElements = 32;
    static constexpr std::size_t maximumSequenceElements = 64;
    static constexpr std::size_t maximumDefinitions = 32;
    static constexpr std::size_t maximumDefinitionParameters = 4;
    static constexpr std::size_t maximumDefinitionNameBytes = 64;
    static constexpr std::size_t maximumDefinitionSourceBytes = 4096;
    static constexpr std::size_t maximumDefinitionSourceBytesTotal = 32768;
    static constexpr std::size_t maximumDefinitionExpansionDepth = 8;
    static constexpr std::size_t maximumDefinitionLocals = 32;
    static constexpr std::size_t maximumStateSlots = 32;
    static constexpr std::size_t maximumEventReferences = 64;
    static constexpr std::size_t maximumIdentifiers = 64;
    static constexpr std::size_t maximumVariableReferences = 64;
    static constexpr std::size_t maximumDiagnostics = 16;
};

enum class UnaryOp
{
    Negate
};

enum class BinaryOp
{
    Add,
    Subtract,
    Multiply,
    Divide,
    Remainder,
    Power,
    IntegerDivide,
    Modulo
};

struct SourceRange
{
    std::size_t offset = 0;
    std::size_t length = 0;

    bool operator==(const SourceRange&) const = default;
};

struct VariableReferenceMetadata
{
    std::string name;
    SourceRange range;
    std::optional<std::size_t> definitionIndex;

    bool operator==(const VariableReferenceMetadata&) const = default;
};

struct ExpressionValue
{
    double value = 0.0;
    ValueDomain domain = ValueDomain::Scalar;

    bool operator==(const ExpressionValue&) const = default;
};

struct ExpressionDefinitionParameter
{
    std::string name;
    ValueDomain domain = ValueDomain::Scalar;

    bool operator==(const ExpressionDefinitionParameter&) const = default;
};

struct ExpressionDefinition
{
    std::string name;
    std::vector<ExpressionDefinitionParameter> parameters;
    ValueDomain resultDomain = ValueDomain::Scalar;
    std::string source;

    bool operator==(const ExpressionDefinition&) const = default;
};

// How a project's rounded and truncated round (format 156). Smalltalk: rounded
// to the nearest integer, a half away from zero, and truncated toward zero, so
// -2.5 rounded is -3 and -2.5 truncated -2. ManualTable: rounded adds 0.5 and
// truncates toward negative infinity, and truncated rounds toward negative
// infinity, so -2.5 rounded is -2 and -2.5 truncated -3.
enum class RoundingRule : std::uint8_t
{
    Smalltalk,
    ManualTable
};

// How a project's inSOSPitch reads a receiver without units (format 156).
// As hertz on the spectral sources' scale: 22050
// hz is 1 and each octave down 1/15 less, so 11025.0 inSOSPitch is 14/15.
// Or as a fraction of 22050 hz, so 11025.0 inSOSPitch is 0.5; a frequency in
// hertz or a pitch in note numbers keeps the spectral sources' scale either
// way.
enum class UnitlessSOSPitch : std::uint8_t
{
    Hertz,
    Linear
};

struct ExpressionDefinitionLibrary
{
    std::vector<ExpressionDefinition> definitions;
    // The project's timecode frame rate, which converts times written
    // hh:mm:ss.ff SMPTE to seconds: 24, 25 or
    // 30 frames per second.
    std::uint8_t timecodeFramesPerSecond = 30;
    // The project's rounded and truncated (format 156); Smalltalk's unless the
    // project chooses ManualTable.
    RoundingRule rounding = RoundingRule::Smalltalk;
    // The project's tempo for a beat written in a field (format 156), in
    // quarter notes a minute, from 1 to 1000: 1 beat lasts 60 / tempo seconds
    // whatever the Timeline's tempo map says. None, as in projects before
    // format 156, a beat follows the tempo the Sound plays to, the tempo map's
    // first. A new project's is 60.
    std::optional<double> fieldBeatTempoBpm;
    // How inSOSPitch reads a receiver without units (format 156).
    UnitlessSOSPitch unitlessSOSPitch = UnitlessSOSPitch::Hertz;

    bool operator==(const ExpressionDefinitionLibrary&) const = default;
};

// A new project's beat for fields, a quarter note at 60; the Timeline keeps
// its own tempo, 120.
inline constexpr double newProjectFieldBeatTempoBpm = 60.0;
// The tempos a field's beat may take.
inline constexpr double minimumFieldBeatTempoBpm = 1.0;
inline constexpr double maximumFieldBeatTempoBpm = 1000.0;

struct Diagnostic
{
    enum class Code
    {
        EmptyExpression,
        UnexpectedCharacter,
        UnexpectedToken,
        ExpectedExpression,
        ExpectedRightParen,
        InvalidNumber,
        UnknownUnit,
        TrailingInput,
        BudgetExceeded,
        UnknownFunction,
        ExpectedComma,
        InvalidArgumentCount,
        InvalidArgument,
        InvalidIndex,
        InvalidSequenceCount,
        InvalidDefinition,
        RecursiveDefinition,
        UnknownEvent,
        UnknownVariable,
        UnknownIdentifier,
        DivisionByZero,
        UnitMismatch,
        MissingSampleRate,
        MissingTempo,
        MissingState,
        NonFiniteResult
    };

    Code code = Code::UnexpectedToken;
    SourceRange range;
    std::string message;
    // Present when a closed definition-library diagnostic belongs to one
    // exact definition. Project/UI layers use it to retain a stable JSON path.
    std::optional<std::size_t> definitionIndex;
};

using NodeId = std::uint32_t;

namespace detail
{
struct ExpressionStorage;
}

struct ParseResult;
struct EvaluationContext;
struct EvaluationOptions;
struct EvaluationResult;

class Expression
{
public:
    [[nodiscard]] NodeId root() const;
    [[nodiscard]] ExpressionLanguageVersion languageVersion() const;
    [[nodiscard]] std::size_t instructionCount() const;
    [[nodiscard]] std::size_t maximumStackDepth() const;
    [[nodiscard]] const std::vector<std::string>& eventReferences() const;
    [[nodiscard]] const std::vector<std::string>& variableReferences() const;
    [[nodiscard]] const std::vector<VariableReferenceMetadata>&
    variableReferenceMetadata() const;
    [[nodiscard]] const std::vector<std::string>& identifiers() const;
    [[nodiscard]] std::size_t stateSlotCount() const;
    [[nodiscard]] bool isStateful() const;
    [[nodiscard]] bool usesTime() const noexcept;
    // Whether the expression reads the MIDI timing clock, as !TimingClock
    // and `MIDITimingClock do: a renderer then
    // keeps midiTimingClockCountEventName for it.
    [[nodiscard]] bool readsMidiTimingClock() const noexcept;
    // Whether the expression holds a value until its receiver first
    // changes, as neg:zero:pos:initially: does: a
    // live field holding it is first evaluated on its starting values.
    [[nodiscard]] bool startsFromItsStartingValues() const noexcept;
    // Whether the expression holds a branch it may not take, as a
    // conditional's or a selection's.
    [[nodiscard]] bool mayLeaveBranchesUntaken() const noexcept;
    [[nodiscard]] bool usesVoiceNumber() const noexcept;
    // Preparation-only immutable binding. Zero is invalid. Unbound templates
    // cannot evaluate, including through a custom identifier resolver. The
    // caller's real voice allocator owns the supported polyphony limit.
    [[nodiscard]] std::optional<Expression> specializeVoiceNumber(
        std::uint16_t oneBasedVoiceNumber) const;
    // Preparation-only binding of a plain identifier, such as the contextual
    // `default`, to a literal value. An expression without the identifier is
    // returned unchanged.
    [[nodiscard]] std::optional<Expression> specializeIdentifier(
        std::string_view name,
        ExpressionValue value) const;
    [[nodiscard]] bool structurallyEquivalent(
        const Expression& other) const noexcept;
    // True when both compile to the same program over the same references,
    // whatever their language or spelling: changing a project's language
    // keeps a field's meaning exactly when this holds.
    [[nodiscard]] bool computesSameAs(const Expression& other) const noexcept;
    [[nodiscard]] bool sharesCompiledStorageWith(
        const Expression& other) const noexcept;

    // A field with pasted Sounds (ADR-0020) split into a skeleton that reads
    // only its Sounds, literals and operators, and the source of each
    // largest Sound-free part that reads Event Values or variables or calls
    // a function; the skeleton names the parts __kc_live_1, __kc_live_2 and
    // so on. `source` is the text the expression was parsed from.
    struct SoundSplit
    {
        std::string skeleton;
        std::vector<std::string> liveParts;
    };
    [[nodiscard]] std::optional<SoundSplit> splitAroundSoundReferences(
        std::string_view source) const;

    // Each random expression returns numbers of its
    // own. A field's salt, mixed into the seeds of its random messages, so
    // that two fields reading 1 s random draw apart; 0, the salt of
    // projects before format 114, draws as they did.
    void saltRandomNumbers(std::uint32_t salt) noexcept { randomSalt_ = salt; }
    [[nodiscard]] std::uint32_t randomSalt() const noexcept { return randomSalt_; }

private:
    explicit Expression(std::shared_ptr<const detail::ExpressionStorage> storage);

    std::shared_ptr<const detail::ExpressionStorage> storage_;
    std::uint32_t randomSalt_ = 0;

    friend ParseResult parse(std::string_view source);
    friend ParseResult parse(
        std::string_view source,
        ExpressionLanguageVersion languageVersion);
    friend ParseResult parse(
        std::string_view source,
        ExpressionLanguageVersion languageVersion,
        const ExpressionDefinitionLibrary& definitions);
    friend EvaluationResult evaluate(const Expression& expression, const EvaluationContext& context, EvaluationOptions options);
    friend bool prepareExpressionState(
        const Expression& expression,
        struct ExpressionState& state) noexcept;
    friend struct EvaluationResult evaluateStateful(
        const Expression& expression,
        const EvaluationContext& context,
        struct ExpressionState& state,
        struct ExpressionStateScratch& scratch,
        EvaluationOptions options);
    friend EvaluationResult evaluateStateful(
        const Expression& expression,
        const EvaluationContext& context,
        struct ExpressionState& state,
        EvaluationOptions options);
    friend bool tryEvaluateStateful(
        const Expression& expression,
        const EvaluationContext& context,
        struct ExpressionState& state,
        EvaluationOptions options,
        ExpressionValue& result) noexcept;
    friend bool tryEvaluateStateful(
        const Expression& expression,
        const EvaluationContext& context,
        struct ExpressionState& state,
        struct ExpressionStateScratch& scratch,
        EvaluationOptions options,
        ExpressionValue& result) noexcept;
    friend bool tryEvaluate(
        const Expression& expression,
        const EvaluationContext& context,
        EvaluationOptions options,
        ExpressionValue& result) noexcept;
};

struct ParseResult
{
    std::optional<Expression> expression;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return expression.has_value() && diagnostics.empty();
    }
};

using ResolveValueFn = bool (*)(void* user, std::string_view name, ExpressionValue& out) noexcept;
using ResolveIndexedValueFn = bool (*)(
    void* user,
    std::size_t referenceIndex,
    ExpressionValue& out) noexcept;

struct EvaluationContext
{
    double sampleRate = 48000.0;
    double tempoBpm = 120.0;
    // ramp, ramp:, fullRamp and fullRamp: rest at 1 until their receiver
    // first becomes positive; projects before
    // format 100 start them at their first value.
    bool rampsStartAtFirstValue = false;
    // ramp: and fullRamp: run on to their end once triggered;
    // projects before format 103 hold them while their receiver is off.
    bool rampsHoldUntriggered = false;
    // A project's choice (format 156): the inverse of 0 is infinity. A number
    // other than 0 divided by 0, or inverted, is infinite with its sign, and
    // arithmetic carries an infinity on as IEEE doubles do; 0 / 0 and anything
    // else undefined still fail. Off, every division by zero fails, as before.
    bool inverseOfZeroIsInfinite = false;
    // Elapsed audio frames in the expression's running Sound, independent of
    // any parent Timeline cursor. Time functions update on millisecond ticks.
    std::uint64_t elapsedSampleFrame = 0;
    // The frames the whole Sound had played when the expression's own Sound
    // started, from which !RealTime and !Time count.
    std::uint64_t soundStartSampleFrame = 0;
    // Format 153: inside a TimeControl or TimeStopper, !Time is the seconds
    // its clock has let pass since the whole Sound started and !LocalTime
    // those since the expression's own Sound started; NaN where no clock
    // controls them.
    double controlledTimeSeconds = std::numeric_limits<double>::quiet_NaN();
    double controlledLocalTimeSeconds = std::numeric_limits<double>::quiet_NaN();
    void* user = nullptr;
    ResolveValueFn resolveEvent = nullptr;
    ResolveValueFn resolveVariable = nullptr;
    // Prepared runtimes use the immutable compiled reference index and avoid
    // callback-time string lookup. Ordinary callers may keep using
    // resolveVariable; it remains the compatibility fallback.
    ResolveIndexedValueFn resolveVariableIndex = nullptr;
    ResolveValueFn resolveIdentifier = nullptr;
};

struct EvaluationOptions
{
    ValueDomain expectedDomain = ValueDomain::Scalar;
};

struct EvaluationResult
{
    ExpressionValue value;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

struct ExpressionStateCell
{
    ExpressionValue value;
    bool initialized = false;
    bool auxiliary = false;
    std::uint64_t startTimeTick = 0;
    std::uint64_t lastTimeTick = 0;
    // The milliseconds a Val time function missed in a branch not
    // taken, which its own time leaves out.
    std::uint64_t pausedTicks = 0;

    bool operator==(const ExpressionStateCell&) const = default;
};

struct ExpressionState
{
    std::array<ExpressionStateCell,
               ExpressionLimits::maximumStateSlots> cells{};
    std::size_t slotCount = 0;
    // The expression's random salt, as prepareExpressionState takes it.
    std::uint32_t randomSalt = 0;
    // One more than the millisecond tick of the last evaluation kept, 0
    // before the first.
    std::uint64_t lastEvaluationTick = 0;

    void reset() noexcept
    {
        cells = {};
        lastEvaluationTick = 0;
    }

    bool operator==(const ExpressionState&) const = default;
};

struct ExpressionStateScratch
{
    ExpressionState candidate;

    bool operator==(const ExpressionStateScratch&) const = default;
};

struct EventReferenceRename
{
    std::string_view from;
    std::string_view to;
};

// Preparation-only lexer scan for distinct Event Value names, without '!'.
[[nodiscard]] std::optional<std::vector<std::string>>
scanEventReferenceNames(
    std::string_view source,
    ExpressionLanguageVersion languageVersion);

// Preparation-only source transformation. Uses the expression lexer so an
// Event Value token is renamed without touching similarly prefixed names or
// the compiled-language != operator. Names omit the leading '!'. An Event
// Source (`name) is left as it is.
[[nodiscard]] std::optional<std::string> renameEventReferences(
    std::string_view source,
    ExpressionLanguageVersion languageVersion,
    std::span<const EventReferenceRename> renames);

// A Val field reads an Event Source
// written after a backquote, `MIDIController07, as the Event Value the
// engine keeps for it: a MIDI controller's raw value from 0 to 1 is the
// built-in !cc07 (!cc100 from 100 on), any other source, `MIDIPitchBend or
// one the global map names as `Glide, the Event Value of its own name. The
// name without the backquote; nothing for a controller past 127.
[[nodiscard]] std::optional<std::string> eventSourceEventName(std::string_view source);

// The Event Value through which a renderer gives a
// Sound the MIDI timing clock messages it receives (from a MIDI input, or
// from the MIDI file a MIDIVoice plays), as the number of them so far, 0
// before the first. The Event Source `MIDITimingClock, also written
// `MIDITimingClock extractClock, triggers once each time it changes, and
// is 0 without it; !TimingClock does the same once it is above 0, and
// until then stands in for the clock at the tempo the Sound plays to (24
// triggers a beat). A renderer's own name, not one for a field.
inline constexpr std::string_view midiTimingClockCountEventName = "MIDITimingClockCount";

struct EventReferenceReplacement
{
    std::string_view from;
    // The expression written in its place, in parentheses.
    std::string_view to;
};

// The source with each Event Value !from (not an Event Source of that
// name) written as its replacement, in parentheses; nothing when it holds
// none of them or the result would be too long. Names omit the '!'.
[[nodiscard]] std::optional<std::string> replaceEventReferences(
    std::string_view source,
    ExpressionLanguageVersion languageVersion,
    std::span<const EventReferenceReplacement> replacements);

// !KeyPitch follows the pitch wheel a half
// step either way, as !KeyNumber nn + !PitchBend nn, and !KeyPitch12 an
// octave. The source with each written out from the key's own pitch,
// (!KeyPitch + !PitchBend nn); nothing when it reads neither.
[[nodiscard]] std::optional<std::string> withKeyPitchBends(
    std::string_view source,
    ExpressionLanguageVersion languageVersion);

// The signal processor's !Random and !Random1000
// draw a number from -1 to 1 every millisecond. The source with each
// written out as (1 ms random); nothing when it reads neither.
[[nodiscard]] std::optional<std::string> withRandomEventSources(
    std::string_view source,
    ExpressionLanguageVersion languageVersion);

// Preparation-only replacement of Replicator's one-based green variables.
// Other ?names stay untouched for their owning expansion layer.
[[nodiscard]] std::optional<std::string>
specializeReplicationVariables(
    std::string_view source,
    std::uint16_t oneBasedVoiceNumber,
    std::uint16_t numberVoices);

// Same binding, padded with spaces to keep every original source offset for
// authoring diagnostics before the Replicator is lowered.
[[nodiscard]] std::optional<std::string>
specializeReplicationVariablesForAudit(
    std::string_view source,
    std::uint16_t oneBasedVoiceNumber,
    std::uint16_t numberVoices);

// A Sound pasted into a parameter field: its
// name, the channel it is read from (L, R, or M for their mix) and, when
// given, the milliseconds between updates. The expression language names it
// as one identifier, such as "Oscillator1 L" or "Oscillator1 L: 5".
struct SoundReference
{
    std::string sound;
    char channel = 'L';
    std::optional<double> periodMilliseconds;

    bool operator==(const SoundReference&) const = default;
};

[[nodiscard]] std::string formatSoundReference(const SoundReference& reference);
[[nodiscard]] std::optional<SoundReference> parseSoundReference(
    std::string_view identifier);

// A Val field holding a Smalltalk program as
// the one expression it gives, each temporary read as the value last
// assigned to it; nothing for a field without one, and an error for a
// program that cannot be read.
struct FieldProgramExpression
{
    std::optional<std::string> expression;
    std::string error;
};
[[nodiscard]] FieldProgramExpression fieldProgramExpression(std::string_view source);

// A Val field with a 0 put before each number written without its
// leading zero, .5 becoming 0.5, which Val refuses since project format
// 106; comments, strings and a field's
// program stay as they are.
[[nodiscard]] std::string withLeadingZeros(std::string_view source);
// A Val expression of a project before format 110, where removeUnits
// gave a bpm frequency's hertz, with `hz` written after each `bpm` that
// removeUnits follows (perhaps after closing parentheses), so that it keeps
// that number: 120 bpm removeUnits becomes 120 bpm hz removeUnits, 2.
[[nodiscard]] std::string withBpmRemoveUnitsInHertz(std::string_view source);
// A Val expression of a project before format 111, where a number
// written right before d was the pitch d of that octave, with a space put
// between them so that it stays that pitch: 4d becomes 4 d. Since 111 such
// a d marks a double, 8.1758d being 8.1758.
[[nodiscard]] std::string withPitchDApart(std::string_view source);
// A Val expression of a project before format 111, where removeUnits
// gave the hertz of a bpm frequency multiplied or divided by a number, with
// `hz` written after each such `bpm` so that it keeps that number: (2 s bpm
// * 4) removeUnits becomes (2 s bpm hz * 4) removeUnits, 2; since 111 it is
// 120, the beats per minute.
[[nodiscard]] std::string withBpmTimesNumbersRemoveUnitsInHertz(std::string_view source);
// A Val expression of a project before format 112, where bpm: and
// bpm:dutyCycle: went straight to 1 on each beat, with each such message
// put in parentheses and followed by abs so that it keeps doing so: 1 bpm:
// 60 becomes (1 bpm: 60) abs. Since 112 each beat is first -1 for five
// milliseconds, stealing its voice as !KeyDown does.
[[nodiscard]] std::string withMetronomesWithoutVoiceStealing(std::string_view source);

// Neg:zero:pos: is 0 until
// its receiver first changes, as neg:zero:pos:initially: 0 is. A Val
// source with each written so; nothing when it holds none.
[[nodiscard]] std::optional<std::string> withSignSelectionsStartingAtZero(std::string_view source);
// A Val time function in a branch not taken
// stops, carrying on from where it left off when the branch is next taken.
// A Val source of a project before format 119 that holds both a
// conditional and something that keeps state, marked so that its time
// functions run on through branches not taken, as they did; nothing for
// one holding neither or not parsing.
[[nodiscard]] std::optional<std::string> withTimeRunningOnInSkippedBranches(
    std::string_view source, const ExpressionDefinitionLibrary& definitions);
// !RealTime and !Time are
// the seconds since the whole Sound started. A Val source reading
// either, marked so that each reads that clock instead of an Event Value of
// its name; nothing for one reading neither. Projects before format 122
// keep them as Event Values.
[[nodiscard]] std::optional<std::string> withWholeSoundClocks(std::string_view source);
// `source` with each 'name' sampleFileDuration written as the number of
// seconds `seconds` gives for that sample file, as Smalltalk answers it before
// the Sound plays; nothing when it asks no file `seconds` knows.
[[nodiscard]] std::optional<std::string> withSampleFileDurations(
    std::string_view source,
    const std::function<std::optional<double>(std::string_view)>& seconds);

[[nodiscard]] ParseResult parse(std::string_view source);
[[nodiscard]] ParseResult parse(
    std::string_view source,
    ExpressionLanguageVersion languageVersion);
[[nodiscard]] ParseResult parse(
    std::string_view source,
    ExpressionLanguageVersion languageVersion,
    const ExpressionDefinitionLibrary& definitions);
[[nodiscard]] EvaluationResult evaluate(const Expression& expression, const EvaluationContext& context, EvaluationOptions options = {});
[[nodiscard]] bool tryEvaluate(
    const Expression& expression,
    const EvaluationContext& context,
    EvaluationOptions options,
    ExpressionValue& result) noexcept;
[[nodiscard]] bool prepareExpressionState(
    const Expression& expression,
    ExpressionState& state) noexcept;
[[nodiscard]] bool prepareExpressionStateScratch(
    const Expression& expression,
    ExpressionStateScratch& scratch) noexcept;
[[nodiscard]] EvaluationResult evaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    EvaluationOptions options = {});
[[nodiscard]] EvaluationResult evaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    ExpressionStateScratch& scratch,
    EvaluationOptions options = {});
[[nodiscard]] bool tryEvaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    EvaluationOptions options,
    ExpressionValue& result) noexcept;
[[nodiscard]] bool tryEvaluateStateful(
    const Expression& expression,
    const EvaluationContext& context,
    ExpressionState& state,
    ExpressionStateScratch& scratch,
    EvaluationOptions options,
    ExpressionValue& result) noexcept;
}
