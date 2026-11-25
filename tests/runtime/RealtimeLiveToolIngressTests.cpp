#include "talas/runtime/RealtimeLiveToolIngress.h"

#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::runtime::RealtimeLiveToolTriggerRequest request(std::uint64_t generation)
{
    talas::runtime::RealtimeLiveToolTriggerRequest result;
    result.triggerKind =
        talas::project::ProjectLiveToolTriggerKind::VariableChange;
    result.sourceKind = talas::runtime::RealtimeLiveToolSourceKind::Variable;
    result.preparedToolIndex = 3;
    require(talas::runtime::setRealtimeLiveToolTriggerIdentifier(
                result, "enabled"),
        "bounded trigger identifier should fit");
    result.variableBoolean = true;
    result.hasVariableValue = true;
    result.documentAuthorityGeneration = 11;
    result.stageGeneration = 17;
    result.stageId = 23;
    result.sourceGeneration = 29;
    result.routeGeneration = 31;
    result.requestGeneration = generation;
    return result;
}

void testBoundedIngressAndExactAuthority()
{
    talas::runtime::RealtimeLiveToolIngress invalid(1);
    require(!invalid.ready(), "too-small Live Tool ingress must fail closed");
    talas::runtime::RealtimeLiveToolIngress ingress(2);
    require(ingress.ready() && ingress.capacity() == 2,
        "valid Live Tool ingress should prepare exact fixed storage");

    const auto first = request(1);
    const auto second = request(2);
    require(ingress.tryPush(first) && ingress.tryPush(second)
            && !ingress.tryPush(request(3)),
        "bounded Live Tool ingress must reject overflow without reservation holes");
    talas::runtime::RealtimeLiveToolTriggerRequest popped;
    require(ingress.tryPop(popped)
            && popped.requestGeneration == 1
            && popped.ingressSequence == 1
            && popped.identifierView() == "enabled"
            && ingress.tryPop(popped)
            && popped.requestGeneration == 2
            && popped.ingressSequence == 2
            && !ingress.tryPop(popped),
        "single consumer must observe exact FIFO trigger records");

    talas::runtime::RealtimeLiveToolAuthority authority;
    authority.documentAuthorityGeneration = 11;
    authority.stageGeneration = 17;
    authority.stageId = 23;
    const auto sourceIndex = static_cast<std::size_t>(
        talas::runtime::RealtimeLiveToolSourceKind::Variable);
    authority.sourceGenerations[sourceIndex] = 29;
    authority.routeGenerations[sourceIndex] = 31;
    require(talas::runtime::realtimeLiveToolTriggerMatchesAuthority(
                first, authority),
        "exact tool trigger should match its document/stage/source/route authority");
    authority.routeGenerations[sourceIndex] = 32;
    require(!talas::runtime::realtimeLiveToolTriggerMatchesAuthority(
                first, authority),
        "replacement route authority must reject queued prior-generation triggers");
}

void testIdentifierBound()
{
    auto value = request(1);
    const std::string oversized(
        talas::runtime::maximumRealtimeLiveToolIdentifierBytes + 1U, 'x');
    require(!talas::runtime::setRealtimeLiveToolTriggerIdentifier(
                value, oversized),
        "over-bound trigger identity must fail before queue publication");
}

void testSourceAuthorityPublicationIsCoherent()
{
    talas::runtime::RealtimeLiveToolSourceAuthorityPublication publication;
    std::uint64_t source = 99;
    std::uint64_t route = 99;
    require(publication.read(source, route) && source == 0 && route == 0,
        "empty Live Tool source authority should publish one coherent revoked pair");
    publication.publish(41, 43);
    require(publication.read(source, route) && source == 41 && route == 43,
        "Live Tool source authority should publish one coherent source/route pair");
    publication.clear();
    require(publication.read(source, route) && source == 0 && route == 0,
        "Live Tool source revocation should publish one coherent zero pair");
}
}

int main()
{
    testBoundedIngressAndExactAuthority();
    testIdentifierBound();
    testSourceAuthorityPublicationIsCoherent();
}
