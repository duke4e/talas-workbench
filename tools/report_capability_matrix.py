#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import pathlib
import re
from collections import Counter


EXECUTION_VALUES = ("unsupported", "subset", "supported")
BINDING_VALUES = ("none", "graphExpansion", "legacyAdapter", "kernel")
MATURITY_VALUES = (
    "fullRuntime",
    "functionalSubset",
    "placeholder",
    "utility",
)
NESTED_VALUES = ("unsupported", "expanded", "native")
STATE_VALUES = ("unknown", "stateless", "sessionStateful")
LATENCY_VALUES = (
    "none",
    "fixed",
    "preparationTime",
    "variableOrUnknown",
)
PREPARED_LATENCY_RULE_VALUES = (
    "none",
    "parameterFrames",
    "parameterFramesMinusOne",
    "sampleRateOverParameterNextPowerOfTwoMinusOne",
    "simplePitchShifterInterval",
    "frequencyScaleProfile",
    "monotonizerProfile",
    "timeFrequencyScaleProfile",
    "cdpPhaseVocoderProfile",
    "cdpPortReported",
)
RESOURCE_ACCESS_VALUES = (
    "none",
    "immutableRead",
    "streamRead",
    "streamWrite",
    "variableOrUnknown",
)
FAN_IN_LATENCY_VALUES = (
    "notApplicable",
    "align",
    "preserveRelative",
    "variableOrUnknown",
)
OUTPUT_BUS_LAYOUT_VALUES = (
    "mono",
    "stereo",
    "discrete4",
    "discrete6",
    "discrete8",
    "variableOrUnknown",
)
PORT_TYPE_VALUES = (
    "audio",
    "control",
    "event",
    "sampleBuffer",
    "wavetable",
    "spectrum",
    "complexSpectrum",
    "noteStream",
)
PORT_RATE_VALUES = (
    "constant",
    "block",
    "sample",
    "event",
    "frame",
    "variableOrUnknown",
)
PORT_AUDIO_LAYOUT_VALUES = ("notApplicable",) + OUTPUT_BUS_LAYOUT_VALUES
PORT_EDGE_VALUES = ("notApplicable", "continuous", "gate", "trigger")
SPECTRAL_FRAME_VALUES = (
    "notApplicable",
    "legacyPartialTriples",
    "partialSpectrumV1",
    "complexSpectrumV1",
    "variableOrUnknown",
)
LIMITATION_PATTERN = re.compile(r"^[a-z][a-z0-9]*(?:-[a-z0-9]+)*$")


def _require_choice(value: object, choices: tuple[str, ...], field: str) -> str:
    if not isinstance(value, str) or value not in choices:
        raise ValueError(f"Unknown {field} capability: {value!r}")
    return value


def _require_bool(value: object, field: str) -> bool:
    if not isinstance(value, bool):
        raise ValueError(f"{field} must be boolean")
    return value


def _normalize_ports(raw: object, type_id: str, direction: str) -> list[dict]:
    if not isinstance(raw, list):
        raise ValueError(f"{type_id} {direction} ports must be an array")

    ports = []
    port_ids = set()
    for value in raw:
        if not isinstance(value, dict):
            raise ValueError(f"{type_id} {direction} port must be an object")
        port_id = value.get("id")
        if not isinstance(port_id, str) or not port_id:
            raise ValueError(f"{type_id} {direction} port id must be non-empty")
        if port_id in port_ids:
            raise ValueError(f"{type_id} has a duplicate {direction} port id")
        port_ids.add(port_id)

        port_type = _require_choice(
            value.get("type"), PORT_TYPE_VALUES, "port type"
        )
        rate = _require_choice(
            value.get("rate"), PORT_RATE_VALUES, "port rate"
        )
        audio_layout = _require_choice(
            value.get("audioLayout"),
            PORT_AUDIO_LAYOUT_VALUES,
            "port audioLayout",
        )
        edge = _require_choice(
            value.get("edge"), PORT_EDGE_VALUES, "port edge"
        )
        spectral_format = _require_choice(
            value.get("spectralFormat"),
            SPECTRAL_FRAME_VALUES,
            "port spectralFormat",
        )
        required = _require_bool(value.get("required"), "port required")
        timeline_bearing = _require_bool(
            value.get("timelineBearing"), "port timelineBearing"
        )

        if port_type == "audio":
            valid_contract = (
                rate == "sample"
                and audio_layout != "notApplicable"
                and edge == "continuous"
                and timeline_bearing
                and spectral_format == "notApplicable"
            )
        elif port_type == "control":
            valid_contract = (
                rate != "frame"
                and audio_layout == "notApplicable"
                and edge != "notApplicable"
                and spectral_format == "notApplicable"
            )
        elif port_type in ("event", "noteStream"):
            valid_contract = (
                rate == "event"
                and audio_layout == "notApplicable"
                and edge == "notApplicable"
                and not timeline_bearing
                and spectral_format == "notApplicable"
            )
        elif port_type in ("sampleBuffer", "wavetable"):
            valid_contract = (
                rate == "constant"
                and audio_layout == "notApplicable"
                and edge == "notApplicable"
                and not timeline_bearing
                and spectral_format == "notApplicable"
            )
        elif port_type == "spectrum":
            valid_contract = (
                rate == "frame"
                and audio_layout != "notApplicable"
                and edge == "continuous"
                and timeline_bearing
                and spectral_format
                in ("legacyPartialTriples", "partialSpectrumV1")
            )
        else:
            valid_contract = (
                rate == "frame"
                and audio_layout != "notApplicable"
                and edge == "continuous"
                and timeline_bearing
                and spectral_format == "complexSpectrumV1"
            )
        if not valid_contract:
            raise ValueError(
                f"{type_id} {direction} port {port_id} has an invalid typed contract"
            )

        port = {
            "id": port_id,
            "type": port_type,
            "required": required,
            "rate": rate,
            "audioLayout": audio_layout,
            "edge": edge,
            "timelineBearing": timeline_bearing,
            "spectralFormat": spectral_format,
        }
        # An input that takes many connections, such as a Mixer's Inputs.
        if "acceptsMany" in value:
            if direction != "input" or value["acceptsMany"] is not True:
                raise ValueError(
                    f"{type_id} {direction} port {port_id} acceptsMany must be a true input flag"
                )
            port["acceptsMany"] = True
        ports.append(port)
    return ports


def _normalize_node(raw: object) -> dict:
    if not isinstance(raw, dict):
        raise ValueError("Catalog node must be an object")

    type_id = raw.get("typeId")
    if not isinstance(type_id, str) or not type_id:
        raise ValueError("Catalog node typeId must be non-empty")
    capabilities = raw.get("capabilities")
    if not isinstance(capabilities, dict):
        raise ValueError(f"{type_id} must declare capabilities")

    offline = _require_choice(
        capabilities.get("offline"), EXECUTION_VALUES, "offline"
    )
    realtime = _require_choice(
        capabilities.get("preparedRealtime"),
        EXECUTION_VALUES,
        "preparedRealtime",
    )
    offline_binding = _require_choice(
        capabilities.get("offlineBinding"), BINDING_VALUES, "offlineBinding"
    )
    realtime_binding = _require_choice(
        capabilities.get("preparedRealtimeBinding"),
        BINDING_VALUES,
        "preparedRealtimeBinding",
    )
    for context, support, binding in (
        ("offline", offline, offline_binding),
        ("preparedRealtime", realtime, realtime_binding),
    ):
        if (support == "unsupported") != (binding == "none"):
            raise ValueError(
                f"{type_id} {context} support and binding must agree"
            )
    nested = _require_choice(
        capabilities.get("nestedSounds"), NESTED_VALUES, "nestedSounds"
    )
    state = _require_choice(
        capabilities.get("state"), STATE_VALUES, "state"
    )
    maturity = _require_choice(
        raw.get("supportLevel"), MATURITY_VALUES, "maturity"
    )
    inputs = _normalize_ports(raw.get("inputs"), type_id, "input")
    outputs = _normalize_ports(raw.get("outputs"), type_id, "output")

    latency = capabilities.get("latency")
    if not isinstance(latency, dict):
        raise ValueError(f"{type_id} latency must be an object")
    latency_kind = _require_choice(
        latency.get("kind"), LATENCY_VALUES, "latency"
    )
    fixed_frames = latency.get("fixedFrames")
    if not isinstance(fixed_frames, int) or isinstance(fixed_frames, bool):
        raise ValueError(f"{type_id} fixedFrames must be an integer")
    if fixed_frames < 0:
        raise ValueError(f"{type_id} fixedFrames cannot be negative")
    if latency_kind == "fixed" and fixed_frames == 0:
        raise ValueError(f"{type_id} fixed latency must be positive")
    if latency_kind != "fixed" and fixed_frames != 0:
        raise ValueError(f"{type_id} non-fixed latency cannot declare frames")
    parameter_id = latency.get("parameterId")
    if not isinstance(parameter_id, str):
        raise ValueError(f"{type_id} latency parameterId must be a string")
    prepared_rule = _require_choice(
        latency.get("preparedRule"),
        PREPARED_LATENCY_RULE_VALUES,
        "preparedRule",
    )
    if latency_kind == "preparationTime":
        if not parameter_id or prepared_rule == "none":
            raise ValueError(
                f"{type_id} preparation-time latency requires a parameter rule"
            )
    elif parameter_id or prepared_rule != "none":
        raise ValueError(
            f"{type_id} non-prepared latency cannot declare prepared fields"
        )

    resource_access = _require_choice(
        capabilities.get("resourceAccess"),
        RESOURCE_ACCESS_VALUES,
        "resourceAccess",
    )
    fan_in_latency = _require_choice(
        capabilities.get("fanInLatencyPolicy"),
        FAN_IN_LATENCY_VALUES,
        "fanInLatencyPolicy",
    )
    output_bus_layouts = capabilities.get("outputBusLayouts")
    if not isinstance(output_bus_layouts, list):
        raise ValueError(f"{type_id} outputBusLayouts must be an array")
    normalized_layouts = [
        _require_choice(value, OUTPUT_BUS_LAYOUT_VALUES, "outputBusLayouts")
        for value in output_bus_layouts
    ]
    if len(normalized_layouts) != len(set(normalized_layouts)):
        raise ValueError(f"{type_id} has a duplicate output bus layout")

    limitations = capabilities.get("limitations")
    if not isinstance(limitations, list) or not all(
        isinstance(value, str) and LIMITATION_PATTERN.fullmatch(value)
        for value in limitations
    ):
        raise ValueError(f"{type_id} limitations must be stable kebab-case IDs")
    if len(limitations) != len(set(limitations)):
        raise ValueError(f"{type_id} has a duplicate limitation ID")

    display_name = raw.get("displayName")
    category = raw.get("category")
    summary = raw.get("supportSummary")
    if not all(isinstance(value, str) and value for value in (
        display_name,
        category,
        summary,
    )):
        raise ValueError(f"{type_id} display metadata must be non-empty")

    replaced_by = raw.get("replacedBy")
    if replaced_by is not None and (
        not isinstance(replaced_by, str) or not replaced_by
    ):
        raise ValueError(f"{type_id} replacedBy must name a node type")

    node = {
        "typeId": type_id,
        "displayName": display_name,
        "category": category,
        "maturity": maturity,
        "offline": offline,
        "offlineBinding": offline_binding,
        "preparedRealtime": realtime,
        "preparedRealtimeBinding": realtime_binding,
        "nestedSounds": nested,
        "state": state,
        "supportsReset": _require_bool(
            capabilities.get("supportsReset"), "supportsReset"
        ),
        "typedEvents": _require_bool(
            capabilities.get("typedEvents"), "typedEvents"
        ),
        "transportAware": _require_bool(
            capabilities.get("transportAware"), "transportAware"
        ),
        "latency": {
            "kind": latency_kind,
            "fixedFrames": fixed_frames,
            "parameterId": parameter_id,
            "preparedRule": prepared_rule,
        },
        "resourceAccess": resource_access,
        "fanInLatencyPolicy": fan_in_latency,
        "outputBusLayouts": normalized_layouts,
        "inputs": inputs,
        "outputs": outputs,
        "limitations": list(limitations),
        "supportSummary": summary,
    }
    if replaced_by is not None:
        node["replacedBy"] = replaced_by
    return node


def build_matrix(catalog: object) -> dict:
    if not isinstance(catalog, dict) or not isinstance(catalog.get("nodes"), list):
        raise ValueError("Catalog must contain a nodes array")

    nodes = [_normalize_node(raw) for raw in catalog["nodes"]]
    nodes.sort(key=lambda node: node["typeId"])
    type_ids = [node["typeId"] for node in nodes]
    if len(type_ids) != len(set(type_ids)):
        raise ValueError("Catalog contains duplicate node type IDs")

    offline = Counter(node["offline"] for node in nodes)
    realtime = Counter(node["preparedRealtime"] for node in nodes)
    maturity = Counter(node["maturity"] for node in nodes)
    return {
        "schemaVersion": 2,
        "authority": "NodeRegistry catalog snapshot",
        "counts": {
            "offline": {value: offline[value] for value in EXECUTION_VALUES},
            "preparedRealtime": {
                value: realtime[value] for value in EXECUTION_VALUES
            },
            "maturity": {value: maturity[value] for value in MATURITY_VALUES},
        },
        "nodes": nodes,
    }


def render_json(matrix: dict) -> str:
    return json.dumps(matrix, indent=2, ensure_ascii=False) + "\n"


def _markdown_text(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def _port_summary(ports: list[dict]) -> str:
    if not ports:
        return "none"
    return "; ".join(
        (
            f"{port['id']}{'*' if port['required'] else ''}:"
            f"{port['type']}/{port['rate']}/{port['audioLayout']}/"
            f"{port['edge']}/"
            f"{'timeline' if port['timelineBearing'] else 'nonTimeline'}/"
            f"{port['spectralFormat']}"
        )
        for port in ports
    )


def render_markdown(matrix: dict) -> str:
    lines = [
        "# Capability Matrix v2",
        "",
        "Generated from the deterministic `NodeRegistry` catalog snapshot. Product maturity and execution contexts are orthogonal.",
        "",
        "## Execution summary",
        "",
        "| context | unsupported | subset | supported |",
        "| --- | ---: | ---: | ---: |",
    ]
    for context in ("offline", "preparedRealtime"):
        counts = matrix["counts"][context]
        label = "prepared realtime" if context == "preparedRealtime" else context
        lines.append(
            f"| {label} | {counts['unsupported']} | {counts['subset']} | {counts['supported']} |"
        )

    lines.extend([
        "",
        "## Product maturity",
        "",
        "| maturity | count |",
        "| --- | ---: |",
    ])
    for maturity, count in matrix["counts"]["maturity"].items():
        lines.append(f"| {maturity} | {count} |")

    lines.extend([
        "",
        "## Nodes",
        "",
        "| typeId | maturity | offline | offline binding | prepared realtime | realtime binding | nested Sounds | state | latency | resource access | fan-in latency | output layouts | inputs | outputs | limitations |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ])
    for node in matrix["nodes"]:
        latency = node["latency"]["kind"]
        if latency == "fixed":
            latency = f"{node['latency']['fixedFrames']} frames"
        elif latency == "preparationTime":
            latency = (
                f"{node['latency']['preparedRule']}"
                f"({node['latency']['parameterId']})"
            )
        output_layouts = ", ".join(node["outputBusLayouts"]) or "none"
        limitations = ", ".join(node["limitations"]) or "none"
        lines.append(
            "| "
            + " | ".join(
                _markdown_text(value)
                for value in (
                    node["typeId"],
                    node["maturity"],
                    node["offline"],
                    node["offlineBinding"],
                    node["preparedRealtime"],
                    node["preparedRealtimeBinding"],
                    node["nestedSounds"],
                    node["state"],
                    latency,
                    node["resourceAccess"],
                    node["fanInLatencyPolicy"],
                    output_layouts,
                    _port_summary(node["inputs"]),
                    _port_summary(node["outputs"]),
                    limitations,
                )
            )
            + " |"
        )
    return "\n".join(lines) + "\n"


def _write_or_check(path: pathlib.Path, content: str, check: bool) -> None:
    if check:
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            raise ValueError(f"Generated capability artifact is stale: {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate capability matrix v2 from a NodeRegistry catalog snapshot."
    )
    parser.add_argument(
        "--catalog",
        type=pathlib.Path,
        default=pathlib.Path("docs/coverage/builtin-node-catalog-v2.json"),
    )
    parser.add_argument(
        "--json",
        type=pathlib.Path,
        default=pathlib.Path("docs/coverage/capability-matrix-v2.json"),
    )
    parser.add_argument(
        "--markdown",
        type=pathlib.Path,
        default=pathlib.Path("docs/coverage/capability-matrix-v2.md"),
    )
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    catalog = json.loads(args.catalog.read_text(encoding="utf-8"))
    matrix = build_matrix(catalog)
    _write_or_check(args.json, render_json(matrix), args.check)
    _write_or_check(args.markdown, render_markdown(matrix), args.check)
    counts = matrix["counts"]
    print(
        "Capability matrix v2: "
        f"offline {counts['offline']}; "
        f"prepared realtime {counts['preparedRealtime']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
