#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/ProjectMusicalTime.h"

#include <cmath>
#include <stdexcept>
#include <string>
#include <limits>

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

void downgradeCurrentTransportSyncToFormatSixteen(std::string& source)
{
    const auto version = source.find("\"version\": 156");
    require(version != std::string::npos,
            "fixture should expose the current transport boundary");
    source.replace(version, std::string{"\"version\": 156"}.size(),
                   "\"version\": 16");
    const std::string expressionLanguage =
        "  \"expressionLanguage\": \"variablesV2\",\n";
    const auto expressionLanguagePosition =
        source.find(expressionLanguage);
    require(expressionLanguagePosition != std::string::npos,
            "fixture should expose current expression authority");
    source.erase(
        expressionLanguagePosition,
        expressionLanguage.size());
    const std::string definitions = "  \"expressionDefinitions\": [],\n";
    const auto definitionsPosition = source.find(definitions);
    require(definitionsPosition != std::string::npos,
            "fixture should expose current expression definitions");
    source.erase(definitionsPosition, definitions.size());
    const std::string variables =
        "  \"variableLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto variablesPosition = source.find(variables);
    require(variablesPosition != std::string::npos,
            "fixture should expose current variable authority");
    source.erase(variablesPosition, variables.size());
    const std::string soundVariables =
        "  \"soundVariableLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"variables\": []\n"
        "  },\n";
    const auto soundVariablesPosition = source.find(soundVariables);
    require(soundVariablesPosition != std::string::npos,
            "fixture should expose current Sound-variable authority");
    source.erase(soundVariablesPosition, soundVariables.size());
    const std::string generators =
        "  \"generatorLibrary\": {\n"
        "    \"version\": 2,\n"
        "    \"programs\": []\n"
        "  },\n";
    const auto generatorsPosition = source.find(generators);
    require(generatorsPosition != std::string::npos,
            "fixture should expose current generator authority");
    source.erase(generatorsPosition, generators.size());
    const std::string liveSets =
        "  \"liveSetLibrary\": {\n"
        "    \"version\": 1,\n"
        "    \"liveSets\": [],\n"
        "    \"tools\": []\n"
        "  },\n";
    const auto liveSetsPosition = source.find(liveSets);
    require(liveSetsPosition != std::string::npos,
            "fixture should expose current live-set authority");
    source.erase(liveSetsPosition, liveSets.size());
    const auto partialEdits = source.find("  \"partialTrackEdits\": [],\n");
    if (partialEdits != std::string::npos)
        source.erase(partialEdits, std::string{"  \"partialTrackEdits\": [],\n"}.size());
    const auto score = source.find("    \"score\": {");
    const auto eventValues = source.find("    \"eventValues\":", score);
    require(score != std::string::npos && eventValues != std::string::npos,
            "fixture should expose the format-21 Score boundary");
    source.erase(score, eventValues - score);
    const auto authority = source.find("\"transportSync\": {");
    require(authority != std::string::npos,
            "fixture should expose the current transport authority");
    source.replace(authority, std::string{"\"transportSync\""}.size(),
                   "\"midiClock\"");
    const std::string oscEgressField = R"json(,
    "oscEgress": {
      "enabled": false,
      "port": 9001
    })json";
    const auto oscEgress = source.find(oscEgressField);
    require(oscEgress != std::string::npos,
            "fixture should expose the format-nineteen OSC feedback boundary");
    source.erase(oscEgress, oscEgressField.size());
    const std::string oscField = R"json(,
    "oscIngress": {
      "enabled": false,
      "port": 9000
    })json";
    const auto osc = source.find(oscField);
    require(osc != std::string::npos,
            "fixture should expose the format-eighteen OSC boundary");
    source.erase(osc, oscField.size());
    downgradeEmptyPresentationToV1(source);
}

void testCheckedConversionAndPosition()
{
    talas::project::ProjectMusicalTime musical;
    musical.tempo.push_back({960, 1000000});
    musical.meter.push_back({3840, 3, 4});
    require(talas::project::isValidProjectMusicalTime(musical),
            "bounded tempo and on-bar meter changes should validate");
    require(talas::project::musicalTickToSampleFrame(musical, 0, 48000) == 0,
            "tick zero should map to frame zero");
    require(talas::project::musicalTickToSampleFrame(musical, 960, 48000) == 24000,
            "first quarter should use the first tempo segment");
    require(talas::project::musicalTickToSampleFrame(musical, 1920, 48000) == 72000,
            "tempo integration should cross the exact segment boundary");
    require(talas::project::midiSongPositionToMusicalTick(1) == 240,
            "one SPP unit should equal one 960-PPQ sixteenth note");
    require(talas::project::midiSongPositionToSampleFrame(musical, 1, 48000) == 6000,
            "one SPP unit should map through the persisted tempo map");
    require(!talas::project::midiSongPositionToMusicalTick(0x4000).has_value(),
            "the 14-bit SPP boundary should fail closed");
    require(talas::project::sampleFrameToMusicalTick(musical, 72000, 48000) == 1920,
            "frame-to-tick presentation should invert an exact boundary");
    require(talas::project::musicalTickToSampleFrame(musical, 960, 44100) == 22050
                && talas::project::musicalTickToSampleFrame(musical, 960, 96000) == 48000,
            "conversion should remain exact at 44.1 and 96 kHz");
    require(
        talas::project::midiSongPositionToSampleFrame(
            talas::project::ProjectMusicalTime{},
            0x3fff,
            96000)
            == 196596000,
        "maximum 14-bit SPP should convert through checked integer arithmetic");
    require(
        !talas::project::musicalTickToSampleFrame(
             talas::project::ProjectMusicalTime{},
             std::numeric_limits<std::uint64_t>::max(),
             48000)
             .has_value(),
        "overflowing tick integration should fail closed");

    const auto first = talas::project::musicalTickToPosition(musical, 0);
    const auto changed = talas::project::musicalTickToPosition(musical, 3840);
    const auto nextBeat = talas::project::musicalTickToPosition(musical, 4800);
    require(first == talas::project::MusicalPosition{1, 1, 0},
            "origin should present as bar one beat one");
    require(changed == talas::project::MusicalPosition{2, 1, 0},
            "an on-bar meter change should preserve monotonic bar numbering");
    require(nextBeat == talas::project::MusicalPosition{2, 2, 0},
            "new meter should own subsequent beat presentation");
}

// A new Timeline plays at 120 BPM, a beat
// half a second; a tempo dropped to 60 on bar 2 makes each later beat a
// second long, so the cursor slows there, and a second after it covers
// half the beats a second before it did, the seconds' marks closer together
// in bars and beats.
void testTempoChangesWarpSecondsAgainstBars()
{
    const talas::project::ProjectMusicalTime fresh;
    require(talas::project::musicalTickToSampleFrame(fresh, 960, 48000) == 24000,
        "a new Timeline should play 120 BPM, a beat of half a second");
    auto slowed = fresh;
    slowed.tempo.push_back({3840, 1000000});
    require(talas::project::isValidProjectMusicalTime(slowed), "a tempo change on a bar should validate");
    const auto frameOf = [&](std::uint64_t tick) { return *talas::project::musicalTickToSampleFrame(slowed, tick, 48000); };
    require(frameOf(3840) == 96000 && frameOf(4800) - frameOf(3840) == 48000 && frameOf(960) - frameOf(0) == 24000,
        "beats after the change to 60 BPM should last a second, those before half a second");
    const auto tickAt = [&](std::uint64_t frame) { return *talas::project::sampleFrameToMusicalTick(slowed, frame, 48000); };
    require(tickAt(48000) - tickAt(0) == 1920 && tickAt(144000) - tickAt(96000) == 960,
        "a second after the slowing should cover half the beats a second before it did");
    // A sample loop of 4 beats in 2.96961 s sets the tempo to 80.82
    // BPM, at which the loop lasts one bar of 4/4 exactly.
    const auto loop = talas::project::microsecondsPerQuarterForLoop(2.96961, 4.0);
    require(loop.has_value() && std::abs(60'000'000.0 / *loop - 80.82) < 0.005,
        "4 beats in 2.96961 s should set 80.82 BPM");
    auto matched = fresh;
    matched.tempo.front().microsecondsPerQuarter = *loop;
    require(std::llabs(static_cast<long long>(*talas::project::musicalTickToSampleFrame(matched, 4 * 960, 48000))
                - static_cast<long long>(std::llround(2.96961 * 48000))) <= 1,
        "at that tempo the loop should last one bar");
    require(!talas::project::microsecondsPerQuarterForLoop(2.0, 0.0).has_value(), "no beats should set no tempo");
}

void testValidationAndEditorTransactions()
{
    auto invalid = talas::project::ProjectMusicalTime{};
    invalid.meter.push_back({960, 3, 4});
    require(!talas::project::isValidProjectMusicalTime(invalid),
            "off-bar meter changes should be rejected");
    invalid = {};
    invalid.tempo.push_back({0, 600000});
    require(!talas::project::isValidProjectMusicalTime(invalid),
            "duplicate tempo ticks should be rejected");
    invalid = {};
    invalid.tempo.front().microsecondsPerQuarter = 999;
    require(!talas::project::isValidProjectMusicalTime(invalid),
            "out-of-range tempo should be rejected");
    invalid = {};
    invalid.meter.front().denominator = 3;
    require(!talas::project::isValidProjectMusicalTime(invalid),
            "non-power-of-two meter denominator should be rejected");
    invalid = {};
    invalid.tempo.resize(talas::project::maximumMusicalMapEvents + 1);
    require(!talas::project::isValidProjectMusicalTime(invalid),
            "over-capacity tempo maps should be rejected before traversal");

    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor({}, registry);
    require(editor.addTempoEvent({960, 600000}).ok(),
            "editor should add a valid tempo event");
    require(editor.addMeterEvent({3840, 3, 4}).ok(),
            "editor should add an on-bar meter event");
    require(!editor.removeTempoEvent(0).ok()
                && !editor.removeMeterEvent(0).ok(),
            "editor should protect both tick-zero anchors");
    require(editor.updateTempoEvent(1, {1920, 750000}).ok(),
            "editor should update and re-sort a tempo event");
    require(editor.removeTempoEvent(1).ok()
                && editor.removeMeterEvent(1).ok(),
            "editor should remove non-anchor events");
    require(editor.canUndo(), "musical-map edits should participate in history");
    require(editor.undo().ok() && editor.canRedo() && editor.redo().ok(),
            "musical-map edits should round-trip through undo and redo");
}

void testFormatSixteenRoundTripAndMigration()
{
    talas::project::ProjectDocument document;
    document.timeline.musicalTime.tempo.push_back({960, 1000000});
    document.timeline.musicalTime.meter.push_back({3840, 3, 4});
    const auto canonical = talas::project::serializeProjectDocument(document);
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok()
                && reopened.document.timeline.musicalTime
                    == document.timeline.musicalTime,
            "format sixteen musical maps should round-trip canonically");

    auto fractional = canonical;
    const auto tick = fractional.find("\"tick\": 960");
    require(tick != std::string::npos, "fixture should expose a tempo tick");
    fractional.replace(tick, std::string{"\"tick\": 960"}.size(),
                       "\"tick\": 960.0000000000001");
    require(!talas::project::parseProjectDocument(fractional).ok(),
            "fractional musical authority tokens should fail closed");

    auto missing = canonical;
    const auto missingMusicalStart = missing.find("    \"musicalTime\": {\n");
    const auto missingEventValues = missing.find(
        "    \"eventValues\": ", missingMusicalStart);
    require(missingMusicalStart != std::string::npos
                && missingEventValues != std::string::npos,
            "fixture should expose required format-sixteen musical time");
    missing.erase(
        missingMusicalStart,
        missingEventValues - missingMusicalStart);
    require(!talas::project::parseProjectDocument(missing).ok(),
            "format sixteen should reject a missing musical-time authority");

    auto premature = canonical;
    downgradeCurrentTransportSyncToFormatSixteen(premature);
    const auto prematureVersion = premature.find("\"version\": 16");
    require(prematureVersion != std::string::npos,
            "format-sixteen fixture should expose its version");
    premature.replace(
        prematureVersion,
        std::string{"\"version\": 16"}.size(),
        "\"version\": 15");
    require(!talas::project::parseProjectDocument(premature).ok(),
            "format fifteen should reject a premature musical-time field");

    auto legacy = canonical;
    downgradeCurrentTransportSyncToFormatSixteen(legacy);
    const auto version = legacy.find("\"version\": 16");
    require(version != std::string::npos,
            "format-sixteen fixture should expose its version");
    legacy.replace(version, std::string{"\"version\": 16"}.size(),
                   "\"version\": 15");
    const auto musicalStart = legacy.find("    \"musicalTime\": {\n");
    const auto eventValues = legacy.find("    \"eventValues\": ", musicalStart);
    require(musicalStart != std::string::npos && eventValues != std::string::npos,
            "fixture should expose the format-sixteen boundary");
    legacy.erase(musicalStart, eventValues - musicalStart);
    const auto migrated = talas::project::parseProjectDocument(legacy);
    require(migrated.ok()
                && migrated.document.formatVersion == talas::project::currentProjectFormatVersion
                && migrated.document.timeline.musicalTime
                    == talas::project::ProjectMusicalTime{},
            "format fifteen should migrate to only the canonical anchors: "
                + (migrated.errors.empty()
                        ? std::string{"model mismatch"}
                        : migrated.errors.front().path + " "
                            + migrated.errors.front().message));
}
}

int main()
{
    testCheckedConversionAndPosition();
    testTempoChangesWarpSecondsAgainstBars();
    testValidationAndEditorTransactions();
    testFormatSixteenRoundTripAndMigration();
    return 0;
}
