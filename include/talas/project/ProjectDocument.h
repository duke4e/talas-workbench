#pragma once

#include "talas/core/AutomationInterpolation.h"
#include "talas/core/Graph.h"
#include "talas/core/NodeRegistry.h"
#include "talas/expr/Expression.h"
#include "talas/project/ProjectSnapshots.h"
#include "talas/project/ProjectSampleMaps.h"
#include "talas/project/ProjectMissingNodes.h"
#include "talas/project/ProjectMusicalTime.h"
#include "talas/project/ProjectPartialTrackEdits.h"
#include "talas/project/ProjectScore.h"
#include "talas/project/ProjectSoundVariables.h"
#include "talas/project/ProjectGenerators.h"
#include "talas/project/ProjectLiveSets.h"
#include "talas/project/ProjectVariables.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace talas::project
{
inline constexpr int currentProjectFormatVersion = 156;

struct ProjectParameterExpression
{
    std::string parameterId;
    std::optional<std::string> expression;
    std::optional<std::string> choice;
    std::optional<bool> boolean;
    std::optional<std::string> resource;
    std::optional<std::string> text;

    bool operator==(const ProjectParameterExpression&) const = default;
};

struct CanvasPosition
{
    std::int32_t x = 0;
    std::int32_t y = 0;

    bool operator==(const CanvasPosition&) const = default;
};

struct ProjectNode
{
    std::string instanceId;
    std::string typeId;
    std::vector<ProjectParameterExpression> parameters;
    std::optional<std::string> soundDefinitionId;
    std::optional<ProjectPreservedComponent> preservedComponent;

    bool operator==(const ProjectNode&) const = default;
};

struct ProjectGraph
{
    std::vector<ProjectNode> nodes;
    std::vector<talas::core::Connection> connections;

    bool operator==(const ProjectGraph&) const = default;
};

struct ProjectSoundPort
{
    std::string portId;
    talas::core::PortType type = talas::core::PortType::Audio;
    std::string nodeId;
    std::string nodePortId;
    // Format 138: the Sound's replaceable input, an input boundary naming one
    // of its Sounds by its Audio output. Unconnected, that Sound plays; a
    // Sound an instance connects there plays in its place wherever it fed.
    bool replacesSound = false;

    bool operator==(const ProjectSoundPort&) const = default;
};

enum class ProjectSoundParameterTarget
{
    NodeParameter,
    EventValue,
    // A variable of the class's example, ?name in its fields, which takes
    // the instance's value as the class expands (format 109).
    Variable
};

struct ProjectSoundParameterBinding
{
    ProjectSoundParameterTarget target =
        ProjectSoundParameterTarget::NodeParameter;
    std::string nodeId;
    std::string parameterId;
    std::string eventName;
    talas::expr::ValueDomain eventDomain =
        talas::expr::ValueDomain::Scalar;
    std::string variableName;

    bool operator==(const ProjectSoundParameterBinding&) const = default;
};

// Where a parameter's field sits in the Sound editor, the Class Editor's
// Field Rectangle: `left @ top corner: right @
// bottom`, each from 0 to 1.
struct ProjectSoundFieldRectangle
{
    double left = 0.0;
    double top = 0.0;
    double right = 1.0;
    double bottom = 1.0;

    bool operator==(const ProjectSoundFieldRectangle&) const = default;
};

// The kind of field a class's parameter has in the Sound editor (format 145):
// an expression's value field (constants, variables and expressions, no Event
// Values) or hotValue field (Event Values as well, the default); a text's
// string field (taken as typed) or codeString field (Smalltalk, its syntax
// checked as it is entered); a choice's pop-up symbolList (the default) or
// radio buttons in one column or two. From format 147 a text may also be a
// complex number field (`real % imaginary`), a list of complex poles and
// zeroes or a ContextFreeGrammar's production rules, each checked as it is
// entered.
enum class ProjectSoundFieldKind : std::uint8_t
{
    Default,
    Value,
    HotValue,
    String,
    CodeString,
    SymbolList,
    RadioButtons,
    RadioButtonsTwoColumns,
    Complex,
    PolesAndZeroes,
    Grammar,
};

inline constexpr std::size_t maximumSoundParameterCommentBytes = 4096;
inline constexpr std::size_t maximumSoundDescriptionBytes = 16384;

struct ProjectSoundExposedParameter
{
    std::string parameterId;
    std::string displayName;
    talas::core::ParameterKind kind =
        talas::core::ParameterKind::Expression;
    talas::expr::ValueDomain domain =
        talas::expr::ValueDomain::Scalar;
    std::vector<std::string> choices;
    ProjectParameterExpression defaultValue;
    std::vector<std::string> aliases;
    std::vector<ProjectSoundParameterBinding> bindings;
    // The Class Editor: the range of an
    // expression's constant values in its domain, the field's place, and
    // its comment.
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<ProjectSoundFieldRectangle> field;
    std::string comment;
    ProjectSoundFieldKind fieldKind = ProjectSoundFieldKind::Default;

    bool operator==(const ProjectSoundExposedParameter&) const = default;
};

// A Sound class's icon: another kind of Sound's (a prototype's type or
// a Sound class, `sameAs`), a monochrome picture drawn dot by dot in the
// Class Editor, large and small, or a PNG file named for the class. Its
// outline stays the class's own, from its inputs and outputs.
inline constexpr std::size_t soundIconLargeWidth = 64;
inline constexpr std::size_t soundIconLargeHeight = 32;
inline constexpr std::size_t soundIconSmallWidth = 32;
inline constexpr std::size_t soundIconSmallHeight = 16;

struct ProjectSoundIcon
{
    std::string sameAs;
    // The drawn icons: rows from the top, each dot a bit from the left (the
    // most significant first); none, or a row for each of the icon's.
    std::vector<std::uint64_t> large;
    std::vector<std::uint32_t> small;
    // The PNG icon box: the picture in the class's PNG file.
    bool png = false;
    // Format 141: the input and output types the Class Editor chose, shaping
    // the outline's left and right edges (none, audio, subaudio, spectrum,
    // transparent, constructor, variable or environment; empty for the type
    // the class's ports give), and the Nonaudio side effects box, a pointed
    // top.
    std::string inputType;
    std::string outputType;
    bool sideEffects = false;
    // Format 143: the number of independent input and output channels, 1
    // (mono, the same signal left and right) or 2, drawn as the lines into and
    // out of the class's Sounds in the signal flow; 0 for none chosen.
    std::uint8_t inputChannels = 0;
    std::uint8_t outputChannels = 0;

    bool operator==(const ProjectSoundIcon&) const = default;
};

struct ProjectSoundDefinition
{
    std::string definitionId;
    std::string displayName;
    std::vector<ProjectSoundPort> inputs;
    std::vector<ProjectSoundPort> outputs;
    ProjectGraph graph;
    std::uint32_t interfaceVersion = 1;
    std::vector<ProjectSoundExposedParameter> exposedParameters;
    // The Class Description.
    std::string description;
    // The Class Editor's Expand box: a
    // ParameterTransformer expands an instance before transforming it;
    // unchecked, it transforms the instance's own parameters first, which
    // its Sounds then follow.
    bool expandsBeforeTransformation = true;
    std::optional<ProjectSoundIcon> icon;

    bool operator==(const ProjectSoundDefinition&) const = default;
};

// A Sound collection: a name and the Sounds and Sound collections
// it holds, in order. A Sound or collection is in at most one collection;
// the rest are at the Sound file's top level.
struct ProjectSoundCollection
{
    std::string collectionId;
    std::string name;
    std::vector<std::string> soundIds;
    std::vector<std::string> collectionIds;

    bool operator==(const ProjectSoundCollection&) const = default;
};

struct ProjectMetadata
{
    std::string title;
};

enum class MidiControlSourceMode
{
    LegacyAnyInput,
    ExactInput
};

enum class VirtualControlKind
{
    Fader,
    Gate,
    Toggle
};

struct MidiControlMapping
{
    std::int32_t channel = 1;
    std::int32_t controller = 0;
    MidiControlSourceMode sourceMode = MidiControlSourceMode::LegacyAnyInput;
    std::string sourceIdentifier;

    bool operator==(const MidiControlMapping&) const = default;
};

struct OscControlMapping
{
    std::string address;

    bool operator==(const OscControlMapping&) const = default;
};

struct OscEgressMapping
{
    std::string address;

    bool operator==(const OscEgressMapping&) const = default;
};

// The Virtual Control Surface Editor's options for a widget (format 97): its
// type (Default keeps the control kind's fader, gate button or toggle),
// whether a fader shows its numeric field, its label (empty: the Event Value's
// name), on and off colours (0xRRGGBB) for a gate or toggle, the grid its
// values fall on (0: continuous), a linear or log taper, and the tab it sits
// on (empty: the surface's main tab).
enum class VirtualControlWidgetStyle
{
    Default,
    Fader,
    HorizontalFader,
    SmallFader,
    Potentiometer,
    // Format 137: a momentary button filled with
    // its On colour while it is held.
    ButtonFill,
};

enum class VirtualControlTaper
{
    Linear,
    Log,
};

inline constexpr std::size_t maximumVirtualControlLabelBytes = 128;
inline constexpr std::size_t maximumVirtualControlTabBytes = 64;

// Format 125: a widget's place on the surface once the surface has been
// unlocked and arranged, in pixels.
struct VirtualControlBounds
{
    std::int32_t left = 0;
    std::int32_t top = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;

    [[nodiscard]] std::int32_t right() const noexcept { return left + width; }
    [[nodiscard]] std::int32_t bottom() const noexcept { return top + height; }
    bool operator==(const VirtualControlBounds&) const = default;
};

// How the surface lays its widgets out (format 125): automatically, as
// long as it has never been edited, the widgets growing and shrinking with
// the window; at the places they were given, the window showing part of a
// larger surface; or at those places scaled to fit the window (Clean up's
// Fit).
enum class VirtualControlLayout
{
    Automatic,
    Fixed,
    Fit,
};

// Format 137: a text labelling the surface or a box grouping its controls,
// placed like a widget but with no Event Value.
enum class VirtualControlDecorationKind
{
    Text,
    Box,
};

inline constexpr std::size_t maximumVirtualControlDecorations = 64;
inline constexpr std::size_t maximumDormantVirtualControls = 256;
inline constexpr std::size_t maximumVirtualControlDecorationTextBytes = 4096;

struct VirtualControlDecoration
{
    VirtualControlDecorationKind kind = VirtualControlDecorationKind::Text;
    std::string text;
    VirtualControlBounds bounds;

    bool operator==(const VirtualControlDecoration&) const = default;
};

// Format 126: a layout of the surface made in its editor, by duplicating or
// merging layouts, and the controls it shows.
struct VirtualControlLayoutPage
{
    std::string name;
    std::vector<std::string> eventNames;

    bool operator==(const VirtualControlLayoutPage&) const = default;
};

struct VirtualControlWidget
{
    VirtualControlWidgetStyle style = VirtualControlWidgetStyle::Default;
    bool showNumber = true;
    std::string label;
    std::optional<std::uint32_t> onColour;
    std::optional<std::uint32_t> offColour;
    double grid = 0.0;
    VirtualControlTaper taper = VirtualControlTaper::Linear;
    std::string tab;
    std::optional<VirtualControlBounds> bounds;

    bool operator==(const VirtualControlWidget&) const = default;
};

struct VirtualControl
{
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    VirtualControlKind kind = VirtualControlKind::Fader;
    double value = 0.0;
    double minimum = 0.0;
    double maximum = 1.0;
    std::optional<MidiControlMapping> midiMapping;
    std::optional<OscControlMapping> oscMapping;
    std::optional<OscEgressMapping> oscEgressMapping;
    VirtualControlWidget widget;
};

// Format 155: a widget a layout keeps of its own, the type, place and range
// its control takes on that layout rather than the surface's shared ones.
struct VirtualControlLayoutWidget
{
    std::string eventName;
    VirtualControlKind kind = VirtualControlKind::Fader;
    double minimum = 0.0;
    double maximum = 1.0;
    VirtualControlWidget widget;

    bool operator==(const VirtualControlLayoutWidget&) const = default;
};

inline constexpr std::size_t maximumVirtualControlLayoutSettings = 256;
inline constexpr std::size_t maximumVirtualControlLayoutWidgets = 256;

// Format 155: a layout is its list of presets with the types, places and
// ranges of its widgets. What one layout of the surface, named as the layout
// list names it, keeps of its own: the snapshot bank of its presets (empty:
// none yet), its arrangement and its widgets. A layout keeping none takes the
// surface's shared ones, as every layout did before format 155; the project's
// top layout always does.
struct VirtualControlLayoutSettings
{
    std::string layout;
    std::string presetBankId;
    std::optional<VirtualControlLayout> arrangement;
    std::vector<VirtualControlLayoutWidget> widgets;

    bool operator==(const VirtualControlLayoutSettings&) const = default;
};

struct ProjectNodePresentation
{
    std::string nodeId;
    CanvasPosition position;

    bool operator==(const ProjectNodePresentation&) const = default;
};

struct ProjectStructureLayout
{
    std::optional<std::string> soundDefinitionId;
    std::vector<ProjectNodePresentation> nodes;

    bool operator==(const ProjectStructureLayout&) const = default;
};

inline constexpr std::size_t maximumPerformanceControlCount = 256;
inline constexpr std::size_t maximumPerformanceMeterCount = 32;
inline constexpr std::size_t maximumPerformanceModulationSlotsPerControl = 8;
inline constexpr std::size_t maximumPerformanceModulationSlotCount = 512;
inline constexpr std::size_t maximumPerformanceIdentifierBytes = 128;
inline constexpr std::size_t maximumPerformanceDisplayNameBytes = 256;

enum class ProjectPerformanceControlKind
{
    Knob,
    Fader,
    Gate,
    Toggle,
    Macro
};

struct ProjectPerformanceModulationSlot
{
    SnapshotTargetIdentity target;
    double depth = 0.0;

    bool operator==(const ProjectPerformanceModulationSlot&) const = default;
};

struct ProjectPerformanceControlPresentation
{
    std::string controlId;
    SnapshotTargetIdentity target;
    ProjectPerformanceControlKind kind = ProjectPerformanceControlKind::Fader;
    std::string displayName;
    CanvasPosition position;
    bool visible = true;
    std::vector<ProjectPerformanceModulationSlot> modulationSlots;

    bool operator==(const ProjectPerformanceControlPresentation&) const = default;
};

struct ProjectPerformanceMeterPresentation
{
    std::string meterId;
    std::uint32_t outputChannel = 1;
    CanvasPosition position;
    bool visible = true;

    bool operator==(const ProjectPerformanceMeterPresentation&) const = default;
};

struct ProjectPresentationState
{
    std::uint32_t schemaVersion = 2;
    std::vector<ProjectStructureLayout> structureLayouts;
    std::vector<ProjectPerformanceControlPresentation> performanceControls;
    std::vector<ProjectPerformanceMeterPresentation> performanceMeters;

    bool operator==(const ProjectPresentationState&) const = default;
};

struct TimelineEventValue
{
    double timeSeconds = 0.0;
    std::string eventName;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;
};

struct TimelineLoopRegion
{
    bool enabled = false;
    double startSeconds = 0.0;
    double endSeconds = 4.0;

    bool operator==(const TimelineLoopRegion&) const = default;
};

struct ProjectTimeline
{
    TimelineLoopRegion loop;
    std::vector<TimelineEventValue> eventValues;
    ProjectMusicalTime musicalTime;
    ProjectScore score;
    // Format 124: the MIDI channel, 1 to 16, whose program changes jump the
    // Timeline's cursor to markers rather than choosing presets; 0 for none.
    std::uint8_t markerProgramChannel = 0;
    // Format 135: the time code offset, the starting time code of the video or
    // other source following the Timeline, in seconds; incoming time code at
    // it is the Timeline's start.
    double timeCodeOffsetSeconds = 0.0;
    // Format 146: the Timeline's own time code frames a second, 24, 25 or 30,
    // which override the project's; 0 to take the project's.
    std::uint8_t timeCodeFramesPerSecond = 0;
    // Format 154: the word size of the Timeline's disk caches and frozen
    // clips, 16 or 24 bits, each channel a mono WAV at the current sample
    // rate; 24 by default, 16 before format 154.
    std::uint8_t cacheWordSize = 24;
    // Format 156: a MemoryWriter's recording read across the Timeline's
    // tracks, clips that overlap rendered together, writers before readers, so
    // a reader and its writer on separate tracks with one start time play as
    // they would in a Mixer. Off, as before format 156, only clips of one
    // track whose Sharing Memory Writers box is checked share recordings, an
    // earlier clip's with a later one.
    bool sharingMemoryWritersAcrossTracks = false;

    bool operator==(const ProjectTimeline&) const = default;
};

enum class ProjectAssetKind
{
    Table,
    Binary,
    Audio,
    PartialSpectrum,
};

enum class ProjectAssetDisposition
{
    Referenced,
    Embedded
};

struct ProjectAsset
{
    std::string id;
    ProjectAssetKind kind = ProjectAssetKind::Table;
    std::string path;
    ProjectAssetDisposition disposition =
        ProjectAssetDisposition::Referenced;
    std::string contentHash;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::string encoding;
    std::uint64_t generation = 1;
    std::string provenance;

    bool operator==(const ProjectAsset&) const = default;
};

enum class TransportSyncSourceMode
{
    Internal,
    MidiClockExactInput,
    MidiTimeCodeExactInput
};

struct ProjectTransportSyncSettings
{
    TransportSyncSourceMode sourceMode = TransportSyncSourceMode::Internal;
    std::string sourceIdentifier;

    bool operator==(const ProjectTransportSyncSettings&) const = default;
};

struct ProjectOscIngressSettings
{
    bool enabled = false;
    std::int32_t port = 9000;

    bool operator==(const ProjectOscIngressSettings&) const = default;
};

struct ProjectOscEgressSettings
{
    bool enabled = false;
    std::int32_t port = 9001;

    bool operator==(const ProjectOscEgressSettings&) const = default;
};

struct ProjectDeviceSettings
{
    bool configured = false;
    std::string audioInputDeviceName;
    std::string audioOutputDeviceName;
    double sampleRate = 0.0;
    std::int32_t bufferSize = 0;
    std::vector<std::int32_t> inputChannels;
    std::vector<std::int32_t> outputChannels;
    std::vector<std::string> midiInputDeviceIdentifiers;
    ProjectTransportSyncSettings transportSync;
    ProjectOscIngressSettings oscIngress;
    ProjectOscEgressSettings oscEgress;

    bool operator==(const ProjectDeviceSettings&) const = default;
};

struct ProjectDocument
{
    int formatVersion = currentProjectFormatVersion;
    talas::expr::ExpressionLanguageVersion expressionLanguage =
        talas::expr::ExpressionLanguageVersion::VariablesV2;
    talas::expr::ExpressionDefinitionLibrary expressionDefinitions;
    ProjectVariableLibrary variableLibrary;
    ProjectMetadata metadata;
    ProjectPresentationState presentation{2, {{std::nullopt, {}}}, {}, {}};
    ProjectSnapshotLibraryState snapshotLibrary;
    ProjectSampleMapLibraryState sampleMapLibrary;
    ProjectGraph graph;
    std::vector<VirtualControl> virtualControls;
    VirtualControlLayout virtualControlLayout = VirtualControlLayout::Automatic;
    std::vector<VirtualControlLayoutPage> virtualControlLayoutPages;
    // Format 155: the layouts' own presets, arrangements and widgets.
    std::vector<VirtualControlLayoutSettings> virtualControlLayoutSettings;
    std::vector<VirtualControlDecoration> virtualControlDecorations;
    // Format 142: widgets pasted into the surface for Event Values it has no
    // control of, kept out of sight; a control of that name added later takes
    // the widget's type, place, size and range.
    std::vector<VirtualControl> dormantVirtualControls;
    // The project's global map,
    // `!Name is: `Source` associations a line; empty for the Default map
    // (ADR-0024). See talas/project/ProjectGlobalMap.h.
    std::string globalMap;
    // A legacy choice of projects before format 100: ramp, ramp:, fullRamp
    // and fullRamp: start at their first value instead of resting at 1
    // until their receiver first becomes positive.
    bool rampsStartAtFirstValue = false;
    // A legacy choice of projects before format 101: a Mixer's duration is
    // not known to the Sounds that read their Inputs' durations (the
    // OverlappingMixer, the cellular automata, RandomSelection and
    // ContextFreeGrammar); otherwise a Mixer lasts its longest Input.
    bool mixerDurationsUnknown = false;
    // A legacy choice of projects before format 103 that name a ramp: a
    // ramp:, fullRamp: or their plain forms holds while its receiver is off
    // instead of running on to its end once triggered.
    bool rampsHoldUntriggered = false;
    // A legacy choice of projects before format 104 with a looped
    // GraphicalEnvelope: Gate falling plays the loop's pass out instead of
    // jumping to its end.
    bool graphicalEnvelopeLoopsPlayOut = false;
    // A legacy choice of projects before format 108 with a LogScale spectral
    // source or a SpectrumLinearToLog: an OscillatorBank reads a log
    // spectrum's note numbers as hertz instead of playing its pitches.
    bool banksReadLogSpectraAsHertz = false;
    // A legacy choice of Val projects before format 114 with a random
    // message: fields reading the same random expression draw the same
    // numbers; otherwise each draws its own.
    bool randomExpressionsShareSeeds = false;
    // A legacy choice of Val projects before format 116 that read
    // !KeyPitch: it is the key's number alone; otherwise it follows the pitch
    // wheel a half step either way.
    bool keyPitchIgnoresPitchBend = false;
    // A legacy choice of Val projects before format 117 that read
    // !Random or !Random1000: each is an Event Value of its own, a fader;
    // otherwise each is a number from -1 to 1 drawn every millisecond.
    bool randomEventSourcesAreFaders = false;
    // A legacy choice of Val projects before format 118 that hold a
    // neg:zero:pos:: it selects at once; otherwise it is 0 until its receiver
    // first changes.
    bool signSelectionsSelectAtOnce = false;
    // A legacy choice of Val projects before format 119 that hold a
    // conditional and something keeping state: their time functions run on
    // through branches not taken; otherwise they stop and carry on from where
    // they left off.
    bool timeRunsOnInSkippedBranches = false;
    // A legacy choice of projects before format 121 that hold Smalltalk (a
    // Script, a transformation, a MIDI script or a field's program): every
    // number in it is floating point; otherwise integers and ratios of them
    // are exact.
    bool smalltalkNumbersAreFloats = false;
    // A legacy choice of Val projects before format 122 that read
    // !RealTime or !Time: each is an Event Value of its own, a fader;
    // otherwise they are the seconds since the whole Sound started.
    bool timeEventSourcesAreFaders = false;
    // A legacy choice of projects before format 122 holding a function of
    // time: it counts from the start of the whole Sound; otherwise it counts
    // from the start of the Sound holding it, as !LocalTime does.
    bool timeFunctionsCountFromTheWholeSound = false;
    // A legacy choice of projects before format 153 holding a TimeControl
    // or TimeStopper: !Time and !LocalTime keep real time inside them;
    // otherwise they count the time they let pass.
    bool timeEventsIgnoreTimeControls = false;
    // A legacy choice of projects before format 102: the same for the newer
    // Mixer (talas.mixerV2), whose duration was unknown wherever it had more
    // than one Input.
    bool manyInputMixerDurationsUnknown = false;
    // Choices of format 156, each off by default so that a project plays as it
    // did; written only when on. A Sound pasted into a field and read there
    // each millisecond reads within -1 and 1, saturating as fixed-point
    // signals do, the reason a pasted Sound is scaled.
    bool pastedSoundsSaturateAtUnity = false;
    // A field reads each pasted Sound as it stood just before the
    // millisecond boundary, so that a Constant pasted into a Constant
    // pasted into another lags a millisecond more each time.
    bool pastedSoundsReadThePreviousMillisecond = false;
    // The inverse of 0 is infinity, which a live field takes as its largest
    // or smallest legal value; a field without one still refuses it
    //.
    bool inverseOfZeroIsInfinite = false;
    std::vector<ProjectAsset> assets;
    std::vector<ProjectPartialTrackEditSet> partialTrackEdits;
    ProjectTimeline timeline;
    ProjectDeviceSettings deviceSettings;
    std::vector<ProjectSoundDefinition> soundDefinitions;
    std::vector<ProjectSoundCollection> soundCollections;
    ProjectSoundVariableLibrary soundVariableLibrary;
    ProjectGeneratorLibrary generatorLibrary;
    ProjectLiveSetLibrary liveSetLibrary;
};

// A widget's options within their limits (format 97): label and tab
// without control characters within their byte limits, a finite grid from
// 0 up to the control's range, a log taper only for a minimum above 0, and
// colours of 24 bits.
// A Sound parameter's Class Editor fields within their limits (format
// 99): a minimum below its maximum, a field within 0 to 1 with its corners
// in order, a comment of at most 4096 bytes, a field kind of the
// parameter's kind (format 145).
[[nodiscard]] bool isValidSoundParameterClassFields(const ProjectSoundExposedParameter& parameter) noexcept;
// A field kind's name as it is written (`hotValue`, `radioButtons`), and back;
// nothing for the default or another name.
[[nodiscard]] std::string_view projectSoundFieldKindName(ProjectSoundFieldKind kind) noexcept;
[[nodiscard]] std::optional<ProjectSoundFieldKind> parseProjectSoundFieldKind(std::string_view name) noexcept;

[[nodiscard]] bool isValidVirtualControlWidget(
    const VirtualControlWidget& widget, double minimum, double maximum) noexcept;
// A layout's own settings within their limits (format 155): a name of at
// most 128 bytes, a preset bank named as snapshot banks are (or none), and
// at most 256 widgets, one for each Event Value, each a finite minimum below
// its maximum with a widget valid for that range.
[[nodiscard]] bool isValidVirtualControlLayoutSettings(const VirtualControlLayoutSettings& settings) noexcept;
// A text or box with at most 4096 bytes of text (no control characters but
// tabs and new lines) on the surface, of some size.
[[nodiscard]] bool isValidVirtualControlDecoration(const VirtualControlDecoration& decoration) noexcept;
// A widget pasted onto a control: its type and
// options, its place and size, and its range, the value kept within it.
// False when the widget does not suit the range.
[[nodiscard]] bool takeVirtualControlWidget(VirtualControl& control, const VirtualControl& pasted);
// A control added for an Event Value takes the dormant widget of its name,
// which wakes; true if there was one.
bool wakeDormantVirtualControl(std::vector<VirtualControl>& dormant, VirtualControl& control);
// The input a Sound Browser's replacement input, a track's input or a line
// the Sound is dropped on takes: the
// replaceable input when the Sound has one, else its first Audio input.
[[nodiscard]] const ProjectSoundPort* replaceableSoundInput(const ProjectSoundDefinition& definition) noexcept;
[[nodiscard]] talas::core::Graph makeGraph(const ProjectDocument& document);
[[nodiscard]] ProjectDocument makeProjectDocument(const talas::core::Graph& graph);
// A new, empty project as File > New makes it: a beat written in a field
// lasts a quarter note at 60, while its Timeline
// keeps a tempo of 120. A default-constructed
// document, as an older project opens, follows the Timeline's tempo.
[[nodiscard]] ProjectDocument makeNewProjectDocument();
[[nodiscard]] const ProjectStructureLayout* findProjectStructureLayout(
    const ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId = std::nullopt);
[[nodiscard]] ProjectStructureLayout* findProjectStructureLayout(
    ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId = std::nullopt);
[[nodiscard]] const ProjectNodePresentation* findProjectNodePresentation(
    const ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId,
    std::string_view nodeId);
[[nodiscard]] ProjectNodePresentation* findProjectNodePresentation(
    ProjectDocument& document,
    std::optional<std::string_view> soundDefinitionId,
    std::string_view nodeId);
[[nodiscard]] const ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    const ProjectDocument& document,
    std::string_view eventName,
    talas::expr::ValueDomain domain);
[[nodiscard]] ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    ProjectDocument& document,
    std::string_view eventName,
    talas::expr::ValueDomain domain);
[[nodiscard]] const ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    const ProjectDocument& document,
    const SnapshotTargetIdentity& target);
[[nodiscard]] ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentation(
    ProjectDocument& document,
    const SnapshotTargetIdentity& target);
[[nodiscard]] const ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentationById(
    const ProjectDocument& document,
    std::string_view controlId);
[[nodiscard]] ProjectPerformanceControlPresentation*
findProjectPerformanceControlPresentationById(
    ProjectDocument& document,
    std::string_view controlId);
[[nodiscard]] bool isProjectPerformanceTargetAvailable(
    const ProjectDocument& document,
    const SnapshotTargetIdentity& target);
[[nodiscard]] bool isValidProjectPerformanceIdentifier(
    std::string_view value) noexcept;
[[nodiscard]] bool projectPerformancePresentationHasCycle(
    const ProjectDocument& document);
void reconcileProjectPresentation(ProjectDocument& document);
}
