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

    def test_reset_removes_old_transcripts(self):
        console = os.path.join(self.build, "coverage", "console")
        os.makedirs(console)
        old = os.path.join(console, "BasicTestsF32.txt")
        other = os.path.join(console, "notes.md")
        for path in (old, other):
            open(path, "w").close()
        self.assertEqual(host_coverage.main(["reset", "--build-dir", self.build, "--transcripts", console]), 0)
        self.assertFalse(os.path.exists(old))
        self.assertTrue(os.path.exists(other))

    def test_reset_before_the_first_transcript(self):
        console = os.path.join(self.build, "coverage", "console")
        self.assertEqual(host_coverage.main(["reset", "--build-dir", self.build, "--transcripts", console]), 0)

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

    def test_report_without_counters_removes_previous_reports(self):
        out = os.path.join(self.dir.name, "out")
        os.makedirs(out)
        stale = [os.path.join(out, "c.json"), os.path.join(out, "s.json")]
        for path in stale:
            open(path, "w").close()
        self.assertEqual(self.report(), 1)
        self.assertFalse(any(os.path.exists(path) for path in stale))

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
        # The only search path is the build directory: by default gcovr also searches --root, which can hold
        # the counters of another build.
        self.assertEqual(command[-1], os.path.realpath(self.build))


if __name__ == "__main__":
    unittest.main()
