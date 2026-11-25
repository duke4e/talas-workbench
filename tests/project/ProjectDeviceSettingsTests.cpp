#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/VirtualControlMidi.h"

#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void downgradeEmptyPresentationToV1(std::string& source)
{
    const auto presentation = source.find("  \"presentation\": {\n");
    const auto version = source.find(
        "    \"version\": 2,\n", presentation);
    const std::string controls =
        "    \"performanceControls\": [],\n";
    const auto controlBoundary = source.find(controls, presentation);
    const std::string meters = "    \"performanceMeters\": []\n";
    const auto meterBoundary = source.find(meters, presentation);
    require(
        presentation != std::string::npos
            && version != std::string::npos
            && controlBoundary != std::string::npos
            && meterBoundary != std::string::npos,
        "fixture should contain empty presentation-v2 authority");
    source.erase(meterBoundary, meters.size());
    source.replace(
        controlBoundary,
        controls.size(),
        "    \"performanceControls\": []\n");
    source.replace(
        version,
        std::string{"    \"version\": 2,\n"}.size(),
        "    \"version\": 1,\n");
}

void eraseFormatTwentyOneScore(std::string& source)
{
    const auto score = source.find("    \"score\": {");
    const auto eventValues = source.find("    \"eventValues\":", score);
    require(score != std::string::npos && eventValues != std::string::npos,
            "fixture should expose the format-21 Score boundary");
    source.erase(score, eventValues - score);
}

void eraseFormatTwentyTwoPartialTrackEdits(std::string& source)
{
    const std::string field = "  \"partialTrackEdits\": [],\n";
    const auto position = source.find(field);
    require(position != std::string::npos,
            "fixture should expose the format-22 partial-track boundary");
    source.erase(position, field.size());
}

void eraseFormatThirtyFourLiveSetLibrary(std::string& source)
{
    const std::string field =
        "  \"liveSetLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"liveSets\": [],\n"
        "    \"tools\": []\n"
        "  },\n";
    const auto position = source.find(field);
    require(position != std::string::npos,
            "fixture should expose the format-34 live-set boundary");
    source.erase(position, field.size());
}

void eraseFormatEighteenOscIngress(std::string& source)
{
    const std::string egressField = R"json(,
    "oscEgress": {
      "enabled": false,
      "port": 9001
    })json";
    const auto egressPosition = source.find(egressField);
    require(egressPosition != std::string::npos,
            "fixture should contain format-nineteen OSC feedback settings");
    source.erase(egressPosition, egressField.size());
    const std::string field = R"json(,
    "oscIngress": {
      "enabled": false,
      "port": 9000
    })json";
    const auto position = source.find(field);
    require(position != std::string::npos,
            "fixture should contain format-eighteen OSC ingress settings");
    source.erase(position, field.size());
}

void eraseCurrentExpressionLanguage(std::string& source)
{
    const std::string field =
        "  \"expressionLanguage\": \"variablesV2\",\n";
    const auto position = source.find(field);
    require(position != std::string::npos,
            "fixture should contain current expression authority");
    source.erase(position, field.size());
    const std::string definitions = "  \"expressionDefinitions\": [],\n";
    const auto definitionsPosition = source.find(definitions);
    require(definitionsPosition != std::string::npos,
            "fixture should contain current expression definitions");
    source.erase(definitionsPosition, definitions.size());
    const std::string variables =
        "  \"variableLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto variablesPosition = source.find(variables);
    require(variablesPosition != std::string::npos,
            "fixture should contain current variable authority");
    source.erase(variablesPosition, variables.size());
    const std::string soundVariables =
        "  \"soundVariableLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto soundVariablesPosition = source.find(soundVariables);
    require(soundVariablesPosition != std::string::npos,
            "fixture should contain current Sound-variable authority");
    source.erase(soundVariablesPosition, soundVariables.size());
    const std::string generators =
        "  \"generatorLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"programs\": []\n"
        "  },\n";
    const auto generatorsPosition = source.find(generators);
    require(generatorsPosition != std::string::npos,
            "fixture should contain current generator authority");
    source.erase(generatorsPosition, generators.size());
}

talas::project::ProjectDeviceSettings configuredSettings()
{
    talas::project::ProjectDeviceSettings settings;
    settings.configured = true;
    settings.audioInputDeviceName = "Input Device";
    settings.audioOutputDeviceName = "Output Device";
    settings.sampleRate = 48000.0;
    settings.bufferSize = 256;
    settings.inputChannels = {1, 3};
    settings.outputChannels = {1, 2};
    settings.midiInputDeviceIdentifiers = {"midi-a", "midi-b"};
    return settings;
}

void testVersionSixSettingsRoundTripAndLegacyMigration()
{
    talas::project::ProjectDocument document;
    document.deviceSettings = configuredSettings();
    const auto json = talas::project::serializeProjectDocument(document);
    require(json.find(R"json("version": 156)json") != std::string::npos,
            "configured device settings should serialize as the current version");
    const auto loaded = talas::project::parseProjectDocument(json);
    require(loaded.ok(), "configured device settings should round-trip");
    const auto& settings = loaded.document.deviceSettings;
    require(settings.configured
                && settings.audioInputDeviceName == "Input Device"
                && settings.audioOutputDeviceName == "Output Device"
                && settings.sampleRate == 48000.0
                && settings.bufferSize == 256
                && settings.inputChannels
                    == std::vector<std::int32_t>({1, 3})
                && settings.outputChannels
                    == std::vector<std::int32_t>({1, 2})
                && settings.midiInputDeviceIdentifiers
                    == std::vector<std::string>({"midi-a", "midi-b"}),
            "all device preferences should survive canonical JSON");

    const auto legacy = talas::project::parseProjectDocument(
        R"json({"format":"talas.project","version":4,"metadata":{"title":""},"assets":[],"timeline":{"eventValues":[]},"graph":{"nodes":[],"connections":[]}})json");
    require(legacy.ok() && legacy.document.formatVersion == talas::project::currentProjectFormatVersion
                && !legacy.document.deviceSettings.configured,
            "version four should migrate to unconfigured version-eleven settings");
}

void testInvalidVersionFiveSettingsAreRejected()
{
    const auto prefix =
        R"json({"format":"talas.project","version":5,"metadata":{"title":""},"deviceSettings":)json";
    const auto suffix =
        R"json(,"virtualControls":[],"assets":[],"timeline":{"eventValues":[]},"graph":{"nodes":[],"connections":[]}})json";
    const auto rejects = [&](std::string object) {
        return !talas::project::parseProjectDocument(prefix + object + suffix).ok();
    };
    require(rejects(R"json({"configured":true,"audioInputDevice":"","audioOutputDevice":"","sampleRate":0,"bufferSize":256,"inputChannels":[],"outputChannels":[1,2],"midiInputDevices":[]})json"),
            "configured sample rate must be positive");
    require(rejects(R"json({"configured":true,"audioInputDevice":"","audioOutputDevice":"","sampleRate":48000,"bufferSize":0,"inputChannels":[],"outputChannels":[1,2],"midiInputDevices":[]})json"),
            "configured buffer size must be positive");
    require(rejects(R"json({"configured":true,"audioInputDevice":"","audioOutputDevice":"","sampleRate":48000,"bufferSize":256,"inputChannels":[1,1],"outputChannels":[1,2],"midiInputDevices":[]})json"),
            "channel lists must be unique");
    require(rejects(R"json({"configured":true,"audioInputDevice":"","audioOutputDevice":"","sampleRate":48000,"bufferSize":256,"inputChannels":[],"outputChannels":[1,2],"midiInputDevices":[""]})json"),
            "MIDI identifiers must be non-empty");
}

void testEditorSettingsAreValidatedAndUndoable()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor({}, registry);
    const auto settings = configuredSettings();
    require(editor.setDeviceSettings(settings).ok(),
            "valid device settings should commit");
    require(editor.document().deviceSettings.configured
                && editor.canUndo(),
            "device settings should participate in project history");
    require(editor.setDeviceSettings(settings).ok(),
            "reapplying identical settings should be a no-op");
    require(editor.undo().ok()
                && !editor.document().deviceSettings.configured,
            "undo should restore the prior device preferences");
    require(editor.redo().ok()
                && editor.document().deviceSettings.bufferSize == 256,
            "redo should restore the complete device preferences");

    auto invalid = settings;
    invalid.sampleRate = std::numeric_limits<double>::infinity();
    const auto rejected = editor.setDeviceSettings(std::move(invalid));
    require(!rejected.ok()
                && rejected.error
                    == talas::project::EditError::InvalidDeviceSettings,
            "invalid device settings should fail transactionally");
    require(editor.document().deviceSettings.sampleRate == 48000.0,
            "failed device settings edits must not mutate the document");
}

void testFormatSeventeenTransportSyncAuthorityIsClosed()
{
    auto settings = configuredSettings();
    settings.transportSync.sourceMode =
        talas::project::TransportSyncSourceMode::MidiClockExactInput;
    settings.transportSync.sourceIdentifier = "midi-b";
    talas::project::ProjectDocument document;
    document.deviceSettings = settings;

    const auto canonical =
        talas::project::serializeProjectDocument(document);
    require(
        canonical.find(R"json("transportSync": {)json")
                != std::string::npos
            && canonical.find(R"json("sourceMode": "midiClockExactInput")json")
                != std::string::npos
            && canonical.find(R"json("sourceIdentifier": "midi-b")json")
                != std::string::npos,
        "format seventeen should write exact MIDI Clock authority");
    const auto reopened =
        talas::project::parseProjectDocument(canonical);
    require(
        reopened.ok()
            && reopened.document.deviceSettings.transportSync
                == settings.transportSync,
        "exact MIDI Clock authority should round-trip canonically");

    auto premature = canonical;
    const auto version = premature.find(R"json("version": 156)json");
    require(version != std::string::npos,
            "current MIDI Clock fixture should expose its version");
    premature.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  16)json");
    eraseFormatTwentyOneScore(premature);
    eraseFormatTwentyTwoPartialTrackEdits(premature);
    eraseFormatThirtyFourLiveSetLibrary(premature);
    eraseFormatEighteenOscIngress(premature);
    eraseCurrentExpressionLanguage(premature);
    downgradeEmptyPresentationToV1(premature);
    require(!talas::project::parseProjectDocument(premature).ok(),
            "format sixteen should reject premature transport sync authority");

    auto migratedClock = canonical;
    const auto migratedVersion = migratedClock.find(R"json("version": 156)json");
    require(migratedVersion != std::string::npos,
            "current exact Clock fixture should expose its migration boundaries");
    migratedClock.replace(
        migratedVersion,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  16)json");
    eraseFormatTwentyOneScore(migratedClock);
    eraseFormatTwentyTwoPartialTrackEdits(migratedClock);
    eraseFormatThirtyFourLiveSetLibrary(migratedClock);
    eraseFormatEighteenOscIngress(migratedClock);
    eraseCurrentExpressionLanguage(migratedClock);
    downgradeEmptyPresentationToV1(migratedClock);
    const auto migratedAuthority = migratedClock.find(R"json("transportSync")json");
    require(migratedAuthority != std::string::npos,
            "current exact Clock fixture should expose its authority field");
    migratedClock.replace(
        migratedAuthority,
        std::string_view{R"json("transportSync")json"}.size(),
        R"json("midiClock")json");
    const auto migratedMode = migratedClock.find(R"json("midiClockExactInput")json");
    require(migratedMode != std::string::npos,
            "current exact Clock fixture should expose its authority mode");
    migratedClock.replace(
        migratedMode,
        std::string_view{R"json("midiClockExactInput")json"}.size(),
        R"json("exactInput")json");
    const auto migrated = talas::project::parseProjectDocument(migratedClock);
    require(
        migrated.ok()
            && migrated.document.deviceSettings.transportSync.sourceMode
                == talas::project::TransportSyncSourceMode::MidiClockExactInput
            && migrated.document.deviceSettings.transportSync.sourceIdentifier
                == "midi-b",
        "format sixteen exact Clock authority should migrate without inventing MTC: "
            + (migrated.errors.empty()
                    ? std::string{"model mismatch"}
                    : migrated.errors.front().path + " "
                        + migrated.errors.front().message));

    auto unknown = canonical;
    const auto unknownMode = unknown.find(R"json("midiClockExactInput")json");
    require(unknownMode != std::string::npos,
            "current transport fixture should expose its closed mode");
    unknown.replace(
        unknownMode,
        std::string_view{R"json("midiClockExactInput")json"}.size(),
        R"json("unknown")json");
    require(!talas::project::parseProjectDocument(unknown).ok(),
            "format seventeen should reject unknown transport authorities");

    auto mtc = settings;
    mtc.transportSync.sourceMode =
        talas::project::TransportSyncSourceMode::MidiTimeCodeExactInput;
    mtc.transportSync.sourceIdentifier = "midi-a";
    document.deviceSettings = mtc;
    const auto mtcCanonical = talas::project::serializeProjectDocument(document);
    const auto mtcReopened = talas::project::parseProjectDocument(mtcCanonical);
    require(
        mtcCanonical.find(
            R"json("sourceMode": "midiTimeCodeExactInput")json")
                != std::string::npos
            && mtcReopened.ok()
            && mtcReopened.document.deviceSettings.transportSync
                == mtc.transportSync,
        "exact MTC authority should round-trip canonically");

    auto unconfiguredMtc = mtcCanonical;
    const auto configured = unconfiguredMtc.find(R"json("configured": true)json");
    require(configured != std::string::npos,
            "MTC fixture should expose configured device settings");
    unconfiguredMtc.replace(
        configured,
        std::string_view{R"json("configured": true)json"}.size(),
        R"json("configured": false)json");
    require(!talas::project::parseProjectDocument(unconfiguredMtc).ok(),
            "unconfigured device settings should reject exact MTC authority");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor({}, registry);
    auto missing = settings;
    missing.transportSync.sourceIdentifier = "not-enabled";
    require(!editor.setDeviceSettings(missing).ok(),
            "exact MIDI Clock authority should name one enabled input");
    auto contradictory = settings;
    contradictory.transportSync.sourceMode =
        talas::project::TransportSyncSourceMode::Internal;
    require(!editor.setDeviceSettings(contradictory).ok(),
            "internal MIDI Clock authority should reject a retained identifier");
    auto oversized = settings;
    oversized.midiInputDeviceIdentifiers.push_back(
        std::string(talas::project::maximumMidiSourceIdentifierBytes + 1, 'x'));
    oversized.transportSync.sourceIdentifier =
        oversized.midiInputDeviceIdentifiers.back();
    require(!editor.setDeviceSettings(oversized).ok(),
            "MIDI Clock authority should enforce the identifier bound");
}
}

int main()
{
    testVersionSixSettingsRoundTripAndLegacyMigration();
    testInvalidVersionFiveSettingsAreRejected();
    testEditorSettingsAreValidatedAndUndoable();
    testFormatSeventeenTransportSyncAuthorityIsClosed();
    return 0;
}
