import pathlib
import unittest
from unittest import mock

import sys


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import check_generated_artifacts

from check_generated_artifacts import GeneratedArtifactError
from check_generated_artifacts import canonical_stdout_bytes
from check_generated_artifacts import check_child_projections


class GeneratedArtifactTransportTests(unittest.TestCase):
    def test_catalog_transport_accepts_lf_and_canonicalizes_windows_crlf(self):
        expected = b'{\n  "nodes": []\n}\n'
        self.assertEqual(
            canonical_stdout_bytes(expected.decode("utf-8"), "catalog"),
            expected,
        )
        self.assertEqual(
            canonical_stdout_bytes(
                expected.decode("utf-8").replace("\n", "\r\n"),
                "catalog",
            ),
            expected,
        )

    def test_catalog_transport_rejects_bare_carriage_return(self):
        with self.assertRaisesRegex(
                GeneratedArtifactError, "bare carriage return"):
            canonical_stdout_bytes("bad\rtransport", "catalog")

    def test_child_generators_are_invoked_only_in_read_only_check_mode(self):
        root = pathlib.Path("/repository")
        with mock.patch.object(
            check_generated_artifacts,
            "run_checked",
            side_effect=[
                "Capability matrix v2: offline {'unsupported': 1, 'subset': 2, 'supported': 3}; prepared realtime {'unsupported': 4, 'subset': 5, 'supported': 6}\n",
                "Node help: PASS: 1 nodes, 1 examples\r\n",
            ],
        ) as runner:
            result = check_child_projections(root, "/python")

        self.assertEqual(
            result,
            (
                "Capability matrix v2: offline {'unsupported': 1, 'subset': 2, 'supported': 3}; prepared realtime {'unsupported': 4, 'subset': 5, 'supported': 6}",
                "Node help: PASS: 1 nodes, 1 examples",
            ),
        )
        self.assertEqual(
            runner.call_args_list,
            [
                mock.call(
                    [
                        "/python",
                        "-B",
                        str(root / "tools/report_capability_matrix.py"),
                        "--check",
                    ],
                    root,
                    "capability matrix check",
                ),
                mock.call(
                    [
                        "/python",
                        "-B",
                        str(root / "tools/report_node_help.py"),
                        "--check",
                    ],
                    root,
                    "node help check",
                ),
            ],
        )

    def test_child_generators_require_one_exact_success_line(self):
        root = pathlib.Path("/repository")
        for malformed in (
            "",
            "FAIL\n",
            "Node help: ok\nextra\n",
            "Node help: PASS: 1 nodes\rbare\n",
        ):
            with self.subTest(malformed=malformed), mock.patch.object(
                check_generated_artifacts,
                "run_checked",
                return_value=malformed,
            ):
                with self.assertRaisesRegex(
                    GeneratedArtifactError,
                    "malformed success output|bare carriage return",
                ):
                    check_child_projections(root, "/python")


if __name__ == "__main__":
    unittest.main()
