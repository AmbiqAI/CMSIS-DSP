import sys
import unittest

from suite_status import main, verdict

GOOD = """S: t
S: g 1
S: s 2
S: 1 0 0 t Y
E: 
S: 2 0 0 0 Y
E: 
S: p
S: p
"""
ONE_FAILED = GOOD.replace("S: 2 0 0 0 Y\nE: ", "S: 2 3 117 0 N\nE: 12 >= 15")


def run_printing(output, status=0):
    return [sys.executable, "-c", f"import sys; print({output!r}); sys.exit({status})"]


class Verdict(unittest.TestCase):
    def test_all_passed(self):
        self.assertEqual(verdict(GOOD, 0), (2, [], []))

    def test_failed_test(self):
        self.assertEqual(verdict(ONE_FAILED, 0),
                         (1, ["test 2 failed: error 3 at line 117", "error detail: 12 >= 15"], []))

    def test_only_failures_still_report_a_status(self):
        out = ONE_FAILED.replace("S: 1 0 0 t Y", "S: 1 2 40 0 N")
        passed, failures, problems = verdict(out, 0)
        self.assertEqual((passed, len(failures), problems), (0, 3, []))

    def test_stopped_early(self):
        out = GOOD.rsplit("S: p", 1)[0]
        self.assertIn("output ends inside 1 open group(s): the run stopped early", verdict(out, 0)[2])

    def test_exception(self):
        self.assertIn("framework reported: Exception", verdict(GOOD + "Exception\n", 0)[2])

    def test_out_of_memory(self):
        self.assertIn("framework reported: NOT ENOUGH MEMORY", verdict("NOT ENOUGH MEMORY\n", 0)[2])

    def test_nonzero_exit(self):
        self.assertIn("exit status -11", verdict(GOOD, -11)[2])

    def test_empty_output(self):
        problems = verdict("", 0)[2]
        self.assertIn("no group or suite was run", problems)
        self.assertIn("no test reported a status", problems)

    def test_unbalanced_close(self):
        self.assertIn("group closed that was never opened", verdict("S: p\n" + GOOD, 0)[2])


class Main(unittest.TestCase):
    def test_passing_command(self):
        self.assertEqual(main(["--", *run_printing(GOOD)]), 0)

    def test_failed_test_fails_the_run(self):
        self.assertEqual(main(["--", *run_printing(ONE_FAILED)]), 1)

    def test_failing_command(self):
        self.assertEqual(main(["--", sys.executable, "-c", "import sys; sys.exit(3)"]), 1)

    def test_timeout(self):
        self.assertEqual(main(["--timeout", "0.5", "--", sys.executable, "-c", "import time; time.sleep(5)"]), 1)

    def test_missing_executable(self):
        self.assertEqual(main(["--", "/nonexistent/suite"]), 1)


class ExpectFailures(unittest.TestCase):
    def control(self, output, status=0):
        return main(["--expect-failures", "--", *run_printing(output, status)])

    def test_control_that_catches_the_failure(self):
        self.assertEqual(self.control(ONE_FAILED), 0)

    def test_control_without_a_failure(self):
        self.assertEqual(self.control(GOOD), 1)

    def test_control_without_a_pass(self):
        self.assertEqual(self.control(ONE_FAILED.replace("S: 1 0 0 t Y", "S: 1 2 40 0 N")), 1)

    def test_control_that_crashed(self):
        self.assertEqual(self.control(ONE_FAILED, status=3), 1)

    def test_control_that_stopped_early(self):
        self.assertEqual(self.control(ONE_FAILED.rsplit("S: p", 1)[0]), 1)

    def test_control_with_fatal_message(self):
        self.assertEqual(self.control(ONE_FAILED + "NOT ENOUGH MEMORY\n"), 1)

    def test_control_timeout(self):
        command = [sys.executable, "-c", "import time; time.sleep(5)"]
        self.assertEqual(main(["--expect-failures", "--timeout", "0.5", "--", *command]), 1)


if __name__ == "__main__":
    unittest.main()
