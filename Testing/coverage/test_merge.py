import contextlib
import io
import json
import os
import tempfile
import unittest

import merge


def function(name, lineno, count, gcc=True):
    entry = {"name": name, "lineno": lineno, "execution_count": count, "blocks_percent": 100.0 if count else 0.0}
    if gcc:
        entry["demangled_name"] = name
        entry["pos"] = [f"{lineno}:1", f"{lineno + 9}:1"]
    return entry


def line(number, count, name):
    return {"line_number": number, "function_name": name, "count": count, "branches": []}


# arm_add_f32 as two variants see it: the scalar body (A) and the MVE body (G) of the same file, on
# different lines.
SCALAR = {"file": "Source/BasicMathFunctions/arm_add_f32.c",
          "functions": [function("arm_add_f32", 116, 4)],
          "lines": [line(116, 4, "arm_add_f32"), line(130, 64, "arm_add_f32"), line(150, 0, "arm_add_f32")]}
MVE = {"file": "Source/BasicMathFunctions/arm_add_f32.c",
       "functions": [function("arm_add_f32", 67, 4)],
       "lines": [line(67, 4, "arm_add_f32"), line(89, 68, "arm_add_f32"), line(107, 2, "arm_add_f32")]}


class Merge(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = os.path.join(self.dir.name, "checkout")
        os.makedirs(os.path.join(self.root, "Source", "BasicMathFunctions"))
        with open(os.path.join(self.root, SCALAR["file"]), "w") as source:
            source.write("\n" * 200)
        self.out = os.path.join(self.dir.name, "merged")

    def report(self, variant, *files, name=None, data=None):
        path = os.path.join(self.dir.name, name or f"coverage-{variant}.json")
        with open(path, "w") as out:
            if data is None:
                json.dump({"gcovr/format_version": "0.14", "files": list(files)}, out)
            else:
                out.write(data)
        return path

    def merge(self, *reports):
        return merge.main(["--root", self.root, "--output-dir", self.out, *reports])

    def merged_file(self):
        with open(os.path.join(self.out, "coverage.json")) as report:
            return next(f for f in json.load(report)["files"] if f["file"] == SCALAR["file"])

    def test_scalar_and_mve_lines_of_one_file(self):
        self.assertEqual(self.merge(self.report("A", SCALAR), self.report("G", MVE)), 0)
        merged = self.merged_file()
        counts = {entry["line_number"]: entry["count"] for entry in merged["lines"]}
        self.assertEqual(counts, {67: 4, 89: 68, 107: 2, 116: 4, 130: 64, 150: 0})
        self.assertEqual(sorted(entry["lineno"] for entry in merged["functions"]), [67, 116])
        for name in ("summary.json", os.path.join("html", "index.html")):
            self.assertTrue(os.path.isfile(os.path.join(self.out, name)), name)

    def test_counts_add_across_variants(self):
        other = dict(SCALAR, lines=[line(116, 1, "arm_add_f32"), line(130, 0, "arm_add_f32"),
                                    line(150, 5, "arm_add_f32")], functions=[function("arm_add_f32", 116, 1)])
        self.assertEqual(self.merge(self.report("A", SCALAR), self.report("B", other)), 0)
        counts = {entry["line_number"]: entry["count"] for entry in self.merged_file()["lines"]}
        self.assertEqual(counts, {116: 5, 130: 64, 150: 5})

    def rejects(self, *reports, reason=None):
        os.makedirs(os.path.join(self.out, "html"), exist_ok=True)
        stale = os.path.join(self.out, "coverage.json")
        with open(stale, "w") as out:
            out.write("{}")
        errors = io.StringIO()
        with contextlib.redirect_stderr(errors):
            self.assertEqual(self.merge(*reports), 1)
        self.assertFalse(os.path.exists(stale), "an earlier report survived a failed merge")
        if reason:
            self.assertIn(reason, errors.getvalue())

    def test_same_variant_twice(self):
        first = self.report("A", SCALAR)
        os.makedirs(os.path.join(self.dir.name, "again"))
        self.rejects(first, self.report("A", SCALAR, name=os.path.join("again", "coverage-A.json")))

    def test_gcc_and_llvm_cov_reports(self):
        llvm = dict(SCALAR, functions=[function("arm_add_f32", 116, 4, gcc=False)])
        self.rejects(self.report("A", llvm), self.report("G", MVE))

    def test_report_mixing_producers(self):
        mixed = dict(SCALAR, functions=[function("arm_add_f32", 116, 4), function("helper", 160, 0, gcc=False)])
        self.rejects(self.report("A", mixed))

    def test_file_outside_the_scope(self):
        framework = dict(SCALAR, file="Testing/FrameworkSource/Test.cpp")
        self.rejects(self.report("A", SCALAR, framework))

    def test_common_tables(self):
        self.rejects(self.report("A", dict(SCALAR, file="Source/CommonTables/arm_common_tables.c")))

    def test_path_not_in_normal_form(self):
        for path in ("Source/../Testing/FrameworkSource/Test.cpp", "Source/./CommonTables/arm_common_tables.c",
                     "Source//CommonTables/arm_common_tables.c", "Include/dsp/../../Testing/x.h",
                     "Source/..\\Testing\\x.cpp", "Source/BasicMathFunctions/"):
            with self.subTest(path=path):
                self.rejects(self.report("A", SCALAR, dict(SCALAR, file=path)), reason="outside the scope")

    def test_file_entry_without_a_path(self):
        self.rejects(self.report("A", {k: v for k, v in SCALAR.items() if k != "file"}), reason="outside the scope")

    def test_report_name_without_variant(self):
        self.rejects(self.report("A", SCALAR, name="summary-A.json"))

    def test_missing_report(self):
        self.rejects(self.report("A", SCALAR), os.path.join(self.dir.name, "coverage-G.json"))

    def test_not_json(self):
        self.rejects(self.report("A", data="GCOV-BEGIN\n"))

    def test_not_a_gcovr_report(self):
        self.rejects(self.report("A", data='{"files": []}'))

    def test_empty_report(self):
        self.rejects(self.report("A"))

    def test_report_without_functions(self):
        self.rejects(self.report("A", dict(SCALAR, functions=[])))

    def test_gcovr_failure(self):
        # A line entry that gcovr itself rejects while reading.
        broken = dict(SCALAR, lines=[{"line_number": "x", "count": 1, "branches": []}])
        self.rejects(self.report("A", broken))


if __name__ == "__main__":
    unittest.main()
