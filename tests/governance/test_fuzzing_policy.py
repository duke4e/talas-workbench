import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
FUZZ_ROOT = ROOT / "tests" / "fuzz"


class FuzzingPolicyTests(unittest.TestCase):
    def test_checked_in_corpus_is_small_local_and_project_owned(self):
        corpus_root = FUZZ_ROOT / "corpus"
        files = sorted(path for path in corpus_root.rglob("*") if path.is_file())
        self.assertGreaterEqual(len(files), 6)
        self.assertLessEqual(sum(path.stat().st_size for path in files), 64 * 1024)
        for path in files:
            self.assertTrue(path.resolve().is_relative_to(corpus_root.resolve()))
            payload = path.read_bytes()
            self.assertNotIn(b"_References", payload)
            self.assertNotIn(b"http://", payload)
            self.assertNotIn(b"https://", payload)


    def test_fuzz_entrypoints_are_bounded_and_release_excluded(self):
        project = (FUZZ_ROOT / "ProjectJsonFuzzer.cpp").read_text(encoding="utf-8")
        expression = (FUZZ_ROOT / "ExpressionFuzzer.cpp").read_text(encoding="utf-8")
        replay = (FUZZ_ROOT / "FuzzReplayMain.cpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("maximumProjectJsonBytes", project)
        self.assertIn("1024U * 1024U", project)
        self.assertIn("ExpressionLimits::maximumSourceBytes", expression)
        self.assertNotIn("maximumExpressionBytes", expression)
        self.assertIn("options.maximumLength", replay)
        self.assertIn("options.runs", replay)
        self.assertIn("if(TALAS_BUILD_FUZZERS)", cmake)
        self.assertIn("TALAS_BUILD_FUZZERS requires TALAS_BUILD_TESTS=ON", cmake)
        self.assertNotIn("tests/fuzz", "\n".join(
            line for line in cmake.splitlines() if line.lstrip().startswith("install(")
        ))


if __name__ == "__main__":
    unittest.main()
