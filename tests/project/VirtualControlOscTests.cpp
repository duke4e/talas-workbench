#include "talas/core/BuiltinNodes.h"
#include "talas/project/ProjectEditor.h"
#include "talas/project/ProjectJson.h"
#include "talas/project/VirtualControlOsc.h"

#include <stdexcept>
#include <array>
#include <limits>
#include <string>
#include <string_view>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

talas::project::VirtualControl control(std::string eventName)
{
    talas::project::VirtualControl value;
    value.eventName = std::move(eventName);
    return value;
}

void testExactAddressGrammar()
{
    using talas::project::isValidOscAddress;
    require(isValidOscAddress("/gain"), "one exact segment should pass");
    require(isValidOscAddress("/synth_1/filter-cutoff"),
            "bounded identifier segments should pass");
    require(isValidOscAddress("/a/a/a/a/a/a/a/a"),
            "eight exact segments should pass");
    require(isValidOscAddress("/" + std::string(255, 'a')),
            "the 256-byte boundary should pass");

    const std::string embeddedNul{"/a\0b", 4};
    for (const auto& invalid : {
             std::string{}, std::string{"/"}, std::string{"gain"},
             std::string{"//gain"}, std::string{"/gain/"},
             std::string{"/1gain"}, std::string{"/gain value"},
             std::string{"/gain*"}, std::string{"/gain?"},
             std::string{"/[gain]"}, std::string{"/{gain}"},
             std::string{"/a/a/a/a/a/a/a/a/a"},
             "/" + std::string(256, 'a'),
             std::string{"/caf\xC3\xA9"}, embeddedNul})
    {
        require(!isValidOscAddress(invalid),
                "non-exact OSC address should fail closed");
    }
}

void testEditorAuthorityAndCanonicalRoundTrip()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor({}, registry);
    require(editor.addVirtualControl(control("gain")).ok(),
            "first OSC target should be authorable");
    require(editor.addVirtualControl(control("gate")).ok(),
            "second OSC target should be authorable");
    require(editor.setVirtualControlOscMapping(
                "gain", talas::project::OscControlMapping{"/synth/gain"}).ok(),
            "exact OSC mapping should commit");
    require(editor.setVirtualControlOscMapping(
                "gate", talas::project::OscControlMapping{"/synth/gate"}).ok(),
            "second exact OSC mapping should commit");
    require(!editor.setVirtualControlOscMapping(
                 "gate", talas::project::OscControlMapping{"/synth/gain"}).ok(),
            "duplicate exact OSC addresses should reject transactionally");
    require(editor.document().virtualControls[1].oscMapping->address
                == "/synth/gate",
            "failed conflict must retain the prior mapping");
    require(editor.setVirtualControlOscEgressMapping(
                "gain",
                talas::project::OscEgressMapping{"/feedback/gain"}).ok(),
            "exact OSC feedback mapping should commit");
    require(editor.setVirtualControlOscEgressMapping(
                "gate",
                talas::project::OscEgressMapping{"/feedback/gate"}).ok(),
            "second exact feedback mapping should commit");
    require(!editor.setVirtualControlOscEgressMapping(
                 "gate",
                 talas::project::OscEgressMapping{"/feedback/gain"}).ok(),
            "duplicate feedback addresses should reject transactionally");

    auto settings = editor.document().deviceSettings;
    settings.oscIngress.enabled = true;
    settings.oscIngress.port = 12000;
    settings.oscEgress.enabled = true;
    settings.oscEgress.port = 12001;
    require(editor.setDeviceSettings(settings).ok(),
            "enabled loopback port should commit");

    const auto canonical = talas::project::serializeProjectDocument(
        editor.document());
    require(canonical.find(R"json("version": 156)json")
                != std::string::npos
            && canonical.find(R"json("oscMapping": {)json")
                != std::string::npos
            && canonical.find(R"json("oscIngress": {)json")
                != std::string::npos
            && canonical.find(R"json("oscEgressMapping": {)json")
                != std::string::npos
            && canonical.find(R"json("oscEgress": {)json")
                != std::string::npos,
            "the current format should write format-19 OSC ingress and feedback authority canonically");
    const auto reopened = talas::project::parseProjectDocument(canonical);
    require(reopened.ok(), "canonical OSC authority should reopen");
    require(reopened.document.deviceSettings.oscIngress
                == settings.oscIngress
            && reopened.document.virtualControls[0].oscMapping->address
                == "/synth/gain"
            && reopened.document.deviceSettings.oscEgress
                == settings.oscEgress
            && reopened.document.virtualControls[0]
                   .oscEgressMapping->address
                == "/feedback/gain",
            "OSC ingress and feedback authority should round-trip");
    require(talas::project::serializeProjectDocument(reopened.document)
                == canonical,
            "OSC authority should be byte-stable on second serialization");

    auto premature = canonical;
    const auto version = premature.find(R"json("version": 156)json");
    premature.replace(
        version,
        std::string_view{R"json("version": 156)json"}.size(),
        R"json("version":  18)json");
    require(!talas::project::parseProjectDocument(premature).ok(),
            "format 18 should reject premature OSC feedback fields");

    auto unknown = canonical;
    const auto address = unknown.find(
        R"json("address": "/synth/gain")json");
    unknown.replace(
        address,
        std::string_view{R"json("address": "/synth/gain")json"}.size(),
        R"json("address": "/synth/gain", "extra": true)json");
    require(!talas::project::parseProjectDocument(unknown).ok(),
            "unknown OSC mapping fields should reject");

    auto oversized = canonical;
    const auto oversizedAddress = oversized.find(
        R"json("address": "/synth/gain")json");
    oversized.replace(
        oversizedAddress,
        std::string_view{R"json("address": "/synth/gain")json"}.size(),
        "\"address\": \"/"
            + std::string(talas::project::maximumOscAddressBytes, 'a')
            + "\"");
    require(!talas::project::parseProjectDocument(oversized).ok(),
            "oversized OSC authority should reject during bounded JSON decode");

    auto escapedOversized = canonical;
    const auto escapedAddress = escapedOversized.find(
        R"json("address": "/synth/gain")json");
    escapedOversized.replace(
        escapedAddress,
        std::string_view{R"json("address": "/synth/gain")json"}.size(),
        "\"address\": \"/"
            + std::string(talas::project::maximumOscAddressBytes, 'a')
            + "\"");
    for (std::size_t offset = escapedOversized.find(
             R"json("address": "/)json")
             + std::string_view{R"json("address": "/)json"}.size();
         offset < escapedOversized.size()
             && escapedOversized[offset] != '"';)
    {
        escapedOversized.replace(offset, 1, "\\u0061");
        offset += 6;
    }
    require(!talas::project::parseProjectDocument(escapedOversized).ok(),
            "escaped oversized OSC authority should reject during bounded decode");

    auto duplicate = canonical;
    const auto secondAddress = duplicate.find("/synth/gate");
    duplicate.replace(secondAddress, std::string_view{"/synth/gate"}.size(),
                      "/synth/gain");
    require(!talas::project::parseProjectDocument(duplicate).ok(),
            "duplicate persisted OSC addresses should reject");
}

void testIngressSettingBounds()
{
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::project::ProjectEditor editor({}, registry);
    auto settings = editor.document().deviceSettings;
    settings.oscIngress.port = 9001;
    require(!editor.setDeviceSettings(settings).ok(),
            "disabled OSC must retain canonical port 9000");
    settings.oscIngress.enabled = true;
    settings.oscIngress.port = 1023;
    require(!editor.setDeviceSettings(settings).ok(),
            "privileged OSC ports should reject");
    settings.oscIngress.port = 1024;
    require(editor.setDeviceSettings(settings).ok(),
            "minimum enabled OSC port should pass");
    settings.oscIngress.port = 65535;
    require(editor.setDeviceSettings(settings).ok(),
            "maximum enabled OSC port should pass");

    settings.oscEgress.port = 9000;
    require(!editor.setDeviceSettings(settings).ok(),
            "disabled feedback must retain canonical port 9001");
    settings.oscEgress.enabled = true;
    settings.oscEgress.port = 1023;
    require(!editor.setDeviceSettings(settings).ok(),
            "privileged feedback ports should reject");
    settings.oscEgress.port = 65535;
    require(!editor.setDeviceSettings(settings).ok(),
            "enabled ingress and feedback may not share a port");
    settings.oscEgress.port = 65534;
    require(editor.setDeviceSettings(settings).ok(),
            "distinct bounded feedback port should pass");
}

void testFeedbackNormalization()
{
    using talas::project::normalizeOscEgressValue;
    require(normalizeOscEgressValue(
                talas::project::VirtualControlKind::Fader,
                -2.0,
                -2.0,
                6.0) == 0.0
                && normalizeOscEgressValue(
                    talas::project::VirtualControlKind::Fader,
                    6.0,
                    -2.0,
                    6.0) == 1.0,
            "fader range endpoints should normalize exactly");
    const auto middle = normalizeOscEgressValue(
        talas::project::VirtualControlKind::Fader,
        0.0,
        -std::numeric_limits<double>::max(),
        std::numeric_limits<double>::max());
    require(middle.has_value() && *middle == 0.5,
            "extreme finite fader range should normalize without overflow");
    require(normalizeOscEgressValue(
                talas::project::VirtualControlKind::Gate,
                9.0,
                8.0,
                10.0)
                == 1.0,
            "gate midpoint should normalize to one");
    require(normalizeOscEgressValue(
                talas::project::VirtualControlKind::Toggle,
                -1.0,
                -2.0,
                2.0) == 0.0
                && normalizeOscEgressValue(
                    talas::project::VirtualControlKind::Toggle,
                    0.0,
                    -2.0,
                    2.0) == 1.0,
            "toggle feedback should share the exact persisted midpoint split");
    require(!normalizeOscEgressValue(
                 talas::project::VirtualControlKind::Fader,
                 2.0,
                 0.0,
                 1.0)
                 .has_value(),
            "out-of-range persisted feedback should fail closed");
    require(!normalizeOscEgressValue(
                 talas::project::VirtualControlKind::Fader,
                 0.5,
                 1.0,
                 0.0)
                 .has_value()
                && !normalizeOscEgressValue(
                    talas::project::VirtualControlKind::Fader,
                    std::numeric_limits<double>::quiet_NaN(),
                    0.0,
                    1.0)
                    .has_value(),
            "reversed and nonfinite feedback state should fail closed");
}

void testWholeSetRealtimeRoutes()
{
    talas::project::RealtimeOscControlRoutes routes;
    const std::array prepared{
        talas::project::PreparedOscControlRoute{
            {"/gain"}, 3, talas::project::VirtualControlKind::Fader,
            -1.0, 1.0, talas::expr::ValueDomain::Scalar, 41},
        talas::project::PreparedOscControlRoute{
            {"/gate"}, 7, talas::project::VirtualControlKind::Gate,
            10.0, 20.0, talas::expr::ValueDomain::Hertz, 41},
    };
    require(routes.replace(prepared).ok() && routes.size() == 2,
            "valid OSC route set should publish whole");
    const auto gain = routes.map("/gain", 0.25);
    require(gain.has_value() && gain->eventId == 3
                && gain->value == -0.5
                && gain->stageGeneration == 41,
            "fader route should use overflow-safe normalized interpolation");
    const auto gateLow = routes.map("/gate", 0.499);
    const auto gateHigh = routes.map("/gate", 0.5);
    require(gateLow.has_value() && gateLow->value == 10.0
                && gateHigh.has_value() && gateHigh->value == 20.0,
            "gate threshold should deterministically select min or max");
    require(!routes.map("/unmapped", 0.5).has_value(),
            "unmapped address should remain inert");

    auto duplicate = prepared;
    duplicate[1].mapping.address = "/gain";
    const auto rejected = routes.replace(duplicate);
    require(!rejected.ok()
                && rejected.code
                    == talas::project::OscControlRouteSetDiagnosticCode::DuplicateAddress
                && routes.size() == 2
                && routes.map("/gate", 1.0).has_value(),
            "invalid replacement must leave prior route set active");

    talas::project::RealtimeOscRouteAdmissionGate gate;
    require(gate.tryBegin(), "open OSC admission gate should admit a reader");
    gate.end();
    gate.closeAndWait();
    require(!gate.tryBegin(), "closed OSC admission gate should reject readers");
    gate.open();
    require(gate.tryBegin(), "reopened OSC admission gate should admit readers");
    gate.end();
}
}

int main()
{
    testExactAddressGrammar();
    testEditorAuthorityAndCanonicalRoundTrip();
    testIngressSettingBounds();
    testWholeSetRealtimeRoutes();
    testFeedbackNormalization();
    return 0;
}
