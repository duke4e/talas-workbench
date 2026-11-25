#include "project/SemanticVersion.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace
{
using talas::project::SemanticVersionOrder;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

SemanticVersionOrder compare(std::string_view left, std::string_view right)
{
    return talas::project::compareCanonicalSemanticVersionPrecedence(
        left,
        right,
        64);
}

void testCoreAndLargeNumericIdentifiersUseNumericPrecedence()
{
    require(compare("1.9.0", "1.10.0") == SemanticVersionOrder::Less,
        "core identifiers should compare numerically");
    require(
        compare(
            "999999999999999999999999999999.0.0",
            "1000000000000000000000000000000.0.0")
            == SemanticVersionOrder::Less,
        "comparison should not overflow fixed-width integers");
    require(compare("2.0.0", "1.99.99") == SemanticVersionOrder::Greater,
        "major precedence should dominate later identifiers");
}

void testCanonicalSemVerPrereleaseOrdering()
{
    const std::vector<std::string> ordered{
        "1.0.0-alpha",
        "1.0.0-alpha.1",
        "1.0.0-alpha.beta",
        "1.0.0-beta",
        "1.0.0-beta.2",
        "1.0.0-beta.11",
        "1.0.0-rc.1",
        "1.0.0",
    };
    for (std::size_t index = 1; index < ordered.size(); ++index)
    {
        require(
            compare(ordered[index - 1U], ordered[index])
                == SemanticVersionOrder::Less,
            "SemVer prerelease precedence should follow the canonical chain");
        require(
            compare(ordered[index], ordered[index - 1U])
                == SemanticVersionOrder::Greater,
            "SemVer precedence should be antisymmetric");
    }
}

void testBuildMetadataIsIgnoredAndMalformedInputFailsClosed()
{
    require(
        compare("1.2.3+build.1", "1.2.3+build.2")
            == SemanticVersionOrder::Equivalent,
        "build metadata must not affect SemVer precedence");
    require(
        compare("01.2.3", "1.2.3") == SemanticVersionOrder::Invalid
            && compare("1.2.3", "1.2") == SemanticVersionOrder::Invalid
            && compare("1.2.3", "1.2.3")
                == SemanticVersionOrder::Equivalent,
        "invalid operands should fail closed and exact versions should match");
}
}

int main()
{
    testCoreAndLargeNumericIdentifiersUseNumericPrecedence();
    testCanonicalSemVerPrereleaseOrdering();
    testBuildMetadataIsIgnoredAndMalformedInputFailsClosed();
    return 0;
}
