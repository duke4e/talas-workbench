#include "talas/project/SosSpectrumFile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// The layout is the published description of the Sum-of-Sines analysis
// files (an AIFF header, then the SOS analysis file and its APPL chunk). Loris's
// SpcFile reads and writes the same chunk with an obsolete table after the
// frame duration, and pads a frame's partials to a power of two of at
// least 32; its "enhanced" files keep a second word for each partial (the
// noise and phase), and say so in the word the description calls ignored.
// Both are read here.
namespace talas::project
{
namespace
{
constexpr double octaves = 15.0;
constexpr double amplitudeTop = 127.0;
constexpr double frequencyTop = 65536.0;
constexpr std::size_t wordBytes = 3;
constexpr std::size_t minimumWrittenPartials = 32;
constexpr double microsecondsPerSecond = 1.0e6;
constexpr std::uint16_t forwardLoop = 1;

[[nodiscard]] PartialTrackAssetError failure(PartialTrackAssetErrorCode code, std::string message)
{
    return {code, 0, 0, std::move(message)};
}

[[nodiscard]] bool tagAt(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view tag) noexcept
{
    if (offset > bytes.size() || bytes.size() - offset < tag.size())
        return false;
    for (std::size_t index = 0; index < tag.size(); ++index)
    {
        if (bytes[offset + index] != static_cast<std::uint8_t>(tag[index]))
            return false;
    }
    return true;
}

[[nodiscard]] std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>((bytes[offset] << 8U) | bytes[offset + 1]);
}

[[nodiscard]] std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept
{
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) | bytes[offset + 3];
}

// The 80-bit extended sample rate of a COMM chunk.
[[nodiscard]] double extendedRate(std::span<const std::uint8_t> bytes) noexcept
{
    const auto exponent = static_cast<int>(((bytes[0] & 0x7FU) << 8U) | bytes[1]);
    std::uint64_t mantissa = 0;
    for (std::size_t index = 2; index < 10; ++index)
        mantissa = (mantissa << 8U) | bytes[index];
    if (exponent == 0x7FFF)
        return std::numeric_limits<double>::quiet_NaN();
    if (exponent == 0 && mantissa == 0)
        return 0.0;
    const auto value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return (bytes[0] & 0x80U) != 0 ? -value : value;
}

// Each chunk of the FORM, its four-byte id at the offset given; false when
// the bytes are no AIFF or a chunk runs past the FORM.
template <typename Visit>
[[nodiscard]] bool forEachChunk(std::span<const std::uint8_t> bytes, Visit visit)
{
    if (bytes.size() < 12 || !tagAt(bytes, 0, "FORM") || !tagAt(bytes, 8, "AIFF"))
        return false;
    const auto end = static_cast<std::size_t>(std::min<std::uint64_t>(std::uint64_t{u32(bytes, 4)} + 8, bytes.size()));
    if (end < 12)
        return false;
    std::size_t offset = 12;
    while (end - offset >= 8)
    {
        const auto size = u32(bytes, offset + 4);
        const auto start = offset + 8;
        if (size > end - start)
            return false;
        if (!visit(offset, bytes.subspan(start, size)))
            return true;
        offset = start + size;
        if ((size & 1U) != 0)
            offset = std::min(end, offset + 1);
    }
    return true;
}

void appendTag(std::vector<std::uint8_t>& output, std::string_view tag)
{
    output.insert(output.end(), tag.begin(), tag.end());
}

void appendU16(std::vector<std::uint8_t>& output, std::uint16_t value)
{
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& output, std::uint32_t value)
{
    for (unsigned shift = 24;; shift -= 8)
    {
        output.push_back(static_cast<std::uint8_t>(value >> shift));
        if (shift == 0)
            break;
    }
}

void appendExtended(std::vector<std::uint8_t>& output, double value)
{
    int exponent = 0;
    const auto fraction = std::frexp(value, &exponent);
    const auto mantissa = static_cast<std::uint64_t>(std::ldexp(fraction, 64));
    appendU16(output, static_cast<std::uint16_t>(exponent - 1 + 16383));
    for (unsigned shift = 56;; shift -= 8)
    {
        output.push_back(static_cast<std::uint8_t>(mantissa >> shift));
        if (shift == 0)
            break;
    }
}

[[nodiscard]] std::string loweredExtension(const std::filesystem::path& path)
{
    auto extension = path.extension().string();
    std::ranges::transform(extension, extension.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return extension;
}

struct FileMarker
{
    std::uint16_t id = 0;
    std::uint32_t position = 0;
    std::string label;
};

// A MARK chunk's markers, each an id, a position among the samples and a
// Pascal string padded to an even length; nothing when it is malformed.
[[nodiscard]] std::optional<std::vector<FileMarker>> fileMarkers(std::span<const std::uint8_t> chunk)
{
    std::vector<FileMarker> markers;
    if (chunk.empty())
        return markers;
    if (chunk.size() < 2)
        return std::nullopt;
    const auto count = u16(chunk, 0);
    if (count > maximumPartialTrackAssetMarkers + 2)
        return std::nullopt;
    std::size_t offset = 2;
    for (std::uint16_t index = 0; index < count; ++index)
    {
        if (chunk.size() - offset < 7)
            return std::nullopt;
        FileMarker marker{u16(chunk, offset), u32(chunk, offset + 2), {}};
        const std::size_t length = chunk[offset + 6];
        offset += 7;
        if (chunk.size() - offset < length)
            return std::nullopt;
        // Loris counts the pad of an even name, a zero, in its length.
        const auto name = chunk.subspan(offset, length);
        marker.label.assign(name.begin(), std::ranges::find(name, std::uint8_t{0}));
        offset += length;
        if ((length & 1U) == 0)
            offset = std::min(chunk.size(), offset + 1);
        markers.push_back(std::move(marker));
    }
    return markers;
}

[[nodiscard]] std::uint64_t roundedFrame(double position) noexcept
{
    return position <= 0.0 ? 0 : static_cast<std::uint64_t>(std::llround(position));
}
}

std::uint32_t encodeSosSpectrumPartial(double amplitude, double frequencyHz, double sampleRate) noexcept
{
    std::uint32_t amplitudeByte = 0;
    if (std::isfinite(amplitude) && amplitude > 0.0)
    {
        const auto level = std::log2(amplitude) * amplitudeTop / octaves + amplitudeTop;
        amplitudeByte = level >= amplitudeTop ? 127U
            : level <= 0.0                    ? 0U
                                              : static_cast<std::uint32_t>(std::lround(level));
    }
    std::uint32_t frequencyWord = 0;
    if (std::isfinite(frequencyHz) && frequencyHz > 0.0 && std::isfinite(sampleRate) && sampleRate > 0.0)
    {
        const auto pitch = std::log2(2.0 * frequencyHz / sampleRate) * frequencyTop / octaves + frequencyTop;
        frequencyWord = pitch >= 65535.0 ? 65535U
            : pitch <= 0.0               ? 0U
                                         : std::min<std::uint32_t>(65535U, static_cast<std::uint32_t>(std::lround(pitch)));
    }
    return (amplitudeByte << 16U) | frequencyWord;
}

double sosSpectrumPartialAmplitude(std::uint32_t partial) noexcept
{
    // Only positive values are allowed in the signed top byte; a negative
    // one is read without its sign bit, as Loris reads it.
    const auto level = static_cast<double>((partial >> 16U) & 0x7FU);
    return level == 0.0 ? 0.0 : std::exp2((level - amplitudeTop) * octaves / amplitudeTop);
}

double sosSpectrumPartialFrequency(std::uint32_t partial, double sampleRate) noexcept
{
    const auto pitch = static_cast<double>(partial & 0xFFFFU);
    return 0.5 * sampleRate * std::exp2((pitch - frequencyTop) * octaves / frequencyTop);
}

bool isSosSpectrumFile(std::span<const std::uint8_t> bytes) noexcept
{
    bool found = false;
    (void) forEachChunk(bytes, [&](std::size_t offset, std::span<const std::uint8_t> data) {
        found = tagAt(bytes, offset, "APPL") && tagAt(data, 0, "SOSe");
        return !found;
    });
    return found;
}

PartialTrackAssetDecodeResult decodeSosSpectrumFile(
    std::span<const std::uint8_t> bytes,
    std::function<bool()> shouldCancel)
{
    PartialTrackAssetDecodeResult result;
    const auto fail = [&](PartialTrackAssetErrorCode code, std::string message) {
        result.asset = {};
        result.error = failure(code, std::move(message));
        return result;
    };
    if (bytes.size() > maximumPartialTrackAssetBytes)
        return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file exceeds the fixed byte limit");

    std::span<const std::uint8_t> common;
    std::span<const std::uint8_t> sound;
    std::span<const std::uint8_t> envelopes;
    std::span<const std::uint8_t> markerChunk;
    std::span<const std::uint8_t> instrument;
    bool haveCommon = false;
    bool haveSound = false;
    bool haveEnvelopes = false;
    const auto wellFormed = forEachChunk(bytes, [&](std::size_t offset, std::span<const std::uint8_t> data) {
        if (tagAt(bytes, offset, "COMM"))
            common = data, haveCommon = true;
        else if (tagAt(bytes, offset, "SSND"))
            sound = data, haveSound = true;
        else if (tagAt(bytes, offset, "APPL") && tagAt(data, 0, "SOSe"))
            envelopes = data, haveEnvelopes = true;
        else if (tagAt(bytes, offset, "MARK"))
            markerChunk = data;
        else if (tagAt(bytes, offset, "INST"))
            instrument = data;
        return true;
    });
    if (!wellFormed)
        return fail(PartialTrackAssetErrorCode::InvalidHeader, "Sum-of-Sines spectrum file is not a well-formed AIFF file");
    if (!haveCommon || common.size() < 18 || !haveSound || sound.size() < 8 || !haveEnvelopes || envelopes.size() < 12)
        return fail(PartialTrackAssetErrorCode::InvalidHeader,
            "Sum-of-Sines spectrum file needs its COMM, SSND and APPL 'SOSe' chunks");
    if (u16(common, 0) != 1 || u16(common, 6) != 24)
        return fail(PartialTrackAssetErrorCode::InvalidHeader, "Sum-of-Sines spectrum file must be mono, 24 bits a sample");
    const auto sampleRate = extendedRate(common.subspan(8, 10));
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || sampleRate > maximumPartialTrackSampleRate)
        return fail(PartialTrackAssetErrorCode::InvalidSampleRate, "Sum-of-Sines spectrum file sample rate is unsupported");
    const std::uint64_t sampleFrames = u32(common, 2);

    // The APPL chunk: 'SOSe', the ignored word (Loris's enhanced flag), the
    // partials a frame, a reserved word for each, then the frame duration.
    const auto flag = u32(envelopes, 4);
    const std::uint64_t declared = u32(envelopes, 8);
    if (declared == 0 || envelopes.size() < 16 + 4 * declared)
        return fail(PartialTrackAssetErrorCode::InvalidHeader, "Sum-of-Sines spectrum file's APPL 'SOSe' chunk is malformed");
    const auto frameMicroseconds = u32(envelopes, static_cast<std::size_t>(12 + 4 * declared));
    const auto hop = static_cast<double>(frameMicroseconds) * sampleRate / microsecondsPerSecond;
    if (!(hop >= 1.0))
        return fail(PartialTrackAssetErrorCode::InvalidFrameOrder, "Sum-of-Sines spectrum file's frames are closer than a sample");
    const bool enhanced = flag == 1 && declared % 2 == 0;
    const std::uint64_t wordsAPartial = enhanced ? 2 : 1;
    const auto partials = declared / wordsAPartial;
    if (partials > maximumPartialTrackAssetTracks)
        return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file has too many partials");
    // Loris's frames hold a power of two of partials, at least 32, past the
    // number it declares; its chunk is longer than the documented one.
    const auto padded = std::max<std::uint64_t>(minimumWrittenPartials, std::bit_ceil(partials)) * wordsAPartial;
    const bool longChunk = envelopes.size() > 16 + 4 * declared;
    auto wordsAFrame = declared;
    if (padded != declared && sampleFrames % padded == 0 && (longChunk || sampleFrames % declared != 0))
        wordsAFrame = padded;
    const auto slots = wordsAFrame / wordsAPartial;

    const auto dataOffset = std::uint64_t{u32(sound, 0)} + 8;
    if (dataOffset > sound.size() || (sound.size() - dataOffset) / wordBytes < sampleFrames)
        return fail(PartialTrackAssetErrorCode::SizeMismatch, "Sum-of-Sines spectrum file holds fewer samples than its COMM chunk says");
    const auto samples = sound.subspan(static_cast<std::size_t>(dataOffset));
    const auto frameCount = sampleFrames / wordsAFrame;
    if (frameCount == 0)
        return fail(PartialTrackAssetErrorCode::InvalidSourceExtent, "Sum-of-Sines spectrum file holds no frame");
    if (frameCount > maximumPartialTrackAssetFrames)
        return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file has too many frames");

    try
    {
        auto& asset = result.asset;
        asset.sampleRate = sampleRate;
        asset.frames.reserve(static_cast<std::size_t>(frameCount));
        std::size_t totalPoints = 0;
        for (std::uint64_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            if ((frameIndex & 63U) == 0 && shouldCancel && shouldCancel())
                return fail(PartialTrackAssetErrorCode::Cancelled, "Sum-of-Sines spectrum file decoding cancelled");
            PartialTrackFrame frame{1, roundedFrame(static_cast<double>(frameIndex) * hop), {}};
            const auto base = static_cast<std::size_t>(frameIndex * wordsAFrame * wordBytes);
            for (std::uint64_t slot = 0; slot < slots; ++slot)
            {
                const auto at = base + static_cast<std::size_t>(slot * wordsAPartial * wordBytes);
                const auto word = (std::uint32_t{samples[at]} << 16U) | (std::uint32_t{samples[at + 1]} << 8U) | samples[at + 2];
                const auto amplitude = sosSpectrumPartialAmplitude(word);
                if (amplitude == 0.0)
                    continue;
                if (frame.points.size() == maximumPartialTrackPointsPerFrame || totalPoints == maximumPartialTrackAssetPoints)
                    return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file has too many sounding partials");
                frame.points.push_back({slot + 1, amplitude, sosSpectrumPartialFrequency(word, sampleRate), 0.0});
                ++totalPoints;
            }
            asset.frames.push_back(std::move(frame));
        }
        asset.sourceFrameCount = std::max(roundedFrame(static_cast<double>(frameCount) * hop), asset.frames.back().sourceFrame + 1);

        // The markers, at their frames' times; the sustain loop of an INST
        // chunk, between two of them.
        auto markers = fileMarkers(markerChunk);
        if (!markers.has_value())
            return fail(PartialTrackAssetErrorCode::InvalidHeader, "Sum-of-Sines spectrum file's MARK chunk is malformed");
        const auto markerFrame = [&](std::uint32_t position) {
            return std::min(asset.sourceFrameCount,
                roundedFrame(static_cast<double>(position) / static_cast<double>(wordsAFrame) * hop));
        };
        std::optional<std::pair<std::uint16_t, std::uint16_t>> loop;
        if (instrument.size() >= 20 && u16(instrument, 8) != 0)
        {
            const auto begin = std::ranges::find(*markers, u16(instrument, 10), &FileMarker::id);
            const auto end = std::ranges::find(*markers, u16(instrument, 12), &FileMarker::id);
            if (begin != markers->end() && end != markers->end() && markerFrame(begin->position) < markerFrame(end->position))
            {
                loop = std::pair{begin->id, end->id};
                asset.loopStartFrame = markerFrame(begin->position);
                asset.loopEndFrame = markerFrame(end->position);
            }
        }
        for (auto& marker : *markers)
        {
            if (loop.has_value() && (marker.id == loop->first || marker.id == loop->second))
                continue;
            if (asset.markers.size() == maximumPartialTrackAssetMarkers)
                return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file has too many markers");
            if (marker.label.size() > maximumPartialTrackMarkerLabelBytes)
                marker.label.resize(maximumPartialTrackMarkerLabelBytes);
            asset.markers.push_back({std::move(marker.label), markerFrame(marker.position)});
        }
    }
    catch (const std::bad_alloc&)
    {
        return fail(PartialTrackAssetErrorCode::AllocationFailed, "Sum-of-Sines spectrum file decoding allocation failed");
    }
    result.error = validatePartialTrackAsset(result.asset);
    if (result.error.has_value())
        result.asset = {};
    return result;
}

PartialTrackAssetEncodeResult encodeSosSpectrumFile(const PartialTrackAsset& asset)
{
    PartialTrackAssetEncodeResult result;
    result.error = validatePartialTrackAsset(asset);
    if (result.error.has_value())
        return result;
    const auto fail = [&](PartialTrackAssetErrorCode code, std::string message) {
        result.bytes.clear();
        result.error = failure(code, std::move(message));
        return result;
    };
    try
    {
        const auto& frames = asset.frames;
        const auto rate = asset.sampleRate;
        // The frame duration: the asset's most common spacing.
        double hop = static_cast<double>(asset.sourceFrameCount);
        if (frames.size() > 1)
        {
            std::vector<std::uint64_t> spacings;
            spacings.reserve(frames.size() - 1);
            for (std::size_t index = 1; index < frames.size(); ++index)
                spacings.push_back(frames[index].sourceFrame - frames[index - 1].sourceFrame);
            const auto middle = spacings.begin() + static_cast<std::ptrdiff_t>(spacings.size() / 2);
            std::ranges::nth_element(spacings, middle);
            hop = static_cast<double>(*middle);
        }
        const auto shortest = std::ceil(microsecondsPerSecond / rate);
        const auto microseconds = std::max(shortest, std::round(hop / rate * microsecondsPerSecond));
        if (microseconds > static_cast<double>(std::numeric_limits<std::uint32_t>::max()))
            return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file frames cannot be that long");

        // The tracks laid into partials by when they start: each in the
        // lowest partial whose last track has ended a frame before, the
        // partials then ordered by their mean frequency.
        struct Track
        {
            std::uint64_t id = 0;
            std::size_t first = 0;
            std::size_t last = 0;
            double frequencySum = 0.0;
            std::size_t points = 0;
            double firstFrequency = 0.0;
            std::size_t slot = 0;
        };
        std::vector<Track> tracks;
        std::unordered_map<std::uint64_t, std::size_t> trackIndex;
        for (std::size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
        {
            for (const auto& point : frames[frameIndex].points)
            {
                const auto [found, inserted] = trackIndex.try_emplace(point.trackId, tracks.size());
                if (inserted)
                    tracks.push_back({point.trackId, frameIndex, frameIndex, 0.0, 0, point.frequencyHz, 0});
                auto& track = tracks[found->second];
                track.last = frameIndex;
                track.frequencySum += point.frequencyHz;
                ++track.points;
            }
        }
        std::vector<std::size_t> byStart(tracks.size());
        for (std::size_t index = 0; index < byStart.size(); ++index)
            byStart[index] = index;
        const auto meanFrequency = [&](const Track& track) { return track.frequencySum / static_cast<double>(track.points); };
        std::ranges::sort(byStart, [&](std::size_t left, std::size_t right) {
            const auto& a = tracks[left];
            const auto& b = tracks[right];
            if (a.first != b.first)
                return a.first < b.first;
            if (meanFrequency(a) != meanFrequency(b))
                return meanFrequency(a) < meanFrequency(b);
            return a.id < b.id;
        });
        std::vector<std::size_t> slotLast;
        std::vector<std::size_t> slotFirstTrack;
        std::vector<double> slotFrequencySum;
        std::vector<std::size_t> slotPoints;
        for (const auto index : byStart)
        {
            auto& track = tracks[index];
            std::size_t slot = 0;
            while (slot < slotLast.size() && slotLast[slot] + 2 > track.first)
                ++slot;
            if (slot == slotLast.size())
            {
                slotLast.push_back(0);
                slotFirstTrack.push_back(index);
                slotFrequencySum.push_back(0.0);
                slotPoints.push_back(0);
            }
            slotLast[slot] = track.last;
            slotFrequencySum[slot] += track.frequencySum;
            slotPoints[slot] += track.points;
            track.slot = slot;
        }
        std::vector<std::size_t> order(slotLast.size());
        for (std::size_t index = 0; index < order.size(); ++index)
            order[index] = index;
        std::ranges::stable_sort(order, [&](std::size_t left, std::size_t right) {
            return slotFrequencySum[left] / static_cast<double>(slotPoints[left])
                < slotFrequencySum[right] / static_cast<double>(slotPoints[right]);
        });
        std::vector<std::size_t> position(order.size());
        for (std::size_t index = 0; index < order.size(); ++index)
            position[order[index]] = index;
        for (auto& track : tracks)
            track.slot = position[track.slot];

        const auto partials = std::max<std::size_t>(minimumWrittenPartials, std::bit_ceil(slotLast.size()));
        if (partials > maximumPartialTrackAssetTracks)
            return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file cannot hold that many partials");
        const auto lastFrame = static_cast<double>(frames.back().sourceFrame);
        const auto frameCount = frames.size() == 1
            ? std::size_t{1}
            : std::max(static_cast<std::size_t>(std::ceil(static_cast<double>(asset.sourceFrameCount) / hop)),
                  static_cast<std::size_t>(std::floor(lastFrame / hop)) + 1);
        const auto values = static_cast<std::uint64_t>(frameCount) * partials;
        if (frameCount > maximumPartialTrackAssetFrames || values > std::numeric_limits<std::uint32_t>::max()
            || values * wordBytes + 4096 + 8 * maximumPartialTrackAssetMarkers > maximumPartialTrackAssetBytes)
            return fail(PartialTrackAssetErrorCode::LimitExceeded, "Sum-of-Sines spectrum file would exceed the fixed byte limit");

        // A silent partial keeps the frequency it last had, or will have.
        std::vector<std::uint32_t> held(partials, 0);
        for (std::size_t slot = 0; slot < slotFirstTrack.size(); ++slot)
            held[position[slot]] = encodeSosSpectrumPartial(1.0, tracks[slotFirstTrack[slot]].firstFrequency, rate) & 0xFFFFU;

        auto& output = result.bytes;
        output.reserve(static_cast<std::size_t>(values * wordBytes) + 256);
        appendTag(output, "FORM");
        appendU32(output, 0);
        appendTag(output, "AIFF");
        appendTag(output, "COMM");
        appendU32(output, 18);
        appendU16(output, 1);
        appendU32(output, static_cast<std::uint32_t>(values));
        appendU16(output, 24);
        appendExtended(output, rate);
        appendTag(output, "SSND");
        appendU32(output, static_cast<std::uint32_t>(8 + values * wordBytes));
        appendU32(output, 0);
        appendU32(output, 0);

        std::vector<std::uint32_t> words(partials);
        const auto setPoint = [&](const PartialTrackPoint& point, double amplitude, double frequency) {
            const auto slot = tracks[trackIndex.at(point.trackId)].slot;
            words[slot] = encodeSosSpectrumPartial(amplitude, frequency, rate);
            held[slot] = words[slot] & 0xFFFFU;
        };
        const auto setFrame = [&](const PartialTrackFrame& frame) {
            for (const auto& point : frame.points)
                setPoint(point, point.amplitude, point.frequencyHz);
        };
        std::size_t following = 0;
        for (std::size_t frameIndex = 0; frameIndex < frameCount; ++frameIndex)
        {
            const auto time = static_cast<double>(frameIndex) * hop;
            for (std::size_t slot = 0; slot < partials; ++slot)
                words[slot] = held[slot];
            while (following < frames.size() && static_cast<double>(frames[following].sourceFrame) <= time)
                ++following;
            if (frames.size() == 1)
                setFrame(frames.front());
            else if (following == 0)
            {
                if (static_cast<double>(frames.front().sourceFrame) - time <= hop * 0.5)
                    setFrame(frames.front());
            }
            else if (following == frames.size())
            {
                if (time - lastFrame <= hop * 0.5)
                    setFrame(frames.back());
            }
            else
            {
                const auto& before = frames[following - 1];
                const auto& after = frames[following];
                const auto weight = (time - static_cast<double>(before.sourceFrame))
                    / static_cast<double>(after.sourceFrame - before.sourceFrame);
                auto left = before.points.begin();
                auto right = after.points.begin();
                while (left != before.points.end() || right != after.points.end())
                {
                    if (right == after.points.end() || (left != before.points.end() && left->trackId < right->trackId))
                    {
                        if (weight <= 0.5)
                            setPoint(*left, left->amplitude, left->frequencyHz);
                        ++left;
                    }
                    else if (left == before.points.end() || right->trackId < left->trackId)
                    {
                        if (weight > 0.5)
                            setPoint(*right, right->amplitude, right->frequencyHz);
                        ++right;
                    }
                    else
                    {
                        setPoint(*left, left->amplitude + weight * (right->amplitude - left->amplitude),
                            left->frequencyHz + weight * (right->frequencyHz - left->frequencyHz));
                        ++left;
                        ++right;
                    }
                }
            }
            for (const auto word : words)
            {
                output.push_back(static_cast<std::uint8_t>(word >> 16U));
                output.push_back(static_cast<std::uint8_t>(word >> 8U));
                output.push_back(static_cast<std::uint8_t>(word));
            }
        }

        appendTag(output, "APPL");
        appendU32(output, static_cast<std::uint32_t>(16 + 4 * partials));
        appendTag(output, "SOSe");
        appendU32(output, 0);
        appendU32(output, static_cast<std::uint32_t>(partials));
        output.insert(output.end(), 4 * partials, std::uint8_t{0});
        appendU32(output, static_cast<std::uint32_t>(microseconds));

        // The markers at their nearest frames, the loop's two after them.
        std::vector<FileMarker> markers;
        const auto filePosition = [&](std::uint64_t sourceFrame) {
            const auto frame = std::min<double>(static_cast<double>(frameCount), std::round(static_cast<double>(sourceFrame) / hop));
            return static_cast<std::uint32_t>(frame * static_cast<double>(partials));
        };
        for (const auto& marker : asset.markers)
            markers.push_back({static_cast<std::uint16_t>(markers.size() + 1), filePosition(marker.sourceFrame), marker.label});
        if (asset.loopStartFrame.has_value())
        {
            markers.push_back({static_cast<std::uint16_t>(markers.size() + 1), filePosition(*asset.loopStartFrame), "loop start"});
            markers.push_back({static_cast<std::uint16_t>(markers.size() + 1), filePosition(*asset.loopEndFrame), "loop end"});
        }
        if (!markers.empty())
        {
            std::size_t size = 2;
            for (const auto& marker : markers)
                size += 7 + marker.label.size() + (marker.label.size() % 2 == 0 ? 1 : 0);
            appendTag(output, "MARK");
            appendU32(output, static_cast<std::uint32_t>(size));
            appendU16(output, static_cast<std::uint16_t>(markers.size()));
            for (const auto& marker : markers)
            {
                appendU16(output, marker.id);
                appendU32(output, marker.position);
                output.push_back(static_cast<std::uint8_t>(marker.label.size()));
                output.insert(output.end(), marker.label.begin(), marker.label.end());
                if (marker.label.size() % 2 == 0)
                    output.push_back(0);
            }
        }
        if (asset.loopStartFrame.has_value())
        {
            appendTag(output, "INST");
            appendU32(output, 20);
            output.insert(output.end(), {60, 0, 0, 127, 1, 127});
            appendU16(output, 0);
            appendU16(output, forwardLoop);
            appendU16(output, static_cast<std::uint16_t>(markers.size() - 1));
            appendU16(output, static_cast<std::uint16_t>(markers.size()));
            appendU16(output, 0);
            appendU16(output, 0);
            appendU16(output, 0);
        }
        const auto formSize = static_cast<std::uint32_t>(output.size() - 8);
        for (std::size_t index = 0; index < 4; ++index)
            output[4 + index] = static_cast<std::uint8_t>(formSize >> (24U - 8U * index));
    }
    catch (const std::bad_alloc&)
    {
        return fail(PartialTrackAssetErrorCode::AllocationFailed, "Sum-of-Sines spectrum file encoding allocation failed");
    }
    return result;
}

bool isSpectrumFilePath(const std::filesystem::path& path)
{
    const auto extension = loweredExtension(path);
    return extension == sosSpectrumFileExtension || extension == partialTrackFileExtension || extension == ".talaspartials";
}

bool isSosSpectrumFilePath(const std::filesystem::path& path)
{
    return loweredExtension(path) == sosSpectrumFileExtension;
}

PartialTrackAssetEncodeResult encodeSpectrumFileFor(const std::filesystem::path& path, const PartialTrackAsset& asset)
{
    return isSosSpectrumFilePath(path) ? encodeSosSpectrumFile(asset) : encodePartialTrackAsset(asset);
}
}
