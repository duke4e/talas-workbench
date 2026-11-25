#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace talas::runtime
{
// Immutable dependency ownership with bounded per-frame traversal scratch. A
// delayed input is an edge, not another DSP instance or a recorded audio buffer.
class PreparedActivationSchedule
{
public:
    struct Node
    {
        std::size_t firstOperation = 0, operationCount = 0;
        std::vector<std::size_t> dependencies;
        std::optional<std::size_t> delayedInput;
        std::optional<std::size_t> gateOperation;
    };
    struct Feedback { std::size_t read, commit; };
    PreparedActivationSchedule(std::vector<Node> nodes, std::vector<Feedback> feedback)
        : nodes_(std::move(nodes)), feedback_(std::move(feedback)), state_(nodes_.size()), visits_(nodes_.size())
    {
        std::vector<bool> consumed(nodes_.size());
        for (const auto& node : nodes_)
        {
            for (const auto dependency : node.dependencies)
            {
                if (dependency >= nodes_.size()) return;
                consumed[dependency] = true;
            }
            if (node.delayedInput)
            {
                if (*node.delayedInput >= nodes_.size()) return;
                consumed[*node.delayedInput] = true;
            }
        }
        for (const auto& pair : feedback_)
        {
            if (pair.read >= nodes_.size() || pair.commit >= nodes_.size()) return;
            consumed[pair.commit] = true;
        }
        for (std::size_t node = 0; node < nodes_.size(); ++node)
            if (!consumed[node]) roots_.push_back(node);
        ready_ = !roots_.empty();
    }
    bool ready() const noexcept { return ready_; }
    bool reached(std::size_t node) const noexcept { return node < state_.size() && state_[node] == 2; }
    const auto& feedback() const noexcept { return feedback_; }
    std::optional<std::size_t> gateOperation(std::size_t node) const noexcept
    { return node < nodes_.size() ? nodes_[node].gateOperation : std::nullopt; }

    template<class Condition, class Render>
    bool render(Condition&& condition, Render&& renderNode)
    {
        if (!ready_) return false;
        std::fill(state_.begin(), state_.end(), 0);
        const auto visit = [&](std::size_t root) {
            if (state_[root] == 2) return true;
            std::size_t depth = 1;
            visits_[0] = {root, 0, false}; state_[root] = 1;
            while (depth)
            {
                auto& frame = visits_[depth - 1];
                const auto& node = nodes_[frame.node];
                const auto descend = [&](std::size_t child) {
                    if (state_[child] == 1 || depth >= visits_.size()) return false;
                    state_[child] = 1; visits_[depth++] = {child, 0, false}; return true;
                };
                if (frame.nextDependency < node.dependencies.size())
                {
                    const auto dependency = node.dependencies[frame.nextDependency++];
                    if (state_[dependency] != 2 && !descend(dependency)) return false;
                    continue;
                }
                if (!frame.conditionChecked)
                {
                    frame.conditionChecked = true;
                    if (node.gateOperation)
                    {
                        const auto open = condition(*node.gateOperation);
                        if (!open) return false;
                        if (*open && node.delayedInput && state_[*node.delayedInput] != 2)
                        {
                            if (!descend(*node.delayedInput)) return false;
                            continue;
                        }
                    }
                }
                if (!renderNode(node)) return false;
                state_[frame.node] = 2; --depth;
            }
            return true;
        };
        for (const auto root : roots_) if (!visit(root)) return false;
        // A feedback write belongs to its active read, including reads activated
        // by another feedback commit. It never independently starts a held input.
        for (std::size_t pass = 0; pass <= feedback_.size(); ++pass)
        {
            bool pending = false;
            for (const auto& pair : feedback_)
                if (reached(pair.read) && !reached(pair.commit))
                {
                    if (!visit(pair.commit)) return false;
                    pending = true;
                }
            if (!pending) return true;
        }
        return false;
    }
private:
    struct Visit { std::size_t node, nextDependency; bool conditionChecked; };
    std::vector<Node> nodes_;
    std::vector<Feedback> feedback_;
    std::vector<std::size_t> roots_;
    std::vector<std::uint8_t> state_;
    std::vector<Visit> visits_;
    bool ready_ = false;
};
}
