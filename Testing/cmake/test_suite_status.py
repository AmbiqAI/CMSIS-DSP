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


class Verdict(unittest.TestCase):
    def test_all_passed(self):
        self.assertEqual(verdict(GOOD, 0), (2, 0, []))

    def test_failed_test(self):
        out = GOOD.replace("S: 2 0 0 0 Y\nE: ", "S: 2 3 117 0 N\nE: 12 >= 15")
        passed, failed, problems = verdict(out, 0)
        self.assertEqual((passed, failed), (1, 1))
        self.assertIn("test 2 failed: error 3 at line 117", problems)
        self.assertIn("error detail: 12 >= 15", problems)

    def test_stopped_early(self):
        out = GOOD.rsplit("S: p", 1)[0]
        self.assertIn("output ends inside 1 open group(s): the run stopped early", verdict(out, 0)[2])

    def test_exception(self):
        self.assertIn("framework reported: Exception", verdict(GOOD + "Exception\n", 0)[2])

    def test_out_of_memory(self):
        self.assertIn("framework reported: NOT ENOUGH MEMORY", verdict("NOT ENOUGH MEMORY\n", 0)[2])

    def test_fault_after_a_passing_transcript(self):
        # The fault handler's line, as target/runtime.c prints it; the FVP still exits 0.
        self.assertIn("error detail: HardFault", verdict(GOOD + "\nE: HardFault\n", 0)[2])

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
        self.assertEqual(main(["--", sys.executable, "-c", f"print({GOOD!r})"]), 0)

    def test_failing_command(self):
        self.assertEqual(main(["--", sys.executable, "-c", "import sys; sys.exit(3)"]), 1)

    def test_timeout(self):
        self.assertEqual(main(["--timeout", "0.5", "--", sys.executable, "-c", "import time; time.sleep(5)"]), 1)

    def test_missing_executable(self):
        self.assertEqual(main(["--", "/nonexistent/suite"]), 1)


if __name__ == "__main__":
    unittest.main()
