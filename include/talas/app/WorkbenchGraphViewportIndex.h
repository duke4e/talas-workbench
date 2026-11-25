#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace talas::app
{
struct WorkbenchGraphViewportRect
{
    std::int64_t x = 0;
    std::int64_t y = 0;
    std::int64_t width = 0;
    std::int64_t height = 0;
};

class WorkbenchGraphViewportIndex final
{
public:
    static constexpr std::size_t maximumMembershipsPerItem = 1;
    static constexpr std::size_t maximumLeafItems = 8;

    void rebuild(std::span<const WorkbenchGraphViewportRect> items);
    void query(
        WorkbenchGraphViewportRect viewport,
        std::vector<std::size_t>& result);

    [[nodiscard]] std::size_t itemCount() const noexcept;
    [[nodiscard]] std::size_t membershipCount() const noexcept;
    [[nodiscard]] std::size_t lastCandidateCount() const noexcept;

private:
    struct Node
    {
        WorkbenchGraphViewportRect bounds;
        std::size_t begin = 0;
        std::size_t end = 0;
        std::size_t left = 0;
        std::size_t right = 0;
        bool leaf = true;
    };

    [[nodiscard]] std::size_t buildNode(
        std::size_t begin,
        std::size_t end);

    std::vector<WorkbenchGraphViewportRect> items_;
    std::vector<std::size_t> itemOrder_;
    std::vector<Node> nodes_;
    std::vector<std::size_t> queryStack_;
    std::size_t membershipCount_ = 0;
    std::size_t lastCandidateCount_ = 0;
};
}
