import contextlib
import io
import json
import os
import re
import tempfile
import textwrap
import unittest
from pathlib import Path

import inventory
from inventory import Inventory, InventoryError, parse_desc

ADD = "Source/BasicMathFunctions/arm_add_f32.c"
HELPER = "Source/BasicMathFunctions/arm_helper_f32.c"
ORPHAN = "Source/BasicMathFunctions/arm_orphan_f32.c"
UNNAMED = "Source/BasicMathFunctions/arm_unnamed_f32.c"

DESC = """\
group Root {
    class = Root
    group DSP Tests {
        class = DSPTests
        folder = DSP
        group Basic Tests {
            class = BasicTests
            folder = BasicMaths
            suite Basic Tests F32 {
                class = BasicTestsF32
                folder = BasicMathsF32
                Pattern INPUT1_F32_ID : Input1_f32.txt
                Output  OUT_SAMPLES_F32_ID : Output
                Functions {
                    Test nb=3 arm_add_f32:test_add_f32
                    // Test nb=4 arm_orphan_f32:test_orphan_f32
                    disabled{Test nb=4 arm_orphan_f32:test_orphan_f32}
                }
            }
            suite Basic Benchmarks F32 {
                class = BasicBenchF32
                folder = BasicBenchF32
                Functions {
                    Bench add:test_add_f32
                } -> PARAM1_ID
            }
        }
    }
}
"""

TESTS = """\
#include "BasicTestsF32.h"
void BasicTestsF32::test_add_f32()
{
    arm_add_f32(a, b, c, n); /* arm_unnamed_f32( is only a comment */
}
void BasicTestsF32::test_orphan_f32()
{
    arm_orphan_f32(a, n);
}
void BasicTestsF32::setUp(Testing::testID_t id, std::vector<Testing::param_t>& params, Client::PatternMgr *mgr)
{
    arm_fill_f32(0, a, n);
}
"""


def kernel(name, calls=(), body_extra=""):
    lines = [f"ARM_DSP_ATTRIBUTE void {name}(const float32_t *pSrc, uint32_t n)", "{"]
    lines += [f"    {called}(pSrc, n);" for called in calls]
    return "\n".join(lines + [body_extra, "}", ""])


class Tree(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.root = Path(self.dir.name)
        self.write(ADD, kernel("arm_add_f32", ["arm_helper_f32", "arm_add_f32"]))
        self.write(HELPER, kernel("arm_helper_f32"))
        self.write(ORPHAN, kernel("arm_orphan_f32"))
        self.write(UNNAMED, kernel("arm_unnamed_f32"))
        self.write("Source/SupportFunctions/arm_fill_f32.c", kernel("arm_fill_f32"))
        self.write("Source/CommonTables/arm_common_tables.c", kernel("arm_table_lookup"))
        self.write("Source/BasicMathFunctions/BasicMathFunctions.c", '#include "arm_add_f32.c"\n')
        self.write("Include/dsp/basic_math_functions.h", "void arm_add_f32(void);\n")
        self.write("Testing/desc.txt", DESC)
        self.write("Testing/desc_f16.txt", "group Root {\n    class = Root\n}\n")
        self.write("Testing/Source/Tests/BasicTestsF32.cpp", TESTS)
        self.write("Testing/Include/Tests/BasicTestsF32.h", "")

    def write(self, path, text):
        full = self.root / path
        full.parent.mkdir(parents=True, exist_ok=True)
        full.write_text(text)

    def inventory(self):
        return Inventory(self.root)

    def run_main(self, *args):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            status = inventory.main([args[0], "--root", str(self.root), *args[1:]])
        return status, output.getvalue(), errors.getvalue()


class Analysis(Tree):
    def test_suites_and_kernels(self):
        found = self.inventory()
        self.assertEqual(sorted(found.suites), ["BasicTestsF32"])  # the benchmark has no test source
        self.assertNotIn("Source/CommonTables/arm_common_tables.c", found.kernels)
        self.assertNotIn("Source/BasicMathFunctions/BasicMathFunctions.c", found.kernels)
        self.assertEqual(found.suites["BasicTestsF32"].folder, "DSP/BasicMaths/BasicMathsF32")
        self.assertEqual(found.suites["BasicTestsF32"].enabled, ["test_add_f32"])
        self.assertEqual(found.suites["BasicTestsF32"].disabled, ["test_orphan_f32"])

    def test_reach_follows_calls_and_fixtures(self):
        found = self.inventory()
        self.assertEqual(found.file_suites(ADD), ["BasicTestsF32"])
        self.assertEqual(found.file_suites(HELPER), ["BasicTestsF32"])
        self.assertEqual(found.file_suites("Source/SupportFunctions/arm_fill_f32.c"), ["BasicTestsF32"])

    def test_disabled_test_does_not_reach(self):
        self.assertEqual(self.inventory().file_suites(ORPHAN), [])

    def test_call_in_a_comment_does_not_reach(self):
        self.assertEqual(self.inventory().file_suites(UNNAMED), [])

    def test_names_in_no_test_source(self):
        # The orphan's name is in the test source, though only its disabled test calls it; the helper is
        # reached but never named; a comment does not count.
        self.assertEqual(self.inventory().named_in_no_test(), [HELPER, UNNAMED])

    def test_unreached_functions_by_category(self):
        self.assertEqual(self.inventory().unreached(), {"BasicMathFunctions": ["arm_orphan_f32", "arm_unnamed_f32"]})

    def test_method_that_is_not_found_reads_the_whole_file(self):
        self.write("Testing/Source/Tests/BasicTestsF32.cpp", TESTS.replace("::test_add_f32()", "::renamed()"))
        found = self.inventory()
        self.assertTrue(found.suites["BasicTestsF32"].whole_file)
        self.assertEqual(found.file_suites(ORPHAN), ["BasicTestsF32"])

    def test_definition_inside_conditional_branches(self):
        self.write(ADD, "#if defined(ARM_MATH_MVEF)\n" + kernel("arm_add_f32", ["arm_helper_f32"]) + "#else\n" +
                   kernel("arm_add_f32", ["arm_orphan_f32"]) + "#endif\n")
        self.assertEqual(self.inventory().file_suites(ORPHAN), ["BasicTestsF32"])

    def test_unclosed_kernel_body(self):
        self.write(ADD, "void arm_add_f32(void)\n{\n    arm_orphan_f32(x);\n")
        self.assertEqual(self.inventory().file_suites(ORPHAN), ["BasicTestsF32"])


class Desc(Tree):
    def rejects(self, text, said):
        self.write("Testing/desc.txt", text)
        with self.assertRaises(InventoryError) as caught:
            self.inventory()
        self.assertIn(said, str(caught.exception))

    def test_unbalanced(self):
        self.rejects(DESC + "}\n", "unbalanced }")
        self.rejects(DESC.rsplit("}", 1)[0], "never closed")

    def test_suite_without_a_class(self):
        self.rejects(DESC.replace("class = BasicTestsF32", ""), "a suite without a class")

    def test_unreadable_test_line(self):
        self.rejects(DESC.replace("Test nb=3 arm_add_f32:test_add_f32", "Test nb=3 arm_add_f32"),
                     "cannot read the test line")
        self.rejects(DESC.replace("disabled{Test nb=4 arm_orphan_f32:test_orphan_f32}", "disabled{oops}"),
                     "cannot read the disabled test")

    def test_disabled_outside_functions(self):
        self.rejects(DESC.replace("Output  OUT_SAMPLES_F32_ID : Output", "disabled{a:b}"), "outside a Functions")

    def test_suite_described_twice(self):
        self.write("Testing/desc_f16.txt", DESC)
        with self.assertRaises(InventoryError) as caught:
            self.inventory()
        self.assertIn("suite BasicTestsF32 is described twice", str(caught.exception))

    def test_no_suite_with_a_test_source(self):
        os.remove(self.root / "Testing/Source/Tests/BasicTestsF32.cpp")
        self.rejects(DESC, "no suite in desc.txt or desc_f16.txt has a test source")

    def test_missing_desc(self):
        os.remove(self.root / "Testing/desc_f16.txt")
        self.rejects(DESC, "cannot read")

    def test_not_a_checkout(self):
        with self.assertRaises(InventoryError) as caught:
            Inventory(self.root / "Source")
        self.assertIn("not a CMSIS-DSP checkout", str(caught.exception))


class Suites(Tree):
    def suites(self, *paths):
        status, output, errors = self.run_main("suites", *paths)
        self.assertEqual(status, 0, errors)
        return output.split(), errors

    def test_kernel_file(self):
        self.assertEqual(self.suites(HELPER)[0], ["BasicTestsF32"])

    def test_kernel_file_no_suite_reaches(self):
        found, errors = self.suites(ORPHAN)
        self.assertEqual(found, [])
        self.assertIn(f"no suite reaches {ORPHAN}", errors)

    def test_test_source_header_and_patterns(self):
        for path in ("Testing/Source/Tests/BasicTestsF32.cpp", "Testing/Include/Tests/BasicTestsF32.h",
                     "Testing/Patterns/DSP/BasicMaths/BasicMathsF32/Input1_f32.txt",
                     "Testing/Parameters/DSP/BasicMaths/BasicMathsF32/Params1.txt"):
            with self.subTest(path=path):
                self.assertEqual(self.suites(path)[0], ["BasicTestsF32"])

    def test_paths_whose_effect_is_unbounded(self):
        for path in ("Include/dsp/basic_math_functions.h", "Source/CommonTables/arm_common_tables.c",
                     "Source/BasicMathFunctions/BasicMathFunctions.c", "Testing/desc.txt",
                     "Testing/FrameworkSource/Test.cpp", "Testing/Source/Tests/NotASuite.cpp",
                     "Testing/Patterns/DSP/BasicMaths/Other/x.txt",
                     "Testing/Patterns/DSP/BasicMaths/BasicMathsF32Extra/x.txt"):
            with self.subTest(path=path):
                found, errors = self.suites(path)
                self.assertEqual(found, ["BasicTestsF32"])
                self.assertIn("can affect every suite", errors)

    def test_paths_outside(self):
        self.assertEqual(self.suites("README.md", ".github/workflows/x.yml")[0], [])

    def test_union_of_several_paths(self):
        self.assertEqual(self.suites(ORPHAN, HELPER, "README.md")[0], ["BasicTestsF32"])

    def test_path_not_in_normal_form(self):
        for path in ("/abs/Source/x.c", "Source/../x.c", "./Source/x.c", "Source\\x.c", "../x.c"):
            with self.subTest(path=path):
                status, _, errors = self.run_main("suites", path)
                self.assertEqual(status, 1)
                self.assertIn("not a normal path", errors)


class Report(Tree):
    def test_report_and_json(self):
        out = self.root / "inventory.json"
        status, output, errors = self.run_main("report", "--json", str(out))
        self.assertEqual(status, 0, errors)
        self.assertIn("5 kernel files, 5 functions, 1 suites; 3 files reached by a suite", output)
        self.assertIn(f"kernel files whose names appear in no test source (2):\n  {HELPER}\n  {UNNAMED}\n", output)
        self.assertIn("kernel functions no suite reaches (2):\n  BasicMathFunctions (2): arm_orphan_f32, "
                      "arm_unnamed_f32", output)
        data = json.loads(out.read_text())
        self.assertEqual(data["suites"]["BasicTestsF32"]["files"],
                         [ADD, HELPER, "Source/SupportFunctions/arm_fill_f32.c"])
        self.assertEqual(data["suites"]["BasicTestsF32"]["disabled_tests"], 1)
        orphan = next(entry for entry in data["files"] if entry["file"] == ORPHAN)
        self.assertEqual(orphan, {"file": ORPHAN, "category": "BasicMathFunctions", "named_in_tests": True,
                                  "suites": [], "functions": {"arm_orphan_f32": []}})

    def test_whole_file_suites_are_named(self):
        self.write("Testing/Source/Tests/BasicTestsF32.cpp", TESTS.replace("::test_add_f32()", "::renamed()"))
        self.assertIn("suites read as a whole file (a test method was not found): BasicTestsF32",
                      self.run_main("report")[1])

    def test_unwritable_json(self):
        status, _, errors = self.run_main("report", "--json", str(self.root / "no" / "such" / "dir.json"))
        self.assertEqual(status, 1)
        self.assertIn("cannot write", errors)

    def test_bad_desc_fails_the_report(self):
        self.write("Testing/desc.txt", DESC + "}\n")
        status, _, errors = self.run_main("report")
        self.assertEqual(status, 1)
        self.assertIn("unbalanced", errors)


class Shipped(unittest.TestCase):
    """The repository's own desc files and sources must parse, and the name measure must agree with a plain
    word search of the test sources."""

    def test_this_checkout(self):
        root = Path(__file__).resolve().parents[2]
        found = Inventory(root)
        self.assertIn("BasicTestsF32", found.suites)
        self.assertIn("BasicTestsF32", found.file_suites("Source/BasicMathFunctions/arm_add_f32.c"))
        words = set()
        for path in (root / "Testing/Source/Tests").glob("*.cpp"):
            words.update(re.findall(r"\w+", re.sub(r"/\*.*?\*/|//[^\n]*", " ", path.read_text(errors="replace"),
                                                   flags=re.S)))
        expected = [p for p in found.kernels if os.path.basename(p)[:-2] not in words]
        self.assertEqual(found.named_in_no_test(), expected)
        for name, suite in found.suites.items():
            with self.subTest(suite=name):
                self.assertTrue((root / suite.source).is_file())


if __name__ == "__main__":
    unittest.main()
