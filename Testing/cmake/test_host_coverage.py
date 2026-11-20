import os
import re
import sys
import tempfile
import unittest

import host_coverage


class Coverage(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.build = os.path.join(self.dir.name, "build")
        os.makedirs(os.path.join(self.build, "dsp", "obj"))

    def touch(self, *parts):
        path = os.path.join(self.build, *parts)
        open(path, "w").close()
        return path

    def test_reset_removes_only_counter_files(self):
        counters = [self.touch("a.gcda"), self.touch("dsp", "obj", "b.gcda")]
        notes = self.touch("dsp", "obj", "b.gcno")
        self.assertEqual(host_coverage.main(["reset", "--build-dir", self.build]), 0)
        self.assertFalse(any(os.path.exists(path) for path in counters))
        self.assertTrue(os.path.exists(notes))

    def test_reset_of_missing_directory_fails(self):
        self.assertEqual(host_coverage.main(["reset", "--build-dir", os.path.join(self.dir.name, "none")]), 1)

    def report(self):
        out = os.path.join(self.dir.name, "out")
        return host_coverage.main(["report", "--build-dir", self.build, "--root", self.dir.name, "--gcov", "gcov",
                              "--output", os.path.join(out, "c.json"), "--summary", os.path.join(out, "s.json")])

    def test_report_without_counters_fails_before_running_gcovr(self):
        self.touch("dsp", "obj", "b.gcno")
        self.assertEqual(self.report(), 1)
        self.assertFalse(os.path.exists(os.path.join(self.dir.name, "out", "c.json")))

    def test_gcovr_failure_propagates(self):
        self.touch("a.gcda")
        self.assertNotEqual(self.report(), 0)

    def test_gcovr_command_scope(self):
        command = host_coverage.gcovr_command(self.build, self.dir.name, "xcrun llvm-cov gcov", "c.json", "s.json")
        self.assertEqual(command[:3], [sys.executable, "-m", "gcovr"])
        root = os.path.realpath(self.dir.name)
        filters = [command[i + 1] for i, arg in enumerate(command) if arg == "--filter"]
        excludes = [command[i + 1] for i, arg in enumerate(command) if arg == "--exclude"]
        self.assertTrue(re.match(filters[0], os.path.join(root, "Source", "BasicMathFunctions", "arm_add_f32.c")))
        self.assertTrue(re.match(filters[1], os.path.join(root, "Include", "dsp", "utils.h")))
        self.assertFalse(any(re.match(f, os.path.join(root, "Testing", "FrameworkSource", "Test.cpp"))
                             for f in filters))
        self.assertTrue(re.match(excludes[0], os.path.join(root, "Source", "CommonTables", "arm_common_tables.c")))
        self.assertFalse(re.match(filters[0], os.path.join(root, "SourceExtra", "x.c")))
        self.assertEqual(command[command.index("--gcov-executable") + 1], "xcrun llvm-cov gcov")


if __name__ == "__main__":
    unittest.main()
