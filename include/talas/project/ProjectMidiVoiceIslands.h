#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace talas::project
{
struct ProjectDocument;
struct ProjectGraph;
struct ProjectNode;
struct ProjectSoundExpansionManifest;

inline constexpr std::size_t maximumPreparedMidiVoiceSlotsPerOwner = 128;
// Local preparation budgets.
inline constexpr std::size_t maximumPreparedMidiVoiceHierarchyScopes = 8192;
inline constexpr std::size_t maximumPreparedMidiVoiceNestingDepth = 64;

// Either modern KeyDowns or classic Pitches selects the sequencer's voice
// environment. Empty arrays leave existing amplitude-only projects unchanged.
[[nodiscard]] bool hasProjectAnalogSequencerKeySource(const ProjectNode& node) noexcept;

struct ProjectMidiVoiceOwnerConfiguration
{
    std::string flatOwnerNodeId;
    std::size_t voiceCount = 0;

    bool operator==(const ProjectMidiVoiceOwnerConfiguration&) const = default;
};

struct ProjectMidiVoiceAncestorIdentity
{
    std::string ownerNodeId;
    std::string definitionId;
    std::vector<std::string> instancePath;
    std::uint16_t voiceOrdinal = 0;

    bool operator==(const ProjectMidiVoiceAncestorIdentity&) const = default;
};

struct ProjectMidiVoiceScopeProjection
{
    std::uint32_t sourceScopeOrdinal = 0;
    std::uint32_t scopeOrdinal = 0;
    std::uint32_t parentScopeOrdinal = 0;
    std::vector<ProjectMidiVoiceAncestorIdentity> voiceAncestors;

    bool operator==(const ProjectMidiVoiceScopeProjection&) const = default;
};

struct ProjectMidiVoiceIsland
{
    std::string flatOwnerNodeId;
    std::string localOwnerNodeId;
    std::string definitionId;
    std::vector<std::string> instancePath;
    std::uint32_t parentScopeOrdinal = 0;
    std::uint32_t firstVoiceScopeOrdinal = 0;
    std::size_t voiceCount = 0;
    std::string rootNodeId;
    std::string rootPortId;
    std::string rightRootNodeId;
    std::string rightRootPortId;
    std::vector<std::string> nodeIds;
    // Stable lineage distinguishes copies of this owner under parent voices.
    // sourceScopeOrdinal remains the authored Sound scope; parentScopeOrdinal
    // may instead name its projection into the containing parent voice.
    std::uint32_t sourceScopeOrdinal = 0;
    std::vector<ProjectMidiVoiceAncestorIdentity> voiceAncestors;
    std::vector<ProjectMidiVoiceScopeProjection> scopeProjections;
    // Structural parent owner, expressed as its flat expanded node identity.
    // Empty for a root owner; templates retain this before voice projection.
    std::string parentOwnerNodeId;

    bool operator==(const ProjectMidiVoiceIsland&) const = default;
};

struct ProjectMidiVoiceIslandResult
{
    std::vector<ProjectMidiVoiceIsland> islands;
    std::string error;
    std::string errorNodeId;
    std::string errorPortId;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

struct ProjectMidiVoiceLocalIsland
{
    std::string ownerNodeId;
    // Empty for a root owner. A nested owner is a node of its parent's input
    // region; its own input nodes belong only to the child region.
    std::string parentOwnerNodeId;
    std::string rootNodeId;
    std::string rootPortId;
    std::string rightRootNodeId;
    std::string rightRootPortId;
    std::vector<std::string> nodeIds;

    bool operator==(const ProjectMidiVoiceLocalIsland&) const = default;
};

struct ProjectMidiVoiceLocalIslandResult
{
    std::vector<ProjectMidiVoiceLocalIsland> islands;
    std::string error;
    std::string errorNodeId;
    std::string errorPortId;

    [[nodiscard]] bool ok() const noexcept
    {
        return error.empty();
    }
};

// Discover exclusive input regions with explicit nested-owner boundaries.
// This is structural authority, not proof that a runtime can execute the tree.
// Connected controls of a nested owner belong to the parent input region.
[[nodiscard]] ProjectMidiVoiceLocalIslandResult
discoverProjectMidiVoiceInputHierarchy(const ProjectGraph& graph);

// Structural source authority shared by persistence/editor audits and the
// expanded runtime island projection. Each upstream node belongs to at most
// one exact local voice owner. This flat-runtime adapter still rejects nested
// owners until hierarchical lowering and scheduling are implemented.
[[nodiscard]] ProjectMidiVoiceLocalIslandResult
discoverProjectMidiVoiceLocalIslands(const ProjectGraph& graph);

[[nodiscard]] ProjectMidiVoiceIslandResult discoverProjectMidiVoiceIslands(
    const ProjectDocument& expandedDocument,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceOwnerConfiguration>& configurations);

// Discover reachable owner templates without allocating voice scopes.
// Templates have zero voiceCount and firstVoiceScopeOrdinal; their structural
// parent is retained in parentOwnerNodeId.
[[nodiscard]] ProjectMidiVoiceIslandResult
discoverProjectMidiVoiceHierarchyTemplates(
    const ProjectDocument& expandedDocument,
    const ProjectSoundExpansionManifest& expansion);

// Preparation authority for nested instances, ordered parent before child.
// Each owner needs a nonzero configuration. Descendant Sound scopes in an
// input region are projected once per voice; source scopes outside the owner's
// lexical subtree remain unsupported. This does not enable nested execution.
[[nodiscard]] ProjectMidiVoiceIslandResult discoverProjectMidiVoiceInstanceHierarchy(
    const ProjectDocument& expandedDocument,
    const ProjectSoundExpansionManifest& expansion,
    const std::vector<ProjectMidiVoiceOwnerConfiguration>& configurations);
}
