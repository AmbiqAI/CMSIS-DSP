import os
import tempfile
import textwrap
import unittest

from generate_suite import escape, main


class GenerateSuite(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = self.dir.name
        for name in ("used.txt", "used two.txt", "unused.txt"):
            with open(os.path.join(self.root, name), "w") as f:
                f.write("data\n")
        self.depfile = os.path.join(self.root, "out.d")
        self.output = os.path.join(self.root, "out.cpp")

    def script(self, body):
        path = os.path.join(self.root, "gen.py")
        with open(path, "w") as f:
            f.write(textwrap.dedent(body))
        return path

    def run_generator(self, body, *args):
        return main(["--depfile", self.depfile, "--output", self.output, "--", self.script(body), *args])

    def deps(self):
        with open(self.depfile) as f:
            return f.read()

    def test_records_files_read_and_not_files_written(self):
        status = self.run_generator("""
            import sys
            open(sys.argv[1]).read()
            with open(sys.argv[2], "rb") as f:
                f.read()
            open(sys.argv[3], "w").write("generated")
            """, os.path.join(self.root, "used.txt"), os.path.join(self.root, "used two.txt"), self.output)
        self.assertEqual(status, 0)
        text = self.deps()
        self.assertTrue(text.startswith(escape(self.output) + ":"))
        self.assertIn(escape(os.path.join(self.root, "used.txt")), text)
        self.assertIn(escape(os.path.join(self.root, "used two.txt")), text)
        self.assertNotIn("unused.txt", text)
        self.assertNotIn(escape(self.output) + " ", text.split(":", 1)[1])

    def test_relative_reads_are_made_absolute(self):
        cwd = os.getcwd()
        os.chdir(self.root)
        self.addCleanup(os.chdir, cwd)
        self.assertEqual(self.run_generator("open('used.txt').read()\n"), 0)
        self.assertIn(escape(os.path.join(os.getcwd(), "used.txt")), self.deps())

    def test_generator_failure_writes_no_depfile(self):
        self.assertEqual(self.run_generator("import sys\nsys.exit(3)\n"), 3)
        self.assertFalse(os.path.exists(self.depfile))

    def test_generator_exception_propagates(self):
        with self.assertRaises(FileNotFoundError):
            self.run_generator("open('/nonexistent/pattern.txt')\n")
        self.assertFalse(os.path.exists(self.depfile))

    def test_failed_open_is_not_recorded(self):
        status = self.run_generator("""
            try:
                open('/nonexistent/pattern.txt')
            except OSError:
                pass
            """)
        self.assertEqual(status, 0)
        self.assertNotIn("nonexistent", self.deps())

    def test_escape(self):
        self.assertEqual(escape("/a b/c#d$e"), "/a\\ b/c\\#d$$e")


if __name__ == "__main__":
    unittest.main()
