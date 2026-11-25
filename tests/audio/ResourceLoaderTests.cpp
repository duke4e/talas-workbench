#include "talas/audio/ProjectAssetLoader.h"
#include "talas/audio/ProjectAssetChangeTracker.h"
#include "talas/audio/ResourceLoader.h"
#include "talas/audio/WavReader.h"
#include "talas/audio/WavWriter.h"
#include "WavSmplFixture.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectDiskCache.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireContains(std::string_view haystack, std::string_view needle, const std::string& message)
{
    require(haystack.find(needle) != std::string_view::npos, message);
}

const talas::core::NodeRegistry& registry()
{
    static const auto value = talas::core::makeBuiltinNodeRegistry();
    return value;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
        : path_(std::filesystem::temp_directory_path()
                / ("talas-resource-loader-tests-"
                   + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& path() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void writeText(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path);
    output << text;
    require(output.good(), "test should write text fixture");
}

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "test should write binary fixture");
}

void testLoadsNumericTextTables()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "shape.txt";
    writeText(path, "1, 0.5; -1\n# ignored\n2e-1");

    const auto loaded = talas::audio::loadTableResource(path);

    require(loaded.ok(), "numeric table should load: " + loaded.error);
    require(loaded.samples.size() == 4, "numeric table should retain four samples");
    require(loaded.samples[0] == 1.0 && loaded.samples[1] == 0.5
                && loaded.samples[2] == -1.0 && loaded.samples[3] == 0.2,
            "numeric table syntax should preserve sample values");

    const auto malformedPath = directory.path() / "malformed.txt";
    writeText(malformedPath, "1 nope");
    const auto malformed = talas::audio::loadTableResource(malformedPath);
    require(!malformed.ok(), "malformed numeric table should fail");
    requireContains(malformed.error, "parse error", "malformed numeric table should explain the failure");
}

void testNumericTableHashUsesExactCrLfBytes()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "crlf-table.txt";
    const std::vector<std::uint8_t> bytes = {
        '1', '\r', '\n', '0', '.', '5', '\r', '\n', '-', '1', '\r', '\n',
    };
    writeBytes(path, bytes);

    const auto loaded = talas::audio::loadTableResource(path);
    const auto inspected =
        talas::project::calculateProjectAssetSha256(path);

    require(loaded.ok(), "CRLF numeric table should load: " + loaded.error);
    require(
        inspected.ok()
            && loaded.contentHash == inspected.contentHash
            && loaded.contentHash
                == talas::project::calculateProjectAssetSha256(bytes),
        "numeric table should hash the exact raw CRLF bytes on every platform");
}

void testLoadsMonoAndStereoPcm16WavTables()
{
    TemporaryDirectory directory;
    std::string error;
    const auto monoPath = directory.path() / "mono.wav";
    const std::vector<std::vector<double>> monoChannels = {{0.0, 0.5, -1.0}};
    require(talas::audio::writePcm16Wav(monoPath, 48000.0, monoChannels, error),
            "test should write mono WAV fixture: " + error);
    const auto mono = talas::audio::loadTableResource(monoPath);
    require(mono.ok() && mono.samples.size() == 3, "mono PCM16 WAV should load as a table");
    require(mono.sourceSampleRate == 48000
                && !mono.forwardLoop.has_value(),
            "plain WAV tables should preserve source rate without inventing markers");
    require(std::abs(mono.samples[1] - 16384.0 / 32767.0) <= 1.0e-12,
            "mono table WAV should preserve decoded samples");

    const auto stereoPath = directory.path() / "stereo.wav";
    error.clear();
    const std::vector<std::vector<double>> stereoChannels = {
        {0.0, 0.5, -1.0}, {1.0, -0.5, 0.0}};
    require(talas::audio::writePcm16Wav(stereoPath, 48000.0, stereoChannels, error),
            "test should write stereo WAV fixture: " + error);
    const auto stereo = talas::audio::loadTableResource(stereoPath);
    require(stereo.ok()
                && stereo.samples.size() == 3
                && stereo.rightSamples.size() == 3
                && stereo.sourceSampleRate == 48000,
            "stereo WAV table should retain both equal-length channels");
    require(std::abs(stereo.samples[1] - 16384.0 / 32767.0) <= 1.0e-12
                && std::abs(stereo.rightSamples[1] + 16384.0 / 32767.0) <= 1.0e-12,
            "stereo table channels should retain distinct decoded values");

    talas::project::ProjectDocument document;
    document.assets.push_back({
        "StereoShape", talas::project::ProjectAssetKind::Table,
        "stereo.wav"});
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}, {}});
    document.graph.nodes.push_back({"osc", "talas.oscillator", {}, {}});
    talas::project::ProjectParameterExpression shapingFunction;
    shapingFunction.parameterId = "shapingFunction";
    shapingFunction.resource = "StereoShape";
    document.graph.nodes.push_back({
        "shape", "talas.waveshaper",
        {std::move(shapingFunction)}, {}});
    document.graph.connections.push_back({
        "osc", "audioOut", "shape", "audioIn"});
    document.graph.connections.push_back({
        "shape", "audioOut", "out", "audioIn"});
    document.graph.connections.push_back({
        "shape", "rightOut", "out", "rightIn"});
    const auto project = talas::audio::loadReferencedProjectResourceSnapshots(
        document, registry(), directory.path() / "sound.talas.json");
    require(project.ok() && project.snapshots.size() == 1
                && project.snapshots.front()->tableSamples == stereo.samples
                && project.snapshots.front()->tableRightSamples == stereo.rightSamples,
            "project stereo shaping table should retain both channels in its immutable snapshot");
    const auto compatibility = talas::audio::loadReferencedProjectAssets(
        document, registry(), directory.path() / "sound.talas.json");
    require(compatibility.ok() && compatibility.assets.size() == 1
                && compatibility.assets.front().tableSamples == stereo.samples
                && compatibility.assets.front().tableRightSamples == stereo.rightSamples,
            "legacy project asset view should not discard the right shaping curve");

    const auto loopPath = directory.path() / "loop.wav";
    require(talas::test::writeMonoWavWithSmpl(
                loopPath, 8.0, std::vector<double>(8U, 0.25),
                2U, 4U, error),
            "test should write looped WAV table: " + error);
    const auto looped = talas::audio::loadTableResource(loopPath);
    require(looped.ok() && looped.sourceSampleRate == 8
                && looped.forwardLoop
                    == talas::core::SampleLoop{2U, 5U},
            "table loader should retain immutable WAV source rate and forward loop");
}

// Tables come from samples other programs wrote, WAV or AIFF of any word size
//.
void testLoadsWavAndAiffTablesOfOtherWordSizes()
{
    TemporaryDirectory directory;
    // Two 24-bit frames at half scale, up then down.
    const auto wavePath = directory.path() / "half24.wav";
    writeBytes(wavePath, {'R', 'I', 'F', 'F', 42, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0, 0x80, 0xBB, 0, 0, 0x80, 0x32, 0x02, 0, 3, 0, 24, 0,
        'd', 'a', 't', 'a', 6, 0, 0, 0, 0x00, 0x00, 0x40, 0x00, 0x00, 0xC0});
    const auto wave = talas::audio::loadTableResource(wavePath);
    require(wave.ok() && wave.sourceSampleRate == 48000
            && wave.samples == std::vector<double>{4194304.0 / 8388607.0, -4194304.0 / 8388607.0},
        "a 24-bit WAV should load as a table: " + wave.error);
    // The same in a 16-bit big-endian AIFF at 48 kHz.
    const auto aiffPath = directory.path() / "half16.aif";
    writeBytes(aiffPath, {'F', 'O', 'R', 'M', 0, 0, 0, 50, 'A', 'I', 'F', 'F',
        'C', 'O', 'M', 'M', 0, 0, 0, 18, 0, 1, 0, 0, 0, 2, 0, 16, 0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0,
        'S', 'S', 'N', 'D', 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0x00, 0xC0, 0x00});
    const auto aiff = talas::audio::loadTableResource(aiffPath);
    require(aiff.ok() && aiff.sourceSampleRate == 48000
            && aiff.samples == std::vector<double>{16384.0 / 32767.0, -16384.0 / 32767.0},
        "a 16-bit AIFF should load as a table: " + aiff.error);
}

// IRCAM/MTU sound files (.sf) open, play and
// import as samples: a 1024-byte header naming the machine that wrote it,
// little-endian for a VAX or MIPS, big-endian for a Sun or NeXT.
void testLoadsIrcamSoundFiles()
{
    TemporaryDirectory directory;
    const auto ircam = [](std::vector<std::uint8_t> header, const std::vector<std::uint8_t>& samples) {
        header.resize(1024, 0);
        header.insert(header.end(), samples.begin(), samples.end());
        return header;
    };
    // A VAX's 16-bit stereo at 44.1 kHz: a frame at half scale, up and down.
    const auto vaxPath = directory.path() / "half16.sf";
    writeBytes(vaxPath, ircam({0x64, 0xA3, 0x01, 0x00, 0x00, 0x44, 0x2C, 0x47, 2, 0, 0, 0, 2, 0, 0, 0},
        {0x00, 0x40, 0x00, 0xC0}));
    const auto vax = talas::audio::readSampleFile(vaxPath);
    require(vax.ok() && vax.sampleRate == 44100 && vax.channels.size() == 2
            && vax.channels[0] == std::vector<double>{16384.0 / 32767.0}
            && vax.channels[1] == std::vector<double>{-16384.0 / 32767.0},
        "a little-endian 16-bit IRCAM file should read as a stereo sample: " + vax.error);
    const auto table = talas::audio::loadTableResource(vaxPath);
    require(table.ok() && table.sourceSampleRate == 44100 && table.samples == std::vector<double>{16384.0 / 32767.0},
        "an IRCAM file should import as a table: " + table.error);
    // A NeXT's 32-bit floats, mono at 48 kHz.
    const auto nextPath = directory.path() / "floats.sf";
    writeBytes(nextPath, ircam({0x00, 0x04, 0xA3, 0x64, 0x47, 0x3B, 0x80, 0x00, 0, 0, 0, 1, 0, 0, 0, 4},
        {0x3F, 0x00, 0x00, 0x00, 0xBE, 0x80, 0x00, 0x00}));
    const auto next = talas::audio::readSampleFile(nextPath);
    require(next.ok() && next.sampleRate == 48000 && next.channels.size() == 1
            && next.channels[0] == std::vector<double>{0.5, -0.25},
        "a big-endian IRCAM file of floats should read as a mono sample: " + next.error);
    // A-law (0x10001) is not read.
    const auto alawPath = directory.path() / "alaw.sf";
    writeBytes(alawPath, ircam({0x64, 0xA3, 0x02, 0x00, 0x47, 0x3B, 0x80, 0x00, 0, 0, 0, 1, 0, 1, 0, 1}, {0x55, 0x55}));
    require(!talas::audio::readSampleFile(alawPath).ok(), "an IRCAM file of an unsupported packing should be refused");
}

void testLoadsNonEmptyBinaryFiles()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "part.mid";
    writeBytes(path, {0x4d, 0x54, 0x68, 0x64});
    const auto loaded = talas::audio::loadBinaryResource(path);
    require(loaded.ok(), "binary resource should load: " + loaded.error);
    require(loaded.bytes == std::vector<std::uint8_t>({0x4d, 0x54, 0x68, 0x64}),
            "binary resource should preserve exact bytes");

    const auto emptyPath = directory.path() / "empty.mid";
    writeBytes(emptyPath, {});
    const auto empty = talas::audio::loadBinaryResource(emptyPath);
    require(!empty.ok(), "empty binary resource should fail");
    requireContains(empty.error, "empty", "empty binary failure should explain the constraint");
}

void testResourcePayloadReadsAreCooperativelyCancellable()
{
    TemporaryDirectory directory;
    const auto tablePath = directory.path() / "large-table.txt";
    std::vector<std::uint8_t> tableBytes(128U * 1024U, ' ');
    for (std::size_t offset = 0; offset < tableBytes.size(); offset += 2U)
        tableBytes[offset] = '1';
    writeBytes(tablePath, tableBytes);

    int tableChecks = 0;
    const auto table = talas::audio::loadTableResource(
        tablePath,
        [&] { return ++tableChecks == 2; });
    require(
        table.cancelled && !table.ok() && table.samples.empty(),
        "numeric table payload read should stop between bounded chunks");

    const auto binaryPath = directory.path() / "large.bin";
    writeBytes(
        binaryPath,
        std::vector<std::uint8_t>(128U * 1024U, 0x5a));
    int binaryChecks = 0;
    const auto binary = talas::audio::loadBinaryResource(
        binaryPath,
        [&] { return ++binaryChecks == 2; });
    require(
        binary.cancelled && !binary.ok() && binary.bytes.empty(),
        "binary payload read should stop between bounded chunks");
}

void testResourceParsingAndHashingAreCooperativelyCancellable()
{
    TemporaryDirectory directory;
    const auto tablePath = directory.path() / "parse-table.txt";
    std::vector<std::uint8_t> tableBytes(128U * 1024U, ' ');
    for (std::size_t offset = 0; offset < tableBytes.size(); offset += 2U)
        tableBytes[offset] = '1';
    writeBytes(tablePath, tableBytes);

    int parseChecks = 0;
    const auto table = talas::audio::loadTableResource(
        tablePath,
        [&] { return ++parseChecks >= 5; });
    require(
        table.cancelled && !table.ok() && table.samples.empty(),
        "numeric table parsing should remain cancellable after the bounded file read completes");

    const auto binaryPath = directory.path() / "hash.bin";
    writeBytes(
        binaryPath,
        std::vector<std::uint8_t>(128U * 1024U, 0x33));
    int hashChecks = 0;
    const auto binary = talas::audio::loadBinaryResource(
        binaryPath,
        [&] { return ++hashChecks >= 5; });
    require(
        binary.cancelled && !binary.ok() && binary.bytes.empty(),
        "binary in-memory hashing should remain cancellable after the bounded file read completes");
}

talas::project::ProjectParameterExpression resourceParameter(std::string id, std::string resource)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.resource = std::move(resource);
    return parameter;
}

talas::project::ProjectParameterExpression textParameter(
    std::string id,
    std::string text)
{
    talas::project::ProjectParameterExpression parameter;
    parameter.parameterId = std::move(id);
    parameter.text = std::move(text);
    return parameter;
}

void testLoadsOnlyReferencedProjectAssetsRelativeToProjectFile()
{
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "assets");
    writeText(directory.path() / "assets" / "shape.txt", "1 0 -1");

    talas::project::ProjectDocument document;
    document.assets = {
        {"Shape", talas::project::ProjectAssetKind::Table, "assets/shape.txt"},
        {"Unused", talas::project::ProjectAssetKind::Binary, "missing.mid"},
    };
    document.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resourceParameter("sample", "Shape")},
        {},
    });

    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");

    require(loaded.ok(), "referenced relative project asset should load");
    require(loaded.snapshots.size() == 1
                && loaded.snapshots.front()->id == "Shape",
            "unused project assets should not be loaded");
    require(loaded.snapshots.front()->resolvedPath
                == directory.path() / "assets" / "shape.txt",
            "relative project asset should resolve against the project directory");
    require(loaded.snapshots.front()->tableSamples
                == std::vector<double>({1.0, 0.0, -1.0}),
            "loaded project table should retain samples");

    const auto compatibility =
        talas::audio::loadReferencedProjectAssets(
            document,
            registry(),
            directory.path() / "sound.talas.json");
    require(
        compatibility.ok()
            && compatibility.assets.size() == 1
            && compatibility.assets.front().id == "Shape"
            && compatibility.assets.front().tableSamples
                == std::vector<double>({1.0, 0.0, -1.0}),
        "legacy project asset loader entry point should remain source compatible");
}

// A KeyMappedMultisample's FirstSample brings every sample file in its
// folder, each a table of the bank in the order
// of their names; folders inside it and files that are no samples are
// passed over. Projects before format 82 load FirstSample alone.
void testLoadsAKeyMappedMultisampleBankFromItsFolder()
{
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "bank" / "inner");
    std::string error;
    for (const auto& [name, level] : {std::pair{"c.wav", 0.375}, std::pair{"a.wav", 0.125},
             std::pair{"b.wav", 0.25}, std::pair{"inner/d.wav", 0.5}})
    {
        const std::vector<std::vector<double>> channels{std::vector<double>(1, level)};
        require(talas::audio::writePcm16Wav(directory.path() / "bank" / name, 48000.0, channels, error),
            "test should write a bank sample: " + error);
    }
    writeText(directory.path() / "bank" / "notes.txt", "1 2 3");
    talas::project::ProjectDocument document;
    document.assets = {{"Bank/First", talas::project::ProjectAssetKind::Table, "bank/a.wav"}};
    document.graph.nodes.push_back({"multi", "talas.keyMappedMultisample",
        {resourceParameter("firstSample", "Bank/First")}, {}});
    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document, registry(), directory.path() / "sound.talas.json");
    require(loaded.ok() && loaded.snapshots.size() == 3,
        "a KeyMappedMultisample should load the three samples of its folder");
    const std::vector<std::string> ids{"Bank/First", "Bank/First#b.wav", "Bank/First#c.wav"};
    const std::vector<std::string> names{"a.wav", "b.wav", "c.wav"};
    for (std::size_t index = 0; index < ids.size(); ++index)
    {
        const auto& snapshot = *loaded.snapshots[index];
        require(snapshot.id == ids[index] && snapshot.tableBank == "Bank/First"
                && snapshot.tableBankName == names[index]
                && snapshot.tableSamples.size() == 1
                && std::abs(snapshot.tableSamples.front() - 0.125 * static_cast<double>(index + 1)) < 1.0e-4,
            "each bank sample should load as a table naming the bank, in name order");
    }

    auto legacy = document;
    talas::project::ProjectParameterExpression samples;
    samples.parameterId = "samples";
    samples.choice = "legacyFirstSampleOnly";
    legacy.graph.nodes.front().parameters.push_back(samples);
    const auto alone = talas::audio::loadReferencedProjectResourceSnapshots(
        legacy, registry(), directory.path() / "sound.talas.json");
    require(alone.ok() && alone.snapshots.size() == 1 && alone.snapshots.front()->tableBank.empty(),
        "projects before format 82 should load FirstSample alone");
}

void testMarkedWavMetadataSurvivesProjectSnapshotLoading()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "marked.wav";
    std::string error;
    require(talas::test::writeMonoWavWithSmpl(
                path, 8.0, std::vector<double>(8U, 0.25),
                2U, 4U, error, 0U, 69U, 0x80000000U),
        "test should write project smpl WAV: " + error);
    talas::project::ProjectDocument document;
    document.assets = {
        {"Samples/Marked", talas::project::ProjectAssetKind::Table,
         "marked.wav"},
    };
    document.graph.nodes.push_back({
        "index", "talas.timeIndexFromFileName",
        {resourceParameter("fileName", "Samples/Marked")}, {},
    });
    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document, registry(), directory.path() / "sound.talas.json");
    require(loaded.ok() && loaded.snapshots.size() == 1,
        "project should load the referenced marked table asset");
    const auto& snapshot = *loaded.snapshots.front();
    require(snapshot.tableSamples.size() == 8
                && snapshot.tableSourceSampleRate == 8
                && snapshot.tableForwardLoop
                    == talas::core::SampleLoop{2U, 5U}
                && snapshot.tableSamplePitch
                    == talas::core::SamplePitch{69U, 0x80000000U},
        "project snapshot should retain exact source rate, loop and pitch metadata");
    const auto compatibility = talas::audio::loadReferencedProjectAssets(
        document, registry(), directory.path() / "sound.talas.json");
    require(compatibility.ok()
                && compatibility.assets.size() == 1
                && compatibility.assets.front().tableSourceSampleRate == 8
                && compatibility.assets.front().tableForwardLoop
                    == talas::core::SampleLoop{2U, 5U}
                && compatibility.assets.front().tableSamplePitch
                    == talas::core::SamplePitch{69U, 0x80000000U},
        "compatibility asset view should retain marked WAV metadata");
}

// A sample's named markers reach the table and the
// project's snapshot of it.
void testSampleMarkersSurviveLoading()
{
    TemporaryDirectory directory;
    const auto path = directory.path() / "hit.aif";
    writeBytes(path, {'F', 'O', 'R', 'M', 0, 0, 0, 70, 'A', 'I', 'F', 'F',
        'C', 'O', 'M', 'M', 0, 0, 0, 18, 0, 1, 0, 0, 0, 2, 0, 16, 0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0,
        'M', 'A', 'R', 'K', 0, 0, 0, 12, 0, 1, 0, 1, 0, 0, 0, 1, 3, 'h', 'i', 't',
        'S', 'S', 'N', 'D', 0, 0, 0, 12, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0x00, 0xC0, 0x00});
    const std::vector<talas::core::SampleMarker> expected{{"hit", 1}};
    const auto table = talas::audio::loadTableResource(path);
    require(table.ok() && table.markers == expected, "a table should keep its sample's markers: " + table.error);
    talas::project::ProjectDocument document;
    document.assets = {{"Samples/Hit", talas::project::ProjectAssetKind::Table, "hit.aif"}};
    document.graph.nodes.push_back({"index", "talas.timeIndexFromFileName", {resourceParameter("fileName", "Samples/Hit")}, {}});
    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document, registry(), directory.path() / "sound.talas.json");
    require(loaded.ok() && loaded.snapshots.size() == 1 && loaded.snapshots.front()->tableMarkers == expected,
        "the project's snapshot should keep the sample's markers");
}

void testLoadsStaticMultisampleTextListAssets()
{
    TemporaryDirectory directory;
    writeText(directory.path() / "hit.txt", "1 0 -1");
    writeText(directory.path() / "loop.txt", "0 0.5 1 0.5");

    talas::project::ProjectDocument document;
    document.assets = {
        {"Samples/Hit", talas::project::ProjectAssetKind::Table, "hit.txt"},
        {"Samples/Loop", talas::project::ProjectAssetKind::Table, "loop.txt"},
        {"Unused", talas::project::ProjectAssetKind::Binary, "missing.mid"},
    };
    document.graph.nodes.push_back({
        "multisample",
        "talas.multisample",
        {textParameter(
            "samples",
            "Samples/Hit,\tSamples/Loop\nSamples/Hit")},
        {},
    });

    const auto references = talas::audio::reachableProjectAssetReferences(
        document, registry());
    require(
        references.ok()
            && references.references.size() == 3
            && references.references[0].assetId == "Samples/Hit"
            && references.references[1].assetId == "Samples/Loop"
            && references.references[2].assetId == "Samples/Hit"
            && references.references[0].nodeId == "multisample"
            && references.references[0].parameterId == "samples",
        "Multisample static text lists should publish every ordered asset reference");

    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");
    require(
        loaded.ok()
            && loaded.snapshots.size() == 2
            && loaded.snapshots[0]->id == "Samples/Hit"
            && loaded.snapshots[0]->tableSamples
                == std::vector<double>({1.0, 0.0, -1.0})
            && loaded.snapshots[1]->id == "Samples/Loop"
            && loaded.snapshots[1]->tableSamples
                == std::vector<double>({0.0, 0.5, 1.0, 0.5}),
        "Multisample text-list loading should deduplicate through asset iteration and ignore unrelated assets");
}

void testLoadsDescriptorAuthorizedMultifileDiskPlayerTextListAssets()
{
    TemporaryDirectory directory;
    writeText(directory.path() / "hit.txt", "1 0 -1");
    writeText(directory.path() / "wave.txt", "0 0.5 1 0.5");

    talas::project::ProjectDocument document;
    document.assets = {
        {"Samples/Hit", talas::project::ProjectAssetKind::Table, "hit.txt"},
        {"Samples/Wave", talas::project::ProjectAssetKind::Table, "wave.txt"},
        {"Unused", talas::project::ProjectAssetKind::Binary, "missing.mid"},
    };
    document.graph.nodes.push_back({
        "multiDisk",
        "talas.multifileDiskPlayer",
        {textParameter(
            "fileNames",
            "Samples/Hit,\tSamples/Wave\nSamples/Hit")},
        {},
    });

    const auto references = talas::audio::reachableProjectAssetReferences(
        document, registry());
    require(
        references.ok()
            && references.references.size() == 3
            && references.references[0].assetId == "Samples/Hit"
            && references.references[1].assetId == "Samples/Wave"
            && references.references[2].assetId == "Samples/Hit"
            && references.references[0].nodeId == "multiDisk"
            && references.references[0].parameterId == "fileNames",
        "descriptor-authorized Multifile Disk Player lists should publish every ordered asset reference");

    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");
    require(
        loaded.ok()
            && loaded.snapshots.size() == 2
            && loaded.snapshots[0]->id == "Samples/Hit"
            && loaded.snapshots[0]->tableSamples
                == std::vector<double>({1.0, 0.0, -1.0})
            && loaded.snapshots[1]->id == "Samples/Wave"
            && loaded.snapshots[1]->tableSamples
                == std::vector<double>({0.0, 0.5, 1.0, 0.5}),
        "descriptor-authorized static resource lists should load and deduplicate exact assets");
}

// The files a DiskSplicer's segments splice.
void testLoadsTheFilesADiskSplicerSplices()
{
    TemporaryDirectory directory;
    writeText(directory.path() / "hit.txt", "1 0 -1");
    writeText(directory.path() / "wave.txt", "0 0.5 1 0.5");

    talas::project::ProjectDocument document;
    document.assets = {
        {"Samples/Hit", talas::project::ProjectAssetKind::Table, "hit.txt"},
        {"Samples/Wave", talas::project::ProjectAssetKind::Table, "wave.txt"},
        {"Unused", talas::project::ProjectAssetKind::Binary, "missing.mid"},
    };
    document.graph.nodes.push_back({
        "splicer",
        "talas.diskSplicer",
        {textParameter("segments", "Samples/Wave 1 3\n\n  Samples/Hit\nSamples/Wave")},
        {},
    });

    const auto references = talas::audio::reachableProjectAssetReferences(document, registry());
    require(references.ok() && references.references.size() == 3
            && references.references[0].assetId == "Samples/Wave"
            && references.references[1].assetId == "Samples/Hit"
            && references.references[0].parameterId == "segments",
        "each segment's file should be referenced");
    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document, registry(), directory.path() / "sound.talas.json");
    require(loaded.ok() && loaded.snapshots.size() == 2, "the spliced files should load once each");
}

void testRejectsUnboundedMultisampleTextListsBeforeAssetLookup()
{
    const auto requireRejected = [] (
        std::string samples,
        std::string_view context) {
        talas::project::ProjectDocument document;
        document.assets.push_back({
            "Missing",
            talas::project::ProjectAssetKind::Table,
            "missing.txt",
        });
        document.graph.nodes.push_back({
            "multisample",
            "talas.multisample",
            {textParameter("samples", std::move(samples))},
            {},
        });

        const auto references =
            talas::audio::reachableProjectAssetReferences(
                document, registry());
        require(
            !references.ok()
                && references.references.empty()
                && references.failure.has_value()
                && references.failure->nodeId == "multisample"
                && references.failure->parameterId == "samples"
                && references.failure->message.find(
                       "bounded static resource list")
                    != std::string::npos,
            "invalid Multisample list should fail atomically before asset lookup: "
                + std::string{context});
    };

    requireRejected({}, "empty");
    requireRejected(std::string(4'097, 'A'), "text bytes");
    std::string tooManyPositions;
    for (std::size_t index = 0; index < 257; ++index)
        tooManyPositions += index == 0 ? "Missing" : " Missing";
    requireRejected(std::move(tooManyPositions), "positions");
}

void testProjectAssetFailureCarriesReferenceContext()
{
    TemporaryDirectory directory;
    talas::project::ProjectDocument document;
    document.assets.push_back(
        {"Missing", talas::project::ProjectAssetKind::Table, "missing.wav"});
    document.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resourceParameter("sample", "Missing")},
        {},
    });

    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");

    require(!loaded.ok() && loaded.failure.has_value(),
            "missing referenced project asset should fail");
    require(loaded.snapshots.empty(), "project asset loading should be failure-atomic");
    require(loaded.failure->assetId == "Missing"
                && loaded.failure->nodeId == "sample"
                && loaded.failure->parameterId == "sample",
            "project asset failure should identify its first parameter reference");
    require(loaded.failure->resolvedPath == directory.path() / "missing.wav",
            "project asset failure should retain the resolved path");
}

void testReachabilityFailureDoesNotMasqueradeAsNoAssets()
{
    talas::project::ProjectDocument document;
    document.assets.push_back({
        "RootShape",
        talas::project::ProjectAssetKind::Table,
        "root-shape.txt",
    });
    document.graph.nodes.push_back({
        "rootSample",
        "talas.sample",
        {resourceParameter("sample", "RootShape")},
        {},
    });
    talas::project::ProjectSoundDefinition inputBearingCandidate;
    inputBearingCandidate.definitionId = "inputCandidate";
    inputBearingCandidate.displayName = "Input candidate";
    inputBearingCandidate.inputs.push_back({
        "audioIn",
        talas::core::PortType::Audio,
        "gain",
        "audioIn",
    });
    inputBearingCandidate.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "gain",
        "audioOut",
    });
    inputBearingCandidate.graph.nodes.push_back({
        "gain",
        "talas.vca",
        {},
        {},
    });
    document.soundDefinitions.push_back(
        std::move(inputBearingCandidate));

    talas::project::ProjectSoundDefinition wrapper;
    wrapper.definitionId = "variableWrapper";
    wrapper.displayName = "Variable wrapper";
    wrapper.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "selector",
        "audioOut",
    });
    talas::project::ProjectNode selector;
    selector.instanceId = "selector";
    selector.typeId = "talas.variable";
    talas::project::ProjectParameterExpression variable;
    variable.parameterId = "variableId";
    variable.text = "selected";
    selector.parameters.push_back(std::move(variable));
    wrapper.graph.nodes.push_back(std::move(selector));
    document.soundDefinitions.push_back(std::move(wrapper));
    talas::project::ProjectNode wrapperInstance;
    wrapperInstance.instanceId = "wrapper";
    wrapperInstance.soundDefinitionId = "variableWrapper";
    document.graph.nodes.push_back(std::move(wrapperInstance));
    document.graph.nodes.push_back({
        "out",
        "talas.audioOutput",
        {},
        {},
    });
    document.graph.connections.push_back({
        "wrapper",
        "audioOut",
        "out",
        "audioIn",
    });

    talas::project::ProjectSoundVariableDeclaration declaration;
    declaration.variableId = "selected";
    declaration.displayName = "Selected";
    declaration.owner.kind =
        talas::project::ProjectVariableOwnerKind::SoundDefinition;
    declaration.owner.definitionId = "variableWrapper";
    declaration.candidateDefinitionIds = {"inputCandidate"};
    document.soundVariableLibrary.variables.push_back(
        std::move(declaration));

    const auto resolved =
        talas::audio::reachableProjectAssetReferences(
            document,
            registry());
    const auto resolutionDetail = resolved.failure.has_value()
        ? resolved.failure->nodeId + ": " + resolved.failure->message
        : std::string{"unexpected success"};
    require(
        !resolved.ok()
            && resolved.references.empty()
            && resolved.failure.has_value()
            && resolved.failure->nodeId.find("selector")
                != std::string::npos
            && resolved.failure->structuralSourcePath.find("selector")
                != std::string::npos
            && resolved.failure->message.find("source Sound")
                != std::string::npos,
        "unsupported Sound-variable branches must be a typed reachability failure, not a successful empty reference set: "
            + resolutionDetail);

    const auto loaded =
        talas::audio::loadReferencedProjectResourceSnapshots(
            document,
            registry(),
            std::nullopt);
    require(
        !loaded.ok()
            && loaded.snapshots.empty()
            && loaded.failure.has_value()
            && loaded.failure->assetId.empty()
            && loaded.failure->nodeId.find("selector")
                != std::string::npos
            && loaded.failure->message.find("source Sound")
                != std::string::npos,
        "resource preparation must preserve reachability failure before treating root assets as absent");

    const auto watch =
        talas::audio::captureProjectAssetWatchSnapshot(
            document,
            registry(),
            std::nullopt);
    require(
        watch.entries.empty()
            && watch.referenceFailure.has_value()
            && watch.referenceFailure->nodeId.find("selector")
                != std::string::npos,
        "asset watching must retain the same reachability failure instead of publishing an empty successful baseline");
    const auto watchChange =
        talas::audio::findProjectAssetWatchChange({}, watch);
    require(
        watchChange.has_value()
            && watchChange->message.find("source Sound")
                != std::string::npos,
        "a reachability failure must fail the watch comparison closed");
}

void testLoadsAssetsResolvedInsideReachableSoundDefinitions()
{
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "assets");
    writeText(directory.path() / "assets" / "shape.txt", "0 0.5 1");

    talas::project::ProjectDocument document;
    document.assets = {
        {"Shape", talas::project::ProjectAssetKind::Table, "assets/shape.txt"},
        {"Unused", talas::project::ProjectAssetKind::Table, "missing.txt"},
    };
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "sampleVoice";
    voice.displayName = "Sample Voice";
    voice.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "sample",
        "audioOut",
    });
    voice.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resourceParameter("sample", "Shape")},
        {},
    });
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "sampleVoice";
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back({
        "out",
        "talas.audioOutput",
        {},
        {},
    });
    document.graph.connections.push_back({
        "voice1",
        "audioOut",
        "out",
        "audioIn",
    });

    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");

    require(
        loaded.ok()
            && loaded.snapshots.size() == 1
            && loaded.snapshots.front()->id == "Shape"
            && loaded.snapshots.front()->tableSamples
                == std::vector<double>({0.0, 0.5, 1.0}),
        "reachable reusable Sound resources should load while unrelated asset declarations remain ignored");
}

void testReachableSoundAssetFailureCarriesStructuralSourcePath()
{
    TemporaryDirectory directory;
    talas::project::ProjectDocument document;
    document.assets.push_back({
        "MissingShape",
        talas::project::ProjectAssetKind::Table,
        "missing-shape.txt",
    });
    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "sampleVoice";
    voice.displayName = "Sample Voice";
    voice.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "sample",
        "audioOut",
    });
    voice.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resourceParameter("sample", "MissingShape")},
        {},
    });
    document.soundDefinitions.push_back(std::move(voice));
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "sampleVoice";
    document.graph.nodes.push_back(std::move(instance));
    document.graph.nodes.push_back({
        "out",
        "talas.audioOutput",
        {},
        {},
    });
    document.graph.connections.push_back({
        "voice1",
        "audioOut",
        "out",
        "audioIn",
    });

    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");

    require(!loaded.ok() && loaded.failure.has_value(),
            "missing reusable Sound asset should fail");
    require(
        loaded.failure->structuralSourcePath
            == "voice1/sampleVoice::sample",
        "reusable Sound asset failure should retain the manifest source path");
}

void testProjectResourceSnapshotsPinAcceptedGenerations()
{
    static_assert(std::is_same_v<
                  talas::core::ResourceSnapshotPtr::element_type,
                  const talas::core::ResourceSnapshot>);

    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "assets");
    const auto source = directory.path() / "assets" / "shape.txt";
    writeText(source, "1 2 3");

    talas::project::ProjectDocument document;
    talas::project::ProjectAsset asset;
    asset.id = "Shape";
    asset.kind = talas::project::ProjectAssetKind::Table;
    asset.path = "assets/shape.txt";
    asset.generation = 7;
    document.assets.push_back(asset);
    document.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resourceParameter("sample", "Shape")},
        {},
    });

    const auto first = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");
    require(first.ok() && first.snapshots.size() == 1,
            "first resource generation should load");
    const auto generationSeven = first.snapshots.front();
    require(
        generationSeven->generation == 7
            && generationSeven->contentHash.size() == 64
            && generationSeven->tableSamples
                == std::vector<double>({1.0, 2.0, 3.0}),
        "loaded snapshot should pin verified bytes to manifest generation seven");

    writeText(source, "9 8 7");
    require(
        generationSeven->tableSamples
            == std::vector<double>({1.0, 2.0, 3.0}),
        "published generation should remain immutable after disk changes");
    document.assets.front().generation = 8;
    const auto second = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");
    require(second.ok() && second.snapshots.size() == 1,
            "replacement resource generation should load off-thread");
    require(
        second.snapshots.front() != generationSeven
            && second.snapshots.front()->generation == 8
            && second.snapshots.front()->contentHash
                != generationSeven->contentHash
            && second.snapshots.front()->tableSamples
                == std::vector<double>({9.0, 8.0, 7.0}),
        "replacement should publish a distinct immutable generation");

    writeText(source, "9 malformed 7");
    document.assets.front().generation = 9;
    const auto failed = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");
    require(!failed.ok() && failed.snapshots.empty(),
            "failed replacement should publish no partial snapshot set");
    require(
        failed.failure.has_value()
            && failed.failure->assetId == "Shape"
            && failed.failure->structuralSourcePath == "sample",
        "failed replacement should retain the structural source path");
    require(
        generationSeven->tableSamples
            == std::vector<double>({1.0, 2.0, 3.0})
            && second.snapshots.front()->tableSamples
                == std::vector<double>({9.0, 8.0, 7.0}),
        "failed replacement should preserve every previously published generation");
}

void testLoadsCanonicalAudioSnapshotWithoutFullPayload()
{
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "assets");
    const auto source = directory.path() / "assets" / "stereo.wav";
    std::string error;
    const std::vector<std::vector<double>> channels = {
        {0.0, 0.25, -0.5, 1.0},
        {1.0, -0.25, 0.5, 0.0},
    };
    require(
        talas::audio::writePcm16Wav(
            source,
            44100.0,
            channels,
            error),
        "test should write stereo audio snapshot fixture: " + error);

    talas::project::ProjectDocument document;
    talas::project::ProjectAsset asset;
    asset.id = "Audio/Voice";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "assets/stereo.wav";
    asset.generation = 4;
    document.assets.push_back(asset);
    document.graph.nodes.push_back({
        "player",
        "talas.diskPlayer",
        {resourceParameter("fileName", "Audio/Voice")},
        {},
    });

    const auto loaded =
        talas::audio::loadReferencedProjectResourceSnapshots(
            document,
            registry(),
            directory.path() / "sound.talas.json");

    require(
        loaded.ok() && loaded.snapshots.size() == 1,
        loaded.failure.has_value()
            ? loaded.failure->message
            : "canonical audio snapshot should load");
    const auto& snapshot = *loaded.snapshots.front();
    require(
        snapshot.id == "Audio/Voice"
            && snapshot.kind
                == talas::core::ResourceSnapshotKind::Audio
            && snapshot.generation == 4
            && snapshot.contentHash.size() == 64
            && snapshot.resolvedPath == source
            && snapshot.channelCount == 2
            && snapshot.sampleRate == 44100.0
            && snapshot.frameCount == 4
            && snapshot.encoding
                == talas::project::pcm16WavAssetEncoding,
        "audio snapshot should retain exact verified identity and media shape");
    require(
        snapshot.tableSamples.empty()
            && snapshot.binaryBytes.empty(),
        "bounded audio snapshot must not masquerade as a fully loaded table or binary payload");

    const auto compatibility =
        talas::audio::loadReferencedProjectAssets(
            document,
            registry(),
            directory.path() / "sound.talas.json");
    require(
        compatibility.ok()
            && compatibility.assets.size() == 1
            && compatibility.assets.front().kind
                == talas::project::ProjectAssetKind::Audio
            && compatibility.assets.front().channelCount == 2
            && compatibility.assets.front().sampleRate == 44100.0
            && compatibility.assets.front().frameCount == 4
            && compatibility.assets.front().encoding
                == talas::project::pcm16WavAssetEncoding
            && compatibility.assets.front().tableSamples.empty()
            && compatibility.assets.front().binaryBytes.empty(),
        "compatibility loader must preserve Audio identity without misclassifying it as Binary");
}

void testCapturedAudioSnapshotRetainsTheExactInspectedBytes()
{
    TemporaryDirectory directory;
    std::filesystem::create_directories(directory.path() / "DiskCache");
    const auto source = directory.path() / "DiskCache" / "captured.wav";
    std::string error;
    const std::vector<std::vector<double>> channels = {
        {0.25, -0.5, 0.75},
    };
    require(
        talas::audio::writePcm16Wav(
            source,
            48000.0,
            channels,
            error),
        "captured snapshot fixture should write: " + error);

    talas::project::ProjectAsset asset;
    asset.id = "DiskCache/Captured";
    asset.kind = talas::project::ProjectAssetKind::Audio;
    asset.path = "DiskCache/captured.wav";
    asset.generation = 7;
    const auto inspected = talas::project::inspectProjectAsset(asset, source);
    require(inspected.ok(),
        "captured snapshot fixture should inspect before provenance binding");
    asset = inspected.asset;
    asset.provenance = talas::project::makeDiskCacheCaptureProvenance(
        std::string(
            talas::project::diskCacheCaptureFingerprintBytes,
            'a'));

    talas::project::ProjectDocument document;
    document.assets.push_back(asset);
    document.graph.nodes.push_back({
        "cache",
        "talas.diskCache",
        {
            resourceParameter("fileName", asset.id),
            {"record", {}, {}, false},
        },
        {},
    });
    const auto loaded = talas::audio::loadReferencedProjectResourceSnapshots(
        document,
        registry(),
        directory.path() / "sound.talas.json");
    require(
        loaded.ok() && loaded.snapshots.size() == 1,
        loaded.failure.has_value()
            ? loaded.failure->message
            : "captured snapshot should load");
    const auto& snapshot = *loaded.snapshots.front();
    require(
        snapshot.kind == talas::core::ResourceSnapshotKind::Audio
            && !snapshot.audioBytes.empty()
            && talas::project::calculateProjectAssetSha256(snapshot.audioBytes)
                == asset.contentHash,
        "captured snapshot must retain the exact securely inspected WAV bytes");
}

void testProjectSnapshotLoadingPreservesTypedPayloadCancellation()
{
    TemporaryDirectory directory;
    const auto source = directory.path() / "shape.txt";
    writeText(source, "1 0 -1");

    talas::project::ProjectDocument document;
    document.assets.push_back({
        "Shape",
        talas::project::ProjectAssetKind::Table,
        "shape.txt",
    });
    document.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resourceParameter("sample", "Shape")},
        {},
    });

    int cancellationChecks = 0;
    const auto loaded =
        talas::audio::loadReferencedProjectResourceSnapshots(
            document,
            registry(),
            directory.path() / "sound.talas.json",
            {},
            [&] { return ++cancellationChecks == 3; });

    require(
        !loaded.ok()
            && loaded.wasCancelled
            && loaded.failure.has_value()
            && loaded.snapshots.empty(),
        "project resource preparation should retain typed payload cancellation and remain failure-atomic");
}

void testLoadsCanonicalPartialTrackSnapshotWithoutMislabelingIt()
{
    TemporaryDirectory directory;
    talas::project::PartialTrackAsset partials;
    partials.sampleRate = 48000.0;
    partials.sourceFrameCount = 2048;
    partials.frames = {
        {1, 0, {{11, 0.5, 440.0, 8.0}}},
        {1, 1024, {{11, 0.4, 445.0, 9.0}}},
    };
    const auto encoded =
        talas::project::encodePartialTrackAsset(partials);
    require(encoded.ok(), "partial resource fixture should encode");
    const auto source = directory.path() / "voice.talaspartials";
    writeBytes(source, encoded.bytes);

    talas::project::ProjectAsset manifest;
    manifest.id = "Analysis/Voice";
    manifest.kind = talas::project::ProjectAssetKind::PartialSpectrum;
    manifest.path = "voice.talaspartials";
    const auto inspected = talas::project::inspectProjectAsset(
        manifest,
        source);
    require(inspected.ok(), "partial resource fixture should inspect");

    talas::project::ProjectDocument document;
    document.assets.push_back(inspected.asset);
    document.graph.nodes.push_back({
        "sample",
        "talas.sample",
        {resourceParameter("sample", "Analysis/Voice")},
        {},
    });
    const auto loaded =
        talas::audio::loadReferencedProjectResourceSnapshots(
            document,
            registry(),
            directory.path() / "sound.talas.json");
    require(loaded.ok() && loaded.snapshots.size() == 1,
            loaded.failure.has_value()
                ? loaded.failure->message
                : "partial snapshot should load");
    const auto& snapshot = *loaded.snapshots.front();
    require(snapshot.kind
                == talas::core::ResourceSnapshotKind::PartialSpectrum
            && snapshot.partialTrackBytes == encoded.bytes
            && snapshot.binaryBytes.empty()
            && snapshot.tableSamples.empty()
            && snapshot.sampleRate == 48000.0
            && snapshot.frameCount == 2048
            && snapshot.encoding
                == talas::project::partialTrackAssetEncoding,
        "partial snapshot should retain exact typed bytes and media shape");

    const auto compatibility = talas::audio::loadReferencedProjectAssets(
        document,
        registry(),
        directory.path() / "sound.talas.json");
    require(compatibility.ok()
            && compatibility.assets.size() == 1
            && compatibility.assets.front().kind
                == talas::project::ProjectAssetKind::PartialSpectrum
            && compatibility.assets.front().partialTrackBytes
                == encoded.bytes
            && compatibility.assets.front().binaryBytes.empty(),
        "compatibility loader must not relabel partial tracks as binary bytes");
}
}

int main()
{
    testLoadsNumericTextTables();
    testNumericTableHashUsesExactCrLfBytes();
    testLoadsMonoAndStereoPcm16WavTables();
    testLoadsWavAndAiffTablesOfOtherWordSizes();
    testLoadsIrcamSoundFiles();
    testLoadsNonEmptyBinaryFiles();
    testResourcePayloadReadsAreCooperativelyCancellable();
    testResourceParsingAndHashingAreCooperativelyCancellable();
    testLoadsOnlyReferencedProjectAssetsRelativeToProjectFile();
    testMarkedWavMetadataSurvivesProjectSnapshotLoading();
    testSampleMarkersSurviveLoading();
    testLoadsAKeyMappedMultisampleBankFromItsFolder();
    testLoadsStaticMultisampleTextListAssets();
    testLoadsDescriptorAuthorizedMultifileDiskPlayerTextListAssets();
    testLoadsTheFilesADiskSplicerSplices();
    testRejectsUnboundedMultisampleTextListsBeforeAssetLookup();
    testProjectAssetFailureCarriesReferenceContext();
    testReachabilityFailureDoesNotMasqueradeAsNoAssets();
    testLoadsAssetsResolvedInsideReachableSoundDefinitions();
    testReachableSoundAssetFailureCarriesStructuralSourcePath();
    testProjectResourceSnapshotsPinAcceptedGenerations();
    testLoadsCanonicalAudioSnapshotWithoutFullPayload();
    testCapturedAudioSnapshotRetainsTheExactInspectedBytes();
    testProjectSnapshotLoadingPreservesTypedPayloadCancellation();
    testLoadsCanonicalPartialTrackSnapshotWithoutMislabelingIt();
    return 0;
}
