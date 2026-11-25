#include "talas/core/Graph.h"
#include "talas/core/NodeRegistry.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace
{
constexpr std::uint64_t propertySeed = 0x475241504850524fULL;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept
{
    state ^= state << 13U;
    state ^= state >> 7U;
    state ^= state << 17U;
    return state;
}

[[nodiscard]] std::string caseContext(std::size_t caseIndex)
{
    return " seed=" + std::to_string(propertySeed)
        + " case=" + std::to_string(caseIndex);
}

[[nodiscard]] bool contains(
    const talas::core::ValidationResult& result,
    talas::core::ValidationCode code) noexcept
{
    for (const auto& error : result.errors)
    {
        if (error.code == code)
            return true;
    }
    return false;
}

[[nodiscard]] bool sameResult(
    const talas::core::ValidationResult& left,
    const talas::core::ValidationResult& right) noexcept
{
    if (left.errors.size() != right.errors.size())
        return false;
    for (std::size_t index = 0; index < left.errors.size(); ++index)
    {
        if (left.errors[index].code != right.errors[index].code
            || left.errors[index].message != right.errors[index].message
            || left.errors[index].nodeId != right.errors[index].nodeId)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] talas::core::NodeRegistry registry()
{
    talas::core::NodeRegistry result;
    result.addNodeType({
        "source",
        "Source",
        "Property",
        {},
        {{"out", talas::core::PortType::Audio, false}},
        {},
    });
    for (std::size_t index = 0; index < 4; ++index)
    {
        result.addNodeType({
            "processor-" + std::to_string(index),
            "Processor " + std::to_string(index),
            "Property",
            {{"in", talas::core::PortType::Audio, true}},
            {{"out", talas::core::PortType::Audio, false}},
            {},
        });
    }
    result.addNodeType({
        "sink",
        "Sink",
        "Property",
        {{"in", talas::core::PortType::Audio, true}},
        {},
        {},
    });
    // Audio and control signals feed each other (ADR-0016); an event output
    // still cannot feed an audio input.
    result.addNodeType({
        "control",
        "Control",
        "Property",
        {},
        {{"out", talas::core::PortType::Event, false}},
        {},
    });
    return result;
}
}

int main()
{
    const auto nodeRegistry = registry();
    auto random = propertySeed;
    std::array<bool, 7> scenarioCoverage{};
    for (std::size_t caseIndex = 0; caseIndex < 256; ++caseIndex)
    {
        const auto suffix = std::to_string(caseIndex);
        talas::core::Graph graph;
        graph.addNode({"source" + suffix, "source"});
        graph.addNode({"sink" + suffix, "sink"});
        const auto processorCount = 1U + static_cast<std::size_t>(
            nextRandom(random) % 8U);
        for (std::size_t index = 0; index < processorCount; ++index)
        {
            graph.addNode({
                "processor" + suffix + "-" + std::to_string(index),
                "processor-" + std::to_string(nextRandom(random) % 4U),
            });
        }
        graph.connect({
            "source" + suffix,
            "out",
            "processor" + suffix + "-0",
            "in",
        });
        for (std::size_t index = 1; index < processorCount; ++index)
        {
            graph.connect({
                "processor" + suffix + "-" + std::to_string(index - 1U),
                "out",
                "processor" + suffix + "-" + std::to_string(index),
                "in",
            });
        }
        graph.connect({
            "processor" + suffix + "-"
                + std::to_string(processorCount - 1U),
            "out",
            "sink" + suffix,
            "in",
        });
        const auto extraSources = static_cast<std::size_t>(
            nextRandom(random) % 4U);
        for (std::size_t index = 0; index < extraSources; ++index)
        {
            graph.addNode({
                "extra" + suffix + "-" + std::to_string(index),
                "source",
            });
        }

        talas::core::ValidationCode expected{};
        auto expectsError = true;
        const auto scenario = static_cast<std::size_t>(
            nextRandom(random) % scenarioCoverage.size());
        scenarioCoverage[scenario] = true;
        switch (scenario)
        {
        case 0:
            expectsError = false;
            break;
        case 1:
            graph.addNode({"unknown" + suffix, "missing-type"});
            expected = talas::core::ValidationCode::UnknownNodeType;
            break;
        case 2:
            graph.addNode({"source" + suffix, "source"});
            expected = talas::core::ValidationCode::DuplicateNodeInstance;
            break;
        case 3:
            graph.connect({"missing", "out", "sink" + suffix, "in"});
            expected = talas::core::ValidationCode::UnknownNodeInstance;
            break;
        case 4:
            graph.connect({
                "source" + suffix, "missing", "sink" + suffix, "in"});
            expected = talas::core::ValidationCode::UnknownPort;
            break;
        case 5:
            graph.addNode({"control" + suffix, "control"});
            graph.connect({
                "control" + suffix, "out", "sink" + suffix, "in"});
            expected = talas::core::ValidationCode::PortTypeMismatch;
            break;
        default:
            graph.connect({
                "processor" + suffix + "-"
                    + std::to_string(processorCount - 1U),
                "out",
                "processor" + suffix + "-0",
                "in",
            });
            expected = talas::core::ValidationCode::CycleDetected;
            break;
        }

        const auto first = graph.validate(nodeRegistry);
        const auto second = graph.validate(nodeRegistry);
        require(
            sameResult(first, second),
            "graph diagnostics changed" + caseContext(caseIndex));
        require(
            expectsError ? contains(first, expected) : first.ok(),
            "graph property missed its expected result"
                + caseContext(caseIndex));
    }
    for (std::size_t scenario = 0;
         scenario < scenarioCoverage.size();
         ++scenario)
    {
        require(
            scenarioCoverage[scenario],
            "graph generator missed scenario seed="
                + std::to_string(propertySeed)
                + " scenario=" + std::to_string(scenario));
    }
}
