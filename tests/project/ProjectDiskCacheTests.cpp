#include "talas/project/ProjectDiskCache.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSoundExpansion.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace
{
std::atomic<bool> failNextAllocationForTesting = false;
}

void* operator new(std::size_t size)
{
    if (failNextAllocationForTesting.exchange(
            false, std::memory_order_acq_rel))
    {
        throw std::bad_alloc{};
    }
    if (auto* allocation = std::malloc(size == 0 ? 1 : size))
        return allocation;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* allocation) noexcept
{
    std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept
{
    std::free(allocation);
}

void operator delete[](void* allocation) noexcept
{
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept
{
    std::free(allocation);
}

namespace
{
void require(bool condition, std::string_view message)
{
    if (!condition)
        throw std::runtime_error(std::string{message});
}

bool sameDocument(
    const talas::project::ProjectDocument& lhs,
    const talas::project::ProjectDocument& rhs)
{
    return talas::project::serializeProjectDocument(lhs)
        == talas::project::serializeProjectDocument(rhs);
}

std::string fingerprint(char value = 'a')
{
    return std::string(talas::project::diskCacheCaptureFingerprintBytes, value);
}

talas::project::ProjectAsset capturedAsset()
{
    talas::project::ProjectAsset asset;
    asset.id = "Audio/Cache";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "DiskCache/cache-g1.wav";
    asset.contentHash = fingerprint('b');
    asset.channelCount = 1;
    asset.sampleRate = 48000.0;
    asset.frameCount = 24000;
    asset.encoding = "pcm16-wav";
    asset.generation = 1;
    asset.provenance = talas::project::makeDiskCacheCaptureProvenance(
        fingerprint());
    return asset;
}

talas::project::ProjectDocument rootCaptureDocument()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Presentation only";
    document.timeline.loop = {true, 0.0, 0.5};
    document.graph.nodes = {
        {"osc",
         "talas.oscillator",
         {{"frequency", std::string{"220 hz"}},
          {"amplitude", std::string{"0.5"}}},
         {}},
        {"cache",
         "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Audio/Cache"}},
          {"record", {}, {}, true}},
         {}},
        {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    return document;
}

talas::project::ProjectDocument nestedCaptureDocument(
    bool exposedInstanceAuthority = false)
{
    talas::project::ProjectDocument document;
    document.timeline.loop = {true, 0.0, 0.25};
    talas::project::ProjectSoundDefinition definition;
    definition.definitionId = "cachedVoice";
    definition.displayName = "Cached Voice";
    definition.graph.nodes = {
        {"player",
         "talas.diskPlayer",
         {{"fileName", {}, {}, {}, std::string{"Audio/Source"}}},
         {}},
        {"cache",
         "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Audio/Cache"}},
          {"record", {}, {}, true}},
         {}},
    };
    definition.graph.connections = {
        {"player", "audioOut", "cache", "audioIn"},
    };
    definition.outputs = {
        {"audioOut", talas::core::PortType::Audio, "cache", "audioOut"},
    };
    if (exposedInstanceAuthority)
    {
        talas::project::ProjectSoundExposedParameter fileName;
        fileName.parameterId = "cacheFile";
        fileName.displayName = "Cache File";
        fileName.kind = talas::core::ParameterKind::Resource;
        fileName.defaultValue = {
            "cacheFile", {}, {}, {}, std::string{"Audio/Cache"}};
        fileName.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            "cache",
            "fileName",
            {},
            talas::expr::ValueDomain::Scalar,
        });
        talas::project::ProjectSoundExposedParameter record;
        record.parameterId = "cacheRecord";
        record.displayName = "Cache Record";
        record.kind = talas::core::ParameterKind::Boolean;
        record.defaultValue = {"cacheRecord", {}, {}, true};
        record.bindings.push_back({
            talas::project::ProjectSoundParameterTarget::NodeParameter,
            "cache",
            "record",
            {},
            talas::expr::ValueDomain::Scalar,
        });
        definition.exposedParameters = {
            std::move(fileName), std::move(record)};
    }
    document.soundDefinitions.push_back(definition);
    document.graph.nodes = {
        {"voiceA", {}, {}, std::string{"cachedVoice"}},
        {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"voiceA", "audioOut", "out", "audioIn"},
    };
    if (exposedInstanceAuthority)
    {
        document.graph.nodes.insert(
            document.graph.nodes.begin() + 1,
            {"voiceB", {}, {}, std::string{"cachedVoice"}});
        document.graph.nodes.push_back(
            {"outB", "talas.audioOutput", {}, {}});
        document.graph.connections.push_back(
            {"voiceB", "audioOut", "outB", "audioIn"});
    }
    auto source = capturedAsset();
    source.id = "Audio/Source";
    source.path = "Audio/source.wav";
    source.provenance = "repository-original";
    document.assets.push_back(std::move(source));
    return document;
}

void testProvenanceIsClosedAndCanonical()
{
    const auto hash = fingerprint();
    const auto provenance =
        talas::project::makeDiskCacheCaptureProvenance(hash);
    require(
        provenance
            == std::string{talas::project::diskCacheCaptureProvenancePrefix}
                + hash,
        "capture provenance should preserve one canonical fingerprint");
    require(
        talas::project::diskCacheCaptureFingerprint(provenance)
            == std::optional<std::string_view>{hash},
        "capture provenance should expose the exact canonical fingerprint");

    require(
        talas::project::makeDiskCacheCaptureProvenance({}).empty()
            && talas::project::makeDiskCacheCaptureProvenance(
                   fingerprint('A')).empty(),
        "empty and uppercase fingerprints must not acquire provenance authority");
    require(
        !talas::project::diskCacheCaptureFingerprint(
             talas::project::diskCacheCaptureProvenancePrefix).has_value()
            && !talas::project::diskCacheCaptureFingerprint(
                    provenance + "0").has_value()
            && !talas::project::diskCacheCaptureFingerprint(
                    "foreign:" + hash).has_value(),
        "prefix-only, trailing, and foreign provenance must fail closed");
}

void testCapturedAssetClassificationPreservesLegacyAuthority()
{
    auto asset = capturedAsset();
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::CapturedPlayback,
        "exact captured mono PCM16-WAV should acquire playback authority");

    asset.provenance.clear();
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::LegacyPassthrough,
        "pre-C11 Audio without capture provenance must remain legacy passthrough");
    asset.provenance = "repository-original";
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::LegacyPassthrough,
        "ordinary Audio provenance must not change historical DiskCache output");

    asset = capturedAsset();
    asset.provenance = std::string{
        talas::project::diskCacheCaptureProvenancePrefix} + fingerprint('A');
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset,
        "a reserved capture prefix with malformed evidence must not fall back silently");
    asset = capturedAsset();
    asset.provenance = "talas.diskCache.capture.v2:" + fingerprint();
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset,
        "unknown versions in the reserved DiskCache capture namespace must fail closed");

    asset = capturedAsset();
    asset.channelCount = 2;
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset,
        "captured playback authority is mono-only");
    asset = capturedAsset();
    asset.contentHash.clear();
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset,
        "captured playback requires a canonical physical-byte hash");
    asset = capturedAsset();
    asset.generation = 0;
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset,
        "generation zero is not captured project authority");
    asset = capturedAsset();
    asset.generation =
        talas::project::maximumProjectAssetJsonInteger + 1U;
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset,
        "generation above the exact persisted JSON authority must not acquire playback authority");
    asset = capturedAsset();
    asset.path = "Audio/hand-authored.wav";
    require(
        talas::project::diskCacheAssetAuthority(asset)
            == talas::project::DiskCacheAssetAuthority::InvalidCapturedAsset,
        "captured provenance outside the project-local DiskCache directory must not acquire playback authority");
}

void testCaptureGenerationAuthorityRejectsUnrelatedAndExhaustedAssets()
{
    auto document = rootCaptureDocument();
    const auto fresh = talas::project::nextDiskCacheCaptureAssetGeneration(
        document, "Audio/Cache");
    require(
        fresh.ok() && *fresh.generation == 1,
        "a new exact DiskCache asset should begin at generation one");

    auto unrelated = capturedAsset();
    unrelated.provenance = "repository-original";
    document.assets.push_back(unrelated);
    const auto foreign = talas::project::nextDiskCacheCaptureAssetGeneration(
        document, "Audio/Cache");
    require(
        foreign.error
            == talas::project::DiskCacheCaptureGenerationError::UnrelatedAsset,
        "capture must reject an existing identity outside captured authority before rendering");

    document.assets.front() = capturedAsset();
    document.assets.front().generation =
        talas::project::maximumProjectAssetJsonInteger - 1U;
    const auto finalGeneration =
        talas::project::nextDiskCacheCaptureAssetGeneration(
            document, "Audio/Cache");
    require(
        finalGeneration.ok()
            && *finalGeneration.generation
                == talas::project::maximumProjectAssetJsonInteger,
        "the last exactly representable generation should remain capturable");

    document.assets.front().generation =
        talas::project::maximumProjectAssetJsonInteger;
    const auto exhausted =
        talas::project::nextDiskCacheCaptureAssetGeneration(
            document, "Audio/Cache");
    require(
        exhausted.error
            == talas::project::DiskCacheCaptureGenerationError::Exhausted,
        "the first generation beyond persisted authority must fail before capture work");
}

void testCaptureRangeUsesPortableCheckedFrameAuthority()
{
    talas::project::TimelineLoopRegion loop;
    loop.enabled = true;
    loop.startSeconds = 0.25;
    loop.endSeconds = 0.5;
    auto range = talas::project::diskCacheCaptureRange(loop, 48000.0);
    require(
        range.ok() && range.range->startFrame == 12000
            && range.range->endFrame == 24000
            && range.range->frameCount() == 12000,
        "capture range should map the exact enabled Timeline loop");

    loop.enabled = false;
    range = talas::project::diskCacheCaptureRange(loop, 48000.0);
    require(
        range.error == talas::project::DiskCacheCaptureRangeError::Disabled,
        "disabled Timeline loop must not invent a capture duration");

    loop.enabled = true;
    require(
        talas::project::diskCacheCaptureRange(loop, 7999.0).error
                == talas::project::DiskCacheCaptureRangeError::InvalidSampleRate
            && talas::project::diskCacheCaptureRange(loop, 192001.0).error
                == talas::project::DiskCacheCaptureRangeError::InvalidSampleRate
            && talas::project::diskCacheCaptureRange(loop, 48000.5).error
                == talas::project::DiskCacheCaptureRangeError::InvalidSampleRate,
        "capture sample rate must be a supported exact integer");

    loop.startSeconds = 0.0;
    loop.endSeconds =
        static_cast<double>(talas::project::maximumDiskCacheCaptureFrames)
        / 48000.0;
    range = talas::project::diskCacheCaptureRange(loop, 48000.0);
    require(
        range.ok()
            && range.range->endFrame
                == talas::project::maximumDiskCacheCaptureFrames,
        "exact maximum capture horizon should be admitted");
    loop.endSeconds =
        static_cast<double>(talas::project::maximumDiskCacheCaptureFrames + 1U)
        / 48000.0;
    require(
        talas::project::diskCacheCaptureRange(loop, 48000.0).error
            == talas::project::DiskCacheCaptureRangeError::ExceedsFrameLimit,
        "first frame beyond the render horizon must fail before capture work");

    loop.startSeconds = 1.0;
    loop.endSeconds = 1.0;
    require(
        talas::project::diskCacheCaptureRange(loop, 48000.0).error
            == talas::project::DiskCacheCaptureRangeError::InvalidLoop,
        "empty loop must fail closed");
    loop.endSeconds = std::numeric_limits<double>::infinity();
    require(
        talas::project::diskCacheCaptureRange(loop, 48000.0).error
            == talas::project::DiskCacheCaptureRangeError::InvalidLoop,
        "non-finite loop must fail closed");
}

void testSourceFingerprintTracksExecutionAuthorityOnly()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = rootCaptureDocument();
    std::string error;
    const auto original = talas::project::projectDiskCacheSourceFingerprint(
        document,
        registry,
        "cache",
        48000,
        error);
    require(
        error.empty()
            && talas::project::isCanonicalDiskCacheCaptureFingerprint(original),
        "root DiskCache should produce one canonical source fingerprint");

    auto changed = document;
    changed.metadata.title = "Unrelated presentation";
    changed.presentation = {};
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            == original,
        "presentation-only changes must not stale captured audio");

    changed = document;
    changed.graph.nodes.insert(
        changed.graph.nodes.begin(),
        {"unused", "talas.oscillator", {{"frequency", std::string{"999 hz"}}}, {}});
    changed.assets.push_back(capturedAsset());
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            == original,
        "unreachable nodes and unrelated assets must not stale the capture closure");

    changed = document;
    changed.expressionDefinitions.definitions.push_back(
        {"unused", {}, talas::expr::ValueDomain::Scalar, "2"});
    changed.variableLibrary.variables.push_back(
        {"unusedValue",
         "Unused value",
         talas::expr::ValueDomain::Scalar,
         9.0,
         {},
         true});
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            == original,
        "unreferenced expression definitions and variables must not stale capture");

    changed = document;
    changed.soundDefinitions.push_back({
        .definitionId = "unusedCandidate",
        .displayName = "Unused candidate",
    });
    changed.soundVariableLibrary.variables.push_back({
        "unusedSound",
        "Unused Sound",
        {},
        talas::project::ProjectSoundVariableKind::Sound,
        {"unusedCandidate"},
        0,
        true,
    });
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            == original,
        "unreferenced Sound-valued variables must not stale the materialized capture closure");

    changed = document;
    changed.graph.nodes.front().parameters.front().expression = "330 hz";
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            != original,
        "upstream oscillator changes must invalidate the source fingerprint");

    changed = document;
    changed.timeline.loop.endSeconds = 0.75;
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            != original,
        "exact capture range must belong to source authority");
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            document, registry, "cache", 44100, error)
            != original,
        "sample rate must belong to source authority");

    changed = document;
    auto& cache = changed.graph.nodes[1];
    cache.parameters[0].resource = "Audio/Replacement";
    cache.parameters[1].boolean = false;
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            == original,
        "capture commit fields must not make the newly captured cache immediately stale");

    require(
        talas::project::projectDiskCacheSourceFingerprint(
            document, registry, "missing", 48000, error).empty()
            && !error.empty(),
        "missing target identity must fail closed with a diagnostic");

    auto variableDocument = rootCaptureDocument();
    variableDocument.graph.nodes.front().parameters[1].expression = "@gain";
    variableDocument.variableLibrary.variables.push_back(
        {"gain",
         "Gain",
         talas::expr::ValueDomain::Scalar,
         0.5,
         {},
         true});
    const auto variableFingerprint =
        talas::project::projectDiskCacheSourceFingerprint(
            variableDocument, registry, "cache", 48000, error);
    variableDocument.variableLibrary.variables.front().initialValue = 0.75;
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            variableDocument, registry, "cache", 48000, error)
            != variableFingerprint,
        "referenced variable initial state must belong to source authority");

    auto soundVariableDocument = rootCaptureDocument();
    soundVariableDocument.graph.nodes.front() = {
        "selector",
        "talas.variable",
        {{"variableId", {}, {}, {}, {}, std::string{"selected"}}},
        {},
    };
    soundVariableDocument.graph.connections.front().fromNode = "selector";
    for (const auto& [id, frequency] :
         std::array<std::pair<std::string_view, std::string_view>, 2>{
             {{"toneA", "220 hz"}, {"toneB", "440 hz"}}})
    {
        talas::project::ProjectSoundDefinition candidate;
        candidate.definitionId = id;
        candidate.displayName = id;
        candidate.graph.nodes.push_back({
            "osc",
            "talas.oscillator",
            {{"frequency", std::string{frequency}},
             {"amplitude", std::string{"0.5"}}},
            {},
        });
        candidate.outputs.push_back({
            "audioOut", talas::core::PortType::Audio,
            "osc", "audioOut"});
        soundVariableDocument.soundDefinitions.push_back(
            std::move(candidate));
    }
    talas::project::ProjectSoundVariableDeclaration selectedSound;
    selectedSound.variableId = "selected";
    selectedSound.displayName = "Selected";
    selectedSound.candidateDefinitionIds = {"toneA", "toneB"};
    soundVariableDocument.soundVariableLibrary.variables.push_back(
        std::move(selectedSound));
    const auto selectedSoundFingerprint =
        talas::project::projectDiskCacheSourceFingerprint(
            soundVariableDocument, registry, "cache", 48000, error);
    require(
        talas::project::isCanonicalDiskCacheCaptureFingerprint(
            selectedSoundFingerprint),
        "referenced Sound-variable selector should produce exact fingerprint authority: "
            + error);
    soundVariableDocument.soundVariableLibrary.variables.front()
        .initialSelectedOrdinal = 1;
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            soundVariableDocument, registry, "cache", 48000, error)
            != selectedSoundFingerprint,
        "referenced Sound-variable selection must belong to source authority");
}

void testSourceFingerprintClosesOverNestedAndReferencedAssets()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = nestedCaptureDocument();

    std::string error;
    const auto targetPath = std::string_view{"voiceA/cachedVoice::cache"};
    const auto original = talas::project::projectDiskCacheSourceFingerprint(
        document,
        registry,
        targetPath,
        48000,
        error);
    require(
        error.empty()
            && talas::project::isCanonicalDiskCacheCaptureFingerprint(original),
        "nested DiskCache should resolve by exact structural source path");

    auto changed = document;
    changed.assets.front().path = "Relocated/source.wav";
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, targetPath, 48000, error)
            == original,
        "asset relocation without byte-authority change must not stale capture");
    changed.assets.front().contentHash = fingerprint('d');
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, targetPath, 48000, error)
            != original,
        "referenced upstream asset bytes must belong to the source fingerprint");

    changed = document;
    changed.soundDefinitions.front().graph.nodes.front().parameters.push_back(
        {"duration", std::string{"0.1 s"}});
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, targetPath, 48000, error)
            != original,
        "nested definition execution changes must invalidate exact source authority");
}

void testSourceFingerprintClosesOverExactSampleMapAssets()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = rootCaptureDocument();
    document.graph.nodes.front() = {
        "mapPlayer",
        "talas.sampleMapPlayer",
        {{"sampleMapId", {}, {}, {}, {}, std::string{"usedMap"}},
         {"frequency", std::string{"440 hz"}},
         {"gate", std::string{"1"}},
         {"velocity", std::string{"1"}},
         {"scale", std::string{"1"}},
         {"attackTime", std::string{"0 s"}},
         {"releaseTime", std::string{"0 s"}}},
        {}};
    document.graph.connections.front().fromNode = "mapPlayer";

    talas::project::ProjectSampleMap used;
    used.mapId = "usedMap";
    used.displayName = "Used";
    used.regions.push_back({"usedRegion", "Audio/Used"});
    talas::project::ProjectSampleMap unrelated;
    unrelated.mapId = "unrelatedMap";
    unrelated.displayName = "Unrelated";
    unrelated.regions.push_back({"unrelatedRegion", "Audio/Unrelated"});
    document.sampleMapLibrary.maps = {used, unrelated};

    auto usedAsset = capturedAsset();
    usedAsset.id = "Audio/Used";
    usedAsset.path = "Audio/used.wav";
    usedAsset.provenance = "repository-original";
    auto unrelatedAsset = usedAsset;
    unrelatedAsset.id = "Audio/Unrelated";
    unrelatedAsset.path = "Audio/unrelated.wav";
    unrelatedAsset.contentHash = fingerprint('c');
    document.assets = {usedAsset, unrelatedAsset};

    std::string error;
    const auto original = talas::project::projectDiskCacheSourceFingerprint(
        document, registry, "cache", 48000, error);
    require(
        talas::project::isCanonicalDiskCacheCaptureFingerprint(original),
        "used Sample Map closure should produce exact fingerprint authority");

    auto changed = document;
    changed.assets.front().contentHash = fingerprint('d');
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            != original,
        "used Sample Map region asset bytes must stale exact source authority");

    changed = document;
    changed.sampleMapLibrary.maps.back().displayName = "Presentation only";
    changed.assets.back().contentHash = fingerprint('e');
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            changed, registry, "cache", 48000, error)
            == original,
        "unreferenced Sample Maps and assets must not stale exact source authority");
}

void testSourceFingerprintBindsUpstreamDiskCacheExecutionMode()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = rootCaptureDocument();
    document.graph.nodes.insert(
        document.graph.nodes.begin() + 1,
        {"upstreamCache",
         "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Audio/Source"}},
          {"record", {}, {}, false}},
         {}});
    document.graph.connections.front() = {
        "osc", "audioOut", "upstreamCache", "audioIn"};
    document.graph.connections.insert(
        document.graph.connections.begin() + 1,
        {"upstreamCache", "audioOut", "cache", "audioIn"});
    auto source = capturedAsset();
    source.id = "Audio/Source";
    source.path = "DiskCache/source-g1.wav";
    document.assets.push_back(source);

    std::string error;
    const auto captured =
        talas::project::projectDiskCacheSourceFingerprint(
            document, registry, "cache", 48000, error);
    require(
        talas::project::isCanonicalDiskCacheCaptureFingerprint(captured),
        "captured upstream DiskCache should produce exact fingerprint authority");
    document.assets.front().provenance = "repository-original";
    require(
        talas::project::projectDiskCacheSourceFingerprint(
            document, registry, "cache", 48000, error)
            != captured,
        "captured-playback versus legacy-passthrough upstream DiskCache authority must stale the target capture");
}

talas::project::DiskCacheCapturePublication capturePublication(
    const talas::project::ProjectEditor& editor,
    std::string sourcePath);

void testPreparedCaptureEditPreservesNestedOwnership()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto definitionDocument = nestedCaptureDocument();
    talas::project::ProjectEditor definitionEditor{
        definitionDocument, registry};
    talas::project::PreparedDiskCacheCaptureEdit prepared;
    require(
        definitionEditor.prepareDiskCacheCapture(
                            capturePublication(
                                definitionEditor,
                                "voiceA/cachedVoice::cache"),
                            prepared)
            .ok(),
        "single-use definition-owned DiskCache should prepare exactly");
    const auto& definitionCandidate = prepared.document();
    const auto& definitionCache =
        definitionCandidate.soundDefinitions.front().graph.nodes[1];
    require(
        std::ranges::find(
            definitionCache.parameters,
            std::string_view{"record"},
            &talas::project::ProjectParameterExpression::parameterId)
                ->boolean
            == std::optional<bool>{false},
        "definition-owned capture should author the owning definition node");

    auto exposedDocument = nestedCaptureDocument(true);
    const auto expanded = talas::project::expandProjectSounds(
        exposedDocument, registry);
    require(
        expanded.ok()
            && std::ranges::count_if(
                   expanded.manifest.nodes,
                   [](const auto& identity) {
                       return identity.definitionId == "cachedVoice"
                           && identity.localNodeId == "cache";
                   })
                == 2,
        "instance capture fixture must contain two exact cache occurrences");
    talas::project::ProjectEditor exposedEditor{exposedDocument, registry};
    prepared = {};
    require(
        exposedEditor.prepareDiskCacheCapture(
                         capturePublication(
                             exposedEditor,
                             "voiceA/cachedVoice::cache"),
                         prepared)
            .ok(),
        "multi-instance capture should use exact exposed-parameter authority");
    const auto& exposedCandidate = prepared.document();
    const auto voiceA = std::ranges::find(
        exposedCandidate.graph.nodes,
        std::string_view{"voiceA"},
        &talas::project::ProjectNode::instanceId);
    const auto voiceB = std::ranges::find(
        exposedCandidate.graph.nodes,
        std::string_view{"voiceB"},
        &talas::project::ProjectNode::instanceId);
    const auto cacheRecord = std::ranges::find(
        voiceA->parameters,
        std::string_view{"cacheRecord"},
        &talas::project::ProjectParameterExpression::parameterId);
    const auto cacheFile = std::ranges::find(
        voiceA->parameters,
        std::string_view{"cacheFile"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        cacheRecord != voiceA->parameters.end()
            && cacheRecord->boolean == std::optional<bool>{false},
        "instance capture must author the selected record override");
    require(
        cacheFile != voiceA->parameters.end()
            && cacheFile->resource
                == std::optional<std::string>{"Audio/Cache"},
        "instance capture must author the selected resource override");
    require(
        voiceB->parameters.empty(),
        "instance capture must not author the sibling Sound instance");
    const auto& retainedDefinitionCache =
        exposedCandidate.soundDefinitions.front().graph.nodes[1];
    require(
        std::ranges::find(
            retainedDefinitionCache.parameters,
            std::string_view{"record"},
            &talas::project::ProjectParameterExpression::parameterId)
                ->boolean
            == std::optional<bool>{true},
        "instance capture must not change sibling definition authority");

    auto unsupportedDocument = nestedCaptureDocument(true);
    unsupportedDocument.soundDefinitions.front().exposedParameters.clear();
    talas::project::ProjectEditor unsupportedEditor{
        unsupportedDocument, registry};
    prepared = {};
    require(
        unsupportedEditor.prepareDiskCacheCapture(
                             capturePublication(
                                 unsupportedEditor,
                                 "voiceA/cachedVoice::cache"),
                             prepared)
                .error
            == std::optional<talas::project::EditError>{
                talas::project::EditError::StaleDiskCacheCapture},
        "multiply-instantiated nested cache without exact bindings must be unavailable");
}

talas::project::DiskCacheCapturePublication capturePublication(
    const talas::project::ProjectEditor& editor,
    std::string sourcePath = "cache")
{
    std::string error;
    const auto fingerprint =
        talas::project::projectDiskCacheSourceFingerprint(
            editor.document(),
            talas::core::makeBuiltinNodeRegistry(),
            sourcePath,
            48000,
            error);
    require(error.empty(), "capture publication fixture fingerprint should succeed");
    auto asset = capturedAsset();
    asset.provenance =
        talas::project::makeDiskCacheCaptureProvenance(fingerprint);
    return {
        editor.documentAuthorityGeneration(),
        talas::project::serializeProjectDocument(editor.document()),
        std::move(sourcePath),
        std::move(asset),
    };
}

void testPreparedCaptureEditCommitsOneExactUndoStep()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto original = rootCaptureDocument();
    talas::project::ProjectEditor editor{original, registry};
    auto publication = capturePublication(editor);
    talas::project::PreparedDiskCacheCaptureEdit prepared;
    const auto preparedResult = editor.prepareDiskCacheCapture(
        std::move(publication),
        prepared);
    require(
        preparedResult.ok()
            && sameDocument(editor.document(), original)
            && !editor.canUndo()
            && prepared.changesDocument(),
        "capture preparation must build a candidate without mutating editor/history");
    require(
        editor.diskCacheCaptureStillCurrent(prepared),
        "fresh prepared capture must retain exact document authority");

    const auto& candidate = prepared.document();
    require(
        candidate.assets.size() == 1
            && talas::project::diskCacheAssetAuthority(candidate.assets.front())
                == talas::project::DiskCacheAssetAuthority::CapturedPlayback,
        "prepared candidate should contain one exact captured Audio asset");
    const auto cache = std::ranges::find(
        candidate.graph.nodes,
        std::string_view{"cache"},
        &talas::project::ProjectNode::instanceId);
    require(cache != candidate.graph.nodes.end(), "prepared candidate should retain cache node");
    const auto record = std::ranges::find(
        cache->parameters,
        std::string_view{"record"},
        &talas::project::ProjectParameterExpression::parameterId);
    const auto fileName = std::ranges::find(
        cache->parameters,
        std::string_view{"fileName"},
        &talas::project::ProjectParameterExpression::parameterId);
    require(
        record != cache->parameters.end()
            && record->boolean == std::optional<bool>{false}
            && fileName != cache->parameters.end()
            && fileName->resource
                == std::optional<std::string>{"Audio/Cache"},
        "prepared edit should bind captured asset and leave normal runtime in playback mode");

    require(
        editor.commitPreparedDiskCacheCapture(std::move(prepared))
            && editor.canUndo(),
        "current prepared capture should commit without another fallible edit phase");
    const auto committed = editor.document();
    require(
        editor.undo().ok() && sameDocument(editor.document(), original),
        "capture commit should undo as one exact document edit");
    require(
        editor.redo().ok() && sameDocument(editor.document(), committed),
        "capture commit should redo the exact asset and node parameters");
}

void testPreparedCaptureEditRejectsStaleAndConflictingAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto original = rootCaptureDocument();
    talas::project::ProjectEditor editor{original, registry};
    talas::project::PreparedDiskCacheCaptureEdit prepared;
    require(
        editor.prepareDiskCacheCapture(
                  capturePublication(editor),
                  prepared)
            .ok(),
        "stale fixture should prepare");
    auto loop = editor.document().timeline.loop;
    loop.endSeconds = 0.75;
    require(editor.setTimelineLoop(loop).ok(), "sibling edit should advance authority");
    const auto afterSibling = editor.document();
    require(
        !editor.diskCacheCaptureStillCurrent(prepared)
            && !editor.commitPreparedDiskCacheCapture(std::move(prepared))
            && sameDocument(editor.document(), afterSibling),
        "stale prepared capture must not overwrite a later document edit");

    original = rootCaptureDocument();
    auto collision = capturedAsset();
    collision.provenance = "repository-original";
    original.assets.push_back(collision);
    talas::project::ProjectEditor collisionEditor{original, registry};
    auto publication = capturePublication(collisionEditor);
    prepared = {};
    const auto collisionResult = collisionEditor.prepareDiskCacheCapture(
        std::move(publication),
        prepared);
    require(
        collisionResult.error
                == std::optional<talas::project::EditError>{
                    talas::project::EditError::InvalidDiskCacheCapture}
            && sameDocument(collisionEditor.document(), original)
            && !collisionEditor.canUndo(),
        "capture must not replace an unrelated existing asset identity");

    talas::project::ProjectEditor malformedEditor{rootCaptureDocument(), registry};
    publication = capturePublication(malformedEditor);
    publication.asset.provenance = "talas.diskCache.capture.v1:bad";
    prepared = {};
    require(
        malformedEditor.prepareDiskCacheCapture(
            std::move(publication),
            prepared).error
            == std::optional<talas::project::EditError>{
                talas::project::EditError::InvalidDiskCacheCapture},
        "malformed staged capture evidence must fail before candidate mutation");
}

void testPreparedCaptureEditClosesGenerationPathAndTransactionAuthority()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    auto document = rootCaptureDocument();
    auto firstGeneration = capturedAsset();
    std::string error;
    firstGeneration.provenance =
        talas::project::makeDiskCacheCaptureProvenance(
            talas::project::projectDiskCacheSourceFingerprint(
                document, registry, "cache", 48000, error));
    document.assets.push_back(firstGeneration);
    talas::project::ProjectEditor editor{document, registry};
    auto recapture = capturePublication(editor);
    recapture.asset.generation = 2;
    recapture.asset.path = "DiskCache/cache-g2.wav";
    talas::project::PreparedDiskCacheCaptureEdit prepared;
    require(
        editor.prepareDiskCacheCapture(std::move(recapture), prepared).ok()
            && prepared.document().assets.front().generation == 2
            && prepared.document().assets.front().path
                == "DiskCache/cache-g2.wav",
        "recapture must advance the exact captured generation and unique path");

    document = rootCaptureDocument();
    firstGeneration = capturedAsset();
    firstGeneration.generation =
        talas::project::maximumProjectAssetJsonInteger;
    firstGeneration.path = "DiskCache/cache-gmax.wav";
    firstGeneration.provenance =
        talas::project::makeDiskCacheCaptureProvenance(
            talas::project::projectDiskCacheSourceFingerprint(
                document, registry, "cache", 48000, error));
    document.assets.push_back(firstGeneration);
    talas::project::ProjectEditor exhausted{document, registry};
    auto exhaustedPublication = capturePublication(exhausted);
    exhaustedPublication.asset.generation =
        talas::project::maximumProjectAssetJsonInteger;
    exhaustedPublication.asset.path = "DiskCache/cache-next.wav";
    prepared = {};
    require(
        exhausted.prepareDiskCacheCapture(
                     std::move(exhaustedPublication), prepared)
                .error
            == std::optional<talas::project::EditError>{
                talas::project::EditError::InvalidDiskCacheCapture},
        "exhausted capture generation must fail before publication");

    document = rootCaptureDocument();
    auto pathOwner = capturedAsset();
    pathOwner.id = "Audio/Other";
    pathOwner.provenance = "repository-original";
    document.assets.push_back(pathOwner);
    talas::project::ProjectEditor pathConflict{document, registry};
    prepared = {};
    require(
        pathConflict.prepareDiskCacheCapture(
                        capturePublication(pathConflict), prepared)
                .error
            == std::optional<talas::project::EditError>{
                talas::project::EditError::InvalidDiskCacheCapture},
        "capture path owned by another asset must fail atomically");

    talas::project::ProjectEditor transactionEditor{
        rootCaptureDocument(), registry};
    const auto beforeTransaction =
        talas::project::serializeProjectDocument(transactionEditor.document());
    const auto transaction =
        transactionEditor.beginOwnedHistoryTransaction();
    prepared = {};
    require(
        transactionEditor.prepareDiskCacheCapture(
                             capturePublication(transactionEditor), prepared)
                .error
                == std::optional<talas::project::EditError>{
                    talas::project::EditError::StaleDiskCacheCapture}
            && talas::project::serializeProjectDocument(
                   transactionEditor.document())
                == beforeTransaction
            && !transactionEditor.canUndo(),
        "capture preparation must not join or mutate an owned transaction");
    transactionEditor.cancelOwnedHistoryTransaction(transaction);

    talas::project::ProjectEditor bytesEditor{
        rootCaptureDocument(), registry};
    auto mismatched = capturePublication(bytesEditor);
    mismatched.expectedCanonicalDocumentBytes.push_back(' ');
    prepared = {};
    require(
        bytesEditor.prepareDiskCacheCapture(
                       std::move(mismatched), prepared)
                .error
            == std::optional<talas::project::EditError>{
                talas::project::EditError::StaleDiskCacheCapture},
        "same generation with nonmatching canonical bytes must fail closed");
}

void testPreparedCaptureEditContainsAllocationFailureBeforeMutation()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor{rootCaptureDocument(), registry};
    auto publication = capturePublication(editor);
    const auto before =
        talas::project::serializeProjectDocument(editor.document());
    const auto generation = editor.documentAuthorityGeneration();
    talas::project::PreparedDiskCacheCaptureEdit prepared;
    failNextAllocationForTesting.store(true, std::memory_order_release);
    const auto result = editor.prepareDiskCacheCapture(
        std::move(publication), prepared);
    require(
        result.error
                == std::optional<talas::project::EditError>{
                    talas::project::EditError::AllocationFailed}
            && !prepared.changesDocument()
            && talas::project::serializeProjectDocument(editor.document())
                == before
            && editor.documentAuthorityGeneration() == generation
            && !editor.canUndo()
            && !editor.canRedo(),
        "allocation failure must leave document, history, and authority unchanged");
}
}

int main()
{
    testProvenanceIsClosedAndCanonical();
    testCapturedAssetClassificationPreservesLegacyAuthority();
    testCaptureGenerationAuthorityRejectsUnrelatedAndExhaustedAssets();
    testCaptureRangeUsesPortableCheckedFrameAuthority();
    testSourceFingerprintTracksExecutionAuthorityOnly();
    testSourceFingerprintClosesOverNestedAndReferencedAssets();
    testSourceFingerprintClosesOverExactSampleMapAssets();
    testSourceFingerprintBindsUpstreamDiskCacheExecutionMode();
    testPreparedCaptureEditCommitsOneExactUndoStep();
    testPreparedCaptureEditPreservesNestedOwnership();
    testPreparedCaptureEditRejectsStaleAndConflictingAuthority();
    testPreparedCaptureEditClosesGenerationPathAndTransactionAuthority();
    testPreparedCaptureEditContainsAllocationFailureBeforeMutation();
    return 0;
}
