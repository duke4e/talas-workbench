#include "talas/app/VcsLayout.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <map>
#include <cmath>

namespace talas::app
{
std::vector<VcsRect> vcsCleanUp(std::size_t count, unsigned columns, int cellWidth, int cellHeight, int gap)
{
    std::vector<VcsRect> rects;
    const auto across = std::max(1U, columns);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto column = static_cast<int>(index % across);
        const auto row = static_cast<int>(index / across);
        rects.push_back({gap + column * (cellWidth + gap), gap + row * (cellHeight + gap), cellWidth, cellHeight});
    }
    return rects;
}

VcsTypeRow vcsTypeRow(const talas::project::VirtualControl& control) noexcept
{
    switch (control.kind)
    {
    case talas::project::VirtualControlKind::Gate:
        return VcsTypeRow::Gate;
    case talas::project::VirtualControlKind::Toggle:
        return VcsTypeRow::Toggle;
    case talas::project::VirtualControlKind::Fader:
        break;
    }
    return control.widget.style == talas::project::VirtualControlWidgetStyle::SmallFader ? VcsTypeRow::SmallFader : VcsTypeRow::Fader;
}

std::vector<VcsRect> vcsCleanUpByType(const std::vector<VcsTypeRow>& types, unsigned columns, int cellWidth, int cellHeight, int gap)
{
    std::vector<std::size_t> order(types.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::ranges::stable_sort(order, {}, [&types](std::size_t index) { return types[index]; });
    std::vector<VcsRect> rects(types.size());
    const auto across = static_cast<int>(std::max(1U, columns));
    int row = 0;
    int column = 0;
    for (std::size_t at = 0; at < order.size(); ++at)
    {
        if (at > 0 && types[order[at]] != types[order[at - 1]] && column > 0)
        {
            ++row;
            column = 0;
        }
        rects[order[at]] = {gap + column * (cellWidth + gap), gap + row * (cellHeight + gap), cellWidth, cellHeight};
        if (++column == across)
        {
            ++row;
            column = 0;
        }
    }
    return rects;
}

VcsRect vcsExtent(const std::vector<VcsRect>& rects)
{
    VcsRect extent;
    for (const auto& rect : rects)
    {
        extent.width = std::max(extent.width, rect.right());
        extent.height = std::max(extent.height, rect.bottom());
    }
    return extent;
}

std::vector<VcsRect> vcsFit(const std::vector<VcsRect>& rects, int width, int height)
{
    const auto extent = vcsExtent(rects);
    if (extent.width <= 0 || extent.height <= 0 || width <= 0 || height <= 0)
        return rects;
    const auto scaleX = static_cast<double>(width) / extent.width;
    const auto scaleY = static_cast<double>(height) / extent.height;
    std::vector<VcsRect> fitted;
    for (const auto& rect : rects)
    {
        const auto left = static_cast<int>(std::lround(rect.left * scaleX));
        const auto top = static_cast<int>(std::lround(rect.top * scaleY));
        fitted.push_back({left, top, static_cast<int>(std::lround(rect.right() * scaleX)) - left,
            static_cast<int>(std::lround(rect.bottom() * scaleY)) - top});
    }
    return fitted;
}

std::vector<VcsRect> vcsMoved(const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, int dx, int dy)
{
    // The whole selection moves alike, stopping where its first widget meets
    // the surface's edge.
    for (const auto index : selection)
    {
        if (index >= rects.size())
            continue;
        dx = std::max(dx, -rects[index].left);
        dy = std::max(dy, -rects[index].top);
    }
    auto moved = rects;
    for (const auto index : selection)
    {
        if (index >= moved.size())
            continue;
        moved[index].left += dx;
        moved[index].top += dy;
    }
    return moved;
}

VcsRect vcsResized(VcsRect rect, VcsCorner corner, int dx, int dy, int minimum)
{
    auto left = rect.left;
    auto top = rect.top;
    auto right = rect.right();
    auto bottom = rect.bottom();
    if (corner == VcsCorner::TopLeft || corner == VcsCorner::BottomLeft)
        left = std::clamp(left + dx, 0, right - minimum);
    else
        right = std::max(right + dx, left + minimum);
    if (corner == VcsCorner::TopLeft || corner == VcsCorner::TopRight)
        top = std::clamp(top + dy, 0, bottom - minimum);
    else
        bottom = std::max(bottom + dy, top + minimum);
    return {left, top, right - left, bottom - top};
}

std::set<std::size_t> vcsBoxSelection(const std::vector<VcsRect>& rects, VcsRect box)
{
    std::set<std::size_t> selection;
    for (std::size_t index = 0; index < rects.size(); ++index)
    {
        const auto& rect = rects[index];
        if (rect.left < box.right() && box.left < rect.right() && rect.top < box.bottom() && box.top < rect.bottom())
            selection.insert(index);
    }
    return selection;
}

std::set<std::size_t> vcsShiftClicked(std::set<std::size_t> selection, std::size_t widget)
{
    if (!selection.erase(widget))
        selection.insert(widget);
    return selection;
}

std::vector<VcsRect> vcsSqueezed(const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, bool vertical, int gap)
{
    std::vector<std::size_t> order(selection.begin(), selection.end());
    std::erase_if(order, [&rects](std::size_t index) { return index >= rects.size(); });
    std::ranges::sort(order, [&rects, vertical](std::size_t left, std::size_t right) {
        return vertical ? rects[left].top < rects[right].top : rects[left].left < rects[right].left;
    });
    auto squeezed = rects;
    std::optional<int> edge;
    for (const auto index : order)
    {
        auto& rect = squeezed[index];
        if (edge.has_value())
        {
            if (vertical)
                rect.top = *edge + gap;
            else
                rect.left = *edge + gap;
        }
        edge = vertical ? rect.bottom() : rect.right();
    }
    return squeezed;
}

VcsNumberPlacement vcsFaderNumberPlacement(bool upright, bool showNumber) noexcept
{
    if (!showNumber)
        return VcsNumberPlacement::None;
    return upright ? VcsNumberPlacement::Above : VcsNumberPlacement::Right;
}

std::string vcsSelectedValueText(const std::vector<std::string>& names)
{
    if (names.empty())
        return {};
    return names.size() == 1 ? names.front() : "More than one";
}

std::optional<VcsRect> vcsRectFromEdges(int left, int top, int right, int bottom)
{
    if (left < 0 || top < 0 || right <= left || bottom <= top)
        return std::nullopt;
    return VcsRect{left, top, right - left, bottom - top};
}

std::optional<VcsRect> vcsRectFromSize(int left, int top, int width, int height)
{
    if (left < 0 || top < 0 || width <= 0 || height <= 0)
        return std::nullopt;
    return VcsRect{left, top, width, height};
}

bool vcsSurfaceCutOff(const VcsRect& extent, int windowWidth, int windowHeight) noexcept
{
    return extent.width > windowWidth || extent.height > windowHeight;
}

bool vcsCleanUpFitsWindow(const VcsRect& surface, int windowWidth, int windowHeight) noexcept
{
    return !vcsSurfaceCutOff(surface, windowWidth, windowHeight);
}

std::optional<VcsNavigator> vcsNavigator(
    const VcsRect& extent, int viewLeft, int viewTop, int windowWidth, int windowHeight, int size) noexcept
{
    if (!vcsSurfaceCutOff(extent, windowWidth, windowHeight) || size <= 0 || windowWidth <= 0 || windowHeight <= 0)
        return std::nullopt;
    const auto scale = static_cast<double>(size) / static_cast<double>(std::max(extent.width, extent.height));
    const auto scaled = [scale](int value) { return static_cast<int>(std::lround(value * scale)); };
    VcsNavigator navigator;
    navigator.scale = scale;
    navigator.surface = {0, 0, std::max(1, scaled(extent.width)), std::max(1, scaled(extent.height))};
    navigator.view = {scaled(viewLeft), scaled(viewTop), std::max(1, scaled(std::min(windowWidth, extent.width))),
        std::max(1, scaled(std::min(windowHeight, extent.height)))};
    return navigator;
}

std::pair<int, int> vcsNavigatorDragged(const VcsNavigator& navigator, const VcsRect& extent, int viewLeft, int viewTop,
    int windowWidth, int windowHeight, int dx, int dy) noexcept
{
    const auto moved = [&](int origin, int delta, int window, int surface) {
        const auto wanted = origin + static_cast<int>(std::lround(delta / navigator.scale));
        return std::clamp(wanted, 0, std::max(0, surface - window));
    };
    return {moved(viewLeft, dx, windowWidth, extent.width), moved(viewTop, dy, windowHeight, extent.height)};
}

std::pair<int, int> vcsHandDragged(const VcsRect& extent, int viewLeft, int viewTop, int windowWidth, int windowHeight,
    int dx, int dy) noexcept
{
    const auto moved = [](int origin, int delta, int window, int surface) {
        return std::clamp(origin - delta, 0, std::max(0, surface - window));
    };
    return {moved(viewLeft, dx, windowWidth, extent.width), moved(viewTop, dy, windowHeight, extent.height)};
}

namespace
{
// The Event Values an expression reads: each !Name in it.
void collectEventNames(std::string_view text, std::vector<std::string>& names)
{
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] != '!' || index + 1 >= text.size() || !std::isalpha(static_cast<unsigned char>(text[index + 1])))
            continue;
        auto end = index + 1;
        while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_'))
            ++end;
        const auto name = std::string{text.substr(index + 1, end - index - 1)};
        if (std::ranges::find(names, name) == names.end())
            names.push_back(name);
        index = end - 1;
    }
}

[[nodiscard]] std::vector<std::string> nodeControls(
    const talas::project::ProjectDocument& document, const talas::project::ProjectNode& node, std::set<std::string>& visited)
{
    std::vector<std::string> names;
    for (const auto& parameter : node.parameters)
    {
        if (parameter.expression.has_value())
            collectEventNames(*parameter.expression, names);
    }
    if (node.soundDefinitionId.has_value() && visited.insert(*node.soundDefinitionId).second)
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *node.soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition != document.soundDefinitions.end())
        {
            for (const auto& inner : definition->graph.nodes)
            {
                for (auto& name : nodeControls(document, inner, visited))
                {
                    if (std::ranges::find(names, name) == names.end())
                        names.push_back(std::move(name));
                }
            }
        }
    }
    return names;
}

[[nodiscard]] std::string nodeName(const talas::project::ProjectDocument& document, const talas::project::ProjectNode& node)
{
    if (node.soundDefinitionId.has_value())
    {
        const auto definition = std::ranges::find(
            document.soundDefinitions, *node.soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
        if (definition != document.soundDefinitions.end() && !definition->displayName.empty())
            return definition->displayName;
    }
    return node.instanceId;
}
}

std::vector<VcsLayoutEntry> vcsLayouts(const talas::project::ProjectDocument& document)
{
    std::vector<VcsLayoutEntry> layouts;
    VcsLayoutEntry top;
    for (const auto& control : document.virtualControls)
        top.eventNames.push_back(control.eventName);
    const auto known = [&document](const std::string& name) {
        return std::ranges::any_of(document.virtualControls, [&name](const auto& control) { return control.eventName == name; });
    };
    const auto controls = [&](const talas::project::ProjectNode& node) {
        std::set<std::string> visited;
        std::vector<std::string> names;
        for (auto& name : nodeControls(document, node, visited))
        {
            if (known(name))
                names.push_back(std::move(name));
        }
        return names;
    };
    // Playing a Timeline, the top layout is the
    // Timeline's own, the next layer each of its Sounds, and below them
    // the Sounds' subparts.
    if (!document.timeline.score.clips.empty())
    {
        top.name = "Timeline";
        layouts.push_back(std::move(top));
        std::set<std::string> listed;
        for (const auto& clip : document.timeline.score.clips)
        {
            const auto definition = std::ranges::find(
                document.soundDefinitions, clip.soundDefinitionId, &talas::project::ProjectSoundDefinition::definitionId);
            if (definition == document.soundDefinitions.end() || !listed.insert(clip.soundDefinitionId).second)
                continue;
            talas::project::ProjectNode sound;
            sound.instanceId = definition->definitionId;
            sound.soundDefinitionId = definition->definitionId;
            VcsLayoutEntry entry{nodeName(document, sound), 1, controls(sound), false};
            if (entry.eventNames.empty())
                continue;
            layouts.push_back(std::move(entry));
            for (const auto& part : definition->graph.nodes)
            {
                if (!part.soundDefinitionId.has_value())
                    continue;
                VcsLayoutEntry subpart{nodeName(document, part), 2, controls(part), false};
                if (!subpart.eventNames.empty())
                    layouts.push_back(std::move(subpart));
            }
        }
        for (const auto& page : document.virtualControlLayoutPages)
            layouts.push_back({page.name, 0, page.eventNames, true});
        return layouts;
    }
    // The Sounds from the output upstream, each at its distance.
    const auto& graph = document.graph;
    std::deque<std::pair<std::string, std::size_t>> pending;
    for (const auto& node : graph.nodes)
    {
        if (node.typeId == "talas.audioOutput" || node.typeId.starts_with("talas.output"))
        {
            for (const auto& connection : graph.connections)
            {
                if (connection.toNode == node.instanceId)
                    pending.emplace_back(connection.fromNode, 0);
            }
        }
    }
    top.name = pending.empty() ? "All controls" : "";
    std::set<std::string> seen;
    std::vector<VcsLayoutEntry> sounds;
    while (!pending.empty())
    {
        auto [nodeId, depth] = pending.front();
        pending.pop_front();
        if (!seen.insert(nodeId).second)
            continue;
        const auto node = std::ranges::find(graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
        if (node == graph.nodes.end())
            continue;
        if (top.name.empty())
            top.name = nodeName(document, *node);
        std::set<std::string> visited;
        VcsLayoutEntry entry{nodeName(document, *node), depth, {}, false};
        for (auto& name : nodeControls(document, *node, visited))
        {
            if (known(name))
                entry.eventNames.push_back(std::move(name));
        }
        if (!entry.eventNames.empty())
            sounds.push_back(std::move(entry));
        for (const auto& connection : graph.connections)
        {
            if (connection.toNode == nodeId)
                pending.emplace_back(connection.fromNode, depth + 1);
        }
    }
    layouts.push_back(std::move(top));
    // A Sound whose controls are all the surface's adds nothing below the
    // top layout.
    for (auto& sound : sounds)
    {
        if (sound.eventNames.size() < layouts.front().eventNames.size() || sounds.size() > 1)
        {
            // The output's own Sound shares the top layout's name.
            if (sound.depth == 0)
                sound.name += " (own controls)";
            layouts.push_back(std::move(sound));
        }
    }
    for (const auto& page : document.virtualControlLayoutPages)
        layouts.push_back({page.name, 0, page.eventNames, true});
    return layouts;
}

std::vector<VcsLayoutEntry> vcsLoadedSoundLayouts(
    const talas::project::ProjectDocument& document, const talas::project::ProjectDocument& loaded, std::string_view sound)
{
    // A Sound no longer of the project (another project opened since it was
    // loaded, or the Sound removed) leaves the project's own surface.
    const auto loadedNode = std::ranges::find(loaded.graph.nodes, sound, &talas::project::ProjectNode::instanceId);
    if (loadedNode == loaded.graph.nodes.end()
        || (std::ranges::none_of(document.graph.nodes,
                [&](const talas::project::ProjectNode& node) {
                    return node.instanceId == loadedNode->instanceId && node.typeId == loadedNode->typeId
                        && node.soundDefinitionId == loadedNode->soundDefinitionId;
                })
            && (!loadedNode->soundDefinitionId.has_value()
                || std::ranges::none_of(document.soundDefinitions, [&](const talas::project::ProjectSoundDefinition& definition) {
                       return definition.definitionId == *loadedNode->soundDefinitionId;
                   }))))
        return vcsLayouts(document);
    // The controls of the Sounds from the loaded structure's output upstream.
    std::set<std::string> reached;
    std::set<std::string> seen;
    std::set<std::string> visited;
    std::deque<std::string> pending;
    for (const auto& node : loaded.graph.nodes)
    {
        if (node.typeId != "talas.audioOutput" && !node.typeId.starts_with("talas.output"))
            continue;
        for (const auto& connection : loaded.graph.connections)
        {
            if (connection.toNode == node.instanceId)
                pending.push_back(connection.fromNode);
        }
    }
    while (!pending.empty())
    {
        const auto nodeId = pending.front();
        pending.pop_front();
        const auto node = std::ranges::find(loaded.graph.nodes, nodeId, &talas::project::ProjectNode::instanceId);
        if (!seen.insert(nodeId).second || node == loaded.graph.nodes.end())
            continue;
        for (auto& name : nodeControls(loaded, *node, visited))
            reached.insert(std::move(name));
        for (const auto& connection : loaded.graph.connections)
        {
            if (connection.toNode == nodeId)
                pending.push_back(connection.fromNode);
        }
    }
    // The Sound alone, not the Timeline, with the project's own controls and
    // made layouts.
    auto shown = loaded;
    shown.timeline.score.clips.clear();
    shown.virtualControls.clear();
    for (const auto& control : document.virtualControls)
    {
        if (reached.contains(control.eventName))
            shown.virtualControls.push_back(control);
    }
    shown.virtualControlLayoutPages = document.virtualControlLayoutPages;
    auto layouts = vcsLayouts(shown);
    if (!layouts.empty())
        layouts.front().name = nodeName(loaded, *loadedNode);
    return layouts;
}

std::string vcsLayoutLabel(const VcsLayoutEntry& layout)
{
    return std::string(layout.depth * 2, '.') + layout.name;
}

std::size_t vcsAdjacentLayout(std::size_t count, std::size_t current, bool up) noexcept
{
    if (count == 0)
        return 0;
    return up ? (current == 0 ? 0 : current - 1) : std::min(current + 1, count - 1);
}

talas::project::VirtualControlLayoutPage vcsDuplicatedLayout(const VcsLayoutEntry& layout)
{
    return {layout.name + " copy", layout.eventNames};
}

std::set<std::size_t> vcsLayoutsShiftClicked(std::set<std::size_t> chosen, std::size_t shown, std::size_t clicked)
{
    chosen.insert(shown);
    if (clicked != shown && !chosen.erase(clicked))
        chosen.insert(clicked);
    return chosen;
}

talas::project::VirtualControlLayoutPage vcsMergedLayout(std::string name, const std::vector<VcsLayoutEntry>& layouts)
{
    talas::project::VirtualControlLayoutPage merged{std::move(name), {}};
    for (const auto& layout : layouts)
    {
        for (const auto& eventName : layout.eventNames)
        {
            if (std::ranges::find(merged.eventNames, eventName) == merged.eventNames.end())
                merged.eventNames.push_back(eventName);
        }
    }
    return merged;
}

std::vector<VcsRect> vcsResizedTogether(
    std::vector<VcsRect> rects, const std::set<std::size_t>& selection, VcsCorner corner, int dx, int dy, int minimum)
{
    for (const auto index : selection)
    {
        if (index < rects.size())
            rects[index] = vcsResized(rects[index], corner, dx, dy, minimum);
    }
    return rects;
}

const std::vector<std::string_view>& vcsOptionsMenuItems()
{
    static const std::vector<std::string_view> items{
        "Add Widget", "Edit Text...", "Hide Selected Widgets", "Remove Selected Texts and Boxes"};
    return items;
}

const talas::project::VirtualControlLayoutSettings* findVcsLayoutSettings(
    const talas::project::ProjectDocument& document, std::string_view layout)
{
    if (layout.empty())
        return nullptr;
    const auto found = std::ranges::find(
        document.virtualControlLayoutSettings, layout, &talas::project::VirtualControlLayoutSettings::layout);
    return found == document.virtualControlLayoutSettings.end() ? nullptr : &*found;
}

std::vector<talas::project::VirtualControl> vcsLayoutControls(const talas::project::ProjectDocument& document, std::string_view layout)
{
    auto controls = document.virtualControls;
    const auto* settings = findVcsLayoutSettings(document, layout);
    if (settings == nullptr)
        return controls;
    for (auto& control : controls)
    {
        const auto own = std::ranges::find(settings->widgets, control.eventName, &talas::project::VirtualControlLayoutWidget::eventName);
        if (own == settings->widgets.end())
            continue;
        control.kind = own->kind;
        control.widget = own->widget;
        // The value stays within the Event Value's own range, the one the
        // other layouts share; the layout's lies within it as far as it can.
        const auto minimum = std::max(own->minimum, control.minimum);
        const auto maximum = std::min(own->maximum, control.maximum);
        if (minimum < maximum)
        {
            control.minimum = minimum;
            control.maximum = maximum;
        }
        control.value = std::clamp(control.value, control.minimum, control.maximum);
    }
    return controls;
}

talas::project::VirtualControlLayout vcsLayoutArrangement(const talas::project::ProjectDocument& document, std::string_view layout)
{
    const auto* settings = findVcsLayoutSettings(document, layout);
    return settings != nullptr && settings->arrangement.has_value() ? *settings->arrangement : document.virtualControlLayout;
}

namespace
{
// The layout's own settings, made the first time it keeps something.
[[nodiscard]] talas::project::VirtualControlLayoutSettings& ownSettings(
    std::vector<talas::project::VirtualControlLayoutSettings>& settings, std::string_view layout)
{
    const auto found = std::ranges::find(settings, layout, &talas::project::VirtualControlLayoutSettings::layout);
    if (found != settings.end())
        return *found;
    settings.push_back({std::string{layout}, {}, std::nullopt, {}});
    return settings.back();
}

// The layout's own widget for a control, starting as the shared one.
[[nodiscard]] talas::project::VirtualControlLayoutWidget& ownWidget(
    talas::project::VirtualControlLayoutSettings& settings, const talas::project::VirtualControl& control)
{
    const auto found = std::ranges::find(settings.widgets, control.eventName, &talas::project::VirtualControlLayoutWidget::eventName);
    if (found != settings.widgets.end())
        return *found;
    settings.widgets.push_back({control.eventName, control.kind, control.minimum, control.maximum, control.widget});
    return settings.widgets.back();
}
}

std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsPlaced(const talas::project::ProjectDocument& document,
    std::string_view layout, talas::project::VirtualControlLayout arrangement,
    const std::vector<std::pair<std::string, std::optional<talas::project::VirtualControlBounds>>>& places)
{
    auto settings = document.virtualControlLayoutSettings;
    auto& own = ownSettings(settings, layout);
    own.arrangement = arrangement;
    for (const auto& [eventName, bounds] : places)
    {
        const auto control = std::ranges::find(document.virtualControls, eventName, &talas::project::VirtualControl::eventName);
        if (control != document.virtualControls.end())
            ownWidget(own, *control).widget.bounds = bounds;
    }
    return settings;
}

std::optional<std::vector<talas::project::VirtualControlLayoutSettings>> vcsLayoutSettingsWidgetSet(
    const talas::project::ProjectDocument& document, std::string_view layout, std::string_view eventName,
    const talas::project::VirtualControlWidget& widget, double minimum, double maximum)
{
    const auto control = std::ranges::find(document.virtualControls, eventName, &talas::project::VirtualControl::eventName);
    if (layout.empty() || control == document.virtualControls.end() || !std::isfinite(minimum) || !std::isfinite(maximum)
        || !(minimum < maximum) || minimum < control->minimum || maximum > control->maximum
        || !talas::project::isValidVirtualControlWidget(widget, minimum, maximum))
        return std::nullopt;
    auto settings = document.virtualControlLayoutSettings;
    auto& own = ownWidget(ownSettings(settings, layout), *control);
    own.minimum = minimum;
    own.maximum = maximum;
    own.widget = widget;
    return settings;
}

std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsWithPresets(
    const talas::project::ProjectDocument& document, std::string_view layout, std::string bankId)
{
    auto settings = document.virtualControlLayoutSettings;
    ownSettings(settings, layout).presetBankId = std::move(bankId);
    return settings;
}

std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsRenamed(
    std::vector<talas::project::VirtualControlLayoutSettings> settings, std::string_view from, const std::string& to)
{
    std::erase_if(settings, [&](const auto& layout) { return layout.layout == to && from != to; });
    for (auto& layout : settings)
    {
        if (layout.layout == from)
            layout.layout = to;
    }
    return settings;
}

std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsWithout(
    std::vector<talas::project::VirtualControlLayoutSettings> settings, std::string_view layout)
{
    std::erase_if(settings, [&](const auto& kept) { return kept.layout == layout; });
    return settings;
}

std::vector<talas::project::VirtualControlLayoutSettings> vcsLayoutSettingsCopied(
    std::vector<talas::project::VirtualControlLayoutSettings> settings, std::string_view from, const std::string& to)
{
    const auto source = std::ranges::find(settings, from, &talas::project::VirtualControlLayoutSettings::layout);
    if (source == settings.end() || from == to)
        return settings;
    auto copy = *source;
    copy.layout = to;
    copy.presetBankId.clear();
    settings = vcsLayoutSettingsWithout(std::move(settings), to);
    settings.push_back(std::move(copy));
    return settings;
}

talas::project::VirtualControlLayoutPage vcsLayoutWithout(
    talas::project::VirtualControlLayoutPage layout, const std::set<std::string>& removed)
{
    std::erase_if(layout.eventNames, [&removed](const std::string& name) { return removed.contains(name); });
    return layout;
}

std::vector<VcsRect> vcsAligned(const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, VcsAlignment alignment)
{
    std::vector<std::size_t> chosen;
    for (const auto index : selection)
    {
        if (index < rects.size())
            chosen.push_back(index);
    }
    if (chosen.empty())
        return rects;
    auto aligned = rects;
    const auto& first = rects[chosen.front()];
    for (const auto index : chosen)
    {
        auto& rect = aligned[index];
        switch (alignment)
        {
        case VcsAlignment::Left: rect.left = first.left; break;
        case VcsAlignment::Right: rect.left = first.right() - rect.width; break;
        case VcsAlignment::Top: rect.top = first.top; break;
        case VcsAlignment::Bottom: rect.top = first.bottom() - rect.height; break;
        case VcsAlignment::HorizontalCentres: rect.top = first.top + first.height / 2 - rect.height / 2; break;
        case VcsAlignment::VerticalCentres: rect.left = first.left + first.width / 2 - rect.width / 2; break;
        }
        rect.left = std::max(0, rect.left);
        rect.top = std::max(0, rect.top);
    }
    return aligned;
}

std::vector<VcsRect> vcsDistributed(const std::vector<VcsRect>& rects, const std::set<std::size_t>& selection, bool horizontal)
{
    std::vector<std::size_t> order;
    for (const auto index : selection)
    {
        if (index < rects.size())
            order.push_back(index);
    }
    if (order.size() < 3)
        return rects;
    const auto centre = [&rects, horizontal](std::size_t index) {
        const auto& rect = rects[index];
        return horizontal ? rect.left + rect.width / 2 : rect.top + rect.height / 2;
    };
    std::ranges::sort(order, [&centre](std::size_t left, std::size_t right) { return centre(left) < centre(right); });
    const auto first = centre(order.front());
    const auto last = centre(order.back());
    auto spread = rects;
    for (std::size_t step = 1; step + 1 < order.size(); ++step)
    {
        const auto target = first + static_cast<int>(std::lround(static_cast<double>(last - first) * step / (order.size() - 1)));
        auto& rect = spread[order[step]];
        if (horizontal)
            rect.left = target - rect.width / 2;
        else
            rect.top = target - rect.height / 2;
    }
    return spread;
}

namespace
{
constexpr std::string_view decorationPrefix = "#decoration ";
}

std::string vcsDecorationName(std::size_t index)
{
    return std::string{decorationPrefix} + std::to_string(index + 1);
}

std::optional<std::size_t> vcsDecorationIndex(std::string_view name) noexcept
{
    if (!name.starts_with(decorationPrefix))
        return std::nullopt;
    name.remove_prefix(decorationPrefix.size());
    std::size_t number = 0;
    if (name.empty() || name.size() > 3)
        return std::nullopt;
    for (const auto character : name)
    {
        if (character < '0' || character > '9')
            return std::nullopt;
        number = number * 10 + static_cast<std::size_t>(character - '0');
    }
    if (number == 0)
        return std::nullopt;
    return number - 1;
}

talas::project::VirtualControlDecoration vcsAddedDecoration(
    talas::project::VirtualControlDecorationKind kind, const VcsRect& widgets, std::string text)
{
    const auto box = kind == talas::project::VirtualControlDecorationKind::Box;
    const auto left = widgets.width > 0 ? widgets.left + widgets.width + 8 : 8;
    return {kind, std::move(text), VcsRect{left, 8, box ? 240 : 200, box ? 160 : 32}};
}

talas::project::VirtualControlDecoration vcsPastedText(std::string text, const VcsRect& surface)
{
    return {talas::project::VirtualControlDecorationKind::Text, std::move(text),
        VcsRect{0, 0, std::max(surface.left + surface.width, 16), std::max(surface.top + surface.height, 16)}};
}

std::optional<std::vector<talas::project::VirtualControlDecoration>> vcsDecorationRelabelled(
    std::vector<talas::project::VirtualControlDecoration> decorations, std::size_t index, std::string text)
{
    if (index >= decorations.size() || decorations[index].kind != talas::project::VirtualControlDecorationKind::Text)
        return std::nullopt;
    decorations[index].text = std::move(text);
    return decorations;
}

namespace
{
[[nodiscard]] std::vector<VcsWidgetState> widgetStates(const std::vector<talas::project::VirtualControl>& controls)
{
    std::vector<VcsWidgetState> states;
    states.reserve(controls.size());
    for (const auto& control : controls)
        states.push_back({control.eventName, control.kind, control.minimum, control.maximum, control.widget});
    return states;
}

// The layouts' own arrangements and widgets, without their presets, which
// are not changes to the surface.
[[nodiscard]] std::vector<talas::project::VirtualControlLayoutSettings> arrangedSettings(
    std::vector<talas::project::VirtualControlLayoutSettings> settings)
{
    for (auto& layout : settings)
        layout.presetBankId.clear();
    std::erase_if(settings, [](const auto& layout) { return !layout.arrangement.has_value() && layout.widgets.empty(); });
    return settings;
}
}

VcsSurface vcsSurface(const talas::project::ProjectDocument& document)
{
    return {widgetStates(document.virtualControls), document.virtualControlLayout, document.virtualControlLayoutPages,
        document.virtualControlDecorations, document.dormantVirtualControls, document.virtualControlLayoutSettings};
}

bool vcsLayoutSwitchAsksToSave(const std::optional<VcsSurface>& unlocked, const talas::project::ProjectDocument& document)
{
    return unlocked.has_value() && vcsSurfaceChanged(*unlocked, document);
}

bool vcsSurfaceChanged(const VcsSurface& unlocked, const talas::project::ProjectDocument& document)
{
    return widgetStates(document.virtualControls) != unlocked.widgets
        || document.virtualControlLayout != unlocked.layout || document.virtualControlLayoutPages != unlocked.pages
        || document.virtualControlDecorations != unlocked.decorations
        || widgetStates(document.dormantVirtualControls) != widgetStates(unlocked.dormant)
        || arrangedSettings(document.virtualControlLayoutSettings) != arrangedSettings(unlocked.settings);
}

talas::project::ProjectDocument vcsSurfaceRestored(talas::project::ProjectDocument document, const VcsSurface& unlocked)
{
    const auto placeOf = [&](std::string_view name) {
        const auto found = std::ranges::find(unlocked.widgets, name, &VcsWidgetState::eventName);
        return static_cast<std::size_t>(found - unlocked.widgets.begin());
    };
    for (auto& control : document.virtualControls)
    {
        const auto index = placeOf(control.eventName);
        if (index == unlocked.widgets.size())
            continue;
        const auto& was = unlocked.widgets[index];
        control.kind = was.kind;
        control.minimum = was.minimum;
        control.maximum = was.maximum;
        control.widget = was.widget;
        control.value = std::clamp(control.value, std::min(was.minimum, was.maximum), std::max(was.minimum, was.maximum));
    }
    std::ranges::stable_sort(document.virtualControls, {}, [&](const talas::project::VirtualControl& control) {
        return placeOf(control.eventName);
    });
    document.virtualControlLayout = unlocked.layout;
    document.virtualControlLayoutPages = unlocked.pages;
    document.virtualControlDecorations = unlocked.decorations;
    document.dormantVirtualControls = unlocked.dormant;
    // A layout's presets are not the surface's arrangement: the bank it
    // names since it was unlocked stays named.
    auto settings = unlocked.settings;
    for (const auto& now : document.virtualControlLayoutSettings)
    {
        if (!now.presetBankId.empty())
            ownSettings(settings, now.layout).presetBankId = now.presetBankId;
    }
    document.virtualControlLayoutSettings = std::move(settings);
    return document;
}
}
