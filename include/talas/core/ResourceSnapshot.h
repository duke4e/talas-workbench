#pragma once

#include "talas/core/SampleKeyRange.h"
#include "talas/core/SampleLoop.h"
#include "talas/core/SampleMarker.h"
#include "talas/core/SamplePitch.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace talas::core
{
enum class ResourceSnapshotKind : std::uint8_t
{
    Table,
    Binary,
    Audio,
    PartialSpectrum,
};

// Published snapshots are always shared as const. Generation zero is reserved
// for the legacy RenderSettings compatibility adapter; project assets use the
// positive generation accepted by their format-10 manifest.
struct ResourceSnapshot
{
    std::string id;
    ResourceSnapshotKind kind = ResourceSnapshotKind::Table;
    std::uint64_t generation = 0;
    std::string contentHash;
    std::filesystem::path resolvedPath;
    std::vector<double> tableSamples;
    std::vector<double> tableRightSamples;
    std::uint32_t tableSourceSampleRate = 0;
    std::optional<SampleLoop> tableForwardLoop;
    std::optional<SamplePitch> tableSamplePitch;
    // The notes and velocities a sample's INST or inst chunk gives, and, for
    // a sample of a KeyMappedMultisample's bank,
    // the bank's first sample and the file name it orders samples by.
    std::optional<SampleKeyRange> tableKeyRange;
    std::string tableBank;
    std::string tableBankName;
    // A sample's named positions.
    std::vector<SampleMarker> tableMarkers;
    std::vector<std::uint8_t> binaryBytes;
    // Exact already-inspected bytes for bounded Audio authorities that must
    // not be reopened by pathname during stream preparation.
    std::vector<std::uint8_t> audioBytes;
    std::uint32_t channelCount = 0;
    double sampleRate = 0.0;
    std::uint64_t frameCount = 0;
    std::string encoding;
    std::vector<std::uint8_t> partialTrackBytes;
    // Exact persisted manifest authority used by prepared consumers whose
    // behavior depends on more than immutable bytes/media shape.
    std::string projectAssetPath;
    std::string provenance;
};

using ResourceSnapshotPtr =
    std::shared_ptr<const ResourceSnapshot>;
}
