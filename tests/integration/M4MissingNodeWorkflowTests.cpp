#include "talas/audio/ProjectPackager.h"
#include "talas/cli/Cli.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectAutosaveState.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectMissingNodes.h"
#include "talas/project/ProjectPackage.h"
#include "talas/project/ProjectRecoveryFile.h"
#include "talas/project/ProjectSnapshots.h"
#include "talas/project/ProjectValidation.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef TALAS_SOURCE_DIR
#error "TALAS_SOURCE_DIR must identify the repository root"
#endif

namespace
{
constexpr std::string_view missingNodeId = "missingColour";
constexpr std::string_view missingTypeId =
    "org.talas.example.spectral-colour";

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(
              std::filesystem::temp_directory_path()
              / ("talas-m4-missing-node-workflow-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

std::string readText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "file should be readable: " + path.string());
    return {
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{},
    };
}

struct CliResult
{
    int exitCode = -1;
    std::string output;
    std::string error;
};

CliResult runCli(std::vector<std::string> arguments)
{
    std::vector<std::string_view> views;
    views.reserve(arguments.size());
    for (const auto& argument : arguments)
        views.push_back(argument);
    std::ostringstream output;
    std::ostringstream error;
    const auto exitCode = talas::cli::runCli(views, output, error);
    return {exitCode, output.str(), error.str()};
}

talas::project::ProjectParameterExpression expressionParameter(
    std::string id,
    std::string expression)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.expression = std::move(expression);
    return parameter;
}

talas::project::ProjectDocument fixtureDocument()
{
    talas::project::ProjectDocument document;
    document.expressionLanguage =
        talas::expr::ExpressionLanguageVersion::LegacyV1;
    document.metadata.title = "M4 Missing Component Preservation";

    talas::project::ProjectNode oscillator;
    oscillator.instanceId = "oscillator";
    oscillator.typeId = "talas.oscillator";
    // Projects before format 84 play a wavetable whole.
    talas::project::ProjectParameterExpression tableLength;
    tableLength.parameterId = "tableLength";
    tableLength.choice = "legacyWholeTable";
    oscillator.parameters = {
        expressionParameter("frequency", "220 hz"),
        expressionParameter("amplitude", "0.2"),
        tableLength,
    };

    talas::project::ProjectNode missing;
    missing.instanceId = std::string{missingNodeId};
    missing.typeId = std::string{missingTypeId};
    talas::project::ProjectParameterExpression mode;
    mode.parameterId = "mode";
    mode.choice = "colour";
    talas::project::ProjectParameterExpression mix;
    mix.parameterId = "mix";
    mix.expression = "0.75";
    talas::project::ProjectParameterExpression enabled;
    enabled.parameterId = "enabled";
    enabled.boolean = true;
    talas::project::ProjectParameterExpression label;
    label.parameterId = "label";
    label.text = "Preserved tone";
    missing.parameters = {
        std::move(mode),
        std::move(mix),
        std::move(enabled),
        std::move(label),
    };

    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.4.2";
    component.stateSchemaVersion = 7;
    component.state = {0x00, 0xff, 0x41, 0x80, 0x10, 0x20, 0x7f};
    component.displayName = "Spectral Colour Archive";
    component.bundleId = "org.talas.example.spectral-colour";
    component.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        true,
        talas::core::defaultPortSignalContract(
            talas::core::PortType::Audio),
    });
    component.inputs.push_back({
        "modulation",
        talas::core::PortType::Control,
        false,
        talas::core::defaultPortSignalContract(
            talas::core::PortType::Control),
    });
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        talas::core::defaultPortSignalContract(
            talas::core::PortType::Audio),
    });
    component.outputs.push_back({
        "changed",
        talas::core::PortType::Event,
        false,
        talas::core::defaultPortSignalContract(
            talas::core::PortType::Event),
    });
    component.parameters = {
        {
            "mode",
            talas::core::ParameterKind::Choice,
            talas::expr::ValueDomain::Scalar,
            {"clean", "colour", "archive"},
        },
        {
            "mix",
            talas::core::ParameterKind::Expression,
            talas::expr::ValueDomain::Scalar,
            {},
        },
        {
            "enabled",
            talas::core::ParameterKind::Boolean,
            talas::expr::ValueDomain::Scalar,
            {},
        },
        {
            "label",
            talas::core::ParameterKind::Text,
            talas::expr::ValueDomain::Scalar,
            {},
        },
        {
            "impulse",
            talas::core::ParameterKind::Resource,
            talas::expr::ValueDomain::Scalar,
            {},
        },
    };
    component.recoveryHint =
        "Install bundle org.talas.example.spectral-colour 1.4.2 from an owner-approved local source";
    missing.preservedComponent = std::move(component);

    talas::project::ProjectNode output;
    output.instanceId = "output";
    output.typeId = "talas.audioOutput";
    document.graph.nodes = {
        std::move(oscillator),
        std::move(missing),
        std::move(output),
    };
    document.graph.connections = {
        {"oscillator", "audioOut", "missingColour", "audioIn"},
        {"missingColour", "audioOut", "output", "audioIn"},
    };

    talas::project::reconcileProjectPresentation(document);
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "oscillator")
        ->position = {80, 180};
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        missingNodeId)
        ->position = {370, 160};
    talas::project::findProjectNodePresentation(
        document,
        std::nullopt,
        "output")
        ->position = {680, 180};

    talas::project::SnapshotTargetIdentity target;
    target.kind = talas::project::SnapshotTargetKind::NodeParameter;
    target.nodeId = std::string{missingNodeId};
    target.parameterId = "mode";
    talas::project::SnapshotValue value;
    value.kind = talas::project::SnapshotValueKind::Choice;
    value.choice = "colour";
    talas::project::ProjectSnapshot snapshot;
    snapshot.snapshotId = "preserved-colour";
    snapshot.displayName = "Preserved Colour";
    snapshot.values.push_back({target, value});
    talas::project::ProjectSnapshotBank bank;
    bank.bankId = "missing-component";
    bank.displayName = "Missing Component";
    bank.smoothingSeconds = 0.05;
    bank.snapshots.push_back(std::move(snapshot));
    document.snapshotLibrary.banks.push_back(std::move(bank));
    return document;
}

const talas::project::ProjectNode& preservedNode(
    const talas::project::ProjectDocument& document)
{
    const auto found = std::ranges::find(
        document.graph.nodes,
        missingNodeId,
        &talas::project::ProjectNode::instanceId);
    require(
        found != document.graph.nodes.end(),
        "fixture should retain the preserved node identity");
    return *found;
}

void requireCanonicalFixture(
    const talas::project::ProjectDocument& document,
    std::string_view expectedBytes)
{
    const auto& node = preservedNode(document);
    require(
        document.formatVersion == talas::project::currentProjectFormatVersion
            && node.typeId == missingTypeId
            && node.preservedComponent.has_value(),
        "fixture should retain one format-13 preserved component");
    const auto& component = *node.preservedComponent;
    require(
        component.typeVersion == "1.4.2"
            && component.stateSchemaVersion == 7
            && component.state
                == std::vector<std::uint8_t>{
                    0x00, 0xff, 0x41, 0x80, 0x10, 0x20, 0x7f}
            && component.inputs.size() == 2
            && component.outputs.size() == 2
            && component.parameters.size() == 5,
        "fixture should retain exact opaque state and nontrivial frozen interface");
    require(
        document.graph.connections.size() == 2
            && document.snapshotLibrary.banks.size() == 1
            && document.snapshotLibrary.banks.front()
                   .snapshots.front().values.front().target.nodeId
                == missingNodeId,
        "fixture should retain topology and snapshot target identity");
    const auto* presentation =
        talas::project::findProjectNodePresentation(
            document,
            std::nullopt,
            missingNodeId);
    require(
        presentation != nullptr
            && presentation->position
                == talas::project::CanvasPosition{370, 160},
        "fixture should retain portable missing-node coordinates");
    const auto canonical = talas::project::serializeProjectDocument(document);
    require(
        canonical.find(R"json("version": 156)json")
                != std::string::npos
            && canonical != expectedBytes,
        "format-thirteen preserved fixture should migrate to canonical format-32 bytes");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && talas::project::serializeProjectDocument(reopened.document)
                == canonical,
        "migrated preserved fixture should remain byte-stable");
}

void testOriginalMissingNodeLifecycle()
{
    const auto repository = std::filesystem::path{TALAS_SOURCE_DIR};
    const auto example = repository
        / "examples/m4_missing_component_preservation.talas.json";
    auto expected = fixtureDocument();
    // The format-13 fixture caches in 16 bits, as it did (format 154).
    expected.timeline.cacheWordSize = 16;
    if (!std::filesystem::exists(example))
    {
        std::cerr << "CANONICAL_FIXTURE_BEGIN\n"
                  << talas::project::serializeProjectDocument(expected)
                  << "\nCANONICAL_FIXTURE_END\n";
        throw std::runtime_error(
            "original missing-node fixture is not checked in");
    }

    auto source = readText(example);
    if (source.ends_with('\n'))
        source.pop_back();
    const auto sourceDocument = talas::project::parseProjectDocument(source);
    const auto migratedSource = sourceDocument.ok()
        ? talas::project::serializeProjectDocument(sourceDocument.document)
        : std::string{};
    const auto expectedCanonical =
        talas::project::serializeProjectDocument(expected);
    require(
        sourceDocument.ok()
            && migratedSource == expectedCanonical,
        "checked-in missing-node fixture should migrate to its repository-original model");
    require(
        source.find("libraryPath") == std::string::npos
            && source.find("executablePath") == std::string::npos
            && source.find("dynamicLibrary") == std::string::npos,
        "fixture must contain data and recovery copy only, never executable bytes or paths");

    talas::project::ProjectDocument fresh;
    require(
        fresh.formatVersion == talas::project::currentProjectFormatVersion
            && std::ranges::none_of(
                fresh.graph.nodes,
                [](const auto& node) {
                    return node.preservedComponent.has_value();
                }),
        "New should begin at current format 35 without invented capsules");

    const auto opened = talas::project::loadProjectFile(example);
    require(opened.ok(), "Open should load the original missing-node example");
    requireCanonicalFixture(*opened.document, source);

    auto registry = talas::core::makeBuiltinNodeRegistry();
    const auto& openedNode = preservedNode(*opened.document);
    const auto missing =
        talas::project::classifyProjectComponentAvailability(
            openedNode.typeId,
            &*openedNode.preservedComponent,
            registry);
    require(
        missing.availability
                == talas::project::ProjectComponentAvailability::Missing
            && talas::project::validateProjectDocumentForEditing(
                   *opened.document,
                   registry)
                   .ok()
            && !talas::project::validateProjectComponentAvailabilityForExecution(
                    *opened.document,
                    registry)
                    .ok(),
        "fixture should be structurally editable but not executable without its descriptor");

    const auto validation = runCli({
        "validate",
        "--project",
        example.string(),
    });
    require(
        validation.exitCode == 1
            && validation.output
                == "Portable document preserved: yes\nExecutable in this registry: no\n"
            && validation.error.find("missingColour")
                != std::string::npos
            && validation.error.find(missingTypeId)
                != std::string::npos,
        "CLI validate should distinguish preserved portability from execution");

    TemporaryDirectory directory;
    const auto saved = directory.path() / "opened/Missing.talas.json";
    const auto savedResult =
        talas::project::saveProjectFile(saved, *opened.document);
    require(
        savedResult.ok() && savedResult.revision.has_value(),
        "Save should publish the canonical preserved document");
    const auto reopened = talas::project::loadProjectFile(saved);
    require(
        reopened.ok() && reopened.revision == savedResult.revision,
        "saved missing-node document should reopen at its exact revision");
    requireCanonicalFixture(*reopened.document, source);

    {
        std::ofstream external(saved, std::ios::binary | std::ios::app);
        external << "\n ";
    }
    const auto externalBytes = readText(saved);
    const auto conflict = talas::project::saveProjectFile(
        saved,
        *reopened.document,
        reopened.revision);
    require(
        !conflict.ok()
            && conflict.error->code
                == talas::project::ProjectFileErrorCode::ExternalConflict
            && readText(saved) == externalBytes,
        "checked Save should reject external conflict without rewriting preserved bytes");
    require(
        talas::project::saveProjectFile(saved, *reopened.document).ok(),
        "explicit conflict resolution should restore canonical bytes");

    const auto saveAs = directory.path() / "saved-as/Missing.talas.json";
    require(
        talas::project::saveProjectFile(saveAs, *reopened.document).ok(),
        "Save As should publish the preserved document without discovery");
    const auto saveAsOpened = talas::project::loadProjectFile(saveAs);
    require(saveAsOpened.ok(), "Save As should reopen");
    requireCanonicalFixture(*saveAsOpened.document, source);

    talas::project::ProjectAutosaveState autosave(10.0, 10.0, 20.0);
    autosave.documentChanged(true, 100.0);
    require(
        autosave.shouldStartWrite(true, false, 110.0),
        "dirty preserved state should reach its autosave deadline");
    autosave.writeStarted();
    talas::project::ProjectRecoveryRecord recovery;
    recovery.projectPath = saveAs;
    recovery.projectRevision = saveAsOpened.revision;
    recovery.cleanDocument = fresh;
    require(
        talas::project::appendProjectRecoveryVersion(
            recovery,
            *saveAsOpened.document,
            1000),
        "autosave should append the complete preserved document");
    const auto recoveryPath =
        directory.path() / "recovery/current.talas-recovery.json";
    const auto recoverySave =
        talas::project::saveProjectRecoveryFile(recoveryPath, recovery);
    autosave.writeFinished(recoverySave.ok(), true, 111.0);
    const auto recovered =
        talas::project::loadProjectRecoveryFile(recoveryPath);
    require(
        recoverySave.ok()
            && recovered.ok()
            && recovered.record->versions.size() == 1,
        "recovery should reopen one complete preserved version");
    requireCanonicalFixture(
        recovered.record->versions.front().recoveredDocument,
        source);

    talas::audio::ProjectPackageOptions packageOptions;
    const auto package = directory.path() / "portable/Missing.talaspkg";
    packageOptions.outputPackagePath = package;
    packageOptions.sourceProjectFilePath = saveAs;
    const auto packaged = talas::audio::saveProjectPackage(
        *saveAsOpened.document,
        packageOptions);
    require(
        packaged.ok()
            && packaged.assetCount == 0
            && packaged.copiedFileCount == 0,
        "package should preserve the capsule without adding a payload");
    std::vector<std::filesystem::path> packageFiles;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(package))
    {
        if (entry.is_regular_file())
            packageFiles.push_back(entry.path().lexically_relative(package));
    }
    std::ranges::sort(packageFiles);
    require(
        packageFiles
            == std::vector<std::filesystem::path>{
                talas::project::projectPackageManifestFileName,
                talas::project::projectPackageDocumentFileName,
            },
        "data-only package should contain only manifest and canonical project JSON");
    const auto resolvedPackage =
        talas::project::resolveProjectPackagePath(package);
    const auto packageOpened = resolvedPackage.ok()
        ? talas::project::loadProjectFile(
              resolvedPackage.projectFilePath)
        : talas::project::ProjectFileLoadResult{};
    require(
        resolvedPackage.ok() && packageOpened.ok(),
        "data-only package should resolve and reopen");
    requireCanonicalFixture(*packageOpened.document, source);

    const auto packageValidation = runCli({
        "validate",
        "--project",
        package.string(),
    });
    require(
        packageValidation.exitCode == 1
            && packageValidation.output == validation.output
            && packageValidation.error.find(missingTypeId)
                != std::string::npos,
        "packaged CLI validation should retain the same honest missing-component result");

    const auto rejectedWav = directory.path() / "missing-render.wav";
    {
        std::ofstream sentinel(
            rejectedWav,
            std::ios::binary | std::ios::trunc);
        sentinel << "preserve";
    }
    const auto rendered = runCli({
        "render",
        "--project",
        example.string(),
        "--frames",
        "64",
        "--sample-rate",
        "8192",
        "--output",
        rejectedWav.string(),
    });
    require(
        rendered.exitCode == 1
            && rendered.output.empty()
            && rendered.error.find(missingTypeId)
                != std::string::npos
            && readText(rejectedWav) == "preserve",
        "CLI render should fail before stdout or destination publication");

    talas::project::ProjectEditor repairEditor(
        *opened.document,
        registry);
    const auto beforeRepair =
        talas::project::serializeProjectDocument(
            repairEditor.document());
    require(
        !repairEditor.repairPreservedComponent(missingNodeId).ok(),
        "repair should fail while the trusted descriptor is absent");
    auto descriptor =
        talas::project::makeProjectPreservedComponentDescriptor(
            std::string{missingTypeId},
            *openedNode.preservedComponent);
    registry.addNodeType(std::move(descriptor));
    require(
        repairEditor.repairPreservedComponent(missingNodeId).ok()
            && !repairEditor.canUndo()
            && talas::project::serializeProjectDocument(
                   repairEditor.document()) == beforeRepair
            && talas::project::validateProjectComponentAvailabilityForExecution(
                   repairEditor.document(),
                   registry)
                   .ok()
            && talas::project::snapshotLibraryDiagnostics(
                   repairEditor.document(),
                   registry)
                   .empty(),
        "exact trusted repair should reuse identity, topology, state, presentation, and snapshot target without history");
}
}

int main()
{
    try
    {
        testOriginalMissingNodeLifecycle();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
