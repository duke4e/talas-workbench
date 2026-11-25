#pragma once

#include "talas/audio/PartialTrackResynthesis.h"
#include "talas/cdp/DistortEnvelope.h"
#include "talas/cdp/DistortHarmonic.h"
#include "talas/cdp/DistortMultiply.h"
#include "talas/cdp/DistortOmit.h"
#include "talas/cdp/DistortOverload.h"
#include "talas/cdp/DistortReform.h"
#include "talas/cdp/DistortReverse.h"
#include "talas/cdp/DistortShuffle.h"
#include "talas/cdp/Echo.h"
#include "talas/cdp/FixedFilter.h"
#include "talas/cdp/LohiFilter.h"
#include "talas/cdp/LowerResolution.h"
#include "talas/cdp/Pan.h"
#include "talas/cdp/Phasing.h"
#include "talas/cdp/PhaseVocoder.h"
#include "talas/cdp/Reverb.h"
#include "talas/cdp/RingModulate.h"
#include "talas/cdp/SpectralBlur.h"
#include "talas/cdp/SpectralChorus.h"
#include "talas/cdp/SpectralFocus.h"
#include "talas/cdp/SpectralPitch.h"
#include "talas/cdp/SpectralScatter.h"
#include "talas/cdp/SpectralArpeg.h"
#include "talas/cdp/SpectralBands.h"
#include "talas/cdp/SpectralGlis.h"
#include "talas/cdp/SpectralCombine.h"
#include "talas/cdp/SpectralMorph.h"
#include "talas/cdp/SpectralTransposef.h"
#include "talas/cdp/SpectralFormants.h"
#include "talas/cdp/SpectralBlurBlur.h"
#include "talas/cdp/SpectralFocusPeaks.h"
#include "talas/cdp/SpectralPitchFile.h"
#include "talas/cdp/PitchTrack.h"
#include "talas/cdp/SpectralFreeze.h"
#include "talas/cdp/SpectralWholeSound.h"
#include "talas/cdp/VaryingBank2.h"
#include "talas/cdp/TremoloFine.h"
#include "talas/cdp/WavesetLocal.h"
#include "talas/cdp/RoomReverb.h"
#include "talas/cdp/Quantise.h"
#include "talas/cdp/LoudnessGain.h"
#include "talas/cdp/PhaseInvert.h"
#include "talas/cdp/DcShift.h"
#include "talas/cdp/TwoInput.h"
#include "talas/cdp/FastConvolution.h"
#include "talas/cdp/SilenceGate.h"
#include "talas/cdp/Deglitch.h"
#include "talas/cdp/NoiseCut.h"
#include "talas/cdp/AttackBoost.h"
#include "talas/cdp/SubmixPairs.h"
#include "talas/cdp/EnvelopeWarp.h"
#include "talas/cdp/PitchedDelay.h"
#include "talas/cdp/Bounce.h"
#include "talas/cdp/Rejoin.h"
#include "talas/cdp/Cascade.h"
#include "talas/cdp/Phasor.h"
#include "talas/cdp/Spin.h"
#include "talas/cdp/Brassage.h"
#include "talas/cdp/Antiphony.h"
#include "talas/cdp/ChannelEnvelope.h"
#include "talas/cdp/CrossPan.h"
#include "talas/cdp/Flutter.h"
#include "talas/cdp/FrameRemap.h"
#include "talas/cdp/FrameRotate.h"
#include "talas/cdp/MultichannelStadium.h"
#include "talas/cdp/RotatingPan.h"
#include "talas/cdp/SpreadPan.h"
#include "talas/cdp/StepSpread.h"
#include "talas/cdp/TablePan.h"
#include "talas/cdp/ToStereo.h"
#include "talas/cdp/ChannelTools.h"
#include "talas/cdp/EnvelopeWarpStretch.h"
#include "talas/cdp/Prefix.h"
#include "talas/cdp/SilenceMasks.h"
#include "talas/cdp/StereoPlacement.h"
#include "talas/cdp/StereoSpace.h"
#include "talas/cdp/SpectralSimple.h"
#include "talas/cdp/StadiumEcho.h"
#include "talas/cdp/SweepingFilter.h"
#include "talas/cdp/TapDelay.h"
#include "talas/cdp/Tremolo.h"
#include "talas/cdp/UserBank.h"
#include "talas/cdp/VariableFilter.h"
#include "talas/cdp/VaryingBank.h"
#include "talas/cdp/VaryingDelay.h"

#include "talas/core/NodeRegistry.h"
#include "talas/core/ResourceSnapshot.h"
#include "talas/dsp/DiffuseSpectralStretch.h"
#include "talas/dsp/DualMorphOscillator.h"
#include "talas/dsp/DynamicBreakpointOscillator.h"
#include "talas/dsp/LinearVocoderBank.h"
#include "talas/dsp/VocoderBandShaping.h"
#include "talas/dsp/LiveSpectralAnalysis.h"
#include "talas/dsp/SerializedSpectrumFrameAssembler.h"
#include "talas/dsp/SerializedSpectrumFrameSynchronizer.h"
#include "talas/dsp/LowPassGate.h"
#include "talas/dsp/MorphFilter.h"
#include "talas/dsp/NativeStft.h"
#include "talas/dsp/PackedRealFftAdapter.h"
#include "talas/dsp/FrequencyScaleGranulator.h"
#include "talas/dsp/PhaseVocoderPitchShifter.h"
#include "talas/dsp/PreparedTimeFrequencyScale.h"
#include "talas/dsp/PluckedWaveguide.h"
#include "talas/dsp/PreparedSpectrumFilterBank.h"
#include "talas/dsp/ProbabilityChaosGenerator.h"
#include "talas/dsp/QuadFunctionGenerator.h"
#include "talas/dsp/ScannedMassOscillator.h"
#include "talas/dsp/StereoDispersiveResonator.h"
#include "talas/dsp/WaveTerrainOscillator.h"
#include "talas/project/PreparedProjectSoundVariables.h"
#include "talas/project/PreparedProjectVariables.h"
#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectGlobalMap.h"
#include "talas/project/SpeakerPlacement.h"
#include "talas/project/VariableLiterals.h"
#include "talas/runtime/IsolatedComponentRuntime.h"
#include "talas/runtime/KernelRegistry.h"
#include "talas/runtime/LiveEnvelopeSegmentState.h"
#include "talas/runtime/MidiMasterClock.h"
#include "talas/runtime/PreparedMidiMapper.h"
#include "talas/runtime/ProcessorPlacement.h"
#include "talas/runtime/PreparedScore.h"
#include "talas/runtime/RealtimeAnalysisCapture.h"
#include "talas/runtime/RealtimeMidiVoiceAllocator.h"
#include "talas/runtime/SampleMapPlayback.h"
#include "talas/runtime/detail/CloudBankGrainState.h"
#include "talas/runtime/detail/PastedSoundExpression.h"
#include "talas/runtime/detail/RunningExtremumState.h"
#include "talas/runtime/detail/ValToSoundInterpolator.h"
#include "talas/runtime/detail/LossyIntegratorState.h"
#include "talas/runtime/detail/HysteresisState.h"
#include "talas/runtime/detail/BinauralPanState.h"
#include "talas/runtime/detail/OscillatorTimeIndexState.h"
#include "talas/runtime/detail/SaturatorMath.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace talas::runtime
{
inline constexpr std::size_t preparedSoundVariableCrossfadeFrames = 128;

// The port a node of CDP's multichannel tools plays
// (src/runtime/CdpMultichannelPorts.h), empty until it is prepared.
using CdpMultichannelPort = std::variant<std::monostate,
    talas::cdp::RotatingPan,
    talas::cdp::SpreadPan,
    talas::cdp::TablePan,
    talas::cdp::StepSpread,
    talas::cdp::CrossPan,
    talas::cdp::Antiphony,
    talas::cdp::MultichannelStadium,
    talas::cdp::Flutter,
    talas::cdp::FrameRotate,
    talas::cdp::FrameRemap,
    talas::cdp::ChannelEnvelope,
    talas::cdp::ToStereo,
    talas::cdp::StereoSpace,
    talas::cdp::ChannelZero,
    talas::cdp::ChannelMixdown,
    talas::cdp::MonoToStereo,
    talas::cdp::ChannelPhase,
    talas::cdp::MonoToMany,
    talas::cdp::ChannelPair,
    talas::cdp::StereoPlacement,
    talas::cdp::Prefix,
    talas::cdp::SilenceMasks,
    talas::cdp::SilenceOverwrite>;

class DecodedBlockCacheOwner;
enum class AudioStreamChannel : std::uint8_t;
struct DecodedAudioData;
class RealtimeRenderSession;
struct RuntimePlan;

enum class DiagnosticCode
{
    InvalidRenderSettings,
    GraphValidationFailed,
    UnsupportedNodeType,
    MissingAudioOutput,
    MultipleAudioOutputs,
    DuplicateInputConnection,
    UnknownParameter,
    DuplicateParameter,
    ParameterParseError,
    ParameterEvaluationError,
    NegativeFrequency,
    RenderAllocationFailed
};

struct Diagnostic
{
    DiagnosticCode code;
    std::string nodeId;
    std::string portId;
    std::string parameterId;
    std::string message;
    std::string structuralSourcePath;
};

struct TableResource
{
    std::string id;
    std::vector<double> samples;
    std::uint32_t sourceSampleRate = 0;
    std::optional<talas::core::SampleLoop> forwardLoop;
    std::optional<talas::core::SamplePitch> samplePitch;
    std::vector<double> rightSamples;
    // Set when a graph is compiled: its tables never change afterwards, so
    // rendered blocks reuse this instead of rescanning every sample.
    std::optional<bool> samplesFinite;
    // The notes and velocities the sample's header gives, and, for a sample
    // of a KeyMappedMultisample's bank, the id
    // of the bank's first sample and the file name it orders samples by.
    std::optional<talas::core::SampleKeyRange> keyRange;
    std::string bank;
    std::string bankName;
    // The named positions the sample's header gives.
    std::vector<talas::core::SampleMarker> markers;
};

struct BinaryResource
{
    std::string id;
    std::vector<std::uint8_t> bytes;
};

struct AudioInputChannel
{
    std::size_t channel = 1;
    std::vector<double> samples;
    // Set when a graph is compiled, like TableResource::samplesFinite.
    std::optional<bool> samplesFinite;
};

using RuntimeScopeId = std::uint32_t;
inline constexpr RuntimeScopeId globalRuntimeScope = 0;

enum class SnapshotPreparedProjection
{
    DirectEventValues,
    DirectVariable,
    PerformanceOverlay
};

struct SnapshotPreparedEventValueTarget
{
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::uint32_t targetIndex = 0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;

    bool operator==(const SnapshotPreparedEventValueTarget&) const = default;
};

struct SnapshotPreparedTarget
{
    talas::project::SnapshotTargetIdentity target;
    talas::project::SnapshotValueKind valueKind =
        talas::project::SnapshotValueKind::Number;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    std::vector<std::string> choices;
    std::optional<talas::project::SnapshotValue> baselineValue;
    SnapshotPreparedProjection projection =
        SnapshotPreparedProjection::PerformanceOverlay;
    std::vector<SnapshotPreparedEventValueTarget>
        eventValueTargets;
    std::optional<std::uint32_t> variableTargetIndex;

    bool operator==(const SnapshotPreparedTarget&) const = default;
};

struct SnapshotPreparedTargetManifest
{
    std::vector<SnapshotPreparedTarget> targets;

    bool operator==(const SnapshotPreparedTargetManifest&) const = default;
};

struct EventValueBinding
{
    std::string name;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    RuntimeScopeId scopeId = globalRuntimeScope;
    RuntimeScopeId sourceScopeId = globalRuntimeScope;
    std::optional<talas::expr::Expression> projectionExpression;
    std::vector<std::uint32_t> variableCellIndices;
};

struct EventValueChange
{
    std::size_t frame = 0;
    std::string name;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    talas::core::AutomationInterpolation interpolation =
        talas::core::AutomationInterpolation::Step;
    RuntimeScopeId scopeId = globalRuntimeScope;
    // Compile-time symbolic scope for generated Score Sound instances. It is
    // resolved to scopeId before the prepared graph is published.
    std::string targetInstanceId;
};

struct TransportLoopRegion
{
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;

    bool operator==(const TransportLoopRegion&) const = default;
};

struct RenderSettings
{
    double sampleRate = 48000.0;
    std::size_t maxBlockFrames = 512;
    double tempoBpm = 120.0;
    // From the project (format 100): ramps start at their first value, as
    // in older projects, instead of resting at 1.
    bool rampsStartAtFirstValue = false;
    // From the project (format 103): a ramp: or fullRamp: whose trigger
    // falls holds where it is, as in older projects, instead of running on
    // to its end.
    bool rampsHoldUntriggered = false;
    // From the project (format 121): its Smalltalk numbers are all floating
    // point, as in older projects, where integers and ratios are exact
    //.
    bool smalltalkNumbersAreFloats = false;
    // From the project (format 122): functions of time count from the start
    // of the whole Sound, as in older projects, instead of from the start of
    // the Sound holding them.
    bool timeFunctionsCountFromTheWholeSound = false;
    // From the project (format 153): !Time and !LocalTime keep real time
    // inside TimeControls and TimeStoppers, as in older projects holding
    // one, instead of the time those let pass.
    bool timeEventsIgnoreTimeControls = false;
    // From the project (format 156): the inverse of 0 is infinity, which a
    // live field takes as its largest or smallest legal value; off, a division
    // by zero fails, as before.
    bool inverseOfZeroIsInfinite = false;
    // The Sound editor's environment: the
    // values given to variables no declaration names, each playing as a
    // project variable of that value. Not saved with the project.
    std::vector<std::pair<std::string, double>> variableEnvironment;
    // And the values that are more than numbers: strings, symbols, characters
    // and arrays, each field reading its variable as it reads the literal
    // (`talas::project::withVariableLiterals`); a number here joins the
    // numbers above. Not saved with the project.
    talas::project::VariableLiteralEnvironment variableLiteralEnvironment;
    // And for a Variable or SoundCollectionVariable no declaration names,
    // the Sounds it plays: the default
    // Sound, or the default collection's. A Sound of the project is named
    // by its definition's id; another joins the played copy of the project.
    std::vector<std::pair<std::string, std::vector<talas::project::ProjectSoundDefinition>>> soundVariableEnvironment;
    // The Speaker Placement preferences: the
    // speakers a Timeline's panned Sounds go between, one an output
    // channel; none, the left and right of two channels.
    std::vector<talas::project::SpeakerPosition> speakerPlacement;
    // The Timeline's marked Sounds alone, when it
    // plays them only; every Sound otherwise.
    std::optional<std::vector<std::string>> playedScoreClipIds;
    // From the project (format 104): a GraphicalEnvelope whose Gate falls in
    // its loop plays the loop's pass out, as in older projects, instead of
    // jumping to the loop's end.
    bool graphicalEnvelopeLoopsPlayOut = false;
    // From the project (format 108): an OscillatorBank reads a log
    // spectrum's note numbers as hertz, as in older projects, instead of
    // playing its pitches.
    bool banksReadLogSpectraAsHertz = false;
    // From the project (format 114): fields reading the same random
    // expression draw the same numbers, as in older projects.
    bool randomExpressionsShareSeeds = false;
    // Set by compiling once it has transformed the instances of classes
    // whose Expand box is unchecked, before their Sounds expand.
    bool instancesTransformedFirst = false;
    // compileProject always replaces this compatibility default with the
    // owning ProjectDocument authority before parsing project-authored text.
    talas::expr::ExpressionLanguageVersion expressionLanguage =
        talas::expr::ExpressionLanguageVersion::LegacyV1;
    talas::project::ProjectMusicalTime musicalTime;
    std::uint8_t defaultMidiChannel = 1;
    // ADR-0024 and its 2026-10-19 amendment: the global map's associations
    // MIDI also sets (the project's own map, else the Default map);
    // compileProject leaves out those the project maps itself.
    std::vector<talas::project::GlobalMapEntry> globalMap;
    std::vector<EventValueBinding> eventValues;
    std::vector<EventValueChange> eventValueChanges;
    std::shared_ptr<const PreparedScoreSchedule> preparedScore;
    std::vector<PreparedScoreClipAudioPtr> preparedScoreClipAudio;
    // Optional offline render horizon. Score clips beginning at or beyond
    // this exclusive frame do not need source materialization or graph nodes.
    std::optional<std::uint64_t> scorePlaybackEndFrameExclusive;
    std::vector<talas::core::ResourceSnapshotPtr>
        resourceSnapshots;
    // Compatibility-only inputs. compileProject snapshots these values before
    // preparing a graph; new project paths publish resourceSnapshots directly.
    std::vector<TableResource> tableResources;
    // Immutable snapshots inherited by a later offline Score clip on a track
    // whose Sharing Memory Writers option is enabled.
    std::vector<TableResource> sharedMemoryWriterTables;
    bool captureMemoryWriterRecordings = false;
    std::vector<BinaryResource> binaryResources;
    std::vector<AudioInputChannel> audioInputChannels;
    // Maximum retained input frames per prepared temporal node; only allocated
    // when a node needs history.
    std::size_t maxRealtimeHistoryFrames = 480000;
    // Aggregate circular-history budget across all explicit feedback pairs.
    std::size_t maxRealtimeFeedbackFrames = 1920000;
    // Maximum prepared steps admitted to explicit feedback SCC analysis.
    std::size_t maxFeedbackPlanSteps = 8192;
    // Fixed-capacity prepared queue for sample-accurate realtime Event Value
    // snapshots.
    std::size_t maxRealtimeEventTransactions = 256;
    // Maximum caller-visible terminal events produced by one prepared callback.
    std::size_t maxRealtimeTerminalEvents = 256;
    // Optional fixed-capacity raw block capture for prepared display nodes.
    bool enableRealtimeAnalysisCapture = false;
    std::size_t realtimeAnalysisQueueCapacity = 8;
    std::vector<RealtimeAnalysisTargetSpec> realtimeAnalysisTargets;
    std::optional<TransportLoopRegion> transportLoop;
    // Frozen with the same project document/stage authority as the language.
    // All parsing occurs during preparation; callback execution sees only the
    // compiled immutable expression programs.
    talas::expr::ExpressionDefinitionLibrary expressionDefinitions;
    // Immutable prepared identities and initial values. Live sessions own a
    // separate fixed cell array and never mutate project bytes implicitly.
    talas::project::PreparedProjectVariableTable preparedVariables;
    talas::project::PreparedProjectSoundVariableTable preparedSoundVariables;
    // Exact compiler-owned plan authority retained outside the executable
    // RuntimePlan. Prepared Sound-variable admission uses it to bind every
    // candidate ordinal to its original computation closure after MIDI
    // lowering, rather than trusting coherently mutable plan identities.
    std::shared_ptr<const RuntimePlan>
        preparedSoundVariableAuthorityPlan;
    // Exact compiler-owned plan retained whenever fixed Vocoder admission
    // depends on structural identities that cannot be reconstructed from the
    // flattened executable plan alone.
    std::shared_ptr<const RuntimePlan>
        preparedFixedVocoderAuthorityPlan;
    // Exact compiler-owned plan retained whenever Vocoder Channel Bank
    // admission depends on mapped Spectrum and transactional closure
    // identities that cannot be reconstructed from mutable plan bytes.
    std::shared_ptr<const RuntimePlan>
        preparedVocoderChannelBankAuthorityPlan;
    // Exact compiler-owned plan retained whenever WaitUntil admission depends
    // on connected producer and structural identities that cannot be inferred
    // safely from coherently mutable executable plan bytes.
    std::shared_ptr<const RuntimePlan>
        preparedWaitUntilAuthorityPlan;
    // Exact compiler-owned plan retained whenever WarpedTimeIndex admission
    // depends on the complete producer, structural, and floating-point
    // execution projection rather than locally self-consistent plan bytes.
    std::shared_ptr<const RuntimePlan>
        preparedWarpedTimeIndexAuthorityPlan;
    // Exact compiler-owned plan retained whenever Monotonizer admission
    // depends on known-pitch profile, latency ancestry, connected producers,
    // and complete callback transaction identities.
    std::shared_ptr<const RuntimePlan>
        preparedMonotonizerAuthorityPlan;
    // Exact compiler-owned plan retained whenever Multifile Disk Player
    // admission depends on static resource-list ownership, connected control
    // producers, reset metadata, and the complete callback graph identity.
    std::shared_ptr<const RuntimePlan>
        preparedMultifileDiskPlayerAuthorityPlan;
    // Exact compiler-owned immutable resource generations paired with the
    // Multifile Disk Player plan authority. Prepared construction must use
    // these identities rather than trusting coherently mutable compatibility
    // table payloads in this public DTO.
    std::shared_ptr<const std::vector<talas::core::ResourceSnapshotPtr>>
        preparedMultifileDiskPlayerAuthorityResourceSnapshots;
};

struct GranularGrainStart
{
    std::int64_t index = 0;
    double startFrame = 0.0;
};

// What a GrainCloud or SampleCloud grain kept from its first frame: its
// frequency or its start in the sample, its length and its pan.
struct GranularGrainLatch
{
    std::int64_t index = -1;
    bool sounds = false;
    double first = 0.0;
    double durationFrames = 0.0;
    double pan = 0.5;
};

struct BufferSpec
{
    std::string nodeId;
    std::string portId;
    talas::core::PortType type;
    std::size_t frames = 0;
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::size_t complexFrameCapacity = 0;
    std::size_t complexMaximumBinsPerFrame = 0;
    std::size_t complexChannelCount = 0;
    std::size_t partialFrameCapacity = 0;
    std::size_t partialMaximumPartialsPerFrame = 0;

    bool operator==(const BufferSpec&) const = default;
};

struct ParameterValue
{
    std::string id;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    double value = 0.0;
    std::optional<std::string> choice;
    std::optional<bool> boolean;
    std::optional<std::string> resource;
    std::optional<std::string> text;
    std::optional<talas::expr::Expression> expression;
    RuntimeScopeId variableScopeId = globalRuntimeScope;
    // One immutable zero-based cell handle per expression.variableReferences()
    // entry. The compiled evaluator consumes these by reference index.
    std::vector<std::uint32_t> variableCellIndices;

    bool operator==(const ParameterValue& other) const noexcept
    {
        if (id != other.id
            || domain != other.domain
            || std::bit_cast<std::uint64_t>(value)
                != std::bit_cast<std::uint64_t>(other.value)
            || choice != other.choice
            || boolean != other.boolean
            || resource != other.resource
            || text != other.text
            || expression.has_value() != other.expression.has_value()
            || variableScopeId != other.variableScopeId
            || variableCellIndices != other.variableCellIndices)
        {
            return false;
        }
        return !expression.has_value()
            || expression->sharesCompiledStorageWith(*other.expression);
    }
};

struct OutputBinding
{
    std::string outputPortId;
    std::size_t bufferIndex = 0;

    bool operator==(const OutputBinding&) const = default;
};

struct InputBinding
{
    std::string inputPortId;
    std::string sourceNodeId;
    std::string sourcePortId;
    std::size_t bufferIndex = 0;
    RuntimeScopeId sourceScopeId = globalRuntimeScope;

    bool operator==(const InputBinding&) const = default;
};

// Exact immutable authority for the opt-in C11 DiskCache playback subset.
// Absence preserves the historical record=false passthrough/prewarm behavior.
struct PreparedDiskCachePlayback
{
    std::string assetId;
    std::string assetPath;
    std::uint64_t generation = 0;
    std::string contentHash;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::string encoding;
    std::string sourceFingerprint;
    std::string provenance;

    bool operator==(const PreparedDiskCachePlayback&) const = default;
};

enum class DiskRecorderWriteSupport : std::uint8_t
{
    Writable,
    UnsupportedChannels,
    UnsupportedFormat,
    UnsupportedWordSize,
};

// Exact immutable authority for the bounded prepared-realtime Disk Recorder
// publication subset. The project fileName remains a display/name hint only;
// an owner-selected local destination is required before any file is opened.
struct PreparedDiskRecorderCapturePlan
{
    std::size_t stepIndex = 0;
    std::size_t captureBufferIndex = 0;
    std::optional<std::size_t> triggerBufferIndex;
    std::optional<std::size_t> captureDurationBufferIndex;
    std::string destinationNameHint;
    std::string format;
    std::string wordSize;
    std::string channels;
    double trigger = 0.0;
    double captureDurationSeconds = 0.0;
    bool gated = false;
    DiskRecorderWriteSupport writeSupport =
        DiskRecorderWriteSupport::UnsupportedFormat;

    bool operator==(const PreparedDiskRecorderCapturePlan&) const = default;
};

// A DiskRecorder within a Sound: what it records,
// decided as a terminal DiskRecorder's capture, and the buffers of its one
// or two channels. Each records on its own.
struct PreparedDiskRecorderTapPlan
{
    std::string nodeId;
    PreparedDiskRecorderCapturePlan capture;
    std::vector<std::size_t> channelBufferIndices;

    bool operator==(const PreparedDiskRecorderTapPlan&) const = default;
};

struct CompileStep
{
    std::string nodeId;
    std::string typeId;
    std::vector<InputBinding> inputs;
    std::vector<ParameterValue> parameters;
    RuntimeScopeId scopeId = globalRuntimeScope;
    RuntimeScopeId structuralScopeId = globalRuntimeScope;
    RuntimeScopeId eventValueScopeId = globalRuntimeScope;
    talas::core::ExecutionSupport offlineSupport =
        talas::core::ExecutionSupport::Subset;
    talas::core::ExecutionBinding offlineBinding =
        talas::core::ExecutionBinding::LegacyAdapter;
    talas::core::ExecutionSupport preparedRealtimeSupport =
        talas::core::ExecutionSupport::Subset;
    talas::core::ExecutionBinding preparedRealtimeBinding =
        talas::core::ExecutionBinding::LegacyAdapter;
    bool supportsReset = false;
    std::optional<KernelKind> kernelKind;
    bool preparedIsolatedComponent = false;
    std::string structuralSourcePath;
    std::optional<std::size_t> resolvedLatencyFrames;
    std::vector<std::optional<std::size_t>> inputLatencyFrames;
    std::optional<std::size_t> cumulativeInputLatencyFrames;
    std::optional<std::size_t> cumulativeOutputLatencyFrames;
    std::optional<std::size_t> preparedSpectralFrameSize;
    std::optional<std::size_t> preparedSpectralHopSize;
    // Empty preserves the scalar C53 fallback. Exactly 4096 entries are the
    // immutable clipped C54 TrackNumber probability profile.
    std::vector<double> spectrumModifierProbabilityProfile;
    std::optional<PreparedSampleMapPlayback> sampleMapPlayback;
    std::optional<PreparedDiskCachePlayback> diskCachePlayback;
    std::optional<std::size_t> soundVariableCellIndex;
    std::string soundVariableId;
    talas::project::ProjectSoundVariableKind soundVariableKind =
        talas::project::ProjectSoundVariableKind::Sound;
    std::vector<std::string> soundVariableCandidateDefinitionIds;
    // An ADSR's constant Gate falls this many
    // seconds after it starts, its release ending with its Sound.
    std::optional<double> constantGateFallsSeconds;

    // Ordered original registry outputs. Synthetic latency buffers and terminal
    // channel routes have separate owners. Empty preserves legacy plan adapters.
    std::vector<OutputBinding> outputs;

    bool operator==(const CompileStep&) const = default;
};

struct LatencyCompensationPlan
{
    std::size_t targetStepIndex = 0;
    std::size_t inputBindingIndex = 0;
    std::size_t sourceBufferIndex = 0;
    std::size_t outputBufferIndex = 0;
    std::size_t delayFrames = 0;

    bool operator==(const LatencyCompensationPlan&) const = default;
};

// Any signal may feed any input. A connection from an Audio output to a
// Control input, or the reverse, reads its source through a copy typed as the
// input, filled before the input's step runs.
struct SignalAdapterPlan
{
    std::size_t targetStepIndex = 0;
    std::size_t inputBindingIndex = 0;
    std::size_t sourceBufferIndex = 0;
    std::size_t outputBufferIndex = 0;

    bool operator==(const SignalAdapterPlan&) const = default;
};

struct ReprepareDiagnostic
{
    std::size_t stepIndex = 0;
    std::string nodeId;
    std::string parameterId;
    std::string eventValueName;
    RuntimeScopeId eventValueScopeId = globalRuntimeScope;
    std::string message;

    bool operator==(const ReprepareDiagnostic&) const = default;
};

enum class LatencyPlanWarningKind
{
    UncompensatedUnknownLatency,
    UncompensatedFeedback
};

struct LatencyPlanWarning
{
    LatencyPlanWarningKind kind =
        LatencyPlanWarningKind::UncompensatedUnknownLatency;
    std::size_t stepIndex = 0;
    std::string nodeId;
    std::string message;

    bool operator==(const LatencyPlanWarning&) const = default;
};

struct VoiceInstancePlan
{
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::size_t firstStepIndex = 0;
    std::size_t stepCount = 0;
    std::size_t audioOutputBufferIndex = 0;
    std::optional<std::size_t> rightAudioOutputBufferIndex;
    std::uint16_t voiceOrdinal = 0;
    std::size_t firstVariableCellIndex = 0;
    std::size_t variableCellCount = 0;
    std::size_t firstSoundVariableCellIndex = 0;
    std::size_t soundVariableCellCount = 0;
    // Hierarchical plans also own defaults in projected descendant Sounds;
    // those cells need not be contiguous with the owner's declared variables.
    std::vector<std::size_t> variableCellIndices;
    std::vector<std::size_t> soundVariableCellIndices;

    bool operator==(const VoiceInstancePlan&) const = default;
};

struct MidiVoicePlan
{
    std::size_t ownerStepIndex = 0;
    RealtimeMidiVoiceConfiguration configuration;
    std::vector<VoiceInstancePlan> voices;
    std::string ownerNodeId;
    std::string ownerDefinitionId;
    std::vector<std::string> ownerInstancePath;
    std::optional<RuntimeScopeId> parentVoiceScopeId;
    std::vector<talas::project::ProjectMidiVoiceAncestorIdentity> voiceAncestors;

    bool operator==(const MidiVoicePlan&) const = default;
};

struct MidiMapperPlan
{
    std::size_t ownerStepIndex = 0;
    RuntimeScopeId scopeId = globalRuntimeScope;
    PreparedMidiMapperConfiguration configuration;
    std::string nodeId;

    bool operator==(const MidiMapperPlan&) const = default;
};

struct FeedbackPairPlan
{
    RuntimeScopeId scopeId = globalRuntimeScope;
    std::string connectionId;
    std::size_t inputStepIndex = 0;
    std::size_t outputStepIndex = 0;
    std::size_t inputBufferIndex = 0;
    std::size_t outputBufferIndex = 0;
    std::size_t delayFrames = 1;

    bool operator==(const FeedbackPairPlan&) const = default;
};

struct FeedbackComponentPlan
{
    std::vector<std::size_t> stepIndices;
    std::vector<std::size_t> feedbackPairIndices;

    bool operator==(const FeedbackComponentPlan&) const = default;
};

struct FeedbackPairRuntimeState
{
    void prepare(std::size_t delayFrames)
    {
        delayLine.assign(delayFrames, 0.0);
        readIndex = 0;
        pendingInput = 0.0;
        hasPendingInput = false;
    }

    [[nodiscard]] bool ready(std::size_t delayFrames) const noexcept
    {
        return delayFrames > 0
            && delayLine.size() == delayFrames
            && readIndex < delayLine.size();
    }

    void beginFrame() noexcept
    {
        hasPendingInput = false;
    }

    [[nodiscard]] double delayedOutput() const noexcept
    {
        return delayLine[readIndex];
    }

    void stageInput(double value) noexcept
    {
        pendingInput = value;
        hasPendingInput = true;
    }

    [[nodiscard]] bool commitFrame() noexcept
    {
        if (!hasPendingInput || delayLine.empty()
            || readIndex >= delayLine.size())
        {
            return false;
        }
        delayLine[readIndex] = pendingInput;
        readIndex = (readIndex + 1) % delayLine.size();
        hasPendingInput = false;
        return true;
    }

    void reset() noexcept
    {
        for (auto& sample : delayLine)
            sample = 0.0;
        readIndex = 0;
        pendingInput = 0.0;
        hasPendingInput = false;
    }

    std::vector<double> delayLine;
    std::size_t readIndex = 0;
    double pendingInput = 0.0;
    bool hasPendingInput = false;
};

struct LatencyCompensationRuntimeState
{
    void prepare(std::size_t delayFrames)
    {
        delayLine.assign(delayFrames, 0.0);
        writeIndex = 0;
    }

    [[nodiscard]] bool ready(std::size_t delayFrames) const noexcept
    {
        return delayFrames > 0
            && delayLine.size() == delayFrames
            && writeIndex < delayLine.size();
    }

    [[nodiscard]] bool process(
        std::span<const double> input,
        std::span<double> output) noexcept
    {
        if (input.size() != output.size() || delayLine.empty()
            || writeIndex >= delayLine.size())
        {
            return false;
        }
        for (std::size_t frame = 0; frame < input.size(); ++frame)
        {
            output[frame] = delayLine[writeIndex];
            delayLine[writeIndex] = input[frame];
            writeIndex = (writeIndex + 1) % delayLine.size();
        }
        return true;
    }

    void reset() noexcept
    {
        for (auto& sample : delayLine)
            sample = 0.0;
        writeIndex = 0;
    }

    std::vector<double> delayLine;
    std::size_t writeIndex = 0;
};

struct RuntimePlan
{
    std::vector<CompileStep> steps;
    std::vector<BufferSpec> buffers;
    std::size_t outputBufferIndex = 0;
    std::optional<std::size_t> rightOutputBufferIndex;
    std::vector<std::size_t> outputBufferIndices;
    std::vector<MidiMapperPlan> midiMappers;
    std::vector<MidiVoicePlan> midiVoices;
    std::vector<FeedbackPairPlan> feedbackPairs;
    std::vector<FeedbackComponentPlan> feedbackComponents;
    std::vector<LatencyCompensationPlan> latencyCompensations;
    std::vector<SignalAdapterPlan> signalAdapters;
    std::vector<ReprepareDiagnostic> reprepareDiagnostics;
    std::vector<LatencyPlanWarning> latencyWarnings;
    std::optional<std::size_t> criticalPathLatencyFrames;
    std::optional<std::size_t> totalOutputLatencyFrames;
    ProcessorPlacementPlan processorPlacement;
    std::optional<PreparedDiskRecorderCapturePlan> diskRecorderCapture;
    std::vector<PreparedDiskRecorderTapPlan> diskRecorderTaps;
    // Projection marker; execution additionally requires private compiler-owned sources.
    bool requiresLiveScoreSources = false;

    bool operator==(const RuntimePlan&) const = default;
};

struct DiskRecorderCapturePlanValidationResult
{
    std::string error;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

[[nodiscard]] DiskRecorderCapturePlanValidationResult
validateDiskRecorderCapturePlan(const RuntimePlan& runtimePlan);

[[nodiscard]] std::string_view
diskRecorderWriteSupportName(DiskRecorderWriteSupport support) noexcept;

// The clock !Time and
// !LocalTime read inside TimeControls of fixed Rates and TimeStoppers whose
// fixed Resume holds time: from originFrame, the latest of their starts, it
// runs at rate (their Rates' product, 0 under a holding TimeStopper) on
// from originSeconds, the !Time it had reached by then.
struct ControlledTimeClock
{
    bool controlled = false;
    double rate = 1.0;
    std::uint64_t originFrame = 0;
    double originSeconds = 0.0;
};

struct MidiEvent
{
    std::size_t frame = 0;
    std::vector<std::uint8_t> bytes;
};

// MIDIFileEcho events parsed once for one resource, rate, channel range and
// render end; derived from immutable inputs, so checkpoints skip it.
struct OfflineMidiFileEchoCache
{
    const std::vector<std::uint8_t>* resource = nullptr;
    double sampleRate = 0.0;
    std::size_t renderEndFrame = 0;
    std::uint8_t lowChannel = 0;
    std::uint8_t highChannel = 0;
    bool valid = false;
    std::vector<MidiEvent> events;
};

struct GlobalControllerEvent
{
    std::size_t frame = 0;
    std::string name;
    double value = 0.0;
    bool allowLiveOverride = false;
};

struct OfflineRenderResult
{
    std::vector<double> mono;
    std::vector<double> right;
    std::vector<std::vector<double>> channels;
    std::vector<MidiEvent> midiEvents;
    std::vector<GlobalControllerEvent> globalControllerEvents;
    // What each DiskRecorder within the Sound recorded, in the
    // plan's order: every frame, silent where it was not recording.
    struct DiskRecording
    {
        std::string nodeId;
        std::vector<std::vector<double>> channels;
    };
    std::vector<DiskRecording> diskRecordings;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

struct OfflineVariableAssignment
{
    std::uint64_t frame = 0;
    std::uint64_t batchId = 0;
    std::uint64_t sequence = 0;
    talas::project::PreparedProjectVariableIdentity target;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
};

struct OfflineSoundVariableAssignment
{
    std::uint64_t frame = 0;
    std::uint64_t batchId = 0;
    std::uint64_t sequence = 0;
    talas::project::PreparedProjectSoundVariableIdentity target;
    std::string selectedDefinitionId;
};

struct OfflineSoundVariableValue
{
    talas::project::PreparedProjectSoundVariableIdentity identity;
    std::string selectedDefinitionId;
};

enum class OfflineMidiVoiceEventKind : std::uint8_t
{
    NoteOn,
    NoteOff,
    VariableAssignment,
    SoundVariableAssignment,
    EventValueAssignment,
};

struct OfflineMidiVoiceEvent
{
    std::uint64_t frame = 0;
    std::uint64_t batchId = 0;
    std::uint64_t sequence = 0;
    OfflineMidiVoiceEventKind kind =
        OfflineMidiVoiceEventKind::NoteOff;
    std::uint8_t channel = 1;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
    talas::project::PreparedProjectVariableIdentity target;
    talas::project::PreparedProjectSoundVariableIdentity soundTarget;
    std::string selectedDefinitionId;
    std::uint64_t voiceGeneration = 0;
    double value = 0.0;
    talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    // Event Value identity belongs to this prepared graph. Directly writable
    // bindings only; no projection override or voice-generation reinterpretation.
    std::string eventValueName;
    RuntimeScopeId eventValueScopeId = globalRuntimeScope;
};

struct OfflineComplexSpectrumFrame
{
    std::uint32_t formatVersion =
        talas::dsp::complexSpectrumFormatVersion;
    talas::dsp::SpectralFrameTimestamp analysisTimestamp;
    std::uint64_t sourceSequence = 0;
    std::size_t frameSize = 0;
    std::size_t hopSize = 0;
    talas::dsp::SpectralWindowKind window =
        talas::dsp::SpectralWindowKind::Rectangular;
    std::size_t channelIndex = 0;
    std::vector<std::complex<double>> bins;
};

struct OfflineComplexSpectrumResult
{
    std::vector<OfflineComplexSpectrumFrame> frames;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

struct OfflinePartialSpectrumFrame
{
    std::uint32_t formatVersion =
        talas::dsp::partialSpectrumFormatVersion;
    talas::dsp::SpectralFrameTimestamp sourceTimestamp;
    std::uint64_t sourceSequence = 0;
    std::vector<talas::dsp::PartialSpectrumPoint> partials;
};

struct OfflinePartialSpectrumResult
{
    std::vector<OfflinePartialSpectrumFrame> frames;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return diagnostics.empty();
    }
};

struct CompileResult;
struct PreparedLiveScoreSources;

class PreparedRuntimeGraph
{
public:
    PreparedRuntimeGraph();
    explicit PreparedRuntimeGraph(RuntimePlan plan);
    PreparedRuntimeGraph(
        RuntimePlan plan,
        RenderSettings settings,
        std::shared_ptr<RealtimeAnalysisCapture> analysisCapture = {},
        std::vector<std::shared_ptr<DecodedBlockCacheOwner>>
            audioStreamOwners = {},
        SnapshotPreparedTargetManifest snapshotPreparedTargets = {},
        std::shared_ptr<PreparedIsolatedComponentBinding>
            isolatedComponentBinding = {});

    [[nodiscard]] const RuntimePlan& plan() const;
    [[nodiscard]] const RenderSettings& renderSettings() const noexcept;
    [[nodiscard]] std::shared_ptr<RealtimeAnalysisCapture> analysisCapture() const noexcept;
    [[nodiscard]] std::shared_ptr<const PreparedScoreSourceAnalysisSet>
    scoreSourceAnalysisCaptures() const noexcept;
    [[nodiscard]] std::span<const talas::core::ResourceSnapshotPtr>
    resourceSnapshots() const noexcept;
    [[nodiscard]] std::size_t preparedAudioStreamCount() const noexcept;
    [[nodiscard]] const SnapshotPreparedTargetManifest&
    snapshotPreparedTargets() const noexcept;
    [[nodiscard]] const talas::project::PreparedProjectVariableTable&
    preparedVariables() const noexcept;
    [[nodiscard]] const talas::project::PreparedProjectSoundVariableTable&
    preparedSoundVariables() const noexcept;
    [[nodiscard]] std::shared_ptr<PreparedIsolatedComponentBinding>
    isolatedComponentBinding() const noexcept;

private:
    [[nodiscard]] std::shared_ptr<DecodedBlockCacheOwner>
    audioStreamOwner(std::string_view resourceId) const noexcept;
    // The owner reading one channel of a stereo resource (format 95), if one
    // was prepared.
    [[nodiscard]] std::shared_ptr<DecodedBlockCacheOwner>
    audioStreamOwner(std::string_view resourceId, AudioStreamChannel channel) const noexcept;
    void sealSamplingAuthorityForCompiler(
        const talas::core::NodeRegistry& registry);
    [[nodiscard]] static CompileResult compileProjectImpl(
        const talas::project::ProjectDocument&,
        const talas::core::NodeRegistry&,
        RenderSettings,
        std::span<const talas::project::SnapshotValueEntry>,
        std::shared_ptr<PreparedIsolatedComponentBinding>,
        bool requiresLiveScoreSources = false);

    std::shared_ptr<const PreparedLiveScoreSources> compiledLiveScoreSources_;
    RuntimePlan plan_;
    RenderSettings settings_;
    std::shared_ptr<RealtimeAnalysisCapture> analysisCapture_;
    std::vector<std::shared_ptr<DecodedBlockCacheOwner>>
        audioStreamOwners_;
    SnapshotPreparedTargetManifest snapshotPreparedTargets_;
    std::shared_ptr<PreparedIsolatedComponentBinding>
        isolatedComponentBinding_;
    // Unlike the compatibility authorities retained in the public
    // RenderSettings DTO, Sample, both Multisample variants, the checked
    // sampling/cloud nodes, checked Noise, Absolute Value, ArcTan, Equality,
    // Product, Difference, Set Range, Delay With Feedback, Reverb Section,
    // Function Generator, Chopper, and the Multislope/Multisegment generator
    // provenances are compiler-sealed here. Public PreparedRuntimeGraph
    // construction intentionally cannot populate these immutable copies.
    std::shared_ptr<const RuntimePlan>
        compiledSamplingAuthorityPlan_;
    std::shared_ptr<const RenderSettings>
        compiledSamplingAuthoritySettings_;
    std::shared_ptr<const std::vector<TableResource>>
        compiledSamplingAuthorityTableResources_;
    std::shared_ptr<const std::vector<talas::core::ResourceSnapshotPtr>>
        compiledSamplingAuthorityResourceSnapshots_;
    // A copied RuntimePlan proves only that it matches what one registry
    // compiled. Singular promoted nodes additionally require their exact
    // built-in descriptor contracts so a coherently divergent custom registry
    // cannot mint its own otherwise self-consistent authority.
    bool compiledConstantRegistryAuthority_ = false;
    bool compiledAddRegistryAuthority_ = false;
    bool compiledMixerRegistryAuthority_ = false;
    bool compiledLevelRegistryAuthority_ = false;
    bool compiledChannelCrosserRegistryAuthority_ = false;
    bool compiledChannelJoinRegistryAuthority_ = false;
    bool compiledComplexProductRegistryAuthority_ = false;
    bool compiledClippingDivideRegistryAuthority_ = false;
    bool compiledPointwiseExtremumRegistryAuthority_ = false;
    bool compiledChannellerRegistryAuthority_ = false;
    bool compiledPanRegistryAuthority_ = false;
    bool compiledCrossfadeRegistryAuthority_ = false;
    bool compiledStereoMix2RegistryAuthority_ = false;
    bool compiledStereoMix4RegistryAuthority_ = false;
    bool compiledAveragingLowPassFilterRegistryAuthority_ = false;
    bool compiledDelayWithFeedbackRegistryAuthority_ = false;
    bool compiledReverbSectionRegistryAuthority_ = false;
    bool compiledTimeOffsetRegistryAuthority_ = false;
    bool compiledWaitUntilRegistryAuthority_ = false;
    bool compiledTimeControlRegistryAuthority_ = false;
    bool compiledTimeStopperRegistryAuthority_ = false;
    bool compiledSetDurationRegistryAuthority_ = false;
    bool compiledWarpedTimeIndexRegistryAuthority_ = false;
    bool compiledAmplitudeFollowerRegistryAuthority_ = false;
    bool compiledLowPassGateRegistryAuthority_ = false;
    bool compiledMorphFilterRegistryAuthority_ = false;
    bool compiledDualMorphOscillatorRegistryAuthority_ = false;
    bool compiledAnnotationRegistryAuthority_ = false;
    bool compiledThresholdRegistryAuthority_ = false;
    bool compiledLogicRegistryAuthority_ = false;
    bool compiledQuantizerRegistryAuthority_ = false;
    bool compiledGateRegistryAuthority_ = false;
    bool compiledSlewRegistryAuthority_ = false;
    bool compiledClockRegistryAuthority_ = false;
    bool compiledClockDividerRegistryAuthority_ = false;
    bool compiledFilterRegistryAuthority_ = false;
    bool compiledHighShelvingFilterRegistryAuthority_ = false;
    bool compiledLowShelvingFilterRegistryAuthority_ = false;
    bool compiledPresenceFilterRegistryAuthority_ = false;
    bool compiledTwoFormantElementRegistryAuthority_ = false;
    bool compiledTwoFormantVoiceElementRegistryAuthority_ = false;
    bool compiledDualParallelTwoPoleFilterRegistryAuthority_ = false;
    bool compiledGraphicEqRegistryAuthority_ = false;
    bool compiledFirFilterRegistryAuthority_ = false;
    bool compiledReResonatorRegistryAuthority_ = false;
    bool compiledHarmonicResonatorRegistryAuthority_ = false;
    bool compiledDynamicRangeControllerRegistryAuthority_ = false;
    bool compiledFrequencyTrackerRegistryAuthority_ = false;
    bool compiledInterpolateRegistryAuthority_ = false;
    bool compiledScaleAndOffsetRegistryAuthority_ = false;
    bool compiledProbabilityChaosGeneratorRegistryAuthority_ = false;
    bool compiledQuadFunctionGeneratorRegistryAuthority_ = false;
    bool compiledStereoDispersiveResonatorRegistryAuthority_ = false;
    bool compiledPluckedWaveguideRegistryAuthority_ = false;
    bool compiledDynamicBreakpointOscillatorRegistryAuthority_ = false;
    bool compiledWaveTerrainOscillatorRegistryAuthority_ = false;
    bool compiledScannedMassOscillatorRegistryAuthority_ = false;
    bool compiledDiffuseSpectralStretchRegistryAuthority_ = false;
    bool compiledPeakDetectorRegistryAuthority_ = false;
    bool compiledRmsSquaredRegistryAuthority_ = false;
    bool compiledRunningMaxRegistryAuthority_ = false;
    bool compiledRunningMinRegistryAuthority_ = false;
    bool compiledTriggeredSampleAndHoldRegistryAuthority_ = false;
    bool compiledTriggeredTableReadRegistryAuthority_ = false;
    bool compiledFunctionGeneratorRegistryAuthority_ = false;
    bool compiledChopperRegistryAuthority_ = false;
    bool compiledAnalogSequencerRegistryAuthority_ = false;
    bool compiledMultislopeFunctionGeneratorRegistryAuthority_ = false;
    bool compiledMultisegmentEnvelopeRegistryAuthority_ = false;
    bool compiledGraphicalEnvelopeRegistryAuthority_ = false;
    bool compiledArRegistryAuthority_ = false;
    bool compiledAnalysisFilterRegistryAuthority_ = false;
    bool compiledPhaseShiftBy90RegistryAuthority_ = false;
    bool compiledSingleSideBandRmRegistryAuthority_ = false;
    bool compiledSosOscillatorsRegistryAuthority_ = false;
    bool compiledAdsrRegistryAuthority_ = false;
    bool compiledOscillatorRegistryAuthority_ = false;
    bool compiledLfoRegistryAuthority_ = false;
    bool compiledPulseTrainRegistryAuthority_ = false;
    bool compiledRampRegistryAuthority_ = false;
    bool compiledSampleAndHoldRegistryAuthority_ = false;
    std::shared_ptr<const RuntimePlan>
        compiledAudioOutputAuthorityPlan_;
    std::shared_ptr<const RenderSettings>
        compiledAudioOutputAuthoritySettings_;
    bool compiledAudioOutputRegistryAuthority_ = false;

    friend OfflineRenderResult renderOffline(const PreparedRuntimeGraph& graph, std::size_t frameCount);
    friend class OfflineRenderSession;
    friend class RealtimeRenderSession;
    friend CompileResult compileProject(
        const talas::project::ProjectDocument&,
        const talas::core::NodeRegistry&,
        RenderSettings);
    friend CompileResult compileProjectWithIsolatedComponent(
        const talas::project::ProjectDocument&,
        const talas::core::NodeRegistry&,
        RenderSettings,
        std::shared_ptr<PreparedIsolatedComponentBinding>);
    friend CompileResult compileProjectPerformanceOverlay(
        const talas::project::ProjectDocument&,
        const talas::core::NodeRegistry&,
        RenderSettings,
        std::span<const talas::project::SnapshotValueEntry>);
};

struct CompileResult
{
    std::optional<PreparedRuntimeGraph> graph;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const
    {
        return graph.has_value() && diagnostics.empty();
    }
};

// Availability-only preflight for preserved component capsules. Ordinary
// graph/runtime validation remains part of compileProject.
[[nodiscard]] std::vector<Diagnostic>
projectComponentExecutionDiagnostics(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry);

struct FilterStageState
{
    double ic1eq = 0.0;
    double ic2eq = 0.0;
};

struct BiquadState
{
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

// A Monotonizer's pitch tracker: its history's position, the frames to the
// next estimate and the estimate it holds.
struct MonotonizerTrackerState
{
    std::size_t writeIndex = 0;
    std::size_t count = 0;
    std::size_t hopPhase = 0;
    double estimate = 0.0;
    bool hasEstimate = false;
};

// An AnalysisFilter's running sum over one period of its Fundamental, with
// its history's position; the rotations follow the Fundamental and harmonic.
struct AnalysisFilterWindowState
{
    std::size_t writeIndex = 0;
    std::size_t count = 0;
    std::uint64_t frame = 0;
    double fundamental = 0.0;
    double harmonic = 0.0;
    double sumReal = 0.0;
    double sumImaginary = 0.0;
    double rotationReal = 1.0;
    double rotationImaginary = 0.0;
    double lastReal = 1.0;
    double lastImaginary = 0.0;
    double tailReal = 1.0;
    double tailImaginary = 0.0;
    bool hasSum = false;
};

struct NoiseGeneratorState
{
    bool initialized = false;
    std::uint64_t randomState = 0;
    double currentRaw = 0.0;
    double pink0 = 0.0;
    double pink1 = 0.0;
    double pink2 = 0.0;
    double pink3 = 0.0;
    double pink4 = 0.0;
    double pink5 = 0.0;
    double pink6 = 0.0;
    double hotTarget = 0.0;
    double hotPhase = 0.0;

    bool operator==(const NoiseGeneratorState&) const = default;
};

[[nodiscard]] CompileResult compileProject(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    RenderSettings settings);

[[nodiscard]] CompileResult compileProjectWithIsolatedComponent(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    RenderSettings settings,
    std::shared_ptr<PreparedIsolatedComponentBinding> binding);

[[nodiscard]] CompileResult compileProjectPerformanceOverlay(
    const talas::project::ProjectDocument& document,
    const talas::core::NodeRegistry& registry,
    RenderSettings settings,
    std::span<const talas::project::SnapshotValueEntry> values);

[[nodiscard]] OfflineRenderResult renderOffline(
    const PreparedRuntimeGraph& graph,
    std::size_t frameCount);

// Executes one disposable zero-length runtime validation block. Unlike the
// public render(0) transaction, this intentionally validates node execution
// configuration and resources without publishing or retaining session state.
[[nodiscard]] OfflineRenderResult validateOfflineRuntimeGraph(
    const PreparedRuntimeGraph& graph);

class OfflineRenderSession
{
public:
    // The referenced graph must outlive the render session.
    explicit OfflineRenderSession(const PreparedRuntimeGraph& graph);
    ~OfflineRenderSession();
    OfflineRenderSession(OfflineRenderSession&&) noexcept;
    OfflineRenderSession& operator=(OfflineRenderSession&&) noexcept;
    OfflineRenderSession(const OfflineRenderSession&) = delete;
    OfflineRenderSession& operator=(const OfflineRenderSession&) = delete;
    explicit OfflineRenderSession(PreparedRuntimeGraph&& graph) = delete;
    explicit OfflineRenderSession(const PreparedRuntimeGraph&& graph) = delete;

    [[nodiscard]] OfflineRenderResult render(std::size_t frameCount);
    [[nodiscard]] bool scheduleVariableAssignments(
        std::span<const OfflineVariableAssignment> assignments);
    // Schedules non-voice selections on the independent offline renderer.
    [[nodiscard]] bool
    scheduleSoundVariableAssignments(std::span<const OfflineSoundVariableAssignment> assignments);
    [[nodiscard]] std::vector<OfflineSoundVariableValue> soundVariableValues() const;
    [[nodiscard]] std::vector<TableResource> recordedMemoryWriterTables() const;
    // The recording this session's MemoryWriter of
    // that name is making, as far as it has got (empty until it starts),
    // or nullptr when it has none or does not keep its recording.
    [[nodiscard]] const std::vector<double>* memoryWriterRecording(
        std::string_view recordingName) const;
    // A reader and its writer scheduled together on
    // separate tracks of a Timeline. Where this session's Sample or
    // SampleCloud reads a recording no MemoryWriter of its own makes, it
    // asks this first for the recording another session, rendered block by
    // block beside this one, is making; nullptr falls back to the earlier
    // clips' recordings.
    using LiveMemoryWriterRecordings =
        std::function<const std::vector<double>*(std::string_view recordingName)>;
    void readLiveMemoryWriterRecordings(LiveMemoryWriterRecordings recordings);
    // Prepares one immutable absolute-frame combined control/MIDI stream before
    // the first render. Audio and event terminals use the realtime execution
    // machinery in offline-sized chunks, with its callback transaction rules.
    // Event Value assignments use exact prepared scope/name/domain and zero
    // voiceGeneration. Derived projections and reprepare-only targets reject.
    // Numeric and Sound assignments may also target exact non-voice cells with
    // a zero voiceGeneration. Combined controls do not require a MIDI owner;
    // note events still require a matching prepared Voice or Mapper.
    [[nodiscard]] bool scheduleMidiVoiceEvents(
        std::span<const OfflineMidiVoiceEvent> events);
    void reset();
    [[nodiscard]] OfflineComplexSpectrumResult inspectComplexSpectrum(
        std::string_view nodeId,
        std::string_view portId,
        std::size_t frameCount,
        RuntimeScopeId scopeId = globalRuntimeScope);
    [[nodiscard]] OfflinePartialSpectrumResult inspectPartialSpectrum(
        std::string_view nodeId,
        std::string_view portId,
        std::size_t frameCount,
        RuntimeScopeId scopeId = globalRuntimeScope);

private:
    friend struct OfflineRenderSessionTestAccess;
    friend OfflineRenderResult validateOfflineRuntimeGraph(
        const PreparedRuntimeGraph& graph);

    [[nodiscard]] static bool
    exchangeRejectedAddResultInjectionForTesting(bool enabled) noexcept;
    [[nodiscard]] static bool
    exchangeRejectedMixerResultInjectionForTesting(bool enabled) noexcept;
    [[nodiscard]] static std::uint64_t
    exchangeRejectedTransactionEndFrameForTesting(
        std::uint64_t absoluteEndFrame) noexcept;

    // Test work accounting counts frames submitted to native unchecked execution,
    // including replay; it is independent of musical/transaction state.
    [[nodiscard]] static std::uint64_t exchangeNativeWorkFramesForTesting(
        std::uint64_t value) noexcept;

    [[nodiscard]] OfflineRenderResult renderImpl(
        std::size_t frameCount,
        std::optional<std::size_t> captureComplexBufferIndex,
        std::vector<OfflineComplexSpectrumFrame>* capturedComplexFrames,
        std::optional<std::size_t> capturePartialBufferIndex,
        std::vector<OfflinePartialSpectrumFrame>* capturedPartialFrames);
    [[nodiscard]] OfflineRenderResult renderImplUnchecked(
        std::size_t frameCount,
        std::optional<std::size_t> captureComplexBufferIndex,
        std::vector<OfflineComplexSpectrumFrame>* capturedComplexFrames,
        std::optional<std::size_t> capturePartialBufferIndex,
        std::vector<OfflinePartialSpectrumFrame>* capturedPartialFrames,
        bool zeroFrameNoOpOnly = false);
    [[nodiscard]] OfflineRenderResult renderMidiVoiceEvents(
        std::size_t frameCount);

    struct OfflineDecodedAudioResource
    {
        std::string id;
        std::shared_ptr<const DecodedAudioData> data;
    };

    struct OfflineLiveExpression
    {
        std::size_t stepIndex = 0;
        std::size_t parameterIndex = 0;
        talas::expr::ExpressionState state;
        talas::expr::ExpressionState candidateState;
        double value = 0.0;
        double candidateValue = 0.0;
    };

    struct PreparedOfflineVariableAssignment
    {
        std::uint64_t frame = 0;
        std::uint64_t batchId = 0;
        std::uint64_t sequence = 0;
        std::uint32_t cellIndex = 0;
        double value = 0.0;
        talas::expr::ValueDomain domain = talas::expr::ValueDomain::Scalar;
    };

    struct PreparedOfflineSoundVariableAssignment
    {
        std::uint64_t frame = 0;
        std::uint64_t batchId = 0;
        std::uint64_t sequence = 0;
        std::uint32_t cellIndex = 0;
        std::uint16_t selectedOrdinal = 0;
    };

    std::vector<std::uint8_t> affectedEventValueBindings_;
    std::vector<std::uint8_t> affectedVariableCells_;
    std::vector<talas::expr::ExpressionValue> variableCells_;
    std::vector<talas::expr::ExpressionValue> candidateVariableCells_;
    std::vector<PreparedOfflineVariableAssignment>
        scheduledVariableAssignments_;
    std::size_t scheduledVariableReadIndex_ = 0;
    std::size_t scheduledVariablePreparedCount_ = 0;
    std::vector<std::uint16_t> soundVariableCells_;
    std::vector<std::uint16_t> candidateSoundVariableCells_;
    std::vector<PreparedOfflineSoundVariableAssignment> scheduledSoundVariableAssignments_;
    std::size_t scheduledSoundVariableReadIndex_ = 0;
    std::size_t scheduledSoundVariablePreparedCount_ = 0;

    struct OfflineMidiVoiceBackend;
    std::unique_ptr<OfflineMidiVoiceBackend> midiVoiceBackend_;

    [[nodiscard]] bool prepareLiveExpressionsAtFrame(
        std::size_t frame,
        bool& advancesState) noexcept;
    void commitPreparedLiveExpressions() noexcept;

    struct NodeValueState
    {
        detail::val_to_sound::State valToSound;
        detail::lossy_integrator::State lossyIntegrator;
        talas::cdp::LowerResolution cdpLowerResolution;
        talas::cdp::RingModulate cdpRingModulate;
        talas::cdp::Tremolo cdpTremolo;
        talas::cdp::Pan cdpPan;
        talas::cdp::TapDelay cdpTapDelay;
        talas::cdp::Echo cdpEcho;
        talas::cdp::VaryingDelay cdpVaryingDelay;
        talas::cdp::StadiumEcho cdpStadiumEcho;
        talas::cdp::Reverb cdpReverb;
        talas::cdp::VariableFilter cdpVariableFilter;
        talas::cdp::FixedFilter cdpFixedFilter;
        talas::cdp::Phasing cdpPhasing;
        talas::cdp::LohiFilter cdpLohiFilter;
        talas::cdp::SweepingFilter cdpSweepingFilter;
        talas::cdp::UserBank cdpUserBank;
        talas::cdp::DistortOverload cdpDistortOverload;
        talas::cdp::VaryingBank cdpVaryingBank;
        talas::cdp::DistortReform cdpDistortReform;
        talas::cdp::DistortOmit cdpDistortOmit;
        talas::cdp::DistortReverse cdpDistortReverse;
        talas::cdp::DistortEnvelope cdpDistortEnvelope;
        talas::cdp::DistortHarmonic cdpDistortHarmonic;
        talas::cdp::DistortFractal cdpDistortFractal;
        talas::cdp::DistortMultiply cdpDistortMultiply;
        talas::cdp::DistortDivide cdpDistortDivide;
        talas::cdp::DistortShuffle cdpDistortShuffle;
        talas::cdp::PhaseVocoder cdpPhaseVocoder;
        talas::cdp::SpectralGain cdpSpectralGain;
        talas::cdp::SpectralGate cdpSpectralGate;
        talas::cdp::FocusExaggerate cdpFocusExaggerate;
        talas::cdp::FocusFold cdpFocusFold;
        talas::cdp::FocusAccumulate cdpFocusAccumulate;
        talas::cdp::BlurAverage cdpBlurAverage;
        talas::cdp::BlurSuppress cdpBlurSuppress;
        talas::cdp::HiliteTrace cdpHiliteTrace;
        talas::cdp::HiliteFilter cdpHiliteFilter;
        talas::cdp::HilitePluck cdpHilitePluck;
        talas::cdp::FocusStep cdpFocusStep;
        talas::cdp::StrangeShift cdpStrangeShift;
        talas::cdp::RepitchTranspose cdpRepitchTranspose;
        talas::cdp::StrangeWaver cdpStrangeWaver;
        talas::cdp::PitchTransp cdpPitchTransp;
        talas::cdp::BlurChorus cdpBlurChorus;
        talas::cdp::BlurNoise cdpBlurNoise;
        talas::cdp::StretchSpectrum cdpStretchSpectrum;
        talas::cdp::BlurScatter cdpBlurScatter;
        talas::cdp::HiliteArpeg cdpHiliteArpeg;
        talas::cdp::HiliteGraphicEq cdpHiliteGraphicEq;
        talas::cdp::HiliteBand cdpHiliteBand;
        talas::cdp::BlurSpread cdpBlurSpread;
        talas::cdp::StrangeGlis cdpStrangeGlis;
        talas::cdp::CombineSum cdpCombineSum;
        talas::cdp::CombineDiff cdpCombineDiff;
        talas::cdp::CombineMax cdpCombineMax;
        talas::cdp::CombineMean cdpCombineMean;
        talas::cdp::CombineCross cdpCombineCross;
        talas::cdp::FormantsVocode cdpFormantsVocode;
        talas::cdp::MorphMorph cdpMorphMorph;
        talas::cdp::MorphBridge cdpMorphBridge;
        talas::cdp::RepitchTransposeFormants cdpRepitchTransposef;
        talas::cdp::FormantsPut cdpFormantsPut;
        talas::cdp::HiliteVowels cdpHiliteVowels;
        talas::cdp::BlurBlur cdpBlurBlur;
        talas::cdp::HiliteBlurTrace cdpHiliteBlurTrace;
        talas::cdp::FocusFocus cdpFocusFocus;
        talas::cdp::PitchTune cdpPitchTune;
        talas::cdp::PitchPick cdpPitchPick;
        talas::cdp::PitchChord cdpPitchChord;
        talas::cdp::PitchChordFormants cdpPitchChordf;
        talas::cdp::PitchAltHarmonics cdpPitchAltharms;
        talas::cdp::PitchOctaveMove cdpPitchOctmove;
        talas::cdp::SpecBare cdpSpecBare;
        talas::cdp::SpecClean cdpSpecClean;
        talas::cdp::StrangeInvert cdpStrangeInvert;
        talas::cdp::FocusFreeze cdpFocusFreeze;
        talas::cdp::VaryingBank2 cdpVaryingBank2;
        talas::cdp::TremoloFine cdpTremoloFine;
        talas::cdp::DistmoreDouble cdpDistmoreDouble;
        talas::cdp::ClipWaveset cdpClipWaveset;
        talas::cdp::QuirkWaveset cdpQuirkWaveset;
        talas::cdp::RoomReverb cdpRoomReverb;
        talas::cdp::Quantise cdpQuantise;
        talas::cdp::LoudnessGain cdpLoudnessGain;
        talas::cdp::PhaseInvert cdpPhaseInvert;
        talas::cdp::DcShift cdpDcShift;
        talas::cdp::CrossModulate cdpCrossModulate;
        talas::cdp::Subtract cdpSubtract;
        talas::cdp::MixBalance cdpMixBalance;
        talas::cdp::MixCrossfade cdpMixCrossfade;
        talas::cdp::FastConvolution cdpFastConvolution;
        talas::cdp::SilenceGate cdpSilenceGate;
        talas::cdp::Deglitch cdpDeglitch;
        talas::cdp::NoiseCut cdpNoiseCut;
        talas::cdp::AttackBoost cdpAttackBoost;
        talas::cdp::MixMerge cdpMixMerge;
        talas::cdp::MixFaders cdpMixFaders;
        talas::cdp::MixInterleave cdpMixInterleave;
        talas::cdp::MixInbetween cdpMixInbetween;
        talas::cdp::EnvelopeWarp cdpEnvelopeWarp;
        talas::cdp::EnvelopeWarpStretch cdpEnvelopeTimestretch;
        talas::cdp::EnvelopeImpose cdpEnvelopeImpose;
        talas::cdp::EnvelopeTarget cdpEnvelopeTarget;
        talas::cdp::PitchedDelay cdpPitchedDelay;
        talas::cdp::Bounce cdpBounce;
        talas::cdp::Rejoin cdpRejoin;
        talas::cdp::Cascade cdpCascade;
        talas::cdp::Phasor cdpPhasor;
        talas::cdp::Spin cdpSpin;
        talas::cdp::Brassage cdpBrassage;
        CdpMultichannelPort cdpMultichannel;
        detail::hysteresis::State hysteresisControl;
        detail::hysteresis::State hysteresisAudio;
        detail::binaural_pan::State binauralPan;
        std::optional<talas::dsp::PreparedSerializedSpectrumFrameSynchronizer>
            frameSynchronizer;
        std::uint64_t frameSynchronizerNextAbsoluteFrame = 0;
        detail::oscillator_time_index::State oscillatorTimeIndex;
        double phase = 0.0;
        OscillatorKernel oscillatorKernel;
        bool oscillatorKernelPrepared = false;
        talas::dsp::DualMorphOscillator dualMorphOscillator;
        bool dualMorphOscillatorPrepared = false;
        std::size_t renderedFrames = 0;
        std::uint64_t cloudBankRenderedFrames = 0;
        detail::cloud_bank::GrainPool cloudBankGrains;
        std::shared_ptr<const detail::pasted_sound::Program> pastedSoundProgram;
        detail::pasted_sound::State pastedSoundState;
        std::vector<double> delayLine;
        std::size_t delayWriteIndex = 0;
        // The delay a DelayWithFeedback gliding with SmoothDelayChanges has
        // reached (ADR-0025).
        bool delayGlideStarted = false;
        double delayGlideFrames = 0.0;
        std::vector<double> firHistory;
        std::vector<double> reResonatorHistory;
        // An RE table's coefficients interpolated at the frame position
        // last read (ADR-0022).
        std::vector<double> reResonatorCoefficients;
        double reResonatorPosition = -1.0;
        std::vector<double> timeOffsetDelayLine;
        std::size_t timeOffsetWriteIndex = 0;
        detail::RunningExtremumState runningExtremumState;
        bool thresholdActive = false;
        double amplitudeFollowerValue = 0.0;
        talas::dsp::LowPassGate lowPassGate;
        bool lowPassGatePrepared = false;
        talas::dsp::MorphFilter morphFilter;
        bool morphFilterPrepared = false;
        talas::dsp::ProbabilityChaosGenerator probabilityChaosGenerator;
        bool probabilityChaosGeneratorPrepared = false;
        talas::dsp::QuadFunctionGenerator quadFunctionGenerator;
        bool quadFunctionGeneratorPrepared = false;
        talas::dsp::StereoDispersiveResonator stereoDispersiveResonator;
        bool stereoDispersiveResonatorPrepared = false;
        talas::dsp::PluckedWaveguide pluckedWaveguide;
        bool pluckedWaveguidePrepared = false;
        talas::dsp::DynamicBreakpointOscillator dynamicBreakpointOscillator;
        bool dynamicBreakpointOscillatorPrepared = false;
        talas::dsp::WaveTerrainOscillator waveTerrainOscillator;
        bool waveTerrainOscillatorPrepared = false;
        talas::dsp::ScannedMassOscillator scannedMassOscillator;
        bool scannedMassOscillatorPrepared = false;
        double peakDetectorValue = 0.0;
        double rmsSquaredValue = 0.0;
        std::vector<double> frequencyTrackerHistory;
        bool frequencyTrackerHasPreviousInput = false;
        double frequencyTrackerPreviousInput = 0.0;
        std::size_t frequencyTrackerHopPhase = 0;
        double frequencyTrackerEstimate = 0.0;
        bool frequencyTrackerHasEstimate = false;
        double frequencyTrackerAmplitude = 0.0;
        double dynamicRangeEnvelope = 0.0;
        std::vector<double> dynamicRangeDelayLine;
        std::size_t dynamicRangeDelayWriteIndex = 0;
        NoiseGeneratorState noise;
        FilterKernel filterKernel;
        bool filterKernelPrepared = false;
        std::vector<FilterStageState> filterStages;
        double filterFeedbackSample = 0.0;
        BiquadState shelvingFilterState;
        BiquadState presenceFilterState;
        BiquadState twoFormantState1;
        BiquadState twoFormantState2;
        double vibratoPhase = 0.0;
        BiquadState dualParallelTwoPoleState1;
        BiquadState dualParallelTwoPoleState2;
        double harmonicResonatorBrightnessLowPass = 0.0;
        double harmonicResonatorAllpassOutput = 0.0;
        std::vector<BiquadState> filterBankStates;
        std::vector<BiquadState> modalFilterStates;
        double modalFilterPreviousScale = 1.0;
        bool modalFilterInitialized = false;
        std::vector<BiquadState> graphicEqStates;
        std::array<FilterStageState, 4> vcfStages{};
        BiquadState analysisFilterBandPassState;
        BiquadState analysisFilterAllpassState;
        std::vector<double> analysisFilterHistory;
        AnalysisFilterWindowState analysisFilterWindow;
        std::vector<double> monotonizerTrackerHistory;
        MonotonizerTrackerState monotonizerTracker;
        BiquadState phaseShiftBy90State;
        BiquadState phaseShiftBy90RightState;
        BiquadState singleSideBandAllpassState;
        BiquadState singleSideBandRightAllpassState;
        double singleSideBandPhase = 0.0;
        std::size_t sosOscillatorsFramePosition = 0;
        std::vector<double> sosOscillatorsPhases;
        double quadOscillatorPhase = 0.0;
        std::vector<double> sumOfSinesPartialPhases;
        double sumOfSinesPlaybackCursor = 0.0;
        double sumOfSinesPlaybackCursorCompensation = 0.0;
        bool sumOfSinesPlaybackActive = false;
        bool sumOfSinesPlaybackStarted = false;
        bool sumOfSinesGateWasPositive = false;
        std::vector<double> gaOscillatorPartialPhases;
        std::vector<double> oscillatorBankSerializedAmplitudes;
        std::vector<double> oscillatorBankSerializedFrequencies;
        std::size_t oscillatorBankSerializedPhase = 0;
        std::vector<double> oscillatorBankPhases;
        std::size_t syntheticSpectrumFromArraySerializedPhase = 0;
        // The arrays as last updated, once every NbrPartials samples
        // (format 92), and when.
        std::vector<double> syntheticSpectrumFromArrayHeldAmplitudes;
        std::vector<double> syntheticSpectrumFromArrayHeldFrequencies;
        std::vector<double> syntheticSpectrumFromArrayHeldBandwidths;
        std::optional<std::uint64_t> syntheticSpectrumFromArrayUpdatedAt;
        std::vector<double> formantBankPartialPhases;
        std::vector<double> formantBankPartialPhaseCandidates;
        std::vector<double> formantBankWindowPositions;
        std::vector<double> formantBankWindowPositionCandidates;
        double formantBankPulsePhase = 1.0;
        std::size_t formantBankNextImpulse = 0;
        std::vector<double> formantBankSerializedPairs;
        std::size_t formantBankSerializedPhase = 0;
        std::vector<double> formantBankOscillatorPartialPhases;
        std::vector<double> formantBankOscillatorPartialPhaseCandidates;
        std::vector<double> formantBankOscillatorWindowPositions;
        std::vector<double> formantBankOscillatorWindowPositionCandidates;
        double formantBankOscillatorPulsePhase = 1.0;
        std::size_t formantBankOscillatorNextImpulse = 0;
        std::vector<std::uint8_t> spectrumModifierInitialized;
        std::vector<std::uint8_t> spectrumModifierMembership;
        std::vector<std::uint8_t> spectrumModifierCandidateInitialized;
        std::vector<std::uint8_t> spectrumModifierCandidateMembership;
        std::vector<double> spectrumModifierPreviousFrequency;
        std::vector<double> spectrumModifierPreviousAmplitude;
        std::vector<double> spectrumModifierCandidateFrequency;
        std::vector<double> spectrumModifierCandidateAmplitude;
        std::uint64_t spectrumModifierProbabilityFrameOrdinal = 0;
        std::size_t spectrumInRamSerializedPhase = 0;
        double spectrumOnDiskCursor = 0.0;
        bool spectrumOnDiskTriggerWasPositive = false;
        bool spectrumVoicedUnvoicedInitialized = false;
        bool spectrumVoicedUnvoicedStableClassification = false;
        bool spectrumVoicedUnvoicedTimedInitialized = false;
        bool spectrumVoicedUnvoicedTimedStable = false;
        std::size_t spectrumVoicedUnvoicedTimedTransitionRemaining = 0;
        talas::dsp::SpectralFrameTimestamp
            spectrumVoicedUnvoicedTimedNextSample;
        bool spectrumVoicedUnvoicedTimedHasNextSample = false;
        std::vector<double> averagingLowPassHistory;
        std::size_t averagingLowPassWriteIndex = 0;
        double pulseGeneratorPhase = 0.0;
        double pulseTrainFrames = 0.0;
        // Frames of Rate-scaled time into the sequence.
        double analogSequencerPositionFrames = 0.0;
        // A stepped AnalogSequencer's stage (-1 before its first Step), the
        // Rate-scaled frames since it started, and whether Step was positive.
        std::int64_t analogSequencerStage = -1;
        double analogSequencerStageFrames = 0.0;
        bool analogSequencerStepWasPositive = false;
        double euSeqPhase = 0.0;
        std::size_t euSeqStep = 0;
        std::size_t euSeqNextDegree = 0;
        double euSeqFrequency = 0.0;
        bool euSeqInitialized = false;
        bool euSeqResetWasPositive = false;
        double sampleAndHoldValue = 0.0;
        bool sampleAndHoldHasValue = false;
        bool sampleAndHoldTriggerWasPositive = false;
        std::uint64_t rampElapsedFrames = 0;
        double rampActiveDurationFrames = 1.0;
        bool rampActive = false;
        bool rampTriggerWasPositive = false;
        double timeIndexPosition = 0.0;
        std::uint8_t timeIndexStage = 0;
        bool timeIndexGateWasPositive = false;
        std::optional<std::size_t> timeIndexSelectedIndex;
        std::optional<std::uint8_t> midiOutputControllerLastStatus;
        std::optional<std::uint8_t> midiOutputControllerLastControllerNumber;
        std::optional<std::uint8_t> midiOutputControllerLastValue;
        double midiOutputControllerFramesSinceSend = 0.0;
        bool midiOutputEventGateWasPositive = false;
        std::optional<std::uint8_t> midiOutputEventActiveNote;
        std::optional<std::uint8_t> midiOutputEventActiveChannelIndex;
        std::uint16_t midiOutputEventLastBend = 8192;
        MidiMasterClockState midiMasterClock;
        std::vector<double> singleCycleSpectrumFrame;
        std::vector<double> antialiasedOscillatorProfile;
        double antialiasedOscillatorPhase = 0.0;
        std::optional<double> soundToGlobalControllerLastValue;
        double timeStopperSample = 0.0;
        bool timeStopperHasSample = false;
        std::vector<double> setDurationDelayLine;
        std::size_t setDurationDelayWriteIndex = 0;
        std::vector<double> waitUntilInputHistory;
        std::size_t waitUntilReadIndex = 0;
        std::size_t waitUntilWriteIndex = 0;
        std::size_t waitUntilHistoryCount = 0;
        bool waitUntilResumed = false;
        // A TimeOffset or SetDuration that starts its Input late has
        // started it, was first reached at this frame, and read this delay
        // then when it reads its delay as it starts (ADR-0023).
        bool lateInputStarted = false;
        std::optional<std::uint64_t> lateInputReachedFrame;
        std::optional<std::uint64_t> lateInputLatchedDelay;
        // ADR-0019: a paced timing step's clock, in frames since the
        // activation schedule first reached it (or it first rendered), and
        // where its Duration began; a TimeStopper's Resume has let time run.
        long double pacedClockFrames = 0.0L;
        bool pacedClockRunning = false;
        std::optional<long double> pacedDurationOrigin;
        bool timeStopperResumed = false;
        std::vector<double> timeControlHistory;
        std::size_t timeControlHistoryReadIndex = 0;
        std::size_t timeControlHistoryWriteIndex = 0;
        std::size_t timeControlHistoryCount = 0;
        std::size_t timeControlHistoryStartIndex = 0;
        std::size_t timeControlInputFramesSeen = 0;
        double timeControlReadPosition = 0.0;
        double warpedTimePlaybackTime = 0.0;
        bool warpedTimeActive = false;
        bool warpedTimeTriggerWasPositive = false;
        std::vector<double> warpedIdealTimePoints0;
        std::vector<double> warpedIdealTimePoints1;
        std::vector<double> warpedCurrentTimePoints;
        std::vector<double> warpedMorphedTimePoints;
        double multislopeElapsedTime = 0.0;
        bool multislopeTriggered = false;
        bool multislopeGateWasPositive = false;
        LiveEnvelopeSegmentState multislopeLive;
        double multisegmentElapsedTime = 0.0;
        bool multisegmentTriggered = false;
        bool multisegmentGateWasPositive = false;
        bool multisegmentReleased = false;
        double multisegmentDrift = 0.0;
        LiveEnvelopeSegmentState multisegmentLive;
        double graphicalEnvelopeElapsedTime = 0.0;
        bool graphicalEnvelopeTriggered = false;
        bool graphicalEnvelopeGateWasPositive = false;
        // A MultisegmentEnvelope's or GraphicalEnvelope's fall to zero on a
        // negative Gate.
        NegativeGateDecayState envelopeNegativeGateDecay;
        double arElapsedTime = 0.0;
        bool arTriggered = false;
        double arCurrentValue = 0.0;
        bool arGateWasPositive = false;
        double arReleaseElapsedTime = 0.0;
        double arReleaseStartValue = 0.0;
        AdsrKernel adsrKernel;
        bool adsrKernelPrepared = false;
        ClockKernel clockKernel;
        bool clockKernelPrepared = false;
        ClockDividerKernel clockDividerKernel;
        bool clockDividerKernelPrepared = false;
        QuantizerKernel quantizerKernel;
        bool quantizerKernelPrepared = false;
        SchmittGateKernel gateKernel;
        bool gateKernelPrepared = false;
        LogicKernel logicKernel;
        bool logicKernelPrepared = false;
        SlewKernel slewKernel;
        bool slewKernelPrepared = false;
        double adsrElapsedTime = 0.0;
        bool adsrTriggered = false;
        double adsrCurrentValue = 0.0;
        bool adsrGateWasPositive = false;
        double adsrReleaseElapsedTime = 0.0;
        double adsrReleaseStartValue = 0.0;
        double functionGeneratorElapsedTime = 0.0;
        bool functionGeneratorTriggered = false;
        bool functionGeneratorTriggerWasPositive = false;
        double chopperElapsedTime = 0.0;
        std::size_t tableReadIndex = 0;
        double tableReadLastSample = 0.0;
        bool tableReadGateWasPositive = false;
        std::size_t audioInputReadPosition = 0;
        std::size_t genericSourceReadPosition = 0;
        bool diskPlayerTriggerWasPositive = false;
        bool diskPlayerHasLastFilePosition = false;
        double diskPlayerLastFilePosition = 0.0;
        std::size_t diskPlayerLifetimeFrames = 0;
        bool diskRecorderCaptureStarted = false;
        bool diskRecorderCaptureActive = false;
        bool diskRecorderTriggerWasPositive = false;
        std::size_t diskRecorderCapturedFrames = 0;
        bool memoryWriterCaptureActive = false;
        bool memoryWriterCyclicContinuationActive = false;
        bool memoryWriterTriggerWasPositive = false;
        double memoryWriterCyclicControlPhase = 1.0;
        std::size_t memoryWriterCapturedFrames = 0;
        bool memoryWriterCaptureComplete = false;
        std::vector<double> memoryWriterCapturedSamples;
        double cloudDensityPhase = 0.0;
        std::int64_t cloudNextGrainIndex = 0;
        std::size_t cloudRetainedGrainLimit = 0;
        std::vector<GranularGrainStart> cloudGrainStarts;
        std::vector<GranularGrainLatch> cloudGrainLatches;
        double genericSourceSamplePlaybackPosition = 0.0;
        bool genericSourceSamplePlaybackActive = false;
        bool genericSourceSamplePlaybackStarted = false;
        bool genericSourceSampleTriggerWasPositive = false;
        std::size_t genericSourceSamplePlaybackFramesElapsed = 0;
        double samplePlaybackPosition = 0.0;
        std::uint64_t diskCachePlaybackFrame = 0;
        bool samplePlaybackActive = false;
        bool samplePlaybackStarted = false;
        bool sampleGateWasPositive = false;
        bool sampleTriggerWasPositive = false;
        bool sampleLoopFadeHasWrapped = false;
        std::size_t samplePlaybackFramesElapsed = 0;
        double multifileDiskPlaybackPosition = 0.0;
        bool multifileDiskPlaybackActive = false;
        bool multifileDiskPlaybackStarted = false;
        std::size_t multifileDiskSelectedIndex = 0;
        bool multifileDiskTriggerWasPositive = false;
        std::size_t samplesFromDiskSingleStepPlaybackPosition = 0;
        bool samplesFromDiskSingleStepPlaybackActive = false;
        bool samplesFromDiskSingleStepPlaybackStarted = false;
        bool samplesFromDiskGateWasPositive = false;
        double timeFrequencyScalePlaybackPosition = 0.0;
        bool timeFrequencyScalePlaybackActive = false;
        bool timeFrequencyScalePlaybackStarted = false;
        double multisamplePlaybackPosition = 0.0;
        bool multisamplePlaybackActive = false;
        bool multisamplePlaybackStarted = false;
        bool multisampleGateWasPositive = false;
        std::size_t multisampleSelectedIndex = 0;
        std::size_t multisamplePlaybackFramesElapsed = 0;
        double keyMappedMultisamplePlaybackPosition = 0.0;
        bool keyMappedMultisamplePlaybackActive = false;
        bool keyMappedMultisamplePlaybackStarted = false;
        bool keyMappedMultisampleGateWasPositive = false;
        std::size_t keyMappedMultisamplePlaybackFramesElapsed = 0;
        std::size_t keyMappedMultisampleSample = 0;
        SampleMapPlaybackState sampleMapPlaybackState;
        std::vector<SampleMapPlaybackSource> sampleMapPlaybackSources;
        std::vector<double> frequencyScaleDelayScratch;
        std::vector<talas::dsp::PartialSpectrumPoint>
            partialLegacyHoldPartials;
        std::size_t partialLegacyHoldCount = 0;
        std::vector<talas::dsp::PartialSpectrumPoint>
            spectrumTrackSelectorPartials;
        std::size_t spectrumTrackSelectorPartialCount = 0;
        talas::dsp::SpectralFrameTimestamp spectrumTrackSelectorNextSample;
        bool spectrumTrackSelectorHasNextSample = false;
        std::uint16_t soundVariableRenderedOrdinal = 0;
        std::uint16_t soundVariablePreviousOrdinal = 0;
        std::size_t soundVariableCrossfadeRemaining = 0;
        bool soundVariableSelectionInitialized = false;
    };

    struct NodeState : NodeValueState
    {
        std::unique_ptr<talas::dsp::PreparedSerializedSpectrumFrameAssembler>
            frameSynchronizerAssembler;
        std::unique_ptr<talas::dsp::PreparedDiffuseSpectralStretch>
            diffuseSpectralStretch;
        std::unique_ptr<talas::dsp::PreparedSpectrumFilterBank>
            preparedSpectrumFilterBank;
        std::unique_ptr<talas::dsp::PreparedLinearVocoderBank> linearVocoderBank;
        // The tuning a Vocoder's hot band fields last gave its bank.
        talas::dsp::VocoderTuning vocoderTuning;
        std::unique_ptr<talas::dsp::PreparedTimeFrequencyScale>
            timeFrequencyScalePitchShifter;
        std::unique_ptr<talas::dsp::PreparedForwardStft>
            nativeForwardStft;
        std::unique_ptr<talas::dsp::PreparedInverseStft>
            nativeInverseStft;
        std::unique_ptr<talas::dsp::PreparedPackedRealFftAdapter>
            packedRealFft;
        std::unique_ptr<talas::dsp::PreparedPhaseVocoderPitchShifter>
            simplePitchShifter;
        std::unique_ptr<talas::dsp::PreparedDynamicPhaseVocoderPitchShifter>
            dynamicSimplePitchShifter;
        std::unique_ptr<talas::dsp::PreparedDynamicPhaseVocoderPitchShifter>
            frequencyScalePitchShifter;
        std::unique_ptr<talas::dsp::FrequencyScaleGranulator>
            frequencyScaleGranulator;
        std::unique_ptr<talas::dsp::PreparedDynamicPhaseVocoderPitchShifter>
            monotonizerPitchShifter;
        std::unique_ptr<talas::dsp::PreparedLiveSpectralAnalysis>
            liveSpectralAnalysis;
        std::unique_ptr<talas::audio::PreparedPartialTrackResynthesizer>
            partialTrackPlayer;
        template <class Visitor> void visitOwnedProcessors(Visitor&& visit)
        {
            visit(frameSynchronizerAssembler,
                diffuseSpectralStretch,
                preparedSpectrumFilterBank,
                linearVocoderBank,
                timeFrequencyScalePitchShifter,
                nativeForwardStft,
                nativeInverseStft,
                packedRealFft,
                simplePitchShifter,
                dynamicSimplePitchShifter,
                frequencyScalePitchShifter,
                frequencyScaleGranulator,
                monotonizerPitchShifter,
                liveSpectralAnalysis,
                partialTrackPlayer);
        }
    };

    const PreparedRuntimeGraph* graph_;
    std::vector<CompileStep> runtimeSteps_;
    std::vector<OfflineLiveExpression> liveExpressions_;
    bool hasTimedExpressions_ = false;
    std::vector<NodeState> nodeStates_;
    // Each TimeOffset or SetDuration that starts its Input late: its delay,
    // and the frame at which it starts its Input when that frame is fixed
    // (ADR-0023).
    std::vector<std::optional<std::uint64_t>> lateInputDelayFrames_;
    std::vector<std::optional<std::uint64_t>> lateInputStartFrames_;
    // The frame each step's own Sound starts at, from which its functions of
    // time count.
    std::vector<std::uint64_t> soundStartFrames_;
    // The clock each step's !Time and !LocalTime read.
    std::vector<ControlledTimeClock> controlledTimeClocks_;
    // ADR-0019: the TimeControls and TimeStoppers whose clocks pace each
    // step, and whether any step is paced.
    std::vector<std::vector<std::size_t>> pacingClocks_;
    std::vector<std::size_t> pacingStoppers_;
    bool pacesTiming_ = false;
    // ADR-0025: the delay lines DelayWithFeedbacks share by Wavetable name,
    // whether each is kept over a restart, and the line each step shares.
    struct SharedDelayLine
    {
        std::vector<double> samples;
        bool started = false;
        std::uint64_t touchedFrame = 0;
        bool keptOverRestart = true;
    };
    std::vector<SharedDelayLine> sharedDelayLines_;
    std::vector<std::optional<std::size_t>> sharedDelayLineIndices_;
    std::vector<OfflineDecodedAudioResource>
        decodedAudioResources_;
    std::vector<FeedbackPairRuntimeState> feedbackPairStates_;
    std::vector<LatencyCompensationRuntimeState>
        latencyCompensationStates_;
    std::vector<std::unique_ptr<
        talas::dsp::PreparedComplexSpectrumSequence>>
        complexBuffers_;
    std::vector<std::unique_ptr<
        talas::dsp::PreparedPartialSpectrumSequence>>
        partialBuffers_;
    std::string feedbackPreparationError_;
    bool feedbackPreparationAllocationFailed_ = false;
    std::string reverbSectionPreparationError_;
    bool reverbSectionPreparationAllocationFailed_ = false;
    std::string timeOffsetPreparationError_;
    std::string timeOffsetPreparationNodeId_;
    std::string timeOffsetPreparationPortId_;
    std::string timeOffsetPreparationParameterId_;
    bool timeOffsetPreparationAllocationFailed_ = false;
    std::string waitUntilPreparationError_;
    std::string waitUntilPreparationNodeId_;
    std::string waitUntilPreparationPortId_;
    std::string waitUntilPreparationParameterId_;
    bool waitUntilPreparationAllocationFailed_ = false;
    std::string timeControlPreparationError_;
    std::string timeControlPreparationNodeId_;
    bool timeControlPreparationAllocationFailed_ = false;
    std::string setDurationPreparationError_;
    std::string setDurationPreparationNodeId_;
    std::string setDurationPreparationPortId_;
    std::string setDurationPreparationParameterId_;
    bool setDurationPreparationAllocationFailed_ = false;
    std::string warpedTimeIndexPreparationError_;
    std::string warpedTimeIndexPreparationNodeId_;
    std::string warpedTimeIndexPreparationParameterId_;
    bool warpedTimeIndexPreparationAllocationFailed_ = false;
    std::string amplitudeFollowerPreparationError_;
    std::string lowPassGatePreparationError_;
    std::string morphFilterPreparationError_;
    std::string dualMorphOscillatorPreparationError_;
    std::string annotationPreparationError_;
    std::string thresholdPreparationError_;
    std::string logicPreparationError_;
    std::string quantizerPreparationError_;
    std::string gatePreparationError_;
    std::string slewPreparationError_;
    std::string clockPreparationError_;
    std::string clockDividerPreparationError_;
    std::string filterPreparationError_;
    std::string shelvingPresenceFilterPreparationError_;
    std::string checkedBiquadFamilyPreparationError_;
    std::string staticFirFamilyPreparationError_;
    std::string harmonicResonatorPreparationError_;
    std::string dynamicRangeControllerPreparationError_;
    std::string frequencyTrackerPreparationError_;
    std::string interpolatePreparationError_;
    std::string scaleAndOffsetPreparationError_;
    std::string probabilityChaosGeneratorPreparationError_;
    std::string quadFunctionGeneratorPreparationError_;
    std::string stereoDispersiveResonatorPreparationError_;
    std::string pluckedWaveguidePreparationError_;
    std::string dynamicBreakpointOscillatorPreparationError_;
    std::string waveTerrainOscillatorPreparationError_;
    std::string scannedMassOscillatorPreparationError_;
    std::string diffuseSpectralStretchPreparationError_;
    std::string peakDetectorPreparationError_;
    std::string rmsSquaredPreparationError_;
    std::string runningMaxPreparationError_;
    std::string runningMinPreparationError_;
    std::string triggeredSampleAndHoldPreparationError_;
    std::string triggeredTableReadPreparationError_;
    std::string functionGeneratorPreparationError_;
    std::string chopperPreparationError_;
    std::string analogSequencerPreparationError_;
    std::string multislopeFunctionGeneratorPreparationError_;
    std::string multisegmentEnvelopePreparationError_;
    std::string graphicalEnvelopePreparationError_;
    std::string arPreparationError_;
    std::string analysisFilterPreparationError_;
    std::string phaseShiftBy90PreparationError_;
    std::string singleSideBandRmPreparationError_;
    std::string sosOscillatorsPreparationError_;
    std::string adsrPreparationError_;
    std::string oscillatorPreparationError_;
    std::string lfoPreparationError_;
    std::string pulseTrainPreparationError_;
    std::string rampPreparationError_;
    std::string sampleAndHoldPreparationError_;
    std::string latencyCompensationPreparationError_;
    bool latencyCompensationPreparationAllocationFailed_ = false;
    std::string audioResourcePreparationError_;
    std::string packedFftPreparationError_;
    std::string packedFftPreparationNodeId_;
    std::string packedFftPreparationParameterId_;
    std::string nativeStftPreparationError_;
    std::string partialSpectrumPreparationError_;
    std::string spectrumModifierStatePreparationError_;
    std::string liveExpressionPreparationError_;
    std::string processorPlacementPreparationError_;
    std::string diskRecorderCapturePreparationError_;
    LogicalProcessorId logicalProcessorCursor_ = hostLogicalProcessor;
    std::uint64_t logicalProcessorTransitionCount_ = 0;
    std::optional<std::vector<std::uint8_t>> midiOutputEventInBytesLastBytes_;
    OfflineMidiFileEchoCache midiFileEchoCache_;
    std::size_t renderedFrames_ = 0;
    LiveMemoryWriterRecordings liveMemoryWriterRecordings_;
};
}
