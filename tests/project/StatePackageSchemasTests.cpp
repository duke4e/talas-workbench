#include "talas/project/ProjectDocument.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/StatePackageSchemas.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Result>
void requireErrorAt(const Result &result, std::string_view path,
                    const std::string &message) {
  require(!result.ok(), message + " should fail closed");
  require(
      std::ranges::any_of(
          result.errors, [&](const auto &error) { return error.path == path; }),
      message + " should report " + std::string(path));
}

talas::project::HostAutomationReference
activeReference(std::string automationId, std::string targetIdentity) {
  return {
      std::move(automationId),
      std::move(targetIdentity),
      talas::project::HostAutomationDisposition::Active,
      std::nullopt,
  };
}

std::string nodeTargetIdentity(std::string nodeId, std::string parameterId) {
  talas::project::SnapshotTargetIdentity target;
  target.kind = talas::project::SnapshotTargetKind::NodeParameter;
  target.nodeId = std::move(nodeId);
  target.parameterId = std::move(parameterId);
  return talas::project::snapshotTargetIdentityString(target);
}

std::string canonicalSnapshotProject() {
  talas::project::ProjectDocument document;
  document.metadata.title = "Host snapshot";
  document.expressionDefinitions.definitions.push_back({
      "half",
      {{"value", talas::expr::ValueDomain::Scalar}},
      talas::expr::ValueDomain::Scalar,
      "value * 0.5",
  });
  talas::project::ProjectSnapshot snapshot;
  snapshot.snapshotId = "init";
  snapshot.displayName = "Initial";
  talas::project::ProjectSnapshotBank bank;
  bank.bankId = "project-main";
  bank.displayName = "Main";
  bank.snapshots = {snapshot};
  document.snapshotLibrary.banks = {bank};
  return talas::project::serializeProjectDocument(document);
}

void testEmptyStateEnvelopesAreCanonicalAndVersioned() {
  const std::string expectedPreset = R"json({
  "format": "talas.preset-package",
  "version": 1,
  "compatibilityFingerprint": null,
  "snapshotPayload": null,
  "assetIds": []
}
)json";
  const std::string expectedDistribution = R"json({
  "format": "talas.distribution-package",
  "version": 1,
  "projectPayload": null,
  "dependencies": [],
  "assets": []
}
)json";
  const std::string expectedHost = R"json({
  "format": "talas.host-state",
  "version": 2,
  "automationReferences": [],
  "projectStorage": null,
  "structuralDirty": false
}
)json";

  require(talas::project::serializePresetPackageState({}) == expectedPreset,
          "PresetPackageState should reserve one exact empty schema");
  require(talas::project::serializeDistributionPackageState({}) ==
              expectedDistribution,
          "DistributionPackageState should reserve one exact empty schema");
  const auto serializedHost = talas::project::serializeHostState({});
  require(serializedHost.ok() && serializedHost.source == expectedHost,
          "HostState should write one exact version-two empty schema");

  const auto preset = talas::project::parsePresetPackageState(expectedPreset);
  const auto distribution =
      talas::project::parseDistributionPackageState(expectedDistribution);
  const auto host = talas::project::parseHostState(expectedHost);
  require(preset.ok() && distribution.ok() && host.ok() &&
              preset.state.schemaVersion == 1 &&
              distribution.state.schemaVersion == 1 &&
              host.state.schemaVersion == 2,
          "canonical empty state envelopes should round-trip at their current "
          "versions");
}

void testLegacyEmptyHostStateMigratesToVersionTwo() {
  const auto loaded = talas::project::parseHostState(R"json({
  "format": "talas.host-state",
  "version": 1,
  "automationReferences": [],
  "projectStorage": null
}
)json");
  require(loaded.ok() &&
              loaded.state.schemaVersion ==
                  talas::project::currentHostStateSchemaVersion &&
              loaded.state.automationReferences.empty() &&
              !loaded.state.projectStorage.has_value() &&
              !loaded.state.structuralDirty,
          "the reserved version-one host envelope should migrate to empty "
          "version two");
  const auto serialized = talas::project::serializeHostState(loaded.state);
  require(serialized.ok() &&
              serialized.source.find(R"json("version": 2)json") !=
                  std::string::npos,
          "the host-state writer should emit only version two");
}

void testHostStateRoundTripsEmbeddedProjectSnapshotCanonically() {
  talas::project::HostState state;
  state.structuralDirty = true;
  state.automationReferences = {
      activeReference("host.filter.cutoff",
                      nodeTargetIdentity("filter", "cutoff")),
      activeReference("host.voice.gain",
                      nodeTargetIdentity("voice", "gain")),
  };
  state.projectStorage = talas::project::HostProjectStorage{
      talas::project::HostProjectStorageMode::Embedded,
      canonicalSnapshotProject(),
      {},
      {},
  };

  const auto serialized = talas::project::serializeHostState(state);
  require(serialized.ok(), "valid embedded host state should serialize");
  const auto loaded = talas::project::parseHostState(serialized.source);
  require(loaded.ok(), loaded.errors.empty()
                           ? "valid embedded host state should parse"
                           : "valid embedded host state should parse: " +
                                 loaded.errors.front().path + " " +
                                 loaded.errors.front().message);
  require(loaded.state == state, "embedded host state should preserve "
                                 "automation, storage, and dirty state");

  const auto embedded = talas::project::parseProjectDocument(
      loaded.state.projectStorage->projectPayload);
  require(
      embedded.ok() && embedded.document.snapshotLibrary.banks.size() == 1 &&
          embedded.document.snapshotLibrary.banks.front().snapshots.size() == 1 &&
          embedded.document.expressionDefinitions.definitions.size() == 1 &&
          embedded.document.expressionDefinitions.definitions.front().name ==
              "half",
      "embedded host project should preserve its populated snapshot and expression-definition libraries");
  const auto second = talas::project::serializeHostState(loaded.state);
  require(second.ok() && second.source == serialized.source,
          "host-state serialization should be byte-stable");
}

void testHostStateRoundTripsLinkedProjectPolicy() {
  talas::project::HostState state;
  state.projectStorage = talas::project::HostProjectStorage{
      talas::project::HostProjectStorageMode::Linked,
      {},
      "file:///Projects/Set%201.talas.json",
      std::string(64, 'a'),
  };
  const auto serialized = talas::project::serializeHostState(state);
  require(serialized.ok(), "valid linked host state should serialize");
  const auto loaded = talas::project::parseHostState(serialized.source);
  require(loaded.ok() && loaded.state == state,
          "linked host state should preserve its exact URI and digest policy");
}

void testStableAutomationResolutionIgnoresVectorOrderAndIndexReuse() {
  const auto wave = nodeTargetIdentity("osc", "wave");
  const auto cutoff = nodeTargetIdentity("filter", "cutoff");
  const auto resonance = nodeTargetIdentity("new", "resonance");
  talas::project::HostState state;
  state.automationReferences = {
      activeReference("host.cutoff", cutoff),
  };
  const std::vector<std::string> first{
      wave,
      cutoff,
  };
  const auto firstResolution =
      talas::project::resolveHostAutomationReference(state, "host.cutoff", first);
  require(firstResolution.ok() && firstResolution.targetIdentity == cutoff,
          "host automation should resolve the exact structural target");

  const std::vector<std::string> reorderedAndReused{
      resonance,
      cutoff,
  };
  const auto secondResolution = talas::project::resolveHostAutomationReference(
      state, "host.cutoff", reorderedAndReused);
  require(secondResolution.ok() &&
              secondResolution.targetIdentity == firstResolution.targetIdentity,
          "reorder and old-index reuse must not retarget host automation");
}

void testRemovalCreatesStickyTombstoneAndMigrationIsExplicit() {
  const auto cutoff = nodeTargetIdentity("filter", "cutoff");
  const auto frequency = nodeTargetIdentity("filter", "frequency");
  talas::project::HostState state;
  state.automationReferences = {
      activeReference("host.cutoff.v1", cutoff),
  };
  const std::vector<std::string> replacementOnly{
      frequency,
  };
  const auto removed =
      talas::project::reconcileHostAutomationReferences(state, replacementOnly);
  require(removed.ok() &&
              removed.state.automationReferences.front().disposition ==
                  talas::project::HostAutomationDisposition::Tombstone,
          "missing active target should become a tombstone");

  const std::vector<std::string> originalReturns{
      cutoff,
  };
  const auto stillRemoved = talas::project::reconcileHostAutomationReferences(
      removed.state, originalReturns);
  require(stillRemoved.ok() &&
              stillRemoved.state.automationReferences.front().disposition ==
                  talas::project::HostAutomationDisposition::Tombstone,
          "tombstone should not revive when an old target identity reappears");

  auto migratedState = removed.state;
  migratedState.automationReferences.push_back(
      activeReference("host.cutoff.v2", frequency));
  migratedState.automationReferences.front().replacementAutomationId =
      "host.cutoff.v2";
  const auto migrated = talas::project::resolveHostAutomationReference(
      migratedState, "host.cutoff.v1", replacementOnly);
  require(migrated.ok() &&
              migrated.kind ==
                  talas::project::HostAutomationResolutionKind::Migrated &&
              migrated.resolvedAutomationId == "host.cutoff.v2" &&
              migrated.targetIdentity == frequency,
          "only an explicit stable-ID migration should redirect a tombstone");
}

void testPresetEnvelopeRejectsUnsupportedBehavior() {
  requireErrorAt(
      talas::project::parsePresetPackageState(
          R"json({"format":"talas.preset-package","version":2,"compatibilityFingerprint":null,"snapshotPayload":null,"assetIds":[]})json"),
      "$.version", "future preset schema");
  requireErrorAt(
      talas::project::parsePresetPackageState(
          R"json({"format":"talas.preset-package","version":1,"compatibilityFingerprint":"sha256:abc","snapshotPayload":null,"assetIds":[]})json"),
      "$.compatibilityFingerprint", "unimplemented preset fingerprint");
  requireErrorAt(
      talas::project::parsePresetPackageState(
          R"json({"format":"talas.preset-package","version":1,"compatibilityFingerprint":null,"snapshotPayload":{"values":[]},"assetIds":[]})json"),
      "$.snapshotPayload", "unimplemented preset snapshot payload");
  requireErrorAt(
      talas::project::parsePresetPackageState(
          R"json({"format":"talas.preset-package","version":1,"compatibilityFingerprint":null,"snapshotPayload":null,"assetIds":["sample"]})json"),
      "$.assetIds", "unimplemented preset assets");
}

void testDistributionEnvelopeRejectsUnsupportedBehavior() {
  requireErrorAt(
      talas::project::parseDistributionPackageState(
          R"json({"format":"talas.distribution-package","version":1,"projectPayload":{},"dependencies":[],"assets":[]})json"),
      "$.projectPayload", "unimplemented distribution project payload");
  requireErrorAt(
      talas::project::parseDistributionPackageState(
          R"json({"format":"talas.distribution-package","version":1,"projectPayload":null,"dependencies":[{}],"assets":[]})json"),
      "$.dependencies", "unimplemented distribution dependencies");
  requireErrorAt(
      talas::project::parseDistributionPackageState(
          R"json({"format":"talas.distribution-package","version":1,"projectPayload":null,"dependencies":[],"assets":[{}]})json"),
      "$.assets", "unimplemented distribution assets");
}

void testHostEnvelopeRejectsMalformedOrAmbiguousState() {
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":3,"automationReferences":[],"projectStorage":null,"structuralDirty":false})json"),
      "$.version", "future host schema");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2.00000000000000001,"automationReferences":[],"projectStorage":null,"structuralDirty":false})json"),
      "$.version", "fractional host schema token");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[{"automationId":"host.a","targetIdentity":"d0:/n1:a/p1:x","disposition":"active","replacementAutomationId":null},{"automationId":"host.a","targetIdentity":"d0:/n1:b/p1:x","disposition":"active","replacementAutomationId":null}],"projectStorage":null,"structuralDirty":false})json"),
      "$.automationReferences[1].automationId", "duplicate automation id");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[{"automationId":"host.old","targetIdentity":"d0:/n3:old/p1:x","disposition":"tombstone","replacementAutomationId":"host.missing"}],"projectStorage":null,"structuralDirty":false})json"),
      "$.automationReferences[0].replacementAutomationId",
      "dangling migration");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[{"automationId":"host.old","targetIdentity":"d0:/n3:old/p1:x","disposition":"tombstone","replacementAutomationId":""}],"projectStorage":null,"structuralDirty":false})json"),
      "$.automationReferences[0].replacementAutomationId",
      "empty migration id");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[],"projectStorage":{"mode":"linked","projectPayload":null,"externalProjectUri":"file:///project.talas.json","expectedContentHash":"abc"},"structuralDirty":false})json"),
      "$.projectStorage.expectedContentHash", "noncanonical linked digest");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[],"projectStorage":{"mode":"linked","projectPayload":null,"externalProjectUri":"https://example.com/project.talas.json","expectedContentHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"structuralDirty":false})json"),
      "$.projectStorage.externalProjectUri", "nonlocal linked-project URI");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[],"projectStorage":{"mode":"linked","projectPayload":null,"externalProjectUri":"file://server/share/project.talas.json","expectedContentHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"structuralDirty":false})json"),
      "$.projectStorage.externalProjectUri", "remote file URI authority");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[],"projectStorage":{"mode":"linked","projectPayload":null,"externalProjectUri":"file://","expectedContentHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"structuralDirty":false})json"),
      "$.projectStorage.externalProjectUri", "empty file URI");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[],"projectStorage":{"mode":"linked","projectPayload":"","externalProjectUri":"file:///project.talas.json","expectedContentHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"structuralDirty":false})json"),
      "$.projectStorage.projectPayload",
      "linked storage carrying a string payload");
  const auto missingStorageField = talas::project::parseHostState(
      R"json({"format":"talas.host-state","version":2,"automationReferences":[],"projectStorage":{"mode":"linked","projectPayload":null,"externalProjectUri":"file:///project.talas.json"},"structuralDirty":false})json");
  requireErrorAt(missingStorageField,
                 "$.projectStorage.expectedContentHash",
                 "missing linked digest field");
  require(std::ranges::count_if(
              missingStorageField.errors, [](const auto &error) {
                return error.path ==
                       "$.projectStorage.expectedContentHash";
              }) == 1,
          "a missing storage field should consume one diagnostic slot");
  requireErrorAt(
      talas::project::parseHostState(
          R"json({"format":"talas.host-state","version":2,"automationReferences":[],"projectStorage":null,"structuralDirty":false,"plugin":true})json"),
      "$", "unknown host-state field");
  requireErrorAt(talas::project::parseHostState("not-json"), "$",
                 "malformed host-state bytes");

  talas::project::HostState invalidWriterState;
  invalidWriterState.structuralDirty = true;
  requireErrorAt(talas::project::serializeHostState(invalidWriterState),
                 "$.structuralDirty", "dirty state without project storage");
  invalidWriterState = {};
  invalidWriterState.automationReferences = {
      activeReference("host.invalid", nodeTargetIdentity("node", "value")),
  };
  invalidWriterState.automationReferences.front().disposition =
      static_cast<talas::project::HostAutomationDisposition>(99);
  requireErrorAt(talas::project::serializeHostState(invalidWriterState),
                 "$.automationReferences[0].disposition",
                 "unknown automation disposition");
}

void testHostStateBoundsAndCanonicalTargetAuthority() {
  talas::project::HostState state;
  state.automationReferences = {
      activeReference("host.invalid-target", "target-by-index-7"),
  };
  requireErrorAt(talas::project::serializeHostState(state),
                 "$.automationReferences[0].targetIdentity",
                 "noncanonical persisted target identity");

  state.automationReferences = {
      activeReference("host.valid", nodeTargetIdentity("node", "value")),
  };
  const std::vector<std::string> invalidAvailable{"target-by-index-7"};
  requireErrorAt(talas::project::reconcileHostAutomationReferences(
                     state, invalidAvailable),
                 "$.availableTargetIdentities[0]",
                 "noncanonical available target identity");

  const auto empty = talas::project::serializeHostState({});
  require(empty.ok(), "empty host state should provide a size fixture");
  std::string atLimit(
      talas::project::maximumHostStateBytes - empty.source.size(), ' ');
  atLimit += empty.source;
  require(talas::project::parseHostState(atLimit).ok(),
          "host-state parser should accept an exact-limit valid source");
  atLimit.insert(atLimit.begin(), ' ');
  requireErrorAt(talas::project::parseHostState(atLimit), "$",
                 "host-state source above the shared byte limit");

  talas::project::HostState oversizedCount;
  oversizedCount.automationReferences.resize(
      talas::project::maximumHostAutomationReferenceCount + 1);
  const auto oversizedCountResult =
      talas::project::serializeHostState(oversizedCount);
  requireErrorAt(oversizedCountResult, "$.automationReferences",
                 "oversized in-memory automation vector");
  require(oversizedCountResult.errors.size() == 1,
          "oversized vector should fail before traversing its elements");

  const std::vector<std::string> oneTarget{
      nodeTargetIdentity("node", "value"),
  };
  const auto oversizedQuery = talas::project::resolveHostAutomationReference(
      {}, std::string(talas::project::maximumHostAutomationIdBytes + 1, 'x'),
      oneTarget);
  requireErrorAt(oversizedQuery, "$.automationId",
                 "oversized automation query identifier");
  require(oversizedQuery.automationId.empty(),
          "oversized query should be rejected before copying");

  talas::project::HostState aggregateOverflow;
  aggregateOverflow.automationReferences.reserve(
      talas::project::maximumHostAutomationReferenceCount);
  for (std::size_t index = 0;
       index < talas::project::maximumHostAutomationReferenceCount;
       ++index) {
    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::NodeParameter;
    target.instancePath.assign(30, std::string(128, 'x'));
    auto uniqueInstance = std::string("instance_") + std::to_string(index);
    uniqueInstance.append(128 - uniqueInstance.size(), 'x');
    target.instancePath.front() = std::move(uniqueInstance);
    target.containingDefinitionId = "definition";
    target.nodeId = "node";
    target.parameterId = "value";
    aggregateOverflow.automationReferences.push_back(activeReference(
        "host.value." + std::to_string(index),
        talas::project::snapshotTargetIdentityString(target)));
  }
  requireErrorAt(talas::project::serializeHostState(aggregateOverflow), "$",
                 "writer output above the shared host-state byte limit");
}
} // namespace

int main() {
  testEmptyStateEnvelopesAreCanonicalAndVersioned();
  testLegacyEmptyHostStateMigratesToVersionTwo();
  testHostStateRoundTripsEmbeddedProjectSnapshotCanonically();
  testHostStateRoundTripsLinkedProjectPolicy();
  testStableAutomationResolutionIgnoresVectorOrderAndIndexReuse();
  testRemovalCreatesStickyTombstoneAndMigrationIsExplicit();
  testPresetEnvelopeRejectsUnsupportedBehavior();
  testDistributionEnvelopeRejectsUnsupportedBehavior();
  testHostEnvelopeRejectsMalformedOrAmbiguousState();
  testHostStateBoundsAndCanonicalTargetAuthority();
  return 0;
}
