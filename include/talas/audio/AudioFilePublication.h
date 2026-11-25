#pragma once

#include <filesystem>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace talas::audio
{
enum class ExistingAudioFilePolicy
{
    Reject,
    Replace,
};

// Resolves existing parent symlinks and rejects an output that aliases a
// protected file or falls inside a protected directory. Callers use this for
// project/package/source-asset authority before preparing publication.
[[nodiscard]] bool audioFileDestinationConflicts(
    const std::filesystem::path& destination,
    std::span<const std::filesystem::path> protectedFiles,
    std::span<const std::filesystem::path> protectedDirectories) noexcept;

// Owns one private sibling staging directory and publishes only the exact
// staged file. The destination is never replaced unless Replace was selected
// before preparation. Destruction removes only this object's private staging
// artifacts; it never removes the destination.
class AudioFilePublication
{
public:
    [[nodiscard]] static std::unique_ptr<AudioFilePublication> prepare(
        const std::filesystem::path& destination,
        ExistingAudioFilePolicy existingPolicy,
        std::string& error) noexcept;
    [[nodiscard]] static std::unique_ptr<AudioFilePublication> prepareContained(
        const std::filesystem::path& rootDirectory,
        const std::filesystem::path& relativeDestination,
        ExistingAudioFilePolicy existingPolicy,
        std::string& error) noexcept;

    ~AudioFilePublication();

    AudioFilePublication(const AudioFilePublication&) = delete;
    AudioFilePublication& operator=(const AudioFilePublication&) = delete;
    AudioFilePublication(AudioFilePublication&&) = delete;
    AudioFilePublication& operator=(AudioFilePublication&&) = delete;

    [[nodiscard]] const std::filesystem::path& destination() const noexcept;
    [[nodiscard]] const std::filesystem::path& temporaryPath() const noexcept;
    // Creates the exact staging inode relative to the directory authority held
    // since prepare(). Ownership of the returned stream passes to the caller.
    [[nodiscard]] std::FILE* openTemporaryFile(std::string& error) noexcept;
    [[nodiscard]] bool readTemporaryFile(
        std::size_t maximumBytes,
        std::vector<std::uint8_t>& bytes,
        std::string& error) const noexcept;
    [[nodiscard]] bool readDestinationFile(
        std::size_t maximumBytes,
        std::vector<std::uint8_t>& bytes,
        std::string& error) const noexcept;
    [[nodiscard]] bool publish(std::string& error);
    [[nodiscard]] bool retract(std::string& error) noexcept;
    [[nodiscard]] bool discard(std::string& error) noexcept;

private:
    struct Impl;

    AudioFilePublication(
        std::filesystem::path destination,
        std::filesystem::path stagingDirectory,
        std::filesystem::path temporaryPath,
        ExistingAudioFilePolicy existingPolicy,
        std::unique_ptr<Impl> impl);

    std::filesystem::path destination_;
    std::filesystem::path stagingDirectory_;
    std::filesystem::path temporaryPath_;
    ExistingAudioFilePolicy existingPolicy_ =
        ExistingAudioFilePolicy::Reject;
    std::unique_ptr<Impl> impl_;
    bool published_ = false;
};
}
