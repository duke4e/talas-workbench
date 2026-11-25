#include "talas/project/StatePackageSchemas.h"

#include "talas/project/ProjectAssetInspection.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectSnapshots.h"

#include "JsonValue.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace talas::project
{
namespace
{
constexpr std::uint32_t supportedSchemaVersion = 1;
constexpr std::size_t maximumDiagnostics = 32;

struct ParsedEnvelope
{
    std::uint32_t schemaVersion = supportedSchemaVersion;
    std::vector<StateEnvelopeDiagnostic> errors;
};

void addError(
    ParsedEnvelope& result,
    std::string path,
    std::string message)
{
    if (result.errors.size() >= maximumDiagnostics)
        return;
    result.errors.push_back({
        std::move(message),
        std::move(path),
    });
}

[[nodiscard]] const json::Value* member(
    const json::Value& object,
    std::string_view key)
{
    return json::findObjectMember(object, key);
}

[[nodiscard]] const json::Value* requiredMember(
    ParsedEnvelope& result,
    const json::Value& root,
    std::string_view key)
{
    const auto* value = member(root, key);
    if (value == nullptr)
    {
        addError(
            result,
            "$." + std::string(key),
            "Missing required state-envelope field");
    }
    return value;
}

void validateRootMembers(
    ParsedEnvelope& result,
    const json::Value& root,
    std::initializer_list<std::string_view> allowed)
{
    std::unordered_set<std::string> observed;
    for (const auto& [key, ignored] : root.object())
    {
        (void) ignored;
        if (!observed.insert(key).second)
        {
            addError(
                result,
                "$." + key,
                "Duplicate state-envelope field");
        }
        if (std::ranges::find(allowed, key) == allowed.end())
        {
            addError(
                result,
                "$",
                "Unknown state-envelope field: " + key);
        }
    }
}

void validateFormat(
    ParsedEnvelope& result,
    const json::Value& root,
    std::string_view expected)
{
    const auto* value = requiredMember(result, root, "format");
    if (value == nullptr)
        return;
    if (!value->isString() || value->string() != expected)
    {
        addError(
            result,
            "$.format",
            "Unexpected state-envelope format");
    }
}

void validateVersion(
    ParsedEnvelope& result,
    const json::Value& root)
{
    const auto* value = requiredMember(result, root, "version");
    if (value == nullptr)
        return;
    if (!value->isNumber()
        || !std::isfinite(value->number())
        || std::floor(value->number()) != value->number()
        || value->number() < 0.0
        || value->number()
            > static_cast<double>(
                std::numeric_limits<std::uint32_t>::max()))
    {
        addError(
            result,
            "$.version",
            "Expected non-negative integer state-envelope version");
        return;
    }

    result.schemaVersion =
        static_cast<std::uint32_t>(value->number());
    if (result.schemaVersion != supportedSchemaVersion)
    {
        addError(
            result,
            "$.version",
            "Unsupported state-envelope version");
    }
}

void requireNull(
    ParsedEnvelope& result,
    const json::Value& root,
    std::string_view key)
{
    const auto* value = requiredMember(result, root, key);
    if (value != nullptr && !value->isNull())
    {
        addError(
            result,
            "$." + std::string(key),
            "State-envelope payload is reserved and must be null");
    }
}

void requireEmptyArray(
    ParsedEnvelope& result,
    const json::Value& root,
    std::string_view key)
{
    const auto* value = requiredMember(result, root, key);
    if (value != nullptr
        && (!value->isArray() || !value->array().empty()))
    {
        addError(
            result,
            "$." + std::string(key),
            "State-envelope collection is reserved and must be empty");
    }
}

[[nodiscard]] ParsedEnvelope parseEnvelopeRoot(
    std::string_view source,
    std::string_view expectedFormat,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> nullFields,
    std::initializer_list<std::string_view> emptyArrayFields)
{
    ParsedEnvelope result;
    json::ParseLimits limits;
    limits.maximumNestingDepth = json::defaultMaximumNestingDepth;
    const auto parsed = json::parse(source, limits);
    if (!parsed.ok())
    {
        for (const auto& error : parsed.errors)
        {
            addError(
                result,
                error.path.empty() ? "$" : error.path,
                "Invalid state-envelope JSON: " + error.message);
        }
        return result;
    }
    if (!parsed.value.isObject())
    {
        addError(result, "$", "Expected state-envelope object");
        return result;
    }

    validateRootMembers(result, parsed.value, allowed);
    validateFormat(result, parsed.value, expectedFormat);
    validateVersion(result, parsed.value);
    for (const auto key : nullFields)
        requireNull(result, parsed.value, key);
    for (const auto key : emptyArrayFields)
        requireEmptyArray(result, parsed.value, key);
    return result;
}

template <typename State>
[[nodiscard]] StateEnvelopeLoadResult<State> makeLoadResult(
    ParsedEnvelope parsed)
{
    StateEnvelopeLoadResult<State> result;
    if (parsed.errors.empty())
        result.state.schemaVersion = parsed.schemaVersion;
    result.errors = std::move(parsed.errors);
    return result;
}

void appendQuoted(std::string &output, std::string_view value) {
  json::appendEscapedString(output, value);
}

[[nodiscard]] bool containsControlCharacter(std::string_view value) noexcept {
  return std::ranges::any_of(value, [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

[[nodiscard]] bool isStableAutomationId(std::string_view value) noexcept {
  if (value.empty() || value.size() > maximumHostAutomationIdBytes) {
    return false;
  }
  const auto first = static_cast<unsigned char>(value.front());
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z'))) {
    return false;
  }
  return std::ranges::all_of(value, [](unsigned char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == ':' || character == '-';
  });
}

[[nodiscard]] bool isBoundedText(std::string_view value,
                                 std::size_t maximumBytes) noexcept {
  return !value.empty() && value.size() <= maximumBytes &&
         json::isValidUtf8(value) && !containsControlCharacter(value);
}

[[nodiscard]] bool isHexadecimal(unsigned char character) noexcept {
  return (character >= '0' && character <= '9') ||
         (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

[[nodiscard]] bool isLocalFileUri(std::string_view value) noexcept {
  constexpr std::string_view prefix = "file:///";
  if (!value.starts_with(prefix) || value.size() == prefix.size() ||
      value[prefix.size()] == '/') {
    return false;
  }
  for (std::size_t index = prefix.size(); index < value.size(); ++index) {
    const auto character = static_cast<unsigned char>(value[index]);
    if (character == '%') {
      if (index + 2 >= value.size() ||
          !isHexadecimal(static_cast<unsigned char>(value[index + 1])) ||
          !isHexadecimal(static_cast<unsigned char>(value[index + 2]))) {
        return false;
      }
      index += 2;
      continue;
    }
    const bool allowed =
        (character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' ||
        character == '.' || character == '_' || character == '~' ||
        character == '/' || character == ':' || character == '@' ||
        character == '!' || character == '$' || character == '&' ||
        character == '\'' || character == '(' || character == ')' ||
        character == '*' || character == '+' || character == ',' ||
        character == ';' || character == '=';
    if (!allowed)
      return false;
  }
  return true;
}

void appendDiagnostic(std::vector<StateEnvelopeDiagnostic> &errors,
                      std::string path, std::string message) {
  if (errors.size() >= maximumDiagnostics)
    return;
  errors.push_back({std::move(message), std::move(path)});
}

[[nodiscard]] std::vector<StateEnvelopeDiagnostic>
validateHostStateModel(const HostState &state) {
  std::vector<StateEnvelopeDiagnostic> errors;
  if (state.schemaVersion != currentHostStateSchemaVersion) {
    appendDiagnostic(errors, "$.version",
                     "Host state must use the current schema version");
  }
  if (state.automationReferences.size() > maximumHostAutomationReferenceCount) {
    appendDiagnostic(
        errors, "$.automationReferences",
        "Host automation reference count exceeds the supported limit");
    return errors;
  }

  std::unordered_map<std::string, const HostAutomationReference *>
      referencesById;
  std::unordered_set<std::string> activeTargets;
  for (std::size_t index = 0; index < state.automationReferences.size();
       ++index) {
    const auto &reference = state.automationReferences[index];
    const auto path = "$.automationReferences[" + std::to_string(index) + "]";
    if (reference.disposition != HostAutomationDisposition::Active &&
        reference.disposition != HostAutomationDisposition::Tombstone) {
      appendDiagnostic(errors, path + ".disposition",
                       "Expected known automation disposition");
    }
    if (!isStableAutomationId(reference.automationId)) {
      appendDiagnostic(errors, path + ".automationId",
                       "Expected bounded stable automation identifier");
    } else if (!referencesById.emplace(reference.automationId, &reference)
                    .second) {
      appendDiagnostic(errors, path + ".automationId",
                       "Duplicate stable automation identifier");
    }
    if (!isBoundedText(reference.targetIdentity,
                       maximumHostTargetIdentityBytes) ||
        !isCanonicalSnapshotTargetIdentityString(reference.targetIdentity)) {
      appendDiagnostic(errors, path + ".targetIdentity",
                       "Expected canonical structural target identity");
    } else if (reference.disposition == HostAutomationDisposition::Active &&
               !activeTargets.insert(reference.targetIdentity).second) {
      appendDiagnostic(errors, path + ".targetIdentity",
                       "Duplicate active structural target identity");
    }
    if (reference.disposition == HostAutomationDisposition::Active &&
        reference.replacementAutomationId.has_value()) {
      appendDiagnostic(errors, path + ".replacementAutomationId",
                       "Active automation references cannot redirect");
    }
    if (reference.replacementAutomationId.has_value() &&
        !isStableAutomationId(*reference.replacementAutomationId)) {
      appendDiagnostic(errors, path + ".replacementAutomationId",
                       "Expected bounded stable replacement identifier");
    }
  }

  for (std::size_t index = 0; index < state.automationReferences.size();
       ++index) {
    const auto &reference = state.automationReferences[index];
    if (!reference.replacementAutomationId.has_value())
      continue;
    const auto path = "$.automationReferences[" + std::to_string(index) +
                      "].replacementAutomationId";
    const auto replacement =
        referencesById.find(*reference.replacementAutomationId);
    if (*reference.replacementAutomationId == reference.automationId ||
        replacement == referencesById.end() ||
        replacement->second->disposition != HostAutomationDisposition::Active) {
      appendDiagnostic(
          errors, path,
          "Replacement must name a different active automation reference");
    }
  }

  if (!state.projectStorage.has_value()) {
    if (state.structuralDirty) {
      appendDiagnostic(
          errors, "$.structuralDirty",
          "Host state without a project cannot be structurally dirty");
    }
    return errors;
  }

  const auto &storage = *state.projectStorage;
  if (storage.mode != HostProjectStorageMode::Embedded &&
      storage.mode != HostProjectStorageMode::Linked) {
    appendDiagnostic(errors, "$.projectStorage.mode",
                     "Expected known project storage mode");
    return errors;
  }
  if (storage.mode == HostProjectStorageMode::Embedded) {
    if (storage.projectPayload.empty() ||
        storage.projectPayload.size() > maximumHostEmbeddedProjectBytes ||
        !json::isValidUtf8(storage.projectPayload)) {
      appendDiagnostic(errors, "$.projectStorage.projectPayload",
                       "Expected bounded UTF-8 embedded project snapshot");
    } else {
      const auto project = parseProjectDocument(storage.projectPayload);
      if (!project.ok()) {
        appendDiagnostic(errors, "$.projectStorage.projectPayload",
                         "Embedded project snapshot is invalid");
      } else if (serializeProjectDocument(project.document) !=
                 storage.projectPayload) {
        appendDiagnostic(
            errors, "$.projectStorage.projectPayload",
            "Embedded project snapshot must use canonical project bytes");
      }
    }
    if (!storage.externalProjectUri.empty() ||
        !storage.expectedContentHash.empty()) {
      appendDiagnostic(errors, "$.projectStorage",
                       "Embedded storage cannot carry linked-project fields");
    }
  } else {
    if (!storage.projectPayload.empty()) {
      appendDiagnostic(errors, "$.projectStorage.projectPayload",
                       "Linked storage cannot carry embedded project bytes");
    }
    if (!isBoundedText(storage.externalProjectUri,
                       maximumHostExternalProjectUriBytes)) {
      appendDiagnostic(errors, "$.projectStorage.externalProjectUri",
                       "Expected bounded linked-project URI");
    } else if (!isLocalFileUri(storage.externalProjectUri)) {
      appendDiagnostic(errors, "$.projectStorage.externalProjectUri",
                       "Expected canonical local file URI without authority");
    }
    if (!isCanonicalProjectAssetContentHash(storage.expectedContentHash)) {
      appendDiagnostic(errors, "$.projectStorage.expectedContentHash",
                       "Expected canonical linked-project SHA-256 digest");
    }
  }
  return errors;
}

[[nodiscard]] std::optional<std::uint32_t>
exactVersion(const json::Value &value) noexcept {
  if (!value.isNumber())
    return std::nullopt;
  const auto &token = value.numberToken();
  if (token.empty() || !std::ranges::all_of(token, [](unsigned char character) {
        return character >= '0' && character <= '9';
      })) {
    return std::nullopt;
  }
  std::uint32_t version = 0;
  const auto [end, error] =
      std::from_chars(token.data(), token.data() + token.size(), version);
  if (error != std::errc{} || end != token.data() + token.size())
    return std::nullopt;
  return version;
}

void validateObjectMembers(ParsedEnvelope &result, const json::Value &object,
                           std::string_view path,
                           std::initializer_list<std::string_view> allowed) {
  std::unordered_set<std::string> observed;
  for (const auto &[key, ignored] : object.object()) {
    (void)ignored;
    if (!observed.insert(key).second) {
      addError(result, std::string(path) + "." + key,
               "Duplicate host-state field");
    }
    if (std::ranges::find(allowed, key) == allowed.end()) {
      addError(result, std::string(path), "Unknown host-state field: " + key);
    }
  }
}

[[nodiscard]] const json::Value *requiredHostMember(ParsedEnvelope &result,
                                                    const json::Value &object,
                                                    std::string_view key,
                                                    std::string_view path) {
  const auto *value = member(object, key);
  if (value == nullptr) {
    addError(result, std::string(path) + "." + std::string(key),
             "Missing required host-state field");
  }
  return value;
}

[[nodiscard]] bool readHostString(ParsedEnvelope &result,
                                  const json::Value &object,
                                  std::string_view key, std::string_view path,
                                  std::string &destination) {
  const auto *value = requiredHostMember(result, object, key, path);
  if (value == nullptr)
    return false;
  if (!value->isString()) {
    addError(result, std::string(path) + "." + std::string(key),
             "Expected host-state string");
    return false;
  }
  destination = value->string();
  return true;
}

[[nodiscard]] bool readNullableHostString(ParsedEnvelope &result,
                                          const json::Value *value,
                                          std::string_view path,
                                          std::string &destination) {
  if (value == nullptr)
    return false;
  if (value->isNull()) {
    destination.clear();
    return true;
  }
  if (!value->isString()) {
    addError(result, std::string(path), "Expected host-state string or null");
    return false;
  }
  destination = value->string();
  return true;
}

[[nodiscard]] std::vector<StateEnvelopeDiagnostic>
validateAvailableTargetIdentities(std::span<const std::string> targets) {
  std::vector<StateEnvelopeDiagnostic> errors;
  if (targets.size() > maximumHostAutomationReferenceCount) {
    appendDiagnostic(errors, "$.availableTargetIdentities",
                     "Available target count exceeds the supported limit");
    return errors;
  }
  std::unordered_set<std::string> observed;
  for (std::size_t index = 0; index < targets.size(); ++index) {
    if (!isBoundedText(targets[index], maximumHostTargetIdentityBytes) ||
        !isCanonicalSnapshotTargetIdentityString(targets[index])) {
      appendDiagnostic(
          errors, "$.availableTargetIdentities[" + std::to_string(index) + "]",
          "Expected canonical structural target identity");
    } else if (!observed.insert(targets[index]).second) {
      appendDiagnostic(
          errors, "$.availableTargetIdentities[" + std::to_string(index) + "]",
          "Duplicate available structural target identity");
    }
  }
  return errors;
}
}

std::string serializePresetPackageState(
    const PresetPackageState& state)
{
    return R"json({
  "format": "talas.preset-package",
  "version": )json"
        + std::to_string(state.schemaVersion)
        + R"json(,
  "compatibilityFingerprint": null,
  "snapshotPayload": null,
  "assetIds": []
}
)json";
}

PresetPackageStateLoadResult parsePresetPackageState(
    std::string_view source)
{
    return makeLoadResult<PresetPackageState>(
        parseEnvelopeRoot(
            source,
            "talas.preset-package",
            {
                "format",
                "version",
                "compatibilityFingerprint",
                "snapshotPayload",
                "assetIds",
            },
            {
                "compatibilityFingerprint",
                "snapshotPayload",
            },
            {"assetIds"}));
}

std::string serializeDistributionPackageState(
    const DistributionPackageState& state)
{
    return R"json({
  "format": "talas.distribution-package",
  "version": )json"
        + std::to_string(state.schemaVersion)
        + R"json(,
  "projectPayload": null,
  "dependencies": [],
  "assets": []
}
)json";
}

DistributionPackageStateLoadResult
parseDistributionPackageState(std::string_view source)
{
    return makeLoadResult<DistributionPackageState>(
        parseEnvelopeRoot(
            source,
            "talas.distribution-package",
            {
                "format",
                "version",
                "projectPayload",
                "dependencies",
                "assets",
            },
            {"projectPayload"},
            {"dependencies", "assets"}));
}

HostStateSerializationResult serializeHostState(const HostState &state) {
  HostStateSerializationResult result;
  result.errors = validateHostStateModel(state);
  if (!result.errors.empty())
    return result;

  auto references = state.automationReferences;
  std::ranges::sort(references, {}, &HostAutomationReference::automationId);

  auto &output = result.source;
  output += "{\n  \"format\": \"talas.host-state\",\n";
  output +=
      "  \"version\": " + std::to_string(currentHostStateSchemaVersion) + ",\n";
  output += "  \"automationReferences\": [";
  if (!references.empty())
    output.push_back('\n');
  for (std::size_t index = 0; index < references.size(); ++index) {
    const auto &reference = references[index];
    output += "    {\n      \"automationId\": ";
    appendQuoted(output, reference.automationId);
    output += ",\n      \"targetIdentity\": ";
    appendQuoted(output, reference.targetIdentity);
    output += ",\n      \"disposition\": ";
    appendQuoted(output,
                 reference.disposition == HostAutomationDisposition::Active
                     ? "active"
                     : "tombstone");
    output += ",\n      \"replacementAutomationId\": ";
    if (reference.replacementAutomationId.has_value())
      appendQuoted(output, *reference.replacementAutomationId);
    else
      output += "null";
    output += "\n    }";
    if (index + 1 < references.size())
      output.push_back(',');
    output.push_back('\n');
  }
  if (references.empty())
    output += "],\n";
  else
    output += "  ],\n";
  output += "  \"projectStorage\": ";
  if (!state.projectStorage.has_value()) {
    output += "null";
  } else {
    const auto &storage = *state.projectStorage;
    output += "{\n    \"mode\": ";
    appendQuoted(output, storage.mode == HostProjectStorageMode::Embedded
                             ? "embedded"
                             : "linked");
    output += ",\n    \"projectPayload\": ";
    if (storage.mode == HostProjectStorageMode::Embedded)
      appendQuoted(output, storage.projectPayload);
    else
      output += "null";
    output += ",\n    \"externalProjectUri\": ";
    if (storage.mode == HostProjectStorageMode::Linked)
      appendQuoted(output, storage.externalProjectUri);
    else
      output += "null";
    output += ",\n    \"expectedContentHash\": ";
    if (storage.mode == HostProjectStorageMode::Linked)
      appendQuoted(output, storage.expectedContentHash);
    else
      output += "null";
    output += "\n  }";
  }
  output += ",\n  \"structuralDirty\": ";
  output += state.structuralDirty ? "true" : "false";
  output += "\n}\n";
  if (output.size() > maximumHostStateBytes) {
    appendDiagnostic(result.errors, "$",
                     "Serialized host state exceeds the supported byte limit");
    output.clear();
  }
  return result;
}

HostStateLoadResult parseHostState(std::string_view source) {
  HostStateLoadResult result;
  ParsedEnvelope parsed;
  const auto parsedJson = json::parse(
      source, {
                  .maximumInputBytes = maximumHostStateBytes,
                  .maximumNestingDepth = 16,
                  .maximumValues = maximumHostAutomationReferenceCount * 8 + 32,
                  .maximumDecodedStringBytes = maximumHostStateBytes,
                  .maximumContainerElements =
                      maximumHostAutomationReferenceCount * 5 + 32,
                  .requireValidUtf8 = true,
              });
  if (!parsedJson.ok()) {
    for (const auto &error : parsedJson.errors) {
      addError(parsed, error.path.empty() ? "$" : error.path,
               "Invalid host-state JSON: " + error.message);
    }
    result.errors = std::move(parsed.errors);
    return result;
  }
  if (!parsedJson.value.isObject()) {
    addError(parsed, "$", "Expected host-state object");
    result.errors = std::move(parsed.errors);
    return result;
  }

  const auto &root = parsedJson.value;
  validateFormat(parsed, root, "talas.host-state");
  const auto *versionValue = requiredHostMember(parsed, root, "version", "$");
  const auto version = versionValue == nullptr ? std::optional<std::uint32_t>{}
                                               : exactVersion(*versionValue);
  if (!version.has_value() ||
      (*version != 1 && *version != currentHostStateSchemaVersion)) {
    addError(parsed, "$.version",
             "Expected supported exact integer host-state version");
  }

  if (version == 1) {
    validateRootMembers(parsed, root,
                        {
                            "format",
                            "version",
                            "automationReferences",
                            "projectStorage",
                        });
    requireEmptyArray(parsed, root, "automationReferences");
    requireNull(parsed, root, "projectStorage");
    result.errors = std::move(parsed.errors);
    return result;
  }

  validateRootMembers(parsed, root,
                      {
                          "format",
                          "version",
                          "automationReferences",
                          "projectStorage",
                          "structuralDirty",
                      });

  HostState state;
  const auto *references =
      requiredHostMember(parsed, root, "automationReferences", "$");
  if (references != nullptr) {
    if (!references->isArray()) {
      addError(parsed, "$.automationReferences",
               "Expected host automation reference array");
    } else if (references->array().size() >
               maximumHostAutomationReferenceCount) {
      addError(parsed, "$.automationReferences",
               "Host automation reference count exceeds the supported limit");
    } else {
      for (std::size_t index = 0; index < references->array().size(); ++index) {
        const auto &value = references->array()[index];
        const auto path =
            "$.automationReferences[" + std::to_string(index) + "]";
        if (!value.isObject()) {
          addError(parsed, path, "Expected automation reference object");
          continue;
        }
        validateObjectMembers(parsed, value, path,
                              {
                                  "automationId",
                                  "targetIdentity",
                                  "disposition",
                                  "replacementAutomationId",
                              });
        HostAutomationReference reference;
        (void)readHostString(parsed, value, "automationId", path,
                             reference.automationId);
        (void)readHostString(parsed, value, "targetIdentity", path,
                             reference.targetIdentity);
        std::string disposition;
        if (readHostString(parsed, value, "disposition", path, disposition)) {
          if (disposition == "active") {
            reference.disposition = HostAutomationDisposition::Active;
          } else if (disposition == "tombstone") {
            reference.disposition = HostAutomationDisposition::Tombstone;
          } else {
            addError(parsed, path + ".disposition",
                     "Expected active or tombstone disposition");
          }
        }
        const auto *replacement =
            requiredHostMember(parsed, value, "replacementAutomationId", path);
        if (replacement != nullptr && !replacement->isNull()) {
          if (!replacement->isString()) {
            addError(parsed, path + ".replacementAutomationId",
                     "Expected host-state string or null");
          } else {
            reference.replacementAutomationId = replacement->string();
          }
        }
        state.automationReferences.push_back(std::move(reference));
      }
    }
  }

  const auto *storageValue =
      requiredHostMember(parsed, root, "projectStorage", "$");
  if (storageValue != nullptr && !storageValue->isNull()) {
    if (!storageValue->isObject()) {
      addError(parsed, "$.projectStorage",
               "Expected project storage object or null");
    } else {
      validateObjectMembers(parsed, *storageValue, "$.projectStorage",
                            {
                                "mode",
                                "projectPayload",
                                "externalProjectUri",
                                "expectedContentHash",
                            });
      HostProjectStorage storage;
      std::string mode;
      if (readHostString(parsed, *storageValue, "mode", "$.projectStorage",
                         mode)) {
        if (mode == "embedded")
          storage.mode = HostProjectStorageMode::Embedded;
        else if (mode == "linked")
          storage.mode = HostProjectStorageMode::Linked;
        else {
          addError(parsed, "$.projectStorage.mode",
                   "Expected embedded or linked project storage mode");
        }
      }
      const auto *projectPayload = requiredHostMember(
          parsed, *storageValue, "projectPayload", "$.projectStorage");
      const auto *externalProjectUri = requiredHostMember(
          parsed, *storageValue, "externalProjectUri", "$.projectStorage");
      const auto *expectedContentHash = requiredHostMember(
          parsed, *storageValue, "expectedContentHash", "$.projectStorage");
      (void)readNullableHostString(parsed, projectPayload,
                                   "$.projectStorage.projectPayload",
                                   storage.projectPayload);
      (void)readNullableHostString(parsed, externalProjectUri,
                                   "$.projectStorage.externalProjectUri",
                                   storage.externalProjectUri);
      (void)readNullableHostString(parsed, expectedContentHash,
                                   "$.projectStorage.expectedContentHash",
                                   storage.expectedContentHash);
      if (mode == "embedded") {
        if (projectPayload != nullptr && !projectPayload->isString()) {
          addError(parsed, "$.projectStorage.projectPayload",
                   "Embedded storage requires project bytes");
        }
        if (externalProjectUri != nullptr && !externalProjectUri->isNull()) {
          addError(parsed, "$.projectStorage.externalProjectUri",
                   "Embedded storage requires a null external URI");
        }
        if (expectedContentHash != nullptr && !expectedContentHash->isNull()) {
          addError(parsed, "$.projectStorage.expectedContentHash",
                   "Embedded storage requires a null linked digest");
        }
      } else if (mode == "linked") {
        if (projectPayload != nullptr && !projectPayload->isNull()) {
          addError(parsed, "$.projectStorage.projectPayload",
                   "Linked storage requires a null project payload");
        }
        if (externalProjectUri != nullptr && !externalProjectUri->isString()) {
          addError(parsed, "$.projectStorage.externalProjectUri",
                   "Linked storage requires an external URI");
        }
        if (expectedContentHash != nullptr &&
            !expectedContentHash->isString()) {
          addError(parsed, "$.projectStorage.expectedContentHash",
                   "Linked storage requires an expected digest");
        }
      }
      state.projectStorage = std::move(storage);
    }
  }

  const auto *dirty = requiredHostMember(parsed, root, "structuralDirty", "$");
  if (dirty != nullptr) {
    if (!dirty->isBoolean()) {
      addError(parsed, "$.structuralDirty",
               "Expected structural dirty boolean");
    } else {
      state.structuralDirty = dirty->boolean();
    }
  }

  if (parsed.errors.empty()) {
    const auto modelErrors = validateHostStateModel(state);
    parsed.errors.insert(parsed.errors.end(), modelErrors.begin(),
                         modelErrors.end());
  }
  if (parsed.errors.empty()) {
    std::ranges::sort(state.automationReferences, {},
                      &HostAutomationReference::automationId);
    result.state = std::move(state);
  }
  result.errors = std::move(parsed.errors);
  return result;
}

HostStateLoadResult reconcileHostAutomationReferences(
    const HostState &state,
    std::span<const std::string> availableTargetIdentities) {
  HostStateLoadResult result;
  result.errors = validateHostStateModel(state);
  const auto targetErrors =
      validateAvailableTargetIdentities(availableTargetIdentities);
  for (const auto &error : targetErrors)
    appendDiagnostic(result.errors, error.path, error.message);
  if (!result.errors.empty())
    return result;

  result.state = state;
  std::unordered_set<std::string> available(availableTargetIdentities.begin(),
                                            availableTargetIdentities.end());
  for (auto &reference : result.state.automationReferences) {
    if (reference.disposition == HostAutomationDisposition::Active &&
        !available.contains(reference.targetIdentity)) {
      reference.disposition = HostAutomationDisposition::Tombstone;
      reference.replacementAutomationId.reset();
    }
  }

  std::unordered_map<std::string, HostAutomationDisposition> dispositions;
  for (const auto &reference : result.state.automationReferences) {
    dispositions.emplace(reference.automationId, reference.disposition);
  }
  for (auto &reference : result.state.automationReferences) {
    if (!reference.replacementAutomationId.has_value())
      continue;
    const auto replacement =
        dispositions.find(*reference.replacementAutomationId);
    if (replacement == dispositions.end() ||
        replacement->second != HostAutomationDisposition::Active) {
      reference.replacementAutomationId.reset();
    }
  }
  std::ranges::sort(result.state.automationReferences, {},
                    &HostAutomationReference::automationId);
  return result;
}

HostAutomationResolution resolveHostAutomationReference(
    const HostState &state, std::string_view automationId,
    std::span<const std::string> availableTargetIdentities) {
  HostAutomationResolution result;
  result.errors = validateHostStateModel(state);
  const auto targetErrors =
      validateAvailableTargetIdentities(availableTargetIdentities);
  for (const auto &error : targetErrors)
    appendDiagnostic(result.errors, error.path, error.message);
  if (!isStableAutomationId(automationId)) {
    appendDiagnostic(result.errors, "$.automationId",
                     "Expected bounded stable automation identifier");
  }
  if (!result.errors.empty()) {
    result.kind = HostAutomationResolutionKind::InvalidState;
    return result;
  }
  result.automationId = std::string(automationId);

  const auto reference =
      std::ranges::find(state.automationReferences, automationId,
                        &HostAutomationReference::automationId);
  if (reference == state.automationReferences.end()) {
    result.kind = HostAutomationResolutionKind::MissingReference;
    return result;
  }

  const HostAutomationReference *resolved = &*reference;
  bool migrated = false;
  if (reference->disposition == HostAutomationDisposition::Tombstone) {
    if (!reference->replacementAutomationId.has_value()) {
      result.kind = HostAutomationResolutionKind::Tombstone;
      result.resolvedAutomationId = reference->automationId;
      result.targetIdentity = reference->targetIdentity;
      return result;
    }
    const auto replacement = std::ranges::find(
        state.automationReferences, *reference->replacementAutomationId,
        &HostAutomationReference::automationId);
    if (replacement == state.automationReferences.end() ||
        replacement->disposition != HostAutomationDisposition::Active) {
      result.kind = HostAutomationResolutionKind::InvalidState;
      return result;
    }
    resolved = &*replacement;
    migrated = true;
  }

  result.resolvedAutomationId = resolved->automationId;
  result.targetIdentity = resolved->targetIdentity;
  if (std::ranges::find(availableTargetIdentities, resolved->targetIdentity) ==
      availableTargetIdentities.end()) {
    result.kind = HostAutomationResolutionKind::MissingTarget;
    return result;
  }
  result.kind = migrated ? HostAutomationResolutionKind::Migrated
                         : HostAutomationResolutionKind::Resolved;
  return result;
}
} // namespace talas::project
