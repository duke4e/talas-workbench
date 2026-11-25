#include "talas/cli/Cli.h"

#include "talas/audio/CdpProcessToSample.h"
#include "talas/audio/ProjectPackager.h"
#include "talas/audio/ResourceLoader.h"
#include "talas/audio/WavReader.h"
#include "talas/audio/WavWriter.h"
#include "talas/dsp/ReResonator.h"
#include "talas/project/ProjectDiskCache.h"
#include "talas/project/ProjectFile.h"
#include "talas/project/ProjectPathEncoding.h"
#include "talas/project/ProjectJson.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <numbers>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

int run(std::initializer_list<std::string_view> args, std::ostream& output, std::ostream& error)
{
    const std::vector<std::string_view> stableArgs(args);
    return talas::cli::runCli(stableArgs, output, error);
}

std::vector<std::string> nonEmptyLines(const std::string& text)
{
    std::istringstream input(text);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);)
    {
        if (!line.empty())
            lines.push_back(std::move(line));
    }
    return lines;
}

std::filesystem::path writeProject(std::string_view json)
{
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path()
        / ("talas-cli-test-" + std::to_string(now) + "-" + std::to_string(counter++) + ".json");

    std::ofstream output(path);
    require(output.good(), "test should create temporary project file");
    output << json;
    output.close();
    require(output.good(), "test should write temporary project file");
    return path;
}

std::filesystem::path writeTextTable(std::string_view text)
{
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path()
        / ("talas-cli-table-" + std::to_string(now) + "-" + std::to_string(counter++) + ".txt");

    std::ofstream output(path);
    require(output.good(), "test should create temporary table file");
    output << text;
    output.close();
    require(output.good(), "test should write temporary table file");
    return path;
}

std::filesystem::path writeBinaryFile(const std::vector<std::uint8_t>& bytes)
{
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path()
        / ("talas-cli-binary-" + std::to_string(now) + "-" + std::to_string(counter++) + ".bin");

    std::ofstream output(path, std::ios::binary);
    require(output.good(), "test should create temporary binary file");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "test should write temporary binary file");
    return path;
}

std::filesystem::path temporaryWavPath()
{
    static int counter = 0;
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("talas-cli-render-" + std::to_string(now) + "-" + std::to_string(counter++) + ".wav");
}

std::vector<unsigned char> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "test should open rendered WAV file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::uint16_t readU16(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    require(offset + 2 <= bytes.size(), "test should read u16 inside rendered WAV file");
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

std::uint32_t readU32(const std::vector<unsigned char>& bytes, std::size_t offset)
{
    require(offset + 4 <= bytes.size(), "test should read u32 inside rendered WAV file");
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void requireAscii(const std::vector<unsigned char>& bytes, std::size_t offset, std::string_view expected)
{
    require(offset + expected.size() <= bytes.size(), "test should read ascii field inside rendered WAV file");
    require(std::equal(expected.begin(), expected.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset)),
            "rendered WAV field should match " + std::string(expected));
}

std::string monoProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Mono Render"
  },
  "graph": {
    "nodes": [
      {
        "id": "out",
        "typeId": "talas.audioOutput",
        "parameters": []
      },
      {
        "id": "osc",
        "typeId": "talas.oscillator",
        "parameters": [
          {
            "id": "frequency",
            "expression": "1 hz"
          },
          {
            "id": "amplitude",
            "expression": "1.0"
          }
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "osc",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "audioIn"
      }
    ]
  }
})json";
}

void testDiskCacheInspectionAndExplicitCapture()
{
    const auto now = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path()
        / ("talas-cli-disk-cache-" + std::to_string(now));
    std::filesystem::create_directories(directory);
    const auto projectPath = directory / "capture.talas.json";
    talas::project::ProjectDocument document;
    document.timeline.loop = {true, 0.002, 0.006};
    document.graph.nodes = {
        {"osc", "talas.oscillator",
         {{"frequency", std::string{"1000 hz"}},
          {"amplitude", std::string{"1"}}}, {}},
        {"cache", "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Smoke/Cache"}},
          {"record", {}, {}, true}}, {}},
        {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    {
        std::ofstream file(projectPath);
        file << talas::project::serializeProjectDocument(document);
        require(file.good(), "CLI DiskCache fixture should be written");
    }
    const auto collisionDirectory = directory / "DiskCache";
    std::filesystem::create_directories(collisionDirectory);
    const auto collisionPath = collisionDirectory / "Smoke_Cache-g1.wav";
    const std::vector<unsigned char> collisionSentinel{0x4b, 0x43, 0x31, 0x31};
    {
        std::ofstream file(collisionPath, std::ios::binary);
        file.write(
            reinterpret_cast<const char*>(collisionSentinel.data()),
            static_cast<std::streamsize>(collisionSentinel.size()));
        require(file.good(), "CLI DiskCache collision sentinel should be written");
    }
    std::ostringstream inspectOutput;
    std::ostringstream inspectError;
    const std::vector<std::string> inspectStorage{
        "inspect-disk-cache", "--project", projectPath.string(),
        "--source-path", "cache", "--sample-rate", "8000"};
    std::vector<std::string_view> inspectArgs;
    for (const auto& value : inspectStorage) inspectArgs.push_back(value);
    require(
        talas::cli::runCli(inspectArgs, inspectOutput, inspectError) == 0
            && inspectError.str().empty(),
        "CLI should inspect exact DiskCache capture authority");
    requireContains(
        inspectOutput.str(), "frames=32 fingerprint=",
        "CLI inspection should expose exact range and fingerprint");

    std::ostringstream captureOutput;
    std::ostringstream captureError;
    const std::vector<std::string> captureStorage{
        "capture-disk-cache", "--project", projectPath.string(),
        "--source-path", "cache", "--asset-id", "Smoke/Cache",
        "--sample-rate", "8000", "--max-block-frames", "7"};
    std::vector<std::string_view> captureArgs;
    for (const auto& value : captureStorage) captureArgs.push_back(value);
    require(
        talas::cli::runCli(captureArgs, captureOutput, captureError) == 0
            && captureError.str().empty(),
        "explicit CLI DiskCache capture should succeed: "
            + captureError.str());
    const auto loaded = talas::project::loadProjectFile(projectPath);
    require(loaded.ok(), "CLI capture should save the committed project");
    const auto asset = std::ranges::find(
        loaded.document->assets, std::string_view{"Smoke/Cache"},
        &talas::project::ProjectAsset::id);
    require(
        asset != loaded.document->assets.end()
            && asset->generation == 1
            && asset->frameCount == 32
            && talas::project::diskCacheAssetAuthority(*asset)
                == talas::project::DiskCacheAssetAuthority::CapturedPlayback
            && std::filesystem::is_regular_file(
                directory / talas::project::pathFromUtf8(asset->path))
            && directory / talas::project::pathFromUtf8(asset->path)
                != collisionPath
            && readBytes(collisionPath) == collisionSentinel,
        "CLI capture should choose a collision-safe file and preserve prior bytes");
    std::filesystem::remove_all(directory);
}

void testDiskCacheCaptureSaveFailureLeavesNoPublishedArtifact()
{
    const auto now = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path()
        / ("talas-cli-disk-cache-save-failure-" + std::to_string(now));
    std::filesystem::create_directories(directory);
    const auto projectPath = directory / "capture.talas.json";
    talas::project::ProjectDocument document;
    document.timeline.loop = {true, 0.002, 0.006};
    document.graph.nodes = {
        {"osc", "talas.oscillator",
         {{"frequency", std::string{"1000 hz"}},
          {"amplitude", std::string{"1"}}}, {}},
        {"cache", "talas.diskCache",
         {{"fileName", {}, {}, {}, std::string{"Smoke/Cache"}},
          {"record", {}, {}, true}}, {}},
        {"out", "talas.audioOutput", {}, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "cache", "audioIn"},
        {"cache", "audioOut", "out", "audioIn"},
    };
    {
        std::ofstream file(projectPath);
        file << talas::project::serializeProjectDocument(document);
    }
    // The WAV lands in a writable DiskCache folder, but the project folder
    // itself refuses the save's staging directory.
    std::filesystem::create_directories(directory / "DiskCache");
    std::filesystem::permissions(
        directory,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);

    std::ostringstream output;
    std::ostringstream error;
    const std::vector<std::string> storage{
        "capture-disk-cache", "--project", projectPath.string(),
        "--source-path", "cache", "--asset-id", "Smoke/Cache",
        "--sample-rate", "8000", "--max-block-frames", "7"};
    std::vector<std::string_view> args;
    for (const auto& value : storage) args.push_back(value);
    const auto captured = talas::cli::runCli(args, output, error);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    require(
        captured != 0,
        "project-save failure fixture should reject CLI capture");
    std::size_t wavFiles = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator{
             directory})
    {
        if (entry.is_regular_file()
            && entry.path().extension() == ".wav")
        {
            ++wavFiles;
        }
    }
    require(
        wavFiles == 0,
        "failed CLI project publication must not retain an orphan DiskCache WAV");
    std::filesystem::remove_all(directory);
}

std::string missingComponentProjectJson()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode missing;
    missing.instanceId = "missing1";
    missing.typeId = "org.example.component";
    talas::project::ProjectPreservedComponent component;
    component.typeVersion = "1.2.3";
    component.stateSchemaVersion = 4;
    component.state = {0x00, 0xff, 0x41};
    component.displayName = "Unavailable example";
    component.bundleId = "org.example.component";
    const auto monoAudio = talas::core::PortSignalContract{
        talas::core::PortSignalRate::Sample,
        talas::core::AudioBusLayoutKind::Mono,
        talas::core::PortEdgeSemantics::Continuous,
        talas::core::SpectralFrameFormat::NotApplicable,
        true,
    };
    component.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        false,
        monoAudio,
    });
    component.recoveryHint =
        "Install the matching owner-approved bundle";
    missing.preservedComponent = std::move(component);

    document.graph.nodes = {
        std::move(missing),
        {"out", "talas.audioOutput", {}},
    };
    document.graph.connections = {
        {"missing1", "audioOut", "out", "audioIn"},
    };
    return talas::project::serializeProjectDocument(document);
}

std::string packedFftProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Packed FFT Status"
  },
  "graph": {
    "nodes": [
      {
        "id": "out",
        "typeId": "talas.audioOutput",
        "parameters": []
      },
      {
        "id": "fft",
        "typeId": "talas.fft",
        "parameters": [
          {"id": "length", "expression": "16"},
          {"id": "inverse", "boolean": false}
        ]
      },
      {
        "id": "osc",
        "typeId": "talas.oscillator",
        "parameters": [
          {"id": "frequency", "expression": "440 hz"},
          {"id": "amplitude", "expression": "1"}
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "osc",
        "fromPort": "audioOut",
        "toNode": "fft",
        "toPort": "audioIn"
      },
      {
        "fromNode": "fft",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "audioIn"
      }
    ]
  }
})json";
}

std::string eventValueConstantVcaProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Event Value Constant VCA"
  },
  "graph": {
    "nodes": [
      {
        "id": "out",
        "typeId": "talas.audioOutput",
        "parameters": []
      },
      {
        "id": "vca",
        "typeId": "talas.vca",
        "parameters": []
      },
      {
        "id": "osc",
        "typeId": "talas.oscillator",
        "parameters": [
          {
            "id": "frequency",
            "expression": "1 hz"
          },
          {
            "id": "amplitude",
            "expression": "1"
          }
        ]
      },
      {
        "id": "gain",
        "typeId": "talas.constant",
        "parameters": [
          {
            "id": "value",
            "expression": "!Gain"
          }
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "osc",
        "fromPort": "audioOut",
        "toNode": "vca",
        "toPort": "audioIn"
      },
      {
        "fromNode": "gain",
        "fromPort": "value",
        "toNode": "vca",
        "toPort": "gain"
      },
      {
        "fromNode": "vca",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "audioIn"
      }
    ]
  }
})json";
}

std::string eventValuePitchProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Event Value Pitch"
  },
  "graph": {
    "nodes": [
      {
        "id": "out",
        "typeId": "talas.audioOutput",
        "parameters": []
      },
      {
        "id": "osc",
        "typeId": "talas.oscillator",
        "parameters": [
          {
            "id": "frequency",
            "expression": "!Pitch"
          },
          {
            "id": "amplitude",
            "expression": "1"
          }
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "osc",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "audioIn"
      }
    ]
  }
})json";
}

std::string stereoProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Stereo Render"
  },
  "graph": {
    "nodes": [
      {
        "id": "out",
        "typeId": "talas.audioOutput",
        "parameters": []
      },
      {
        "id": "left",
        "typeId": "talas.oscillator",
        "parameters": [
          {
            "id": "frequency",
            "expression": "1 hz"
          },
          {
            "id": "amplitude",
            "expression": "1.0"
          }
        ]
      },
      {
        "id": "right",
        "typeId": "talas.oscillator",
        "parameters": [
          {
            "id": "frequency",
            "expression": "0.5 hz"
          },
          {
            "id": "amplitude",
            "expression": "2.0"
          }
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "left",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "audioIn"
      },
      {
        "fromNode": "right",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "rightIn"
      }
    ]
  }
})json";
}

std::string midiControllerProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI MIDI Controller Render"
  },
  "graph": {
    "nodes": [
      {
        "id": "midi",
        "typeId": "talas.midiOutputController",
        "parameters": [
          {
            "id": "channel",
            "expression": "3"
          },
          {
            "id": "controllerNumber",
            "expression": "10"
          },
          {
            "id": "value",
            "text": "127"
          }
        ]
      }
    ],
    "connections": []
  }
})json";
}

std::string midiFileEchoProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI MIDI File Echo Render"
  },
  "graph": {
    "nodes": [
      {
        "id": "midi",
        "typeId": "talas.midiFileEcho",
        "parameters": [
          {
            "id": "lowChannel",
            "expression": "1"
          },
          {
            "id": "highChannel",
            "expression": "16"
          },
          {
            "id": "fileName",
            "resource": "Sequences/Part.mid"
          }
        ]
      }
    ],
    "connections": []
  }
})json";
}

std::string globalControllerProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Global Controller Render"
  },
  "graph": {
    "nodes": [
      {
        "id": "global",
        "typeId": "talas.soundToGlobalController",
        "parameters": [
          {
            "id": "generatedEvent",
            "text": "!Generated"
          },
          {
            "id": "value",
            "text": "0.75"
          }
        ]
      }
    ],
    "connections": []
  }
})json";
}

std::string sampleResourceProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Sample Resource Render"
  },
  "graph": {
    "nodes": [
      {
        "id": "out",
        "typeId": "talas.audioOutput",
        "parameters": []
      },
      {
        "id": "sample",
        "typeId": "talas.sample",
        "parameters": [
          {
            "id": "frequency",
            "expression": "4 c"
          },
          {
            "id": "gate",
            "expression": "1.0"
          },
          {
            "id": "sample",
            "resource": "Samples/Hit"
          },
          {
            "id": "setLoop",
            "boolean": false
          },
          {
            "id": "loopStart",
            "expression": "0.0"
          },
          {
            "id": "loopEnd",
            "expression": "1.0"
          },
          {
            "id": "loopFade",
            "boolean": false
          },
          {
            "id": "start",
            "expression": "0.0"
          },
          {
            "id": "end",
            "expression": "1.0"
          },
          {
            "id": "fromMemoryWriter",
            "boolean": false
          },
          {
            "id": "attackTime",
            "expression": "0 s"
          },
          {
            "id": "releaseTime",
            "expression": "0 s"
          },
          {
            "id": "scale",
            "expression": "1.0"
          }
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "sample",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "audioIn"
      }
    ]
  }
})json";
}

std::string audioInputProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Audio Input Render"
  },
  "graph": {
    "nodes": [
      {
        "id": "out",
        "typeId": "talas.audioOutput",
        "parameters": []
      },
      {
        "id": "input",
        "typeId": "talas.audioInput",
        "parameters": [
          {
            "id": "channel1",
            "boolean": true
          },
          {
            "id": "channel2",
            "boolean": false
          },
          {
            "id": "channel3",
            "boolean": false
          },
          {
            "id": "channel4",
            "boolean": false
          },
          {
            "id": "channel5",
            "boolean": false
          },
          {
            "id": "channel6",
            "boolean": false
          },
          {
            "id": "channel7",
            "boolean": false
          },
          {
            "id": "channel8",
            "boolean": false
          }
        ]
      }
    ],
    "connections": [
      {
        "fromNode": "input",
        "fromPort": "audioOut",
        "toNode": "out",
        "toPort": "audioIn"
      }
    ]
  }
})json";
}

std::string noOutputProjectJson()
{
    return R"json({
  "format": "talas.project",
  "version": 1,
  "metadata": {
    "title": "CLI Compile Failure"
  },
  "graph": {
    "nodes": [
      {
        "id": "osc",
        "typeId": "talas.oscillator",
        "parameters": [
          {
            "id": "frequency",
            "expression": "1 hz"
          }
        ]
      }
    ],
    "connections": []
  }
})json";
}

// ADR-0029: the export itself is covered by the plugin tests; here, its usage.
void testExportPluginReportsUsageErrors()
{
    const auto expectUsage = [](std::initializer_list<std::string_view> args, std::string_view message) {
        std::ostringstream output;
        std::ostringstream error;
        require(run(args, output, error) == 2, "export-plugin should report a usage error: " + std::string{message});
        requireContains(error.str(), message, "export-plugin should explain: " + error.str());
        require(output.str().empty(), "a usage error exports nothing");
    };
    expectUsage({"export-plugin", "--project", "x.talas.json"}, "requires --project, --name, --kind and --output");
    expectUsage({"export-plugin", "--project", "x.talas.json", "--name", "N", "--kind", "synth", "--output", "out"},
        "--kind must be instrument or effect");
    expectUsage({"export-plugin", "--project", "x.talas.json", "--name", "N", "--kind", "effect", "--output", "out",
                    "--format", "vst3,vst2"},
        "--format takes vst3, au or both");
    expectUsage({"export-plugin", "--project", "x.talas.json", "--name", "N", "--kind", "effect", "--output", "out",
                    "--colour", "blue"},
        "Unknown export-plugin option");
    expectUsage({"export-plugin", "--name"}, "--name requires a value");

    std::ostringstream output;
    std::ostringstream error;
    const auto missing = std::filesystem::temp_directory_path() / "talas-cli-no-such-project.talas.json";
    require(run({"export-plugin", "--project", missing.string(), "--name", "N", "--kind", "effect", "--output",
                    (std::filesystem::temp_directory_path() / "talas-cli-export-unused").string(), "--plugin-code",
                    "bad"},
                output, error)
            == 1,
        "export-plugin should fail on an invalid plugin code");
    requireContains(error.str(), "plugin code 'bad'", "export-plugin should name the bad code: " + error.str());
}

void testHelpPrintsUsage()
{
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"--help"}, output, error);

    require(exitCode == 0, "help should return success");
    requireContains(output.str(), "Usage:", "help should print usage");
    requireContains(output.str(), "catalog", "help should mention catalog command");
    requireContains(output.str(), "graph", "help should mention graph command");
    requireContains(output.str(), "export-plugin", "help should mention export-plugin command");
    requireContains(output.str(), "validate", "help should mention validate command");
    requireContains(output.str(), "render", "help should mention render command");
    requireContains(output.str(), "--project", "help should mention project option");
    requireContains(output.str(), "--output", "help should mention output option");
    requireContains(output.str(), "--table", "help should mention table resource option");
    requireContains(output.str(), "--audio-input", "help should mention audio input option");
    requireContains(output.str(), "--status", "help should mention prepared graph status validation");
    requireContains(output.str(), "prepared graph", "help should explain prepared graph status output");
    requireContains(output.str(), "in place of the value the project saved for its control",
        "help should say that --event wins over a saved control value");
    require(error.str().empty(), "help should not write errors");
}

void testCatalogWritesJson()
{
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"catalog", "--format", "json"}, output, error);

    require(exitCode == 0, "catalog JSON should return success");
    require(error.str().empty(), "catalog JSON should not write errors");
    requireContains(output.str(), R"json("nodes": [)json", "catalog JSON should include nodes array");
    requireContains(output.str(), R"json("typeId": "talas.oscillator")json", "catalog JSON should include oscillator");
    requireContains(output.str(), R"json("kind": "expression")json", "catalog JSON should include parameter kinds");
    requireContains(output.str(), R"json("preparedRealtime": "subset")json", "catalog JSON should include prepared realtime support");
    requireContains(output.str(), R"json("preparedRealtimeBinding": "legacyAdapter")json", "catalog JSON should include the implementation binding");
    requireContains(output.str(), R"json("offline-functional-subset")json", "catalog JSON should include stable limitation IDs");
}

void testCatalogRejectsUnsupportedFormat()
{
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"catalog", "--format", "yaml"}, output, error);

    require(exitCode == 2, "unsupported catalog format should return usage error");
    require(output.str().empty(), "unsupported catalog format should not write output");
    requireContains(error.str(), "Unsupported catalog format", "unsupported catalog format should explain failure");
}

void testGraphWritesDot()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"graph", "--project", projectPath.string(), "--format", "dot"}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "DOT graph export should return success: " + error.str());
    require(error.str().empty(), "DOT graph export should not write errors");
    requireContains(output.str(), "digraph Talas {", "DOT graph should include graph header");
    requireContains(output.str(), R"dot("osc" [label="osc\ntalas.oscillator"];)dot", "DOT graph should include oscillator node");
    requireContains(output.str(), R"dot("out" [label="out\ntalas.audioOutput"];)dot", "DOT graph should include output node");
    requireContains(output.str(), R"dot("osc" -> "out" [label="audioOut -> audioIn"];)dot", "DOT graph should include edge label");
}

void testGraphLabelsReusableSoundInstances()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectNode instance;
    instance.instanceId = "voice1";
    instance.soundDefinitionId = "voice";
    document.graph.nodes.push_back(std::move(instance));
    const auto projectPath = writeProject(
        talas::project::serializeProjectDocument(document));
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {
            "graph",
            "--project",
            projectPath.string(),
            "--format",
            "dot",
        },
        output,
        error);

    std::filesystem::remove(projectPath);
    require(
        exitCode == 0,
        "DOT reusable Sound graph export should return success: "
            + error.str());
    requireContains(
        output.str(),
        R"dot("voice1" [label="voice1\nSound: voice"];)dot",
        "DOT graph should label reusable Sound identity explicitly");
}

void testGraphRejectsUnsupportedFormat()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"graph", "--project", projectPath.string(), "--format", "json"}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 2, "unsupported graph format should return usage error");
    require(output.str().empty(), "unsupported graph format should not write output");
    requireContains(error.str(), "Unsupported graph format", "unsupported graph format should explain failure");
}

void testInitWritesOscillatorProject()
{
    const auto outputPath = std::filesystem::temp_directory_path()
        / ("talas-cli-init-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".talas.json");
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"init", "--template", "oscillator", "--output", outputPath.string()}, output, error);

    require(exitCode == 0, "init oscillator should return success: " + error.str());
    require(error.str().empty(), "init oscillator should not write errors");
    requireContains(output.str(), "Created ", "init oscillator should report created file");

    std::ifstream input(outputPath);
    require(input.good(), "init oscillator should create output file");
    std::ostringstream content;
    content << input.rdbuf();
    std::filesystem::remove(outputPath);

    const auto loaded = talas::project::parseProjectDocument(content.str());
    require(loaded.ok(), "init oscillator output should parse as project JSON");
    require(loaded.document.metadata.title == "Oscillator Starter", "init oscillator should set starter title");
    require(loaded.document.graph.nodes.size() == 2, "init oscillator should create two nodes");
    require(loaded.document.graph.nodes[0].typeId == "talas.audioOutput", "init oscillator should create audio output");
    require(loaded.document.graph.nodes[1].typeId == "talas.oscillator", "init oscillator should create oscillator");
    require(loaded.document.graph.connections.size() == 1, "init oscillator should connect oscillator to output");
}

void testInitRejectsUnsupportedTemplate()
{
    const auto outputPath = std::filesystem::temp_directory_path()
        / ("talas-cli-init-unsupported-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".talas.json");
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"init", "--template", "granulator", "--output", outputPath.string()}, output, error);

    require(exitCode == 2, "unsupported init template should return usage error");
    require(output.str().empty(), "unsupported init template should not write output");
    require(!std::filesystem::exists(outputPath), "unsupported init template should not create file");
    requireContains(error.str(), "Unsupported init template", "unsupported init template should explain failure");
}

void testInitDoesNotOverwriteExistingFile()
{
    const auto outputPath = std::filesystem::temp_directory_path()
        / ("talas-cli-init-existing-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".talas.json");
    {
        std::ofstream existing(outputPath);
        existing << "keep";
    }
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"init", "--template", "oscillator", "--output", outputPath.string()}, output, error);

    std::ifstream input(outputPath);
    std::ostringstream content;
    content << input.rdbuf();
    std::filesystem::remove(outputPath);
    require(exitCode == 1, "init existing output should return failure");
    require(output.str().empty(), "init existing output should not write success output");
    require(content.str() == "keep", "init existing output should not overwrite file");
    requireContains(error.str(), "already exists", "init existing output should explain overwrite refusal");
}

void testFilesystemErrorsAndLostOutputFailCleanly()
{
    const auto root = std::filesystem::temp_directory_path()
        / ("talas-cli-unreadable-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    std::filesystem::permissions(root, std::filesystem::perms::none);
    std::ostringstream output;
    std::ostringstream error;
    int exitCode = -1;
    try
    {
        exitCode = run({"init", "--template", "oscillator", "--output", (root / "child" / "Project.talas.json").string()},
            output, error);
    }
    catch (const std::exception&)
    {
        exitCode = -2;
    }
    std::filesystem::permissions(root, std::filesystem::perms::owner_all);
    std::filesystem::remove_all(root);
    require(exitCode == 1, "init into an unreadable directory should fail instead of throwing");
    require(!error.str().empty(), "init into an unreadable directory should explain the failure");

    struct LostOutput : std::streambuf
    {
        int_type overflow(int_type) override { return traits_type::eof(); }
    } lost;
    std::ostream closed(&lost);
    std::ostringstream lostError;
    require(run({"--help"}, closed, lostError) == 1,
        "a command whose output cannot be written must not report success");
}

void testCliRejectsAmbiguousNumbersAndNamesInvalidEventValues()
{
    const auto projectPath = writeProject(monoProjectJson());
    const auto project = projectPath.string();
    const auto wavPath = std::filesystem::temp_directory_path()
        / ("talas-cli-precise-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())
            + ".wav");
    const auto wav = wavPath.string();
    const auto fails = [](int exitCode, const std::ostringstream& error,
                          std::string_view expected, const std::string& message) {
        require(exitCode != 0
                && error.str().find(expected) != std::string::npos,
            message + " (stderr: " + error.str() + ")");
    };
    {
        std::ostringstream output;
        std::ostringstream error;
        fails(run({"render", "--project", project, "--frames", "4",
                  "--sample-rate", "0x10"}, output, error),
            error, "--sample-rate",
            "a hexadecimal sample rate must be rejected");
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        fails(run({"render", "--project", project, "--frames", "4",
                  "--output", wav, "--format", "json"}, output, error),
            error, "--format",
            "--format must not be silently ignored with --output");
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        fails(run({"render", "--project", project, "--frames", "4",
                  "--sample-rate", "8.5", "--output", wav}, output, error),
            error, "whole number",
            "a fractional sample rate must not be truncated into a WAV header");
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        fails(run({"render", "--project", project, "--frames", "4",
                  "--event", "Gain=1", "--event", "Gain=2"}, output, error),
            error, "Gain is bound more than once",
            "a duplicate Event Value should be named");
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        fails(run({"render", "--project", project, "--frames", "4",
                  "--event-change", "2:Missing=1"}, output, error),
            error, "Missing at frame 2",
            "an Event Value change without a binding should be named");
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        fails(run({"render", "--project", project,
                  "--duration", "18446744073709551616",
                  "--sample-rate", "1"}, output, error),
            error, "too many frames",
            "2^64 frames must be rejected before the size conversion");
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        fails(run({"inspect-disk-cache", "--project", project,
                  "--source-path", "missing", "--sample-rate", "44100.5"},
                  output, error),
            error, "whole number",
            "a DiskCache sample rate must not be truncated to an integer");
    }
    std::filesystem::remove(wavPath);
    std::filesystem::remove(projectPath);
}

void testValidateProjectReturnsOk()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"validate", "--project", projectPath.string()}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "valid project validation should return success: " + error.str());
    require(output.str() == "OK\n", "valid project validation should print OK");
    require(error.str().empty(), "valid project validation should not write errors");
}

void testValidateStatusReportsPreparedM3Contract()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"validate", "--project", projectPath.string(), "--status"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "prepared status validation should succeed: " + error.str());
    require(error.str().empty(), "prepared status validation should not write errors");
    requireContains(output.str(), "OK\nPrepared graph status\n", "status output should retain the ordinary OK result before the report");
    requireContains(output.str(), "sample-rate: 48000 Hz", "status should expose prepared sample rate");
    requireContains(output.str(), "max-block: 512 frames", "status should expose prepared block bound");
    requireContains(output.str(), "outputs: mono (1)", "status should expose logical output layout and count");
    requireContains(output.str(), "device=not-attached", "CLI status should state that no physical device host is attached");
    requireContains(output.str(), "disk-recorder: none", "ordinary status should state that no Disk Recorder capture is prepared");
    requireContains(output.str(), "critical-path: 0 frames", "status should expose exact critical-path latency");
    requireContains(output.str(), "terminal-output: 0 frames", "status should expose exact terminal latency");
    requireContains(output.str(), "compensations: none", "status should expose absence of compensation explicitly");
    requireContains(output.str(), "spectral-nodes: none", "status should expose absence of spectral frames explicitly");
    requireContains(output.str(), "resources: none", "status should expose absence of prepared resources explicitly");
    requireContains(output.str(), "stream-contracts: none", "status should expose absence of streams explicitly");
    requireContains(output.str(), "midi-mappers: none", "status should expose absence of MIDI mappers explicitly");
    requireContains(output.str(), "logical-processors: 1 mode=portable-serial", "status should expose the implicit host placement domain");
    requireContains(output.str(), "processor 0 steps=2 boundaries=0", "host-only status should publish exact step and boundary counts");
    requireContains(output.str(), "processor-transfers: none", "host-only status should expose absence of cross-domain transfers");
}

void testValidateStatusReportsWritableDiskRecorderAuthority()
{
    talas::project::ProjectParameterExpression fileName;
    fileName.parameterId = "fileName";
    fileName.resource = "Recordings/CLI Capture";
    talas::project::ProjectParameterExpression format;
    format.parameterId = "format";
    format.text = "WAV";
    talas::project::ProjectParameterExpression wordSize;
    wordSize.parameterId = "wordSize";
    wordSize.text = "16";
    talas::project::ProjectParameterExpression channels;
    channels.parameterId = "channels";
    channels.choice = "mono";
    talas::project::ProjectParameterExpression gated;
    gated.parameterId = "gated";
    gated.boolean = false;

    talas::project::ProjectDocument document;
    document.graph.nodes = {
        {"recorder", "talas.diskRecorder",
         {fileName, format, wordSize, channels,
          {"trigger", std::string{"1"}},
          {"captureDuration", std::string{"0.25 s"}}, gated}},
        {"osc", "talas.oscillator",
         {{"frequency", std::string{"100 hz"}},
          {"amplitude", std::string{"0.5"}}}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "recorder", "audioIn"},
    };
    const auto projectPath = writeProject(
        talas::project::serializeProjectDocument(document));
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"validate", "--project", projectPath.string(), "--status"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0,
        "Disk Recorder prepared status should succeed: " + error.str());
    require(error.str().empty(),
        "Disk Recorder prepared status should not write errors");
    requireContains(
        output.str(),
        "disk-recorder: writable node=recorder scope=0 buffer=0 profile=mono/WAV/16",
        "CLI should publish the exact writable Disk Recorder profile");
    requireContains(
        output.str(),
        "duration=0.25s gated=false owner-action=required path-authority=none name-hint=Recordings/CLI Capture",
        "CLI should state duration, explicit owner action, and absent path authority");
}

void testValidateStatusReportsNestedProcessorPlacement()
{
    talas::project::ProjectDocument document;
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}});
    document.graph.nodes.push_back({
        "outer", "talas.forcedProcessorAssignment", {{"processor", "7"}}});
    document.graph.nodes.push_back({
        "inner", "talas.forcedProcessorAssignment", {{"processor", "3"}}});
    document.graph.nodes.push_back({"osc", "talas.oscillator", {}});
    document.graph.connections.push_back(
        {"osc", "audioOut", "inner", "audioIn"});
    document.graph.connections.push_back(
        {"inner", "audioOut", "outer", "audioIn"});
    document.graph.connections.push_back(
        {"outer", "audioOut", "out", "audioIn"});
    const auto projectPath = writeProject(
        talas::project::serializeProjectDocument(document));
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"validate", "--project", projectPath.string(), "--status"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(
        exitCode == 0,
        "nested placement status should succeed: " + error.str());
    require(error.str().empty(), "nested placement status should not write errors");
    requireContains(output.str(), "logical-processors: 3 mode=portable-serial", "nested status should expose host plus two logical domains");
    requireContains(output.str(), "processor 3 steps=2 boundaries=1", "nested status should expose the inner closure");
    requireContains(output.str(), "boundary inner [inner] scope=0", "nested status should expose exact inner boundary identity");
    requireContains(output.str(), "processor 7 steps=1 boundaries=1", "nested status should expose the outer boundary");
    requireContains(output.str(), "processor-transfers: 2", "nested status should expose both transfers");
    requireContains(output.str(), "inner.audioOut [inner] scope=0 processor=3 -> outer.audioIn [outer] scope=0 processor=7", "nested status should expose the exact inner-to-outer transfer");
}

void testValidateStatusKeepsPackedAudioDistinctFromComplexFrames()
{
    const auto projectPath = writeProject(packedFftProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"validate", "--project", projectPath.string(), "--status",
         "--sample-rate", "48000", "--max-block-frames", "64"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "packed FFT status validation should succeed: " + error.str());
    require(error.str().empty(), "packed FFT status should not write errors");
    requireContains(output.str(), "fft [fft]", "spectral report should retain node and structural path");
    requireContains(output.str(), "format=PackedRealAudioV1", "historical FFT must report its packed Audio format");
    requireContains(output.str(), "role=forward", "historical FFT should report transform direction");
    requireContains(output.str(), "N=16 H=16 window=rectangular cadence=3000 Hz", "packed FFT should report exact frame timing");
    requireContains(output.str(), "limitation=packed-real-audio-compatibility-layout", "packed FFT should report its stable compatibility limitation");
    require(output.str().find("format=ComplexSpectrumV1") == std::string::npos,
        "packed Audio status must never masquerade as complex frames");
}

void testValidateProjectReportsCompileFailure()
{
    const auto projectPath = writeProject(noOutputProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"validate", "--project", projectPath.string()}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 1, "compile-invalid project validation should fail");
    require(output.str().empty(), "compile-invalid project validation should not write output");
    requireContains(error.str(), "Compile failed", "compile-invalid validation should identify compile stage");
    requireContains(error.str(), "MissingAudioOutput", "compile-invalid validation should include diagnostic code");
}

void testValidateDistinguishesPortableMissingComponentFromExecution()
{
    const auto projectPath =
        writeProject(missingComponentProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"validate",
         "--project",
         projectPath.string(),
         "--table",
         "UnreachableAsset=/definitely/not/a/real/resource.table"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(
        exitCode == 1,
        "missing-component validation should remain nonzero for execution");
    require(
        output.str()
            == "Portable document preserved: yes\nExecutable in this registry: no\n",
        "validate should distinguish preserved portability from executability; output="
            + output.str() + "; error=" + error.str());
    requireContains(
        error.str(),
        "Compile failed",
        "missing-component validation should identify the execution gate");
    requireContains(
        error.str(),
        "missing1",
        "missing-component validation should retain root scope");
    requireContains(
        error.str(),
        "org.example.component",
        "missing-component validation should name the unavailable type before touching assets");
    require(
        error.str().find("UnreachableAsset") == std::string::npos,
        "missing-component validation must not resolve unrelated assets first");
}

void testRenderMissingComponentWritesNoOutputOrWav()
{
    const auto projectPath =
        writeProject(missingComponentProjectJson());
    const auto wavPath = temporaryWavPath();
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render",
         "--project",
         projectPath.string(),
         "--frames",
         "4",
         "--output",
         wavPath.string(),
         "--table",
         "UnreachableAsset=/definitely/not/a/real/resource.table"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 1, "missing-component render should fail");
    require(
        output.str().empty(),
        "missing-component render should publish no stream output");
    require(
        !std::filesystem::exists(wavPath),
        "missing-component render should publish no partial WAV");
    requireContains(
        error.str(),
        "org.example.component",
        "missing-component render should report the unavailable component");
    require(
        error.str().find("UnreachableAsset") == std::string::npos,
        "missing-component render must fail before unrelated asset loading");
}

void testValidateProjectReportsRenderResourceFailure()
{
    const auto projectPath = writeProject(sampleResourceProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"validate", "--project", projectPath.string()}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 1, "render-invalid project validation should fail");
    require(output.str().empty(), "render-invalid project validation should not write output");
    requireContains(error.str(), "Render failed", "render-invalid validation should identify render stage");
    requireContains(error.str(), "sample", "render-invalid validation should include resource parameter");
}

void testValidateReportsMalformedMidiBinaryResource()
{
    const auto projectPath = writeProject(midiFileEchoProjectJson());
    const auto midiPath = writeBinaryFile({0x00, 0x01, 0x02});
    std::ostringstream output;
    std::ostringstream error;
    const std::string resourceArgument = "Sequences/Part.mid=" + midiPath.string();

    const int exitCode = run(
        {"validate", "--project", projectPath.string(), "--binary-resource", resourceArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(midiPath);
    require(exitCode == 1, "malformed MIDI binary resource validation should fail");
    require(output.str().empty(), "malformed MIDI binary resource validation should not print OK");
    requireContains(error.str(), "Render failed", "malformed MIDI validation should identify render validation stage");
    requireContains(error.str(), "failed to parse MIDI file", "malformed MIDI validation should explain parse failure");
}

void testRenderWritesMonoCsvRows()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "mono render should return success: " + error.str());
    require(error.str().empty(), "mono render should not write errors");

    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 5, "mono render should write one header plus four sample rows");
    require(lines[0] == "frame,left", "mono render should write mono header");
    require(lines[1] == "0,0", "mono render should start at frame zero with initial sine sample");
    require(lines[2].starts_with("1,"), "mono render should include frame one");
}

void testRenderDurationWritesCsvRows()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--duration", "0.5", "--sample-rate", "4"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "duration render should return success: " + error.str());
    require(error.str().empty(), "duration render should not write errors");

    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 3, "duration render should write one header plus two sample rows");
    require(lines[0] == "frame,left", "duration render should write mono header");
    require(lines[1] == "0,0", "duration render should start at frame zero");
    require(lines[2].starts_with("1,"), "duration render should include resolved final frame");
}

void testRenderWritesMonoJsonRows()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--duration", "0.5", "--sample-rate", "4", "--format", "json"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "mono JSON render should return success: " + error.str());
    require(error.str().empty(), "mono JSON render should not write errors");
    requireContains(output.str(), R"json("sampleRate": 4)json", "mono JSON render should include sample rate");
    requireContains(output.str(), R"json("frames": 2)json", "mono JSON render should include resolved frame count");
    requireContains(output.str(), R"json("name": "left")json", "mono JSON render should include left channel");
    requireContains(output.str(), R"json("samples": [
        0,)json", "mono JSON render should include sample array");
}

void testRenderAppliesEventValueChangesFromCli()
{
    const auto projectPath = writeProject(eventValueConstantVcaProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {
            "render",
            "--project", projectPath.string(),
            "--frames", "4",
            "--sample-rate", "4",
            "--max-block-frames", "2",
            "--event", "Gain=0",
            "--event-change", "2:Gain=1",
        },
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "CLI Event Value render should return success: " + error.str());
    require(error.str().empty(), "CLI Event Value render should not write errors");
    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 5, "CLI Event Value render should write four samples");
    require(lines[1] == "0,0", "CLI Event Value render frame 0");
    require(lines[2] == "1,0", "CLI Event Value render frame 1");
    require(lines[3].starts_with("2,"), "CLI Event Value render should include changed frame 2");
    require(lines[4] == "3,-1", "CLI Event Value render should apply changed gain at frame 3");
}

void testRenderAppliesPersistedEventValueInsideSoundDefinition()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Nested persisted Event Value";

    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.5;
    document.virtualControls.push_back(std::move(gain));

    talas::project::ProjectSoundDefinition voice;
    voice.definitionId = "voice";
    voice.displayName = "Voice";
    voice.outputs.push_back({
        "audioOut",
        talas::core::PortType::Audio,
        "osc",
        "audioOut",
    });
    voice.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            {"frequency", std::string{"1 hz"}},
            {"amplitude", std::string{"!Gain"}},
        },
        {},
    });
    document.soundDefinitions.push_back(std::move(voice));

    talas::project::ProjectNode voiceInstance;
    voiceInstance.instanceId = "voice1";
    voiceInstance.soundDefinitionId = "voice";
    document.graph.nodes.push_back(std::move(voiceInstance));
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back({
        "voice1",
        "audioOut",
        "out",
        "audioIn",
    });

    const auto projectPath = writeProject(
        talas::project::serializeProjectDocument(document));
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {
            "render",
            "--project", projectPath.string(),
            "--frames", "2",
            "--sample-rate", "4",
        },
        output,
        error);

    std::filesystem::remove(projectPath);
    require(
        exitCode == 0,
        "CLI should apply persisted Event Values used inside Sound definitions: "
            + error.str());
    require(
        error.str().empty(),
        "persisted nested Event Value render should not write errors");
    const auto lines = nonEmptyLines(output.str());
    require(
        lines.size() == 3 && lines[2] == "1,0.5",
        "persisted nested Event Value should control the rendered amplitude");
}

// --event gives a render its starting value, in place of the value the
// project saved for the control of that name, for render and validate alike.
void testCliEventOverridesPersistedVirtualControl()
{
    talas::project::ProjectDocument document;
    document.metadata.title = "Saved control under --event";
    talas::project::VirtualControl gain;
    gain.eventName = "Gain";
    gain.value = 0.5;
    document.virtualControls.push_back(std::move(gain));
    document.graph.nodes.push_back({
        "osc",
        "talas.oscillator",
        {
            {"frequency", std::string{"1 hz"}},
            {"amplitude", std::string{"!Gain"}},
        },
        {},
    });
    document.graph.nodes.push_back({"out", "talas.audioOutput", {}, {}});
    document.graph.connections.push_back({"osc", "audioOut", "out", "audioIn"});
    const auto projectPath = writeProject(
        talas::project::serializeProjectDocument(document));
    const auto project = projectPath.string();
    const auto renderFrameOne = [&](std::vector<std::string_view> events) {
        std::vector<std::string_view> args{
            "render", "--project", project, "--frames", "2", "--sample-rate", "4"};
        args.insert(args.end(), events.begin(), events.end());
        std::ostringstream output;
        std::ostringstream error;
        const auto exitCode = talas::cli::runCli(args, output, error);
        require(exitCode == 0 && error.str().empty(),
            "render with a saved control should succeed: " + error.str());
        const auto lines = nonEmptyLines(output.str());
        require(lines.size() == 3, "render with a saved control should write two samples");
        return lines[2];
    };

    require(renderFrameOne({}) == "1,0.5", "without --event the saved control value plays");
    require(renderFrameOne({"--event", "Gain=0.25"}) == "1,0.25",
        "--event should win over the saved control value");
    require(renderFrameOne({"--event-change", "0:Gain=0.25"}) == "1,0.25",
        "--event-change at frame 0 should match --event");

    {
        std::ostringstream output;
        std::ostringstream error;
        const auto exitCode = run(
            {"validate", "--project", project, "--event", "Gain=0.25"}, output, error);
        require(exitCode == 0 && output.str() == "OK\n" && error.str().empty(),
            "validate should accept --event for a saved control: " + error.str());
    }
    for (const auto command : {std::string_view{"validate"}, std::string_view{"render"}})
    {
        std::vector<std::string_view> args{command, "--project", project, "--event", "Gain:hz=1"};
        if (command == "render")
            args.insert(args.end(), {"--frames", "2"});
        std::ostringstream output;
        std::ostringstream error;
        const auto exitCode = talas::cli::runCli(args, output, error);
        require(exitCode == 1, std::string{command} + " should reject --event in another domain than its saved control");
        requireContains(error.str(), "wrong domain for its Virtual Control (Gain)",
            std::string{command} + " should name the saved control whose domain --event contradicts");
    }
    {
        std::ostringstream output;
        std::ostringstream error;
        const auto exitCode = run(
            {"render", "--project", project, "--frames", "2", "--event", "Gain=0.25", "--event", "Gain=0.75"},
            output, error);
        require(exitCode == 1, "a saved control should not hide a duplicate --event");
        requireContains(error.str(), "Gain is bound more than once",
            "a duplicate --event for a saved control should be named");
    }
    std::filesystem::remove(projectPath);
}

void testCliEventValueValidationAndSyntaxErrors()
{
    auto projectPath = writeProject(eventValuePitchProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    int exitCode = run(
        {"validate", "--project", projectPath.string(), "--event", "Pitch:nn=60"},
        output,
        error);
    require(exitCode == 0, "typed CLI Event Value should validate: " + error.str());
    require(output.str() == "OK\n", "typed CLI Event Value validation output");
    require(error.str().empty(), "typed CLI Event Value validation error output");

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--event", "Gain:db=0"},
        output,
        error);
    require(exitCode == 2, "unknown CLI Event Value domain should return usage error");
    requireContains(error.str(), "Unknown Event Value domain", "unknown CLI Event Value domain error");

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--event", "Gain=nan"},
        output,
        error);
    require(exitCode == 2, "non-finite CLI Event Value should return usage error");
    requireContains(error.str(), "--event value must be finite", "non-finite CLI Event Value error");

    output.str({});
    output.clear();
    error.str({});
    error.clear();
    exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--event-change", "before:Gain=1"},
        output,
        error);
    std::filesystem::remove(projectPath);
    require(exitCode == 2, "malformed CLI Event Value change frame should return usage error");
    requireContains(error.str(), "--event-change frame must be a non-negative integer", "malformed CLI Event Value change frame error");
}

void testRenderWritesStereoCsvRows()
{
    const auto projectPath = writeProject(stereoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "3", "--sample-rate", "4"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "stereo render should return success: " + error.str());
    require(error.str().empty(), "stereo render should not write errors");

    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 4, "stereo render should write one header plus three sample rows");
    require(lines[0] == "frame,left,right", "stereo render should write stereo header");
    require(lines[1] == "0,0,0", "stereo render should start with both initial sine samples at zero");
    require(lines[2].starts_with("1,"), "stereo render should include frame one");
}

void testRenderWritesStereoJsonRows()
{
    const auto projectPath = writeProject(stereoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "3", "--sample-rate", "4", "--format", "json"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "stereo JSON render should return success: " + error.str());
    require(error.str().empty(), "stereo JSON render should not write errors");
    requireContains(output.str(), R"json("sampleRate": 4)json", "stereo JSON render should include sample rate");
    requireContains(output.str(), R"json("frames": 3)json", "stereo JSON render should include frame count");
    requireContains(output.str(), R"json("name": "left")json", "stereo JSON render should include left channel");
    requireContains(output.str(), R"json("name": "right")json", "stereo JSON render should include right channel");
}

void testRenderWritesMidiEventsInJson()
{
    const auto projectPath = writeProject(midiControllerProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "2", "--sample-rate", "4", "--format", "json"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "MIDI JSON render should return success: " + error.str());
    require(error.str().empty(), "MIDI JSON render should not write errors");
    requireContains(output.str(), R"json("midiEvents": [)json", "MIDI JSON render should include midiEvents array");
    requireContains(output.str(), R"json("frame": 0)json", "MIDI JSON render should include event frame");
    requireContains(output.str(), R"json("bytes": [178, 10, 127])json", "MIDI JSON render should include decimal MIDI bytes");
}

std::vector<std::uint8_t> minimalMidiFileEchoFixture()
{
    return {
        'M', 'T', 'h', 'd',
        0x00, 0x00, 0x00, 0x06,
        0x00, 0x00,
        0x00, 0x01,
        0x00, 0x60,
        'M', 'T', 'r', 'k',
        0x00, 0x00, 0x00, 0x0c,
        0x00, 0x90, 0x3c, 0x40,
        0x60, 0x80, 0x3c, 0x00,
        0x00, 0xff, 0x2f, 0x00,
    };
}

void testRenderLoadsMidiFileBinaryResource()
{
    const auto projectPath = writeProject(midiFileEchoProjectJson());
    const auto midiPath = writeBinaryFile(minimalMidiFileEchoFixture());
    std::ostringstream output;
    std::ostringstream error;
    const std::string resourceArgument = "Sequences/Part.mid=" + midiPath.string();

    const int exitCode = run(
        {
            "render",
            "--project",
            projectPath.string(),
            "--frames",
            "8",
            "--sample-rate",
            "10",
            "--format",
            "json",
            "--binary-resource",
            resourceArgument,
        },
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(midiPath);
    require(exitCode == 0, "MIDI file resource render should return success: " + error.str());
    require(error.str().empty(), "MIDI file resource render should not write errors");
    requireContains(output.str(), R"json("midiEvents": [)json", "MIDI file resource render should include midiEvents array");
    requireContains(output.str(), R"json("bytes": [144, 60, 64])json", "MIDI file resource render should include note-on bytes");
    requireContains(output.str(), R"json("frame": 5)json", "MIDI file resource render should schedule note-off frame");
    requireContains(output.str(), R"json("bytes": [128, 60, 0])json", "MIDI file resource render should include note-off bytes");
}

void testRenderWritesGlobalControllerEventsInJson()
{
    const auto projectPath = writeProject(globalControllerProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "2", "--sample-rate", "4", "--format", "json"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "global-controller JSON render should return success: " + error.str());
    require(error.str().empty(), "global-controller JSON render should not write errors");
    requireContains(output.str(), R"json("globalControllerEvents": [)json", "global-controller JSON render should include event array");
    requireContains(output.str(), R"json("frame": 0)json", "global-controller JSON render should include event frame");
    requireContains(output.str(), R"json("name": "!Generated")json", "global-controller JSON render should include EventValue name");
    requireContains(output.str(), R"json("value": 0.75)json", "global-controller JSON render should include static value");
}

void testRenderWritesMonoWavOutput()
{
    const auto projectPath = writeProject(monoProjectJson());
    const auto wavPath = temporaryWavPath();
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--output", wavPath.string()},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "mono WAV render should return success: " + error.str());
    require(output.str().empty(), "mono WAV render should not write CSV output");
    require(error.str().empty(), "mono WAV render should not write errors");

    const auto bytes = readBytes(wavPath);
    std::filesystem::remove(wavPath);
    requireAscii(bytes, 0, "RIFF");
    requireAscii(bytes, 8, "WAVE");
    require(readU16(bytes, 22) == 1, "mono WAV render should write one channel");
    require(readU32(bytes, 24) == 4, "mono WAV render should preserve CLI sample rate");
    require(readU32(bytes, 40) == 8, "mono WAV render should write four PCM16 frames");
}

void testRenderDurationWritesWavOutput()
{
    const auto projectPath = writeProject(monoProjectJson());
    const auto wavPath = temporaryWavPath();
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--duration", "0.75", "--sample-rate", "4", "--output", wavPath.string()},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "duration WAV render should return success: " + error.str());
    require(output.str().empty(), "duration WAV render should not write CSV output");
    require(error.str().empty(), "duration WAV render should not write errors");

    const auto bytes = readBytes(wavPath);
    std::filesystem::remove(wavPath);
    requireAscii(bytes, 0, "RIFF");
    requireAscii(bytes, 8, "WAVE");
    require(readU16(bytes, 22) == 1, "duration WAV render should write one channel");
    require(readU32(bytes, 24) == 4, "duration WAV render should preserve CLI sample rate");
    require(readU32(bytes, 40) == 6, "duration WAV render should write three PCM16 frames");
}

void testRenderWritesStereoWavOutput()
{
    const auto projectPath = writeProject(stereoProjectJson());
    const auto wavPath = temporaryWavPath();
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "3", "--sample-rate", "4", "--output", wavPath.string()},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 0, "stereo WAV render should return success: " + error.str());
    require(output.str().empty(), "stereo WAV render should not write CSV output");
    require(error.str().empty(), "stereo WAV render should not write errors");

    const auto bytes = readBytes(wavPath);
    std::filesystem::remove(wavPath);
    requireAscii(bytes, 0, "RIFF");
    requireAscii(bytes, 8, "WAVE");
    require(readU16(bytes, 22) == 2, "stereo WAV render should write two channels");
    require(readU32(bytes, 24) == 4, "stereo WAV render should preserve CLI sample rate");
    require(readU32(bytes, 40) == 12, "stereo WAV render should write three frames of two PCM16 channels");
}

void testRenderLoadsMonoWavTableResource()
{
    const auto projectPath = writeProject(sampleResourceProjectJson());
    const auto wavPath = temporaryWavPath();
    std::string wavError;
    const std::vector<std::vector<double>> samples = {{1.0, 0.0, -1.0}};
    require(talas::audio::writePcm16Wav(wavPath, 4.0, samples, wavError), "test should write sample resource WAV: " + wavError);

    std::ostringstream output;
    std::ostringstream error;
    const std::string tableArgument = "Samples/Hit=" + wavPath.string();

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--table", tableArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(wavPath);
    require(exitCode == 0, "table resource render should return success: " + error.str());
    require(error.str().empty(), "table resource render should not write errors");

    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 5, "table resource render should write one header plus four sample rows");
    require(lines[0] == "frame,left", "table resource render should write mono header");
    require(lines[1] == "0,1", "table resource frame 0 should come from WAV sample 0");
    require(lines[2] == "1,0", "table resource frame 1 should come from WAV sample 1");
    require(lines[3] == "2,-1", "table resource frame 2 should come from WAV sample 2");
    require(lines[4] == "3,0", "table resource frame 3 should be silence after sample end");
}

void testRenderLoadsNumericTextTableResource()
{
    const auto projectPath = writeProject(sampleResourceProjectJson());
    const auto tablePath = writeTextTable("# hit table\n1, 0\n-1\n");

    std::ostringstream output;
    std::ostringstream error;
    const std::string tableArgument = "Samples/Hit=" + tablePath.string();

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--table", tableArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(tablePath);
    require(exitCode == 0, "numeric table resource render should return success: " + error.str());
    require(error.str().empty(), "numeric table resource render should not write errors");

    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 5, "numeric table resource render should write one header plus four sample rows");
    require(lines[0] == "frame,left", "numeric table resource render should write mono header");
    require(lines[1] == "0,1", "numeric table resource frame 0 should come from text sample 0");
    require(lines[2] == "1,0", "numeric table resource frame 1 should come from text sample 1");
    require(lines[3] == "2,-1", "numeric table resource frame 2 should come from text sample 2");
    require(lines[4] == "3,0", "numeric table resource frame 3 should be silence after sample end");
}

void testRenderLoadsPersistedRelativeProjectAsset()
{
    const auto tablePath = writeTextTable("# persisted hit\n1 0 -1\n");
    auto parsed = talas::project::parseProjectDocument(sampleResourceProjectJson());
    require(parsed.ok(), "test should parse sample project fixture");
    parsed.document.assets.push_back({
        "Samples/Hit",
        talas::project::ProjectAssetKind::Table,
        tablePath.filename().string(),
    });
    const auto projectPath = writeProject(talas::project::serializeProjectDocument(parsed.document));

    std::ostringstream output;
    std::ostringstream error;
    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4"},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(tablePath);
    require(exitCode == 0, "persisted project asset render should succeed: " + error.str());
    require(error.str().empty(), "persisted project asset render should not write errors");
    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 5 && lines[1] == "0,1" && lines[2] == "1,0"
                && lines[3] == "2,-1" && lines[4] == "3,0",
            "persisted relative table asset should drive rendered samples");
}

void testPackageProjectGraphsValidatesAndRenders()
{
    const auto tablePath =
        writeTextTable("# packaged hit\n1 0 -1\n");
    auto parsed = talas::project::parseProjectDocument(
        sampleResourceProjectJson());
    require(parsed.ok(), "package CLI fixture should parse");
    parsed.document.assets.push_back({
        "Samples/Hit",
        talas::project::ProjectAssetKind::Table,
        tablePath.string(),
    });
    const auto packagePath =
        std::filesystem::path(
            tablePath.string() + ".talaspkg");
    talas::audio::ProjectPackageOptions packageOptions;
    packageOptions.outputPackagePath = packagePath;
    const auto packaged =
        talas::audio::saveProjectPackage(
            parsed.document,
            packageOptions);
    require(
        packaged.ok(),
        "package CLI fixture should publish: "
            + packaged.message);

    std::ostringstream graphOutput;
    std::ostringstream graphError;
    const auto graphExit = run(
        {"graph",
         "--project",
         packagePath.string(),
         "--format",
         "dot"},
        graphOutput,
        graphError);
    require(
        graphExit == 0
            && graphError.str().empty()
            && graphOutput.str().find("digraph Talas")
                != std::string::npos,
        "CLI graph should resolve a package directory: "
            + graphError.str());

    std::ostringstream validateOutput;
    std::ostringstream validateError;
    const auto validateExit = run(
        {"validate",
         "--project",
         packagePath.string(),
         "--sample-rate",
         "4"},
        validateOutput,
        validateError);
    require(
        validateExit == 0
            && validateOutput.str() == "OK\n"
            && validateError.str().empty(),
        "CLI validate should load packaged assets: "
            + validateError.str());

    std::ostringstream renderOutput;
    std::ostringstream renderError;
    const auto renderExit = run(
        {"render",
         "--project",
         packagePath.string(),
         "--frames",
         "4",
         "--sample-rate",
         "4"},
        renderOutput,
        renderError);
    std::filesystem::remove_all(packagePath);
    std::filesystem::remove(tablePath);
    require(
        renderExit == 0 && renderError.str().empty(),
        "CLI render should load package project and assets: "
            + renderError.str());
    const auto lines = nonEmptyLines(renderOutput.str());
    require(
        lines.size() == 5
            && lines[1] == "0,1"
            && lines[2] == "1,0"
            && lines[3] == "2,-1",
        "packaged table asset should drive CLI render");
}

void testCliTableBindingOverridesPersistedProjectAsset()
{
    const auto tablePath = writeTextTable("1 0 -1\n");
    auto parsed = talas::project::parseProjectDocument(sampleResourceProjectJson());
    require(parsed.ok(), "test should parse sample project fixture for override");
    parsed.document.assets.push_back({
        "Samples/Hit",
        talas::project::ProjectAssetKind::Table,
        "missing-persisted-table.txt",
    });
    const auto projectPath = writeProject(talas::project::serializeProjectDocument(parsed.document));
    const std::string tableArgument = "Samples/Hit=" + tablePath.string();

    std::ostringstream output;
    std::ostringstream error;
    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4",
         "--table", tableArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(tablePath);
    require(exitCode == 0, "CLI table override should bypass missing persisted asset: " + error.str());
    require(error.str().empty(), "CLI table override should not write errors");
}

void testRenderRejectsMalformedNumericTextTableResource()
{
    const auto projectPath = writeProject(sampleResourceProjectJson());
    const auto tablePath = writeTextTable("1 nope\n");

    std::ostringstream output;
    std::ostringstream error;
    const std::string tableArgument = "Samples/Hit=" + tablePath.string();

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--table", tableArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(tablePath);
    require(exitCode == 1, "malformed numeric table resource should fail");
    require(output.str().empty(), "malformed numeric table resource should not write output");
    requireContains(error.str(), "numeric table", "malformed numeric table resource should explain parser failure");
}

void testRenderLoadsStereoWavShapingResource()
{
    talas::project::ProjectDocument document;
    talas::project::ProjectParameterExpression shapingFunction;
    shapingFunction.parameterId = "shapingFunction";
    shapingFunction.resource = "Tables/StereoShape";
    document.graph.nodes = {
        {"out", "talas.audioOutput", {}, {}},
        {"shape", "talas.waveshaper", {shapingFunction}, {}},
        {"osc", "talas.oscillator",
         {{"frequency", std::string{"1 hz"}},
          {"amplitude", std::string{"1"}}}, {}},
    };
    document.graph.connections = {
        {"osc", "audioOut", "shape", "audioIn"},
        {"shape", "audioOut", "out", "audioIn"},
        {"shape", "rightOut", "out", "rightIn"},
    };
    const auto projectPath = writeProject(
        talas::project::serializeProjectDocument(document));
    const auto wavPath = temporaryWavPath();
    std::string wavError;
    const std::vector<std::vector<double>> samples = {
        {-1.0, 0.0, 1.0},
        {1.0, 0.0, -1.0},
    };
    require(talas::audio::writePcm16Wav(wavPath, 4.0, samples, wavError), "test should write stereo table WAV: " + wavError);

    std::ostringstream output;
    std::ostringstream error;
    const std::string tableArgument = "Tables/StereoShape=" + wavPath.string();

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--table", tableArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(wavPath);
    require(exitCode == 0 && error.str().empty(),
        "CLI should render a stereo Waveshaper table: " + error.str());
    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 5 && lines[0] == "frame,left,right",
        "stereo table resource render should publish both output channels");
    const auto firstComma = lines[2].find(',');
    const auto secondComma = lines[2].find(',', firstComma + 1);
    require(firstComma != std::string::npos
                && secondComma != std::string::npos
                && std::stod(lines[2].substr(firstComma + 1,
                       secondComma - firstComma - 1)) > 0.9
                && std::stod(lines[2].substr(secondComma + 1)) < -0.9,
        "CLI should retain visibly different stereo shaping curves");
}

void testRenderRejectsStereoTableForMonoSample()
{
    const auto projectPath = writeProject(sampleResourceProjectJson());
    const auto wavPath = temporaryWavPath();
    std::string wavError;
    const std::vector<std::vector<double>> samples = {
        {1.0, 0.0, -1.0}, {0.0, 1.0, 0.0}};
    require(talas::audio::writePcm16Wav(
                wavPath, 4.0, samples, wavError),
        "test should write stereo sample table: " + wavError);

    std::ostringstream output;
    std::ostringstream error;
    const auto exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4",
         "--sample-rate", "4", "--table",
         "Samples/Hit=" + wavPath.string()},
        output, error);
    std::filesystem::remove(projectPath);
    std::filesystem::remove(wavPath);
    require(exitCode == 1 && output.str().empty(),
        "mono Sample must not silently discard a stereo table channel");
    requireContains(error.str(),
        "Stereo table resources require Waveshaper or an explicit AntialiasedOscillator channel",
        "unsupported stereo table use should explain the supported consumer");
}

void testRenderLoadsMonoWavAudioInput()
{
    const auto projectPath = writeProject(audioInputProjectJson());
    const auto wavPath = temporaryWavPath();
    std::string wavError;
    const std::vector<std::vector<double>> samples = {{1.0, 0.0, -1.0}};
    require(talas::audio::writePcm16Wav(wavPath, 4.0, samples, wavError), "test should write audio input WAV: " + wavError);

    std::ostringstream output;
    std::ostringstream error;
    const std::string inputArgument = "1=" + wavPath.string();

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--audio-input", inputArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(wavPath);
    require(exitCode == 0, "audio input render should return success: " + error.str());
    require(error.str().empty(), "audio input render should not write errors");

    const auto lines = nonEmptyLines(output.str());
    require(lines.size() == 5, "audio input render should write one header plus four sample rows");
    require(lines[0] == "frame,left", "audio input render should write mono header");
    require(lines[1] == "0,1", "audio input frame 0 should come from WAV input sample 0");
    require(lines[2] == "1,0", "audio input frame 1 should come from WAV input sample 1");
    require(lines[3] == "2,-1", "audio input frame 2 should come from WAV input sample 2");
    require(lines[4] == "3,0", "audio input frame 3 should be silence after input end");
}

void testRenderRejectsStereoWavAudioInput()
{
    const auto projectPath = writeProject(audioInputProjectJson());
    const auto wavPath = temporaryWavPath();
    std::string wavError;
    const std::vector<std::vector<double>> samples = {
        {1.0, 0.0, -1.0},
        {0.0, 1.0, 0.0},
    };
    require(talas::audio::writePcm16Wav(wavPath, 4.0, samples, wavError), "test should write stereo audio input WAV: " + wavError);

    std::ostringstream output;
    std::ostringstream error;
    const std::string inputArgument = "1=" + wavPath.string();

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--audio-input", inputArgument},
        output,
        error);

    std::filesystem::remove(projectPath);
    std::filesystem::remove(wavPath);
    require(exitCode == 1, "stereo audio input render should fail");
    require(output.str().empty(), "stereo audio input failure should not write output");
    requireContains(error.str(), "audio input WAV files must be mono", "stereo audio input failure should explain mono restriction");
}

void testMissingProjectFileReturnsFailure()
{
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"render", "--project", "/path/that/does/not/exist.talas.json", "--frames", "4"}, output, error);

    require(exitCode == 1, "missing project should return failure");
    require(output.str().empty(), "missing project should not write render output");
    requireContains(error.str(), "Failed to read project", "missing project should explain file error");
}

void testInvalidJsonReturnsFailure()
{
    const auto projectPath = writeProject(R"json({"format":"wrong"})json");
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"render", "--project", projectPath.string(), "--frames", "4"}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 1, "invalid project JSON should return failure");
    require(output.str().empty(), "invalid project JSON should not write render output");
    requireContains(error.str(), "Project parse failed", "invalid project JSON should explain parser failure");
}

void testCompileFailureReturnsDiagnostics()
{
    const auto projectPath = writeProject(noOutputProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"render", "--project", projectPath.string(), "--frames", "4"}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 1, "compile failure should return failure");
    require(output.str().empty(), "compile failure should not write render output");
    requireContains(error.str(), "Compile failed", "compile failure should identify runtime compile stage");
    requireContains(error.str(), "MissingAudioOutput", "compile failure should include diagnostic code");
}

// The RE Analysis Tool writes an REResonator's
// coefficient sets and, if asked, the excitation that recreates the sample
// through them (ADR-0022).
// CDP8's spectral processes that change a sound's length or order, as an
// offline process to a sample (ADR-0018 amendment, 2026-10-19): `talas_cli
// cdp-process` writes exactly what CDP8's `pvoc synth` writes.
void testCdpProcessWritesCdpsSample()
{
    // A whole-file fixture's sections: "input N" and "output N" lines.
    std::ifstream fixture(std::string{TALAS_CDP_FIXTURE_DIR} + "/stretch-time-2-5-256-points.txt");
    require(fixture.good(), "the stretch time fixture should be readable");
    std::vector<double> input;
    std::vector<double> cdp;
    std::vector<double>* section = nullptr;
    std::string line;
    while (std::getline(fixture, line))
    {
        if (line.empty() || line.front() == '#')
            continue;
        if (line.starts_with("input "))
            section = &input;
        else if (line.starts_with("output "))
            section = &cdp;
        else if (line.starts_with("analysis "))
            section = nullptr;
        else if (section != nullptr)
            section->push_back(std::strtod(line.c_str(), nullptr));
    }
    require(!input.empty() && !cdp.empty(), "the fixture should hold its input and CDP's output");
    const auto sample = std::filesystem::path{temporaryWavPath()};
    const auto stretched = std::filesystem::path{temporaryWavPath()};
    std::string writeError;
    require(talas::audio::writeFloat32Wav(sample, 48000, input, writeError),
        "the CDP input should write as a float WAV: " + writeError);

    std::ostringstream listed;
    std::ostringstream listError;
    require(run({"cdp-process", "--list"}, listed, listError) == 0, "cdp-process --list should succeed");
    for (const auto* id : {"stretchTime", "focusHold", "specCut", "specGrab", "specMagnify", "combineInterleave",
             "blurDrunk", "blurShuffle", "blurWeave"})
        requireContains(listed.str(), id, std::string{"cdp-process --list should name "} + id);

    std::ostringstream output;
    std::ostringstream error;
    const int exitCode = run({"cdp-process", "--process", "stretchTime", "--input", sample.string(), "--output",
                                 stretched.string(), "--points", "256", "--set", "stretch=2.5"},
        output, error);
    require(exitCode == 0, "stretch time should succeed: " + error.str());
    requireContains(output.str(), "5728 frames at 48000 Hz", "stretch time should report CDP's frames");
    const auto written = talas::audio::readSampleFile(stretched);
    require(written.ok() && written.channels.size() == 1 && written.channels.front() == cdp,
        "the written sample should hold CDP8's stretch time output exactly");

    std::ostringstream unknownOutput;
    std::ostringstream unknownError;
    require(run({"cdp-process", "--process", "stretchSpace", "--input", sample.string(), "--output",
                    stretched.string()},
                unknownOutput, unknownError)
            == 2,
        "an unknown process should be a usage error");
    std::ostringstream refusedOutput;
    std::ostringstream refusedError;
    require(run({"cdp-process", "--process", "stretchTime", "--input", sample.string(), "--output",
                    stretched.string(), "--set", "stretch=0"},
                refusedOutput, refusedError)
            == 1,
        "a stretch CDP refuses should fail");
    std::filesystem::remove(sample);
    std::filesystem::remove(stretched);
}

// ADR-0027: `talas_cli ga-analysis` writes a GA analysis a
// GAOscillators plays.
void testGaAnalysisWritesAnAnalysis()
{
    const auto sample = std::filesystem::path{temporaryWavPath()};
    const auto analysis = std::filesystem::path{temporaryWavPath()};
    std::vector<double> sound(24000);
    for (std::size_t frame = 0; frame < sound.size(); ++frame)
    {
        const auto t = static_cast<double>(frame) / 48000.0;
        sound[frame] = std::exp(-3.0 * t) * (0.4 * std::sin(2.0 * std::numbers::pi * 220.0 * t)
            + 0.2 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
    }
    std::string writeError;
    require(talas::audio::writeFloat32Wav(sample, 48000, sound, writeError), "the GA sample should write: " + writeError);
    std::ostringstream output;
    std::ostringstream error;
    const int exitCode = run({"ga-analysis", "--input", sample.string(), "--output", analysis.string(),
                                 "--waveforms", "2", "--harmonics", "8"},
        output, error);
    require(exitCode == 0, "GA analysis should succeed: " + error.str());
    requireContains(output.str(), "GA analysis: 2 waveforms of 8 harmonics, 50 envelope frames",
        "half a second at 100 frames a second should give 50 envelope frames");
    const auto loaded = talas::audio::loadTableResource(analysis);
    require(loaded.ok() && loaded.samples.size() == 6 + 2 * 4096 + 2 * 50 + 50 && loaded.samples[2] == 2.0,
        "the GA file should hold its header, 2 waveforms, 2 envelopes and the deviation");
    std::ostringstream invalidOutput;
    std::ostringstream invalidError;
    require(run({"ga-analysis", "--input", sample.string(), "--output", analysis.string(), "--waveforms", "65"},
                invalidOutput, invalidError)
            == 2,
        "65 waveforms should be a usage error");
    std::filesystem::remove(sample);
    std::filesystem::remove(analysis);
}

void testReAnalysisWritesTableAndExcitation()
{
    const auto sample = temporaryWavPath();
    const auto table = std::filesystem::path{temporaryWavPath()}.replace_extension(".txt");
    const auto excitation = temporaryWavPath();
    std::vector<double> sound(4800);
    for (std::size_t frame = 0; frame < sound.size(); ++frame)
    {
        const auto time = static_cast<double>(frame) / 48000.0;
        sound[frame] = 0.5 * std::exp(-8.0 * time) * std::sin(2.0 * std::numbers::pi * 440.0 * time);
    }
    std::string writeError;
    require(talas::audio::writePcm16Wav(sample, 48000.0, std::span<const std::vector<double>>{&sound, 1}, writeError),
        "RE analysis fixture sample should write: " + writeError);

    std::ostringstream output;
    std::ostringstream error;
    const int exitCode = run({"re-analysis", "--input", sample.string(), "--output", table.string(),
                                 "--excitation", excitation.string(), "--order", "8"},
        output, error);
    require(exitCode == 0, "RE analysis should succeed: " + error.str());
    requireContains(output.str(), "RE table: order 8, 11 coefficient sets",
        "a tenth of a second at 100 updates per second should give 11 coefficient sets");
    const auto loaded = talas::audio::loadTableResource(table);
    require(loaded.ok() && talas::dsp::re_resonator::parseTable(loaded.samples).has_value()
            && loaded.samples.size() == 1 + 8 * 11,
        "the RE table file should load as an RE table");
    const auto recreated = talas::audio::readPcm16Wav(excitation);
    require(recreated.ok() && recreated.channels.size() == 1 && recreated.channels.front().size() == sound.size(),
        "the EX file should hold one excitation sample per sample");

    std::ostringstream invalidOutput;
    std::ostringstream invalidError;
    const int invalid = run({"re-analysis", "--input", sample.string(), "--output", table.string(), "--order", "129"},
        invalidOutput, invalidError);
    std::filesystem::remove(sample);
    std::filesystem::remove(table);
    std::filesystem::remove(excitation);
    require(invalid == 2, "an order above 128 should return a usage error");
    requireContains(invalidError.str(), "--order must be a whole number from 1 to 128",
        "an invalid order should explain the range");
}

void testInvalidFramesReturnsUsageError()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"render", "--project", projectPath.string(), "--frames", "0"}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 2, "zero frames should return usage error");
    require(output.str().empty(), "zero frames should not write render output");
    requireContains(error.str(), "--frames must be greater than zero", "zero frames should explain validation failure");
}

void testUnsupportedRenderFormatReturnsUsageError()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--sample-rate", "4", "--format", "xml"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 2, "unsupported render format should return usage error");
    require(output.str().empty(), "unsupported render format should not write render output");
    requireContains(error.str(), "Unsupported render format", "unsupported render format should explain validation failure");
}

void testFramesAndDurationConflictReturnsUsageError()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run(
        {"render", "--project", projectPath.string(), "--frames", "4", "--duration", "1", "--sample-rate", "4"},
        output,
        error);

    std::filesystem::remove(projectPath);
    require(exitCode == 2, "frames and duration conflict should return usage error");
    require(output.str().empty(), "frames and duration conflict should not write render output");
    requireContains(error.str(), "--frames and --duration cannot both be specified", "frames and duration conflict should explain validation failure");
}

void testMissingFramesAndDurationReturnsUsageError()
{
    const auto projectPath = writeProject(monoProjectJson());
    std::ostringstream output;
    std::ostringstream error;

    const int exitCode = run({"render", "--project", projectPath.string(), "--sample-rate", "4"}, output, error);

    std::filesystem::remove(projectPath);
    require(exitCode == 2, "missing frames and duration should return usage error");
    require(output.str().empty(), "missing frames and duration should not write render output");
    requireContains(error.str(), "render requires either --frames <count> or --duration <seconds>", "missing render length should explain validation failure");
}
}

int main()
{
    testFilesystemErrorsAndLostOutputFailCleanly();
    testCliRejectsAmbiguousNumbersAndNamesInvalidEventValues();
    testDiskCacheInspectionAndExplicitCapture();
    testDiskCacheCaptureSaveFailureLeavesNoPublishedArtifact();
    testHelpPrintsUsage();
    testExportPluginReportsUsageErrors();
    testReAnalysisWritesTableAndExcitation();
    testCdpProcessWritesCdpsSample();
    testGaAnalysisWritesAnAnalysis();
    testCatalogWritesJson();
    testCatalogRejectsUnsupportedFormat();
    testGraphWritesDot();
    testGraphLabelsReusableSoundInstances();
    testGraphRejectsUnsupportedFormat();
    testInitWritesOscillatorProject();
    testInitRejectsUnsupportedTemplate();
    testInitDoesNotOverwriteExistingFile();
    testValidateProjectReturnsOk();
    testValidateStatusReportsPreparedM3Contract();
    testValidateStatusReportsWritableDiskRecorderAuthority();
    testValidateStatusReportsNestedProcessorPlacement();
    testValidateStatusKeepsPackedAudioDistinctFromComplexFrames();
    testValidateProjectReportsCompileFailure();
    testValidateDistinguishesPortableMissingComponentFromExecution();
    testRenderMissingComponentWritesNoOutputOrWav();
    testValidateProjectReportsRenderResourceFailure();
    testValidateReportsMalformedMidiBinaryResource();
    testRenderWritesMonoCsvRows();
    testRenderDurationWritesCsvRows();
    testRenderWritesMonoJsonRows();
    testRenderAppliesEventValueChangesFromCli();
    testRenderAppliesPersistedEventValueInsideSoundDefinition();
    testCliEventOverridesPersistedVirtualControl();
    testCliEventValueValidationAndSyntaxErrors();
    testRenderWritesStereoCsvRows();
    testRenderWritesStereoJsonRows();
    testRenderWritesMidiEventsInJson();
    testRenderLoadsMidiFileBinaryResource();
    testRenderWritesGlobalControllerEventsInJson();
    testRenderWritesMonoWavOutput();
    testRenderDurationWritesWavOutput();
    testRenderWritesStereoWavOutput();
    testRenderLoadsMonoWavTableResource();
    testRenderLoadsNumericTextTableResource();
    testRenderLoadsPersistedRelativeProjectAsset();
    testPackageProjectGraphsValidatesAndRenders();
    testCliTableBindingOverridesPersistedProjectAsset();
    testRenderRejectsMalformedNumericTextTableResource();
    testRenderLoadsStereoWavShapingResource();
    testRenderRejectsStereoTableForMonoSample();
    testRenderLoadsMonoWavAudioInput();
    testRenderRejectsStereoWavAudioInput();
    testMissingProjectFileReturnsFailure();
    testInvalidJsonReturnsFailure();
    testCompileFailureReturnsDiagnostics();
    testInvalidFramesReturnsUsageError();
    testUnsupportedRenderFormatReturnsUsageError();
    testFramesAndDurationConflictReturnsUsageError();
    testMissingFramesAndDurationReturnsUsageError();
    return 0;
}
