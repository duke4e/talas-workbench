#!/usr/bin/env python3
"""Generate descriptor-derived node help and an original-example index."""

from __future__ import annotations

import argparse
import html
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import urllib.parse
from dataclasses import dataclass

sys.dont_write_bytecode = True

from report_capability_matrix import build_matrix


POLICY_KEYS = {
    "schemaVersion",
    "format",
    "catalogPath",
    "markdownOutput",
    "jsonOutput",
    "examples",
    "allowedUnregisteredTypeIds",
}
EXAMPLE_KEYS = {"path", "title", "expectedOutcome"}
NODE_KEYS = {
    "typeId",
    "displayName",
    "category",
    "vendor",
    "origin",
    "supportLevel",
    "supportSummary",
    "capabilities",
    "inputs",
    "outputs",
    "parameters",
}
CAPABILITY_KEYS = {
    "offline",
    "offlineBinding",
    "preparedRealtime",
    "preparedRealtimeBinding",
    "nestedSounds",
    "state",
    "supportsReset",
    "typedEvents",
    "transportAware",
    "latency",
    "resourceAccess",
    "fanInLatencyPolicy",
    "outputBusLayouts",
    "limitations",
}
LATENCY_KEYS = {"kind", "fixedFrames", "parameterId", "preparedRule"}
PORT_KEYS = {
    "id",
    "type",
    "required",
    "rate",
    "audioLayout",
    "edge",
    "timelineBearing",
    "spectralFormat",
}
PARAMETER_DOMAINS = {"scalar", "hertz", "seconds", "noteNumber"}
PARAMETER_KIND_KEYS = {
    "expression": {"id", "kind", "domain", "defaultExpression"},
    "choice": {"id", "kind", "domain", "defaultChoice", "choices"},
    "boolean": {"id", "kind", "domain", "defaultBoolean"},
    "resource": {"id", "kind", "domain", "defaultResource"},
    "text": {"id", "kind", "domain", "defaultText"},
}
EXPECTED_OUTCOMES = {"renderable", "validation-only", "preserved-unavailable"}
ORIGIN_VALUES = {
    "builtIn",
    "project",
    "preservedComponent",
    "isolatedComponent",
    "external",
}
TYPE_ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")


@dataclass(frozen=True)
class ExamplePolicy:
    path: str
    title: str
    expected_outcome: str


@dataclass(frozen=True)
class HelpPolicy:
    catalog_path: str
    markdown_output: str
    json_output: str
    examples: tuple[ExamplePolicy, ...]
    allowed_unregistered_type_ids: tuple[str, ...]


def _strict_object(pairs: list[tuple[str, object]]) -> dict:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON member: {key}")
        result[key] = value
    return result


def load_json(path: pathlib.Path) -> object:
    def reject_constant(value: str) -> object:
        raise ValueError(f"Non-RFC JSON constant: {value}")

    try:
        return json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=_strict_object,
            parse_constant=reject_constant,
        )
    except (json.JSONDecodeError, UnicodeError, OSError) as error:
        raise ValueError(f"Cannot read strict JSON {path}: {error}") from error


def _require_exact_keys(value: object, expected: set[str], context: str) -> dict:
    if not isinstance(value, dict) or set(value) != expected:
        raise ValueError(f"{context} must contain exact keys {sorted(expected)}")
    return value


def _safe_relative_path(value: object, prefix: str, suffix: str, context: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{context} must be a non-empty relative path")
    if "\\" in value:
        raise ValueError(f"{context} must use POSIX separators")
    path = pathlib.PurePosixPath(value)
    raw_parts = value.split("/")
    if (
        path.is_absolute()
        or any(part in {"", ".", ".."} for part in raw_parts)
    ):
        raise ValueError(f"{context} must stay within its repository scope")
    if not value.startswith(prefix) or not value.endswith(suffix):
        raise ValueError(f"{context} must match {prefix}*{suffix}")
    return value


def parse_policy(raw: object) -> HelpPolicy:
    policy = _require_exact_keys(raw, POLICY_KEYS, "node-help policy")
    if type(policy["schemaVersion"]) is not int or policy["schemaVersion"] != 1:
        raise ValueError("node-help policy schemaVersion must be integer 1")
    if policy["format"] != "talas.node-help-policy":
        raise ValueError("node-help policy format is unsupported")

    catalog_path = _safe_relative_path(
        policy["catalogPath"], "docs/coverage/", ".json", "catalogPath"
    )
    markdown_output = _safe_relative_path(
        policy["markdownOutput"], "docs/help/", ".md", "markdownOutput"
    )
    json_output = _safe_relative_path(
        policy["jsonOutput"], "docs/help/", ".json", "jsonOutput"
    )
    if markdown_output == json_output:
        raise ValueError("node-help outputs must be distinct")

    raw_examples = policy["examples"]
    if not isinstance(raw_examples, list) or not raw_examples:
        raise ValueError("node-help policy examples must be a non-empty array")
    examples: list[ExamplePolicy] = []
    seen_paths: set[str] = set()
    seen_titles: set[str] = set()
    for index, raw_example in enumerate(raw_examples):
        example = _require_exact_keys(
            raw_example, EXAMPLE_KEYS, f"examples[{index}]"
        )
        path = _safe_relative_path(
            example["path"], "examples/", ".talas.json", f"examples[{index}].path"
        )
        if len(pathlib.PurePosixPath(path).parts) != 2:
            raise ValueError("node-help examples must be top-level project files")
        title = example["title"]
        if not isinstance(title, str) or not title.strip() or title != title.strip():
            raise ValueError(f"examples[{index}].title must be trimmed and non-empty")
        outcome = example["expectedOutcome"]
        if not isinstance(outcome, str) or outcome not in EXPECTED_OUTCOMES:
            raise ValueError(f"examples[{index}].expectedOutcome is unsupported")
        if path in seen_paths or title in seen_titles:
            raise ValueError("node-help example paths and titles must be unique")
        seen_paths.add(path)
        seen_titles.add(title)
        examples.append(ExamplePolicy(path, title, outcome))
    if [example.path for example in examples] != sorted(seen_paths):
        raise ValueError("node-help examples must be ordered by path")

    raw_allowed = policy["allowedUnregisteredTypeIds"]
    if not isinstance(raw_allowed, list) or not all(
        isinstance(value, str) and TYPE_ID_PATTERN.fullmatch(value)
        for value in raw_allowed
    ):
        raise ValueError("allowedUnregisteredTypeIds must be type-id strings")
    if raw_allowed != sorted(set(raw_allowed)):
        raise ValueError("allowedUnregisteredTypeIds must be unique and ordered")
    return HelpPolicy(
        catalog_path,
        markdown_output,
        json_output,
        tuple(examples),
        tuple(raw_allowed),
    )


def _normalize_parameters(raw: object, type_id: str) -> list[dict]:
    if not isinstance(raw, list):
        raise ValueError(f"{type_id} parameters must be an array")
    result: list[dict] = []
    seen_ids: set[str] = set()
    for index, value in enumerate(raw):
        context = f"{type_id} parameter[{index}]"
        if not isinstance(value, dict):
            raise ValueError(f"{context} must be an object")
        kind = value.get("kind")
        if not isinstance(kind, str) or kind not in PARAMETER_KIND_KEYS:
            raise ValueError(f"{context} kind is unsupported")
        _require_exact_keys(value, PARAMETER_KIND_KEYS[kind], context)
        parameter_id = value["id"]
        if not isinstance(parameter_id, str) or not parameter_id:
            raise ValueError(f"{context} id must be non-empty")
        if parameter_id in seen_ids:
            raise ValueError(f"{type_id} has a duplicate parameter id")
        seen_ids.add(parameter_id)
        if value["domain"] not in PARAMETER_DOMAINS:
            raise ValueError(f"{context} domain is unsupported")
        normalized = dict(value)
        if kind == "choice":
            choices = value["choices"]
            if not isinstance(choices, list) or not choices or not all(
                isinstance(choice, str) and choice for choice in choices
            ):
                raise ValueError(f"{context} choices must be non-empty strings")
            if choices != list(dict.fromkeys(choices)):
                raise ValueError(f"{context} choices must be unique")
            if value["defaultChoice"] not in choices:
                raise ValueError(f"{context} defaultChoice must be listed")
        elif kind == "boolean":
            if type(value["defaultBoolean"]) is not bool:
                raise ValueError(f"{context} defaultBoolean must be boolean")
        else:
            default_key = next(key for key in value if key.startswith("default"))
            if not isinstance(value[default_key], str):
                raise ValueError(f"{context} {default_key} must be a string")
        result.append(normalized)
    return result


def normalize_catalog(raw: object) -> list[dict]:
    root = _require_exact_keys(raw, {"nodes"}, "catalog")
    if not isinstance(root["nodes"], list):
        raise ValueError("catalog nodes must be an array")
    for index, raw_node in enumerate(root["nodes"]):
        node = _require_exact_keys(
            raw_node,
            NODE_KEYS | ({"replacedBy"} if isinstance(raw_node, dict) and "replacedBy" in raw_node else set()),
            f"catalog node[{index}]",
        )
        vendor = node["vendor"]
        if (
            not isinstance(vendor, str)
            or not vendor
            or len(vendor.encode("utf-8")) > 512
        ):
            raise ValueError(
                f"catalog node[{index}] vendor must be non-empty and at most 512 UTF-8 bytes"
            )
        if node["origin"] not in ORIGIN_VALUES:
            raise ValueError(f"catalog node[{index}] origin is unsupported")
        capabilities = _require_exact_keys(
            node["capabilities"], CAPABILITY_KEYS, f"catalog node[{index}] capabilities"
        )
        _require_exact_keys(
            capabilities["latency"], LATENCY_KEYS, f"catalog node[{index}] latency"
        )
        for direction in ("inputs", "outputs"):
            ports = node[direction]
            if not isinstance(ports, list):
                raise ValueError(f"catalog node[{index}] {direction} must be an array")
            for port_index, port in enumerate(ports):
                _require_exact_keys(
                    port,
                    PORT_KEYS | ({"acceptsMany"} if isinstance(port, dict) and "acceptsMany" in port else set()),
                    f"catalog node[{index}] {direction}[{port_index}]",
                )

    matrix_nodes = build_matrix(raw)["nodes"]
    raw_by_id = {node["typeId"]: node for node in root["nodes"]}
    result: list[dict] = []
    for node in matrix_nodes:
        enriched = dict(node)
        enriched["vendor"] = raw_by_id[node["typeId"]]["vendor"]
        enriched["origin"] = raw_by_id[node["typeId"]]["origin"]
        enriched["parameters"] = _normalize_parameters(
            raw_by_id[node["typeId"]]["parameters"], node["typeId"]
        )
        result.append(enriched)
    return result


def _extract_graph_types(graph: object, context: str) -> list[str]:
    graph_object = _require_exact_keys(graph, {"nodes", "connections"}, context)
    nodes = graph_object["nodes"]
    if not isinstance(nodes, list):
        raise ValueError(f"{context}.nodes must be an array")
    result: list[str] = []
    for index, node in enumerate(nodes):
        if not isinstance(node, dict):
            raise ValueError(f"{context}.nodes[{index}] must be an object")
        type_id = node.get("typeId")
        sound_definition_id = node.get("soundDefinitionId")
        if type_id is not None:
            if (
                not isinstance(type_id, str)
                or not TYPE_ID_PATTERN.fullmatch(type_id)
                or sound_definition_id is not None
            ):
                raise ValueError(f"{context}.nodes[{index}].typeId is invalid")
            result.append(type_id)
        elif (
            not isinstance(sound_definition_id, str)
            or not TYPE_ID_PATTERN.fullmatch(sound_definition_id)
        ):
            raise ValueError(
                f"{context}.nodes[{index}] must reference a node type or Sound"
            )
    return result


def extract_example_types(raw: object, context: str) -> list[str]:
    if not isinstance(raw, dict) or raw.get("format") != "talas.project":
        raise ValueError(f"{context} must be a talas.project object")
    result = _extract_graph_types(raw.get("graph"), f"{context}.graph")
    definitions = raw.get("soundDefinitions", [])
    if not isinstance(definitions, list):
        raise ValueError(f"{context}.soundDefinitions must be an array")
    for index, definition in enumerate(definitions):
        if not isinstance(definition, dict):
            raise ValueError(f"{context}.soundDefinitions[{index}] must be an object")
        result.extend(_extract_graph_types(
            definition.get("graph"),
            f"{context}.soundDefinitions[{index}].graph",
        ))
    return sorted(set(result))


def _repository_root(root: pathlib.Path) -> pathlib.Path:
    resolved = root.resolve(strict=True)
    if not resolved.is_dir():
        raise ValueError("Repository root must be a regular directory")
    return resolved


def _repository_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    root = _repository_root(root)
    path = root
    for part in pathlib.PurePosixPath(relative).parts:
        path = path / part
        if path.is_symlink():
            raise ValueError(f"Repository path must not traverse symlinks: {relative}")
    if not path.is_file():
        raise ValueError(f"Required regular repository file is missing: {relative}")
    try:
        path.resolve(strict=True).relative_to(root)
    except ValueError as error:
        raise ValueError(f"Repository path escapes its root: {relative}") from error
    return path


def _repository_output_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    root = _repository_root(root)
    path = root
    parts = pathlib.PurePosixPath(relative).parts
    for part in parts[:-1]:
        path = path / part
        if path.is_symlink() or not path.is_dir():
            raise ValueError(
                f"Output parent must be a regular repository directory: {relative}"
            )
    path = path / parts[-1]
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise ValueError(f"Output must be a regular repository file: {relative}")
    return path


def _tracked_example_paths(root: pathlib.Path) -> set[str]:
    completed = subprocess.run(
        [
            "git",
            "-C",
            str(_repository_root(root)),
            "ls-files",
            "-z",
            "--",
            "examples",
        ],
        capture_output=True,
        check=False,
        timeout=30,
    )
    if completed.returncode != 0 or completed.stderr:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise ValueError(
            "Cannot establish tracked example provenance"
            + (f": {detail}" if detail else "")
        )
    try:
        paths = completed.stdout.decode("utf-8", errors="strict").split("\0")
    except UnicodeError as error:
        raise ValueError("Tracked example paths are not UTF-8") from error
    return {
        path
        for path in paths
        if path
        and len(pathlib.PurePosixPath(path).parts) == 2
        and path.startswith("examples/")
        and path.endswith(".talas.json")
    }


def build_help_model(
    root: pathlib.Path,
    policy: HelpPolicy,
    tracked_example_paths: set[str] | None = None,
) -> dict:
    nodes = normalize_catalog(load_json(_repository_path(root, policy.catalog_path)))
    node_ids = {node["typeId"] for node in nodes}
    allowed_unknown = set(policy.allowed_unregistered_type_ids)
    observed_unknown: set[str] = set()
    example_records: list[dict] = []
    node_examples = {type_id: [] for type_id in node_ids}

    actual_examples = (
        _tracked_example_paths(root)
        if tracked_example_paths is None
        else set(tracked_example_paths)
    )
    policy_examples = {example.path for example in policy.examples}
    if actual_examples != policy_examples:
        missing = sorted(actual_examples - policy_examples)
        stale = sorted(policy_examples - actual_examples)
        raise ValueError(
            f"node-help example allowlist mismatch; unlisted={missing}, missing={stale}"
        )

    for example in policy.examples:
        path = _repository_path(root, example.path)
        type_ids = extract_example_types(load_json(path), example.path)
        known = [type_id for type_id in type_ids if type_id in node_ids]
        unknown = [type_id for type_id in type_ids if type_id not in node_ids]
        invalid_unknown = sorted(set(unknown) - allowed_unknown)
        if invalid_unknown:
            raise ValueError(
                f"{example.path} contains unregistered node types {invalid_unknown}"
            )
        if unknown and example.expected_outcome != "preserved-unavailable":
            raise ValueError(
                f"{example.path} has preserved types but is not preserved-unavailable"
            )
        if example.expected_outcome == "preserved-unavailable" and not unknown:
            raise ValueError(
                f"{example.path} claims preserved-unavailable without preserved types"
            )
        observed_unknown.update(unknown)
        for type_id in known:
            node_examples[type_id].append(example.path)
        example_records.append({
            "path": example.path,
            "title": example.title,
            "expectedOutcome": example.expected_outcome,
            "nodeTypeIds": known,
            "preservedUnregisteredTypeIds": unknown,
        })
    if observed_unknown != allowed_unknown:
        raise ValueError(
            "allowedUnregisteredTypeIds must exactly match observed preserved types"
        )

    examples_by_path = {record["path"]: record for record in example_records}
    node_records: list[dict] = []
    for node in nodes:
        record = dict(node)
        record["examples"] = [
            {
                "path": path,
                "title": examples_by_path[path]["title"],
                "expectedOutcome": examples_by_path[path]["expectedOutcome"],
            }
            for path in sorted(node_examples[node["typeId"]])
        ]
        node_records.append(record)
    return {
        "schemaVersion": 1,
        "generatedBy": "tools/report_node_help.py",
        "authority": "NodeRegistry catalog snapshot plus repository-original examples",
        "nonClaims": [
            "semantic parity",
            "tutorial completeness",
            "hosted or embedded help availability",
        ],
        "catalogPath": policy.catalog_path,
        "examples": example_records,
        "nodes": node_records,
    }


def render_json(model: dict) -> str:
    return json.dumps(model, indent=2, ensure_ascii=False) + "\n"


def _one_line(value: object) -> str:
    text = str(value)
    if any(ord(character) < 32 or ord(character) == 127 for character in text):
        raise ValueError("Markdown metadata must not contain control characters")
    return text


def _md_text(value: object) -> str:
    text = html.escape(_one_line(value), quote=False)
    text = text.replace("#", "&#35;")
    for character in "\\`*_[]()|!":
        text = text.replace(character, f"&#{ord(character)};")
    return text


def _md_code(value: object) -> str:
    text = html.escape(_one_line(value), quote=False).replace("|", "&#124;")
    return f"<code>{text}</code>"


def _md_link(relative: str) -> str:
    return urllib.parse.quote(relative, safe="/._-")


def _yes_no(value: bool) -> str:
    return "yes" if value else "no"


def _default(parameter: dict) -> str:
    kind = parameter["kind"]
    key = {
        "expression": "defaultExpression",
        "choice": "defaultChoice",
        "boolean": "defaultBoolean",
        "resource": "defaultResource",
        "text": "defaultText",
    }[kind]
    value = parameter[key]
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value) if value != "" else "(empty)"


def _anchor(type_id: str) -> str:
    return "node-" + re.sub(r"[^a-z0-9]+", "-", type_id.lower()).strip("-")


def _latency(node: dict) -> str:
    latency = node["latency"]
    if latency["kind"] == "fixed":
        return f"fixed ({latency['fixedFrames']} frames)"
    if latency["kind"] == "preparationTime":
        return f"{latency['preparedRule']} via {latency['parameterId']}"
    return latency["kind"]


def render_markdown(model: dict) -> str:
    nodes = model["nodes"]
    categories: dict[str, list[dict]] = {}
    for node in nodes:
        categories.setdefault(node["category"], []).append(node)
    anchor_sources = [
        (f"category {category}", _anchor("category-" + category))
        for category in categories
    ] + [
        (f"node {node['typeId']}", _anchor(node["typeId"]))
        for node in nodes
    ]
    seen_anchors: dict[str, str] = {}
    for source, anchor in anchor_sources:
        if not anchor or anchor in seen_anchors:
            previous = seen_anchors.get(anchor, "empty anchor")
            raise ValueError(
                f"Markdown anchor collision: {previous} and {source} -> {anchor}"
            )
        seen_anchors[anchor] = source
    lines = [
        "# Built-in node reference",
        "",
        "<!-- Generated by tools/report_node_help.py; do not edit by hand. -->",
        "",
        "Generated from the live-checked `NodeRegistry` catalog and the exact",
        "repository-original example allowlist. Maturity and execution contexts",
        "are separate facts; `placeholder`, `subset`, and `unsupported` are not",
        "promoted by appearing in this reference.",
        "",
        "This is a descriptor reference, not a parity claim, semantic tutorial,",
        "hosted help service, or guarantee that every node has an example.",
        "",
        f"Nodes: **{len(nodes)}** · Examples: **{len(model['examples'])}**",
        "",
        "## Categories",
        "",
    ]
    for category in sorted(categories, key=str.casefold):
        lines.append(
            f"- [{_md_text(category)}](#{_anchor('category-' + category)}) "
            f"({len(categories[category])})"
        )

    for category in sorted(categories, key=str.casefold):
        lines.extend([
            "",
            f"<a id=\"{_anchor('category-' + category)}\"></a>",
            f"## {_md_text(category)}",
        ])
        for node in sorted(
            categories[category],
            key=lambda value: (value["displayName"].casefold(), value["typeId"]),
        ):
            lines.extend([
                "",
                f"<a id=\"{_anchor(node['typeId'])}\"></a>",
                f"### {_md_text(node['displayName'])}",
                "",
                f"{_md_code(node['typeId'])} · maturity {_md_code(node['maturity'])}",
                f"Vendor {_md_text(node['vendor'])} · origin {_md_code(node['origin'])}",
                "",
                _md_text(node["supportSummary"]),
                *(
                    [
                        "",
                        f"Replaced by {_md_code(node['replacedBy'])}; kept so older graphs still compile.",
                    ]
                    if "replacedBy" in node
                    else []
                ),
                "",
                "| capability | declared value |",
                "| --- | --- |",
                f"| Offline | {_md_code(node['offline'])} via {_md_code(node['offlineBinding'])} |",
                f"| Prepared realtime | {_md_code(node['preparedRealtime'])} via {_md_code(node['preparedRealtimeBinding'])} |",
                f"| Nested Sounds | {_md_code(node['nestedSounds'])} |",
                f"| State / reset | {_md_code(node['state'])} / {_yes_no(node['supportsReset'])} |",
                f"| Typed events / transport | {_yes_no(node['typedEvents'])} / {_yes_no(node['transportAware'])} |",
                f"| Latency | {_md_text(_latency(node))} |",
                f"| Resource access | {_md_code(node['resourceAccess'])} |",
                f"| Fan-in latency | {_md_code(node['fanInLatencyPolicy'])} |",
                f"| Output layouts | {_md_text(', '.join(node['outputBusLayouts']) or 'none')} |",
            ])
            for label, ports in (("Inputs", node["inputs"]), ("Outputs", node["outputs"])):
                lines.extend(["", f"**{label}**", ""])
                if not ports:
                    lines.append("None.")
                else:
                    lines.extend([
                        "| id | type | required | rate | layout | edge | timeline | spectral format |",
                        "| --- | --- | --- | --- | --- | --- | --- | --- |",
                    ])
                    for port in ports:
                        lines.append(
                            "| " + " | ".join((
                                _md_code(port['id'])
                                + (" (takes many)" if port.get("acceptsMany") else ""),
                                _md_code(port['type']),
                                _yes_no(port["required"]),
                                _md_code(port['rate']),
                                _md_code(port['audioLayout']),
                                _md_code(port['edge']),
                                _yes_no(port["timelineBearing"]),
                                _md_code(port['spectralFormat']),
                            )) + " |"
                        )
            lines.extend(["", "**Parameters**", ""])
            if not node["parameters"]:
                lines.append("None.")
            else:
                lines.extend([
                    "| id | kind | domain | default | choices |",
                    "| --- | --- | --- | --- | --- |",
                ])
                for parameter in node["parameters"]:
                    lines.append(
                        "| " + " | ".join((
                            _md_code(parameter['id']),
                            _md_code(parameter['kind']),
                            _md_code(parameter['domain']),
                            _md_code(_default(parameter)),
                            ", ".join(
                                _md_code(choice)
                                for choice in parameter.get("choices", [])
                            ) or "none",
                        )) + " |"
                    )
            lines.extend(["", "**Limitations**", ""])
            if node["limitations"]:
                lines.extend(f"- {_md_code(value)}" for value in node["limitations"])
            else:
                lines.append("None declared.")
            lines.extend(["", "**Original examples**", ""])
            if node["examples"]:
                for example in node["examples"]:
                    relative = _md_link("../../" + example["path"])
                    lines.append(
                        f"- [{_md_text(example['title'])}]({relative}) — "
                        f"{_md_code(example['expectedOutcome'])}"
                    )
            else:
                lines.append("No checked-in example currently references this node.")
    return "\n".join(lines) + "\n"


def _write_pair(paths_and_content: list[tuple[pathlib.Path, str]]) -> None:
    temporary: list[tuple[pathlib.Path, pathlib.Path]] = []
    for path, content in paths_and_content:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            prefix=f".{path.name}.tmp-",
            dir=path.parent,
            delete=False,
        ) as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
            temp = pathlib.Path(stream.name)
        temporary.append((temp, path))
    for temp, path in temporary:
        os.replace(temp, path)


def write_or_check(root: pathlib.Path, policy: HelpPolicy, model: dict, check: bool) -> None:
    outputs = [
        (_repository_output_path(root, policy.markdown_output), render_markdown(model)),
        (_repository_output_path(root, policy.json_output), render_json(model)),
    ]
    if check:
        for path, content in outputs:
            if not path.is_file() or path.read_bytes() != content.encode("utf-8"):
                raise ValueError(f"Generated node-help artifact is stale: {path}")
        return
    _write_pair(outputs)


def main() -> int:
    parser = argparse.ArgumentParser(allow_abbrev=False)
    parser.add_argument(
        "--policy",
        type=pathlib.Path,
        default=pathlib.Path("docs/governance/node-help-policy.json"),
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    try:
        if args.policy.is_absolute():
            raise ValueError("--policy must be a repository-relative path")
        policy_relative = _safe_relative_path(
            args.policy.as_posix(), "docs/governance/", ".json", "--policy"
        )
        policy_path = _repository_path(root, policy_relative)
        policy = parse_policy(load_json(policy_path))
        model = build_help_model(root, policy)
        write_or_check(root, policy, model, args.check)
    except (
        OSError,
        UnicodeError,
        ValueError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"Node help: FAIL: {error}", file=sys.stderr)
        return 1
    print(
        f"Node help: PASS: {len(model['nodes'])} nodes, "
        f"{len(model['examples'])} examples"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
