import contextlib
import io
import os
import shutil
import subprocess
import unittest

import ci_plan
from test_inventory import HELPER, ORPHAN, TESTS, Tree


class Plan(Tree):
    def setUp(self):
        super().setUp()
        # A second suite, which reaches the orphan.
        self.write("Testing/desc.txt", open(self.root / "Testing/desc.txt").read().replace(
            "            suite Basic Benchmarks F32 {",
            "            suite Basic Tests Q31 {\n                class = BasicTestsQ31\n"
            "                folder = BasicMathsQ31\n                Functions {\n"
            "                    Test orphan:test_orphan_f32\n                }\n            }\n"
            "            suite Basic Benchmarks F32 {"))
        self.write("Testing/Source/Tests/BasicTestsQ31.cpp", TESTS.replace("BasicTestsF32", "BasicTestsQ31"))
        self.write("Testing/cmake/ci_suites.txt", "# enabled suites\nBasicTestsF32\nBasicTestsQ31  # second\n")
        self.git("init", "-q", "-b", "base")
        self.commit("base")
        self.git("checkout", "-q", "-b", "change")
        self.output = self.root / "github_output"

    def git(self, *args):
        subprocess.run(["git", "-C", str(self.root), "-c", "user.name=plan", "-c", "user.email=plan@example.invalid",
                        *args], check=True, capture_output=True)

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "--allow-empty", "-m", message)

    def run_plan(self, *extra):
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors), contextlib.redirect_stdout(io.StringIO()):
            status = ci_plan.main(["--root", str(self.root), "--diff-base", "base", "--output", str(self.output),
                                   *extra])
        written = self.output.read_text() if self.output.exists() else ""
        return status, dict(line.split("=", 1) for line in written.splitlines()), errors.getvalue()

    def planned(self):
        status, result, errors = self.run_plan()
        self.assertEqual(status, 0, errors)
        return result

    def test_no_change_runs_no_target(self):
        self.commit("empty")
        self.assertEqual(self.planned(), {"host_suites": "BasicTestsF32;BasicTestsQ31", "target_suites": ""})

    def test_kernel_change_runs_the_suites_that_reach_it(self):
        self.write(HELPER, (self.root / HELPER).read_text() + "// changed\n")
        self.commit("helper")
        self.assertEqual(self.planned()["target_suites"], "BasicTestsF32")

    def test_kernel_only_one_suite_reaches(self):
        self.write(ORPHAN, (self.root / ORPHAN).read_text() + "\n")
        self.commit("orphan")
        self.assertEqual(self.planned()["target_suites"], "BasicTestsQ31")

    def test_change_outside_runs_no_target(self):
        self.write("README.md", "x\n")
        self.commit("readme")
        self.assertEqual(self.planned()["target_suites"], "")

    def test_header_runs_every_enabled_suite(self):
        self.write("Include/dsp/basic_math_functions.h", "// changed\n")
        self.commit("header")
        self.assertEqual(self.planned()["target_suites"], "BasicTestsF32;BasicTestsQ31")

    def test_deleted_kernel_runs_every_enabled_suite(self):
        os.remove(self.root / ORPHAN)
        self.commit("remove")
        self.assertEqual(self.planned()["target_suites"], "BasicTestsF32;BasicTestsQ31")

    def test_renamed_kernel_runs_every_enabled_suite(self):
        self.git("mv", ORPHAN, "Source/BasicMathFunctions/arm_moved_f32.c")
        self.commit("rename")
        self.assertEqual(self.planned()["target_suites"], "BasicTestsF32;BasicTestsQ31")

    def test_suites_outside_the_list_do_not_run(self):
        self.write("Testing/cmake/ci_suites.txt", "BasicTestsF32\n")
        self.commit("only F32")
        self.git("branch", "-f", "base", "HEAD")
        self.write(ORPHAN, (self.root / ORPHAN).read_text() + "// changed\n")
        self.commit("orphan")
        self.assertEqual(self.planned(), {"host_suites": "BasicTestsF32", "target_suites": ""})

    def test_order_follows_the_list(self):
        self.write("Testing/cmake/ci_suites.txt", "BasicTestsQ31\nBasicTestsF32\n")
        self.commit("order")
        self.assertEqual(self.planned()["host_suites"], "BasicTestsQ31;BasicTestsF32")

    def test_output_is_appended(self):
        self.output.write_text("earlier=1\n")
        self.commit("empty")
        self.assertEqual(self.planned()["earlier"], "1")

    def test_stdout_without_output(self):
        self.commit("empty")
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            status = ci_plan.main(["--root", str(self.root), "--diff-base", "base"])
        self.assertEqual(status, 0)
        self.assertEqual(output.getvalue(), "host_suites=BasicTestsF32;BasicTestsQ31\ntarget_suites=\n")

    def test_full_runs_every_listed_suite_on_the_targets(self):
        self.write(HELPER, (self.root / HELPER).read_text() + "// changed\n")
        self.commit("helper")
        status, result, errors = self.run_full()
        self.assertEqual(status, 0, errors)
        self.assertEqual(result, {"host_suites": "BasicTestsF32;BasicTestsQ31",
                                  "target_suites": "BasicTestsF32;BasicTestsQ31"})
        self.assertIn("full run", errors)

    def test_full_follows_the_list(self):
        self.write("Testing/cmake/ci_suites.txt", "BasicTestsQ31\n")
        status, result, _ = self.run_full()
        self.assertEqual((status, result), (0, {"host_suites": "BasicTestsQ31", "target_suites": "BasicTestsQ31"}))

    def test_full_needs_no_git(self):
        shutil.rmtree(self.root / ".git")
        status, result, errors = self.run_full()
        self.assertEqual(status, 0, errors)
        self.assertEqual(result["target_suites"], "BasicTestsF32;BasicTestsQ31")

    def test_full_still_checks_the_suites_file(self):
        self.write("Testing/cmake/ci_suites.txt", "NoSuchSuite\n")
        status, result, errors = self.run_full()
        self.assertEqual((status, result), (1, {}))
        self.assertIn("NoSuchSuite is not a suite", errors)

    def test_full_and_diff_base_are_exclusive(self):
        for argv in (["--root", str(self.root)],
                     ["--root", str(self.root), "--full", "--diff-base", "base"]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                ci_plan.main(argv + ["--output", str(self.output)])
            self.assertEqual(caught.exception.code, 2)
        self.assertFalse(self.output.exists())

    def run_full(self):
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors), contextlib.redirect_stdout(io.StringIO()):
            status = ci_plan.main(["--root", str(self.root), "--full", "--output", str(self.output)])
        written = self.output.read_text() if self.output.exists() else ""
        return status, dict(line.split("=", 1) for line in written.splitlines()), errors.getvalue()

    def rejects(self, said, *extra):
        status, result, errors = self.run_plan(*extra)
        self.assertEqual(status, 1, errors)
        self.assertIn(said, errors)
        self.assertEqual(result, {})

    def test_bad_suites_files(self):
        for text, said in (("BasicTestsF32\nNoSuchSuite\n", "NoSuchSuite is not a suite"),
                           ("BasicBenchF32\n", "BasicBenchF32 is not a suite"),
                           ("BasicTestsF32\nBasicTestsF32\n", "listed twice"),
                           ("# nothing\n\n", "lists no suite")):
            with self.subTest(said=said):
                self.write("Testing/cmake/ci_suites.txt", text)
                self.rejects(said)
        self.rejects("cannot read", "--suites-file", str(self.root / "none.txt"))

    def test_unknown_base(self):
        self.rejects("git diff against no-such-ref failed", "--diff-base", "no-such-ref")

    def test_unwritable_output(self):
        status, _, errors = self.run_plan("--output", str(self.root / "no" / "dir" / "out"))
        self.assertEqual(status, 1)
        self.assertIn("cannot write", errors)


class Shipped(unittest.TestCase):
    def test_suites_file_names_suites(self):
        root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        inventory = ci_plan.Inventory(root)
        self.assertTrue(ci_plan.read_suites(os.path.join(root, "Testing", "cmake", "ci_suites.txt"), inventory))


if __name__ == "__main__":
    unittest.main()
