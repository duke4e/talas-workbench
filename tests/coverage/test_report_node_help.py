import json
import pathlib
import tempfile
import unittest
from unittest import mock

import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from report_node_help import build_help_model
from report_node_help import extract_example_types
from report_node_help import load_json
from report_node_help import normalize_catalog
from report_node_help import parse_policy
from report_node_help import render_json
from report_node_help import render_markdown
from report_node_help import _repository_output_path
from report_node_help import _repository_path
from report_node_help import _tracked_example_paths
from report_node_help import write_or_check


def descriptor(type_id="talas.fixture"):
    return {
        "typeId": type_id,
        "displayName": "Fixture | Node",
        "category": "Testing",
        "vendor": "Fixture Vendor",
        "origin": "external",
        "supportLevel": "placeholder",
        "supportSummary": "A truthful fixture summary.",
        "capabilities": {
            "offline": "subset",
            "offlineBinding": "legacyAdapter",
            "preparedRealtime": "unsupported",
            "preparedRealtimeBinding": "none",
            "nestedSounds": "expanded",
            "state": "stateless",
            "supportsReset": False,
            "typedEvents": False,
            "transportAware": False,
            "latency": {
                "kind": "none",
                "fixedFrames": 0,
                "parameterId": "",
                "preparedRule": "none",
            },
            "resourceAccess": "none",
            "fanInLatencyPolicy": "notApplicable",
            "outputBusLayouts": [],
            "limitations": ["fixture-placeholder"],
        },
        "inputs": [],
        "outputs": [],
        "parameters": [
            {
                "id": "amount",
                "kind": "choice",
                "domain": "scalar",
                "defaultChoice": "medium",
                "choices": ["low", "medium", "high"],
            }
        ],
    }


def policy(examples=None, allowed=None):
    return {
        "schemaVersion": 1,
        "format": "talas.node-help-policy",
        "catalogPath": "docs/coverage/catalog.json",
        "markdownOutput": "docs/help/reference.md",
        "jsonOutput": "docs/help/index.json",
        "examples": examples or [
            {
                "path": "examples/fixture.talas.json",
                "title": "Fixture project",
                "expectedOutcome": "renderable",
            }
        ],
        "allowedUnregisteredTypeIds": allowed or [],
    }


def project(*type_ids):
    return {
        "format": "talas.project",
        "graph": {
            "nodes": [
                {"id": f"node-{index}", "typeId": type_id}
                for index, type_id in enumerate(type_ids)
            ],
            "connections": [],
        },
        "soundDefinitions": [
            {
                "graph": {
                    "nodes": [
                        {
                            "id": "nested",
                            "typeId": type_ids[0],
                        }
                    ],
                    "connections": [],
                }
            }
        ] if type_ids else [],
    }


class NodeHelpTests(unittest.TestCase):
    def test_policy_is_closed_ordered_and_path_bounded(self):
        parsed = parse_policy(policy())
        self.assertEqual(parsed.examples[0].path, "examples/fixture.talas.json")

        extra = policy()
        extra["invented"] = True
        with self.assertRaisesRegex(ValueError, "exact keys"):
            parse_policy(extra)

        unsafe = policy()
        unsafe["examples"][0]["path"] = "examples/../outside.talas.json"
        with self.assertRaisesRegex(ValueError, "repository scope"):
            parse_policy(unsafe)

        noncanonical = policy()
        noncanonical["examples"][0]["path"] = "examples//fixture.talas.json"
        with self.assertRaisesRegex(ValueError, "repository scope"):
            parse_policy(noncanonical)

        demo = policy()
        demo["examples"][0]["path"] = "examples/demos/fixture.talas.json"
        self.assertEqual(
            parse_policy(demo).examples[0].path,
            "examples/demos/fixture.talas.json",
        )
        for nested in (
            "examples/resources/fixture.talas.json",
            "examples/demos/deeper/fixture.talas.json",
        ):
            unlisted_folder = policy()
            unlisted_folder["examples"][0]["path"] = nested
            with self.subTest(path=nested), self.assertRaisesRegex(
                ValueError, "top-level or demos/"
            ):
                parse_policy(unlisted_folder)

        unordered = policy(examples=[
            {
                "path": "examples/z.talas.json",
                "title": "Z",
                "expectedOutcome": "renderable",
            },
            {
                "path": "examples/a.talas.json",
                "title": "A",
                "expectedOutcome": "validation-only",
            },
        ])
        with self.assertRaisesRegex(ValueError, "ordered by path"):
            parse_policy(unordered)

        with tempfile.TemporaryDirectory() as directory:
            duplicate = pathlib.Path(directory) / "duplicate.json"
            duplicate.write_text('{"schemaVersion":1,"schemaVersion":1}',
                                 encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Duplicate JSON member"):
                load_json(duplicate)
            for constant in ("NaN", "Infinity", "-Infinity"):
                poison = pathlib.Path(directory) / "poison.json"
                poison.write_text(f'{{"poison":{constant}}}', encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "Non-RFC JSON constant"):
                    load_json(poison)

    def test_catalog_parameter_contract_is_strict(self):
        node = normalize_catalog({"nodes": [descriptor()]})[0]
        self.assertEqual(node["parameters"][0]["defaultChoice"], "medium")
        self.assertEqual(node["vendor"], "Fixture Vendor")
        self.assertEqual(node["origin"], "external")

        invalid = descriptor()
        invalid["parameters"][0]["defaultChoice"] = "missing"
        with self.assertRaisesRegex(ValueError, "must be listed"):
            normalize_catalog({"nodes": [invalid]})

        unknown = descriptor()
        unknown["parameters"][0]["invented"] = True
        with self.assertRaisesRegex(ValueError, "exact keys"):
            normalize_catalog({"nodes": [unknown]})

        empty_vendor = descriptor()
        empty_vendor["vendor"] = ""
        with self.assertRaisesRegex(ValueError, "vendor must be non-empty"):
            normalize_catalog({"nodes": [empty_vendor]})

        oversized_vendor = descriptor()
        oversized_vendor["vendor"] = "é" * 257
        with self.assertRaisesRegex(ValueError, "at most 512 UTF-8 bytes"):
            normalize_catalog({"nodes": [oversized_vendor]})

        unknown_origin = descriptor()
        unknown_origin["origin"] = "guessedFromTypeId"
        with self.assertRaisesRegex(ValueError, "origin is unsupported"):
            normalize_catalog({"nodes": [unknown_origin]})

    def test_repository_paths_reject_symlinked_parent_components(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "docs").mkdir()
            with mock.patch.object(
                pathlib.Path,
                "is_symlink",
                autospec=True,
                side_effect=lambda path: path.name in {"examples", "help"},
            ):
                with self.assertRaisesRegex(ValueError, "must not traverse"):
                    _repository_path(root, "examples/fixture.talas.json")
                with self.assertRaisesRegex(ValueError, "regular repository"):
                    _repository_output_path(root, "docs/help/reference.md")

    def test_example_provenance_uses_only_tracked_top_level_and_demo_projects(self):
        with tempfile.TemporaryDirectory() as directory, mock.patch(
            "report_node_help.subprocess.run",
            return_value=mock.Mock(
                returncode=0,
                stderr=b"",
                stdout=(
                    b"examples/fixture.talas.json\0"
                    b"examples/demos/demo.talas.json\0"
                    b"examples/demos/README.md\0"
                    b"examples/demos/deeper/nested.talas.json\0"
                    b"examples/resources/nested.talas.json\0"
                    b"examples/README.md\0"
                ),
            ),
        ) as runner:
            self.assertEqual(
                _tracked_example_paths(pathlib.Path(directory)),
                {
                    "examples/fixture.talas.json",
                    "examples/demos/demo.talas.json",
                },
            )
        self.assertEqual(runner.call_args.args[0][-1], "examples")

    def test_project_extraction_includes_nested_nodes_and_sound_instances(self):
        source = project("talas.beta", "talas.alpha")
        source["graph"]["nodes"].append({
            "id": "voice",
            "soundDefinitionId": "voiceDefinition",
        })
        self.assertEqual(
            extract_example_types(source, "fixture"),
            ["talas.alpha", "talas.beta"],
        )

    def test_model_binds_exact_examples_and_preserved_unknown_types(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            (root / "docs/coverage").mkdir(parents=True)
            (root / "docs/help").mkdir()
            (root / "examples").mkdir()
            (root / "docs/coverage/catalog.json").write_text(
                json.dumps({"nodes": [descriptor()]}), encoding="utf-8"
            )
            (root / "examples/fixture.talas.json").write_text(
                json.dumps(project("talas.fixture", "vendor.preserved")),
                encoding="utf-8",
            )
            accepted = policy(allowed=["vendor.preserved"])
            accepted["examples"][0]["expectedOutcome"] = "preserved-unavailable"
            parsed = parse_policy(accepted)

            tracked = {"examples/fixture.talas.json"}
            with self.assertRaisesRegex(ValueError, "unregistered node types"):
                build_help_model(root, parse_policy(policy()), tracked)

            false_outcome = policy(allowed=["vendor.preserved"])
            false_outcome["examples"][0]["expectedOutcome"] = "renderable"
            with self.assertRaisesRegex(ValueError, "not preserved-unavailable"):
                build_help_model(root, parse_policy(false_outcome), tracked)

            model = build_help_model(root, parsed, tracked)

            self.assertEqual(model["examples"][0]["nodeTypeIds"], ["talas.fixture"])
            self.assertEqual(
                model["examples"][0]["preservedUnregisteredTypeIds"],
                ["vendor.preserved"],
            )
            self.assertEqual(model["nodes"][0]["examples"][0]["path"],
                             "examples/fixture.talas.json")

            (root / "examples/fixture.talas.json").write_text(
                json.dumps(project("talas.fixture")), encoding="utf-8"
            )
            unsupported_preservation = policy()
            unsupported_preservation["examples"][0]["expectedOutcome"] = (
                "preserved-unavailable"
            )
            with self.assertRaisesRegex(ValueError, "without preserved types"):
                build_help_model(
                    root,
                    parse_policy(unsupported_preservation),
                    tracked,
                )

            markdown = render_markdown(model)
            self.assertIn("maturity <code>placeholder</code>", markdown)
            self.assertIn("Vendor Fixture Vendor · origin <code>external</code>", markdown)
            self.assertIn("Fixture &#124; Node", markdown)
            self.assertIn("../../examples/fixture.talas.json", markdown)
            self.assertEqual(json.loads(render_json(model)), model)

            write_or_check(root, parsed, model, False)
            write_or_check(root, parsed, model, True)
            generated_markdown = root / "docs/help/reference.md"
            generated_markdown.write_bytes(
                generated_markdown.read_bytes().replace(b"\n", b"\r\n")
            )
            with self.assertRaisesRegex(ValueError, "stale"):
                write_or_check(root, parsed, model, True)
            write_or_check(root, parsed, model, False)
            (root / "docs/help/reference.md").write_text(
                "stale\n", encoding="utf-8"
            )
            with self.assertRaisesRegex(ValueError, "stale"):
                write_or_check(root, parsed, model, True)

    def test_markdown_escapes_metadata_and_rejects_anchor_collisions(self):
        first = normalize_catalog({"nodes": [descriptor("talas.a-b")]})[0]
        first["displayName"] = "Unsafe [label](target) <tag> `code`"
        first["examples"] = []
        safe = render_markdown({"nodes": [first], "examples": []})
        self.assertIn(
            "Unsafe &#91;label&#93;&#40;target&#41; &lt;tag&gt; &#96;code&#96;",
            safe,
        )

        second = normalize_catalog({"nodes": [descriptor("talas.a_b")]})[0]
        second["examples"] = []
        with self.assertRaisesRegex(ValueError, "anchor collision"):
            render_markdown({"nodes": [first, second], "examples": []})

    def test_repository_outputs_are_current(self):
        parsed = parse_policy(json.loads((
            ROOT / "docs/governance/node-help-policy.json"
        ).read_text(encoding="utf-8")))
        model = build_help_model(ROOT, parsed)
        self.assertEqual(len(model["nodes"]), 364)
        self.assertEqual(len(model["examples"]), 202)
        self.assertEqual(
            sum(
                example["path"].startswith("examples/demos/")
                for example in model["examples"]
            ),
            24,
        )
        catalog = json.loads((
            ROOT / parsed.catalog_path
        ).read_text(encoding="utf-8"))
        self.assertEqual(
            [node["typeId"] for node in model["nodes"]],
            sorted(node["typeId"] for node in catalog["nodes"]),
        )
        self.assertEqual(
            model["nonClaims"],
            [
                "semantic parity",
                "tutorial completeness",
                "hosted or embedded help availability",
            ],
        )
        write_or_check(ROOT, parsed, model, True)


if __name__ == "__main__":
    unittest.main()
