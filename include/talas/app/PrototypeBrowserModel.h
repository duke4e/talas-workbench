#pragma once

#include "talas/core/NodeRegistry.h"
#include "talas/project/ProjectDocument.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace talas::app
{
enum class PrototypeBrowserScope
{
    All,
    Favorites,
    Recent
};

enum class PrototypeMaturityFilter
{
    All,
    Full,
    Subset,
    Placeholder,
    Utility
};

enum class PrototypeRateFilter
{
    All,
    Constant,
    Block,
    Sample,
    Event,
    Frame,
    VariableOrUnknown
};

enum class PrototypeOriginFilter
{
    All,
    BuiltIn,
    Project,
    PreservedComponent,
    IsolatedComponent,
    External
};

struct PrototypeBrowserEntry
{
    std::string typeId;
    std::string displayName;
    std::string category;
    PrototypeMaturityFilter maturity = PrototypeMaturityFilter::Utility;
    std::vector<talas::core::PortSignalRate> rates;
    std::string vendor = "Talas";
    talas::core::NodeOrigin origin = talas::core::NodeOrigin::BuiltIn;
    std::string searchIndex;
};

[[nodiscard]] PrototypeBrowserEntry prototypeBrowserEntry(
    const talas::core::NodeTypeDescriptor& descriptor);

[[nodiscard]] bool prototypeBrowserMatches(
    const PrototypeBrowserEntry& entry,
    std::string_view query,
    PrototypeBrowserScope scope,
    PrototypeMaturityFilter maturity,
    const std::unordered_set<std::string>& favorites,
    const std::vector<std::string>& recentTypeIds,
    PrototypeRateFilter rate = PrototypeRateFilter::All,
    PrototypeOriginFilter origin = PrototypeOriginFilter::All);

[[nodiscard]] std::string prototypeBrowserGroupName(
    PrototypeBrowserScope scope,
    std::string_view category);

[[nodiscard]] std::string prototypeCapabilityTooltip(
    const talas::core::NodeTypeDescriptor& descriptor);

void recordRecentPrototype(
    std::vector<std::string>& recentTypeIds,
    std::string typeId,
    std::size_t limit = 8);

// Zooming the Prototypes: Ctrl+] makes their entries
// larger, Ctrl+[ smaller to see more of them without scrolling; steps from
// -2 to 2 about the usual size, step 0.
inline constexpr int prototypeZoomSteps = 2;
[[nodiscard]] int zoomedPrototypeStep(int step, bool in) noexcept;
[[nodiscard]] int prototypeRowHeight(int step) noexcept;

// AR Drag&Drop, among the Prototypes'
// envelopes, is a VCA whose Gain an AR sets, made to be dropped on a line
// so that it shapes the Sound the line carries. Its AR comes with !KeyDown
// as its trigger and !KeyVelocity as its amplitude, which the Prototypes
// write into it as they make it: the AR's own defaults stay a Gate and a
// Scale of 1, so an AR saved without them plays as before.
inline constexpr std::string_view arDragAndDropPrototypeId = "talas.arDragAndDrop";
[[nodiscard]] bool isPrototypeAssembly(std::string_view typeId) noexcept;

// What a prototype made of several Sounds holds: the Sound that takes a
// line it is dropped on in its input `headInput`, and the Sound whose
// `sourceOutput` feeds the head's `headControl`.
struct PrototypeAssembly
{
    talas::project::ProjectNode head;
    std::string headInput;
    talas::project::ProjectNode source;
    std::string sourceOutput;
    std::string headControl;
};

// The assembly `prototypeId` names, its Sounds named after it and numbered
// so that `taken` holds neither; nothing for any other id.
[[nodiscard]] std::optional<PrototypeAssembly> prototypeAssembly(const talas::core::NodeRegistry& registry,
    std::string_view prototypeId, const std::function<bool(std::string_view)>& taken);

// The descriptor the Prototypes list an assembly by: its head's ports and
// fields under its own name and category.
[[nodiscard]] std::optional<talas::core::NodeTypeDescriptor> prototypeAssemblyDescriptor(
    const talas::core::NodeRegistry& registry, std::string_view prototypeId);

// The document with the assembly placed in the project's graph or the Sound
// `definitionId`: on `line` when given, the line's source feeding the head
// and the head the line's destination, else at `position`, its source
// below it. The caller applies it as one edit; `headId` names the head.
// Nothing, with `error` set, when it cannot be placed.
[[nodiscard]] std::optional<talas::project::ProjectDocument> documentWithPrototypeAssembly(
    const talas::project::ProjectDocument& document, const talas::core::NodeRegistry& registry,
    std::string_view prototypeId, std::optional<std::string_view> definitionId, talas::project::CanvasPosition position,
    const std::optional<talas::core::Connection>& line, std::string& error, std::string* headId = nullptr);

// The system prototypes: one example
// of each kind of Sound, legacy kinds left for their replacements, by
// category and then by name, and AR Drag&Drop.
[[nodiscard]] std::vector<talas::core::NodeTypeDescriptor> prototypePalette(
    const talas::core::NodeRegistry& registry);

// Some Sounds appear in more than one category. The
// categories a kind of Sound is listed in besides its own.
[[nodiscard]] std::vector<std::string_view> prototypeAlsoListedIn(std::string_view typeId);

// The rows of the palette for the entries matched, each in its category and
// the others it is listed in, by category and then by name.
struct PrototypePaletteRow
{
    std::size_t index = 0;
    std::string category;
};
[[nodiscard]] std::vector<PrototypePaletteRow> prototypePaletteRows(
    const std::vector<talas::core::NodeTypeDescriptor>& palette, const std::vector<std::size_t>& matches);

// A click in the Prototypes' list: on a
// prototype's icon it selects that prototype, the Sound the Action and Info
// menus then refer to and Ctrl+Space plays; on a category's name it shows
// or hides the prototypes listed under it and selects none.
struct PrototypeListRow
{
    // The prototype's place in the palette; none for a category's name.
    std::optional<std::size_t> nodeIndex;
    std::string category;
};

struct PrototypeListClick
{
    std::optional<std::size_t> selected;
    std::optional<std::string> toggledCategory;

    bool operator==(const PrototypeListClick&) const = default;
};

[[nodiscard]] PrototypeListClick prototypeListClick(const PrototypeListRow& row);

// Find prototype... lists the Sounds whose names
// contain the partial name typed, case aside, in a pull-down list to
// choose from, each name once in the palette's order; none for an empty
// name.
[[nodiscard]] std::vector<std::string> prototypeFindChoices(
    const std::vector<talas::core::NodeTypeDescriptor>& palette, std::string_view partialName);
}
