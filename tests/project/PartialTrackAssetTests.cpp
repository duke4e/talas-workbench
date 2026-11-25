#include "talas/project/PartialTrackAsset.h"

#include <bit>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

talas::project::PartialTrackAsset fixture()
{
    talas::project::PartialTrackAsset asset;
    asset.sampleRate = 48000.0;
    asset.sourceFrameCount = 4096;
    asset.frames = {
        {1, 0, {{1, 0.5, 440.0, 12.0}, {7, 0.25, 880.0, 18.0}}},
        {1, 1024, {{1, 0.4, 445.0, 13.0}, {7, 0.2, 875.0, 17.0}}},
        {2, 2048, {}},
        {2, 3072, {{9, 0.1, 220.0, 5.0}}},
    };
    return asset;
}

// A spectrum file's markers, labelled, and its loop
// markers are kept in its header; a file without them is written as before.
void testSpectrumFileKeepsItsMarkers()
{
    auto marked = fixture();
    marked.markers = {{"attack", 0}, {"m2", 2048}};
    marked.loopStartFrame = 1024;
    marked.loopEndFrame = 3072;
    const auto encoded = talas::project::encodePartialTrackAsset(marked);
    const auto decoded = encoded.ok() ? talas::project::decodePartialTrackAsset(encoded.bytes)
                                      : talas::project::PartialTrackAssetDecodeResult{};
    require(encoded.ok() && encoded.bytes[8] == 2 && decoded.ok() && decoded.asset == marked,
        "the markers and loop markers should be kept in the spectrum file's header");
    const auto plain = talas::project::encodePartialTrackAsset(fixture());
    require(plain.ok() && plain.bytes[8] == 1 && plain.bytes.size() < encoded.bytes.size(),
        "a spectrum file without markers should be written as before");
    auto outside = marked;
    outside.loopEndFrame = 5000;
    auto reversed = marked;
    reversed.loopStartFrame = 3072;
    reversed.loopEndFrame = 1024;
    auto late = marked;
    late.markers.push_back({"late", 4097});
    require(talas::project::validatePartialTrackAsset(outside).has_value()
            && talas::project::validatePartialTrackAsset(reversed).has_value()
            && talas::project::validatePartialTrackAsset(late).has_value(),
        "markers outside the source, or loop markers out of order, should be refused");
    auto truncated = encoded.bytes;
    truncated.resize(truncated.size() - 2);
    require(!talas::project::decodePartialTrackAsset(truncated).ok(), "a truncated marker label should be refused");
}
}

int main()
{
    testSpectrumFileKeepsItsMarkers();
    const auto source = fixture();
    require(
        !talas::project::validatePartialTrackAsset(source).has_value(),
        "valid partial-track fixture should validate");
    const auto encoded = talas::project::encodePartialTrackAsset(source);
    require(encoded.ok(), "valid partial-track fixture should encode");
    const auto decoded = talas::project::decodePartialTrackAsset(encoded.bytes);
    require(decoded.ok() && decoded.asset == source,
            "partial-track codec should round-trip exact identities/timing/bits");
    const auto reencoded = talas::project::encodePartialTrackAsset(decoded.asset);
    require(reencoded.ok() && reencoded.bytes == encoded.bytes,
            "partial-track encoding should be canonical");

    auto malformed = encoded.bytes;
    malformed.push_back(0);
    require(!talas::project::decodePartialTrackAsset(malformed).ok(),
            "trailing bytes should be rejected");
    malformed = encoded.bytes;
    malformed.resize(malformed.size() - 1);
    require(!talas::project::decodePartialTrackAsset(malformed).ok(),
            "truncated point bytes should be rejected");
    malformed = encoded.bytes;
    malformed[8] = 2;
    require(!talas::project::decodePartialTrackAsset(malformed).ok(),
            "future sidecar version should be rejected");
    malformed = encoded.bytes;
    malformed[68] = 1;
    require(!talas::project::decodePartialTrackAsset(malformed).ok(),
            "reserved frame bits should be rejected");
    malformed = encoded.bytes;
    for (std::size_t index = 16; index < 24; ++index)
        malformed[index] = 0xff;
    const auto oversizedHeader =
        talas::project::decodePartialTrackAsset(malformed);
    require(!oversizedHeader.ok()
            && oversizedHeader.error->code
                == talas::project::PartialTrackAssetErrorCode::LimitExceeded,
        "oversized declared counts should fail before proportional allocation");
    malformed = encoded.bytes;
    malformed[64] = 1;
    malformed[65] = 0x10;
    require(!talas::project::decodePartialTrackAsset(malformed).ok(),
            "per-frame point counts above the fixed limit should be rejected");

    int cancellationChecks = 0;
    const auto cancelled = talas::project::decodePartialTrackAsset(
        encoded.bytes,
        [&] { return ++cancellationChecks == 3; });
    require(!cancelled.ok()
            && cancelled.error->code
                == talas::project::PartialTrackAssetErrorCode::Cancelled,
        "partial-track decoding should cooperatively cancel between bounded records");

    auto invalid = source;
    invalid.frames[0].points[1].trackId = 1;
    require(talas::project::validatePartialTrackAsset(invalid).has_value(),
            "duplicate frame track identity should be rejected");
    invalid = source;
    invalid.frames[1].sourceFrame = 0;
    require(talas::project::validatePartialTrackAsset(invalid).has_value(),
            "non-increasing frame time should be rejected");
    invalid = source;
    invalid.frames[2].segmentId = 3;
    require(talas::project::validatePartialTrackAsset(invalid).has_value(),
            "segment gaps should be rejected");
    invalid = source;
    invalid.frames[0].points[0].frequencyHz = 24001.0;
    require(talas::project::validatePartialTrackAsset(invalid).has_value(),
            "frequency above Nyquist should be rejected");
    invalid = source;
    invalid.frames[0].points[0].bandwidthHz = -0.0;
    require(talas::project::validatePartialTrackAsset(invalid).has_value(),
            "noncanonical negative zero should be rejected");
    invalid = source;
    invalid.frames[0].points[0].amplitude =
        std::numeric_limits<double>::infinity();
    require(talas::project::validatePartialTrackAsset(invalid).has_value(),
            "non-finite amplitude should be rejected");

    std::cout << "Partial track asset tests passed\n";
    return 0;
}
