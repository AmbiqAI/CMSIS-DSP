import contextlib
import io
import json
import os
import subprocess
import tempfile
import unittest

import coverage_gate

ADD = "Source/BasicMathFunctions/arm_add_f32.c"
SUB = "Source/BasicMathFunctions/arm_sub_f32.c"


def line(number, count, name="arm_add_f32", branches=()):
    return {"line_number": number, "count": count, "function_name": name,
            "branches": [{"count": c, "fallthrough": False, "throw": False} for c in branches]}


def function(name, first, last, count):
    return {"name": name, "demangled_name": name, "lineno": first, "execution_count": count,
            "pos": [f"{first}:1", f"{last}:1"]}


def add_file(count=4, branches=(4, 2)):
    """arm_add_f32 at lines 10-20; count 0 is the report of a run whose test of it was removed."""
    return {"file": ADD, "functions": [function("arm_add_f32", 10, 20, count)],
            "lines": [line(10, count), line(12, count * 16, branches=branches), line(15, count), line(18, 0)]}


def sub_file(count=0):
    return {"file": SUB, "functions": [function("arm_sub_f32", 30, 40, count)],
            "lines": [line(30, count, "arm_sub_f32"), line(31, count, "arm_sub_f32"),
                      line(32, count, "arm_sub_f32", branches=(count, 0))]}


class Gate(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.waivers = self.path("waivers.toml", "")
        self.baseline = self.path("branch_baseline.json", "{}")

    def path(self, name, text):
        path = os.path.join(self.dir.name, name)
        with open(path, "w") as out:
            out.write(text)
        return path

    def report(self, *files, data=None):
        return self.path("coverage.json", data if data is not None else
                         json.dumps({"gcovr/format_version": "0.14", "files": list(files)}))

    def waive(self, *entries):
        text = ""
        for entry in entries:
            text += "[[waiver]]\n" + "".join(f"{key} = {json.dumps(value)}\n" for key, value in entry.items())
        self.path("waivers.toml", text)

    def run_gate(self, *extra, report=None):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            status = coverage_gate.main(["check", "--report", report or self.report_path, "--root", self.dir.name,
                                         "--waivers", self.waivers, "--baseline", self.baseline, *extra])
        return status, output.getvalue() + errors.getvalue()

    def passes(self, *files, extra=()):
        self.report_path = self.report(*files)
        status, output = self.run_gate(*extra)
        self.assertEqual(status, 0, output)
        self.assertIn("PASS", output)

    def fails(self, *files, said=(), extra=()):
        self.report_path = self.report(*files)
        status, output = self.run_gate(*extra)
        self.assertEqual(status, 1, output)
        for text in said:
            self.assertIn(text, output)
        return output


class Full(Gate):
    def test_everything_ran_or_is_waived(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.passes(add_file())

    def test_removed_test_names_the_lines(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.fails(add_file(count=0), said=(f"{ADD}: line(s) 10, 12, 15 never ran",
                                            f"{ADD}:10: function arm_add_f32 never ran"))

    def test_file_no_test_calls(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.fails(add_file(), sub_file(), said=(f"{SUB}: line(s) 30-32 never ran",
                                                 f"{SUB}:30: function arm_sub_f32 never ran"))

    def test_line_that_ran_in_another_variant(self):
        for first in (True, False):
            with self.subTest(ran_first=first):
                add = add_file()
                if first:
                    add["lines"].insert(0, line(18, 3))
                else:
                    add["lines"].append(line(18, 3))
                self.passes(add)

    def test_function_waiver_covers_its_lines(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."},
                   {"file": SUB, "function": "arm_sub_f32", "reason": "Deprecated."})
        self.passes(add_file(), sub_file())

    def test_line_waiver_covers_a_function_starting_inside_it(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."},
                   {"file": SUB, "lines": [30, 32], "reason": "Deprecated."})
        self.passes(add_file(), sub_file())

    def test_whole_file_waiver(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."}, {"file": SUB, "reason": "Deprecated."})
        self.passes(add_file(), sub_file())

    def test_partial_line_waiver(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."},
                   {"file": SUB, "lines": [30, 31], "reason": "Deprecated."})
        self.fails(add_file(), sub_file(), said=(f"{SUB}: line(s) 32 never ran",))

    def test_waiver_that_matches_nothing(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."},
                   {"file": ADD, "lines": [10, 12], "reason": "Was untested."})
        self.fails(add_file(), said=(f"{ADD} lines 10-12: the waiver matches nothing",))

    def test_waiver_for_a_function_that_ran(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."},
                   {"file": SUB, "function": "arm_sub_f32", "reason": "Was untested."})
        self.fails(add_file(), sub_file(count=1), said=(f"{SUB} function arm_sub_f32: the waiver matches nothing",))

    def test_waiver_for_a_file_not_in_the_report(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."}, {"file": SUB, "reason": "Removed."})
        self.fails(add_file(), said=(f"{SUB} (whole file): the waiver matches nothing",))

    def test_branch_coverage_at_or_above_its_baseline(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        for value in (100, 50, 0):
            with self.subTest(baseline=value):
                self.path("branch_baseline.json", json.dumps({ADD: value}))
                self.passes(add_file())

    def test_branch_coverage_dropped(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.path("branch_baseline.json", json.dumps({ADD: 100}))
        self.fails(add_file(branches=(4, 0)), said=(f"{ADD}: branch coverage 50.0000% (1/2) is below its baseline "
                                                    "100.0000%",))

    def test_file_without_branches_meets_any_baseline(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.path("branch_baseline.json", json.dumps({ADD: 100}))
        self.passes(add_file(branches=()))

    def test_baseline_for_a_file_not_in_the_report(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.path("branch_baseline.json", json.dumps({SUB: 10}))
        self.fails(add_file(), said=(f"{SUB}: has a baseline but is not in the report",))


class Inputs(Gate):
    def rejects(self, said, *files, data=None):
        self.report_path = self.report(*(files or (add_file(),)), data=data)
        status, output = self.run_gate()
        self.assertEqual(status, 1, output)
        self.assertIn(said, output)
        self.assertNotIn("PASS", output)

    def test_bad_waivers(self):
        cases = {
            "gives no reason": {"file": ADD},
            "gives both a function and a line range": {"file": ADD, "function": "f", "lines": [1, 2],
                                                       "reason": "r"},
            "lines must be [first, last]": {"file": ADD, "lines": [5, 3], "reason": "r"},
            "unknown key(s) owner": {"file": ADD, "owner": "x", "reason": "r"},
            "is not a path in the scope": {"file": "Testing/main.cpp", "reason": "r"},
        }
        for said, entry in cases.items():
            with self.subTest(said=said):
                self.waive(entry)
                self.rejects(said)
        for path in ("Source/../Testing/main.cpp", "Source/CommonTables/arm_common_tables.c"):
            with self.subTest(path=path):
                self.waive({"file": path, "reason": "r"})
                self.rejects("is not a path in the scope")
        for lines in ([0, 2], [1], [1, 2, 3], ["1", "2"], [True, 2]):
            with self.subTest(lines=lines):
                self.waive({"file": ADD, "lines": lines, "reason": "r"})
                self.rejects("lines must be [first, last]")
        with self.subTest(said="blank reason"):
            self.waive({"file": ADD, "reason": "  "})
            self.rejects("gives no reason")

    def test_waived_twice(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "a"}, {"file": ADD, "lines": [18, 18], "reason": "b"})
        self.rejects("is waived twice")

    def test_waivers_not_toml(self):
        self.path("waivers.toml", "[[waiver]\nfile = 1\n")
        self.rejects("not TOML")

    def test_waivers_with_another_table(self):
        self.path("waivers.toml", "[waivers]\nfile = 'x'\n")
        self.rejects("unknown key(s) waivers")

    def test_missing_waivers(self):
        os.remove(self.waivers)
        self.rejects("cannot read")

    def test_bad_baselines(self):
        for data, said in ((json.dumps({ADD: True}), "bad percentage"), (json.dumps({ADD: 100.5}), "bad percentage"),
                           (json.dumps({ADD: "90"}), "bad percentage"), (json.dumps({ADD: -1}), "bad percentage"),
                           (json.dumps({"Source/./x.c": 1}), "not a path in the scope"),
                           (json.dumps([ADD]), "not a JSON object"), ("{", "not JSON")):
            with self.subTest(data=data):
                self.path("branch_baseline.json", data)
                self.rejects(said)

    def test_bad_reports(self):
        excluded = add_file()
        excluded["lines"][0]["gcovr/excluded"] = True
        malformed = add_file()
        malformed["lines"][0]["count"] = -1
        bad_branch = add_file()
        bad_branch["lines"][1]["branches"][0]["count"] = None
        bad_function = add_file()
        bad_function["functions"][0]["lineno"] = 0
        bad_pos = add_file()
        bad_pos["functions"][0]["pos"] = ["10:1"]
        excluded_branch = add_file()
        excluded_branch["lines"][1]["branches"][1]["gcovr/excluded"] = True
        cases = (("use a waiver instead", (excluded,)), ("malformed line entry", (malformed,)),
                 ("has a branch excluded in the source", (excluded_branch,)),
                 ("malformed branch", (bad_branch,)), ("malformed function entry", (bad_function,)),
                 ("malformed pos", (bad_pos,)), ("is listed twice", (add_file(), add_file())),
                 ("outside the scope", (dict(add_file(), file="Testing/main.cpp"),)),
                 ("outside the scope", (dict(add_file(), file="Source/../Testing/main.cpp"),)),
                 ("no lines or functions list", ({"file": ADD},)))
        for said, files in cases:
            with self.subTest(said=said):
                self.rejects(said, *files)
        for data, said in (("{", "not JSON"), ('{"files": []}', "not a gcovr JSON report"),
                           ('{"gcovr/format_version": "0.14", "files": []}', "covers no file")):
            with self.subTest(data=data):
                self.rejects(said, data=data)

    def test_missing_report(self):
        status, output = self.run_gate(report=os.path.join(self.dir.name, "none.json"))
        self.assertEqual(status, 1)
        self.assertIn("cannot read", output)


class Diff(Gate):
    """Diff mode in a Git checkout whose base has arm_add_f32.c and arm_sub_f32.c of 40 lines each."""

    def setUp(self):
        super().setUp()
        self.git("init", "-q", "-b", "base")
        for path in (ADD, SUB, "Source/CommonTables/arm_common_tables.c", "Testing/main.cpp"):
            self.write(path, [f"line {n}" for n in range(1, 41)])
        self.commit("base")
        self.git("checkout", "-q", "-b", "change")

    def git(self, *args):
        subprocess.run(["git", "-C", self.dir.name, "-c", "user.name=gate", "-c", "user.email=gate@example.invalid",
                        *args], check=True, capture_output=True)

    def write(self, path, lines):
        full = os.path.join(self.dir.name, path)
        os.makedirs(os.path.dirname(full), exist_ok=True)
        with open(full, "w") as out:
            out.write("\n".join(lines) + "\n")

    def commit(self, message):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def change(self, path, *numbers):
        full = os.path.join(self.dir.name, path)
        lines = open(full).read().splitlines()
        for number in numbers:
            lines[number - 1] += " changed"
        self.write(path, lines)
        self.commit("change")

    def diff(self):
        return ("--diff-base", "base")

    def test_changed_line_that_never_ran(self):
        self.change(ADD, 12)
        output = self.fails(add_file(count=0), extra=self.diff(), said=(f"{ADD}: line(s) 12 never ran",
                                                                        "function arm_add_f32 never ran"))
        self.assertNotIn("10", output.split("line(s)")[1].split("\n")[0])

    def test_unchanged_lines_that_never_ran(self):
        self.change(ADD, 12)
        self.passes(add_file(), sub_file(), extra=self.diff())

    def test_change_next_to_a_line_that_never_ran(self):
        self.change(ADD, 17, 19)
        self.passes(add_file(), extra=self.diff())

    def test_deletion_next_to_a_line_that_never_ran(self):
        lines = open(os.path.join(self.dir.name, ADD)).read().splitlines()
        self.write(ADD, lines[:18] + lines[19:])
        self.commit("delete line 19")
        self.passes(add_file(), extra=self.diff())

    def test_change_outside_the_scope(self):
        self.change("Testing/main.cpp", 3)
        self.change("Source/CommonTables/arm_common_tables.c", 3)
        output = self.run_gate(*self.diff(), report=self.report(add_file(count=0), sub_file()))[1]
        self.assertIn("PASS (diff mode against base, 0 changed file(s))", output)

    def test_change_in_a_header(self):
        self.write("Include/dsp/basic_math_functions.h", ["int x;"])
        self.commit("header")
        self.passes(add_file(), extra=self.diff())

    def test_waiver_of_unchanged_lines_in_a_changed_file(self):
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.change(ADD, 12)
        self.passes(add_file(), extra=self.diff())

    def test_stale_waiver_in_a_changed_file(self):
        self.waive({"file": ADD, "lines": [10, 10], "reason": "Was untested."})
        self.change(ADD, 12)
        self.fails(add_file(), extra=self.diff(), said=(f"{ADD} lines 10-10: the waiver matches nothing",))

    def test_stale_waiver_in_an_unchanged_file(self):
        self.waive({"file": SUB, "reason": "Was untested."})
        self.change(ADD, 12)
        self.passes(add_file(), sub_file(count=1), extra=self.diff())

    def test_branch_drop_in_a_changed_file(self):
        self.path("branch_baseline.json", json.dumps({ADD: 100}))
        self.change(ADD, 12)
        self.fails(add_file(branches=(4, 0)), extra=self.diff(), said=("below its baseline",))

    def test_branch_drop_in_an_unchanged_file(self):
        self.path("branch_baseline.json", json.dumps({ADD: 100, "Source/BasicMathFunctions/arm_mult_f32.c": 50}))
        self.change(SUB, 31)
        self.passes(add_file(branches=(4, 0)), sub_file(count=1), extra=self.diff())

    def test_changed_file_no_variant_compiled(self):
        self.change(SUB, 31)
        self.fails(add_file(), extra=self.diff(), said=(f"{SUB}: changed, but no variant compiled it",))

    def test_deletion_only_edit_of_a_file_no_variant_compiled(self):
        lines = open(os.path.join(self.dir.name, SUB)).read().splitlines()
        self.write(SUB, lines[:30] + lines[31:])
        self.commit("delete a line")
        self.fails(add_file(), extra=self.diff(), said=(f"{SUB}: changed, but no variant compiled it",))

    def test_renamed_file_no_variant_compiled(self):
        moved = "Source/BasicMathFunctions/arm_sub_f32_moved.c"
        self.git("mv", SUB, moved)
        self.commit("rename")
        self.path("branch_baseline.json", json.dumps({moved: 10}))
        self.fails(add_file(), extra=self.diff(), said=(f"{moved}: changed, but no variant compiled it",
                                                        f"{moved}: changed and has a baseline, but is not in the "
                                                        "report"))

    def test_changed_file_no_variant_compiled_is_waived(self):
        self.waive({"file": SUB, "reason": "Neon only."})
        self.change(SUB, 31)
        self.passes(add_file(), extra=self.diff())

    def test_new_function_that_no_test_calls(self):
        self.write(ADD, [f"line {n}" for n in range(1, 41)] + ["int new_function(void)", "{", "return 0;", "}"])
        self.commit("add a function")
        add = add_file()
        add["functions"].append(function("new_function", 41, 44, 0))
        add["lines"] += [line(41, 0, "new_function"), line(43, 0, "new_function")]
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        self.fails(add, extra=self.diff(), said=(f"{ADD}: line(s) 41, 43 never ran",
                                                 f"{ADD}:41: function new_function never ran"))

    def test_deleted_file(self):
        os.remove(os.path.join(self.dir.name, SUB))
        self.commit("remove")
        self.passes(add_file(), extra=self.diff())

    def test_deleted_file_with_a_baseline_or_waiver(self):
        os.remove(os.path.join(self.dir.name, SUB))
        self.commit("remove")
        self.path("branch_baseline.json", json.dumps({SUB: 10}))
        self.fails(add_file(), extra=self.diff(), said=(f"{SUB}: removed or renamed, but still has a baseline",))
        self.path("branch_baseline.json", "{}")
        self.waive({"file": SUB, "reason": "Deprecated."})
        self.fails(add_file(), extra=self.diff(), said=(f"{SUB}: removed or renamed, but still has 1 waiver(s)",))

    def rename(self, new):
        self.git("mv", SUB, new)
        self.commit("rename")

    def test_renamed_file_keeps_its_old_baseline(self):
        moved = "Source/BasicMathFunctions/arm_sub_f32_moved.c"
        self.rename(moved)
        self.path("branch_baseline.json", json.dumps({SUB: 10}))
        self.fails(add_file(), dict(sub_file(count=1), file=moved), extra=self.diff(),
                   said=(f"{SUB}: removed or renamed, but still has a baseline",))

    def test_renamed_file_is_judged(self):
        moved = "Source/BasicMathFunctions/arm_sub_f32_moved.c"
        self.rename(moved)
        self.path("branch_baseline.json", json.dumps({moved: 100}))
        self.waive({"file": moved, "lines": [1, 2], "reason": "Was untested."})
        output = self.fails(add_file(), dict(sub_file(count=1), file=moved), extra=self.diff(),
                            said=(f"{moved}: branch coverage 50.0000% (1/2) is below its baseline",
                                  f"{moved} lines 1-2: the waiver matches nothing"))
        self.assertNotIn("line(s)", output)
        self.assertIn("1 changed file(s)", output)

    def test_source_lines_that_read_like_diff_headers(self):
        lines = open(os.path.join(self.dir.name, ADD)).read().splitlines()
        lines[19] = "-- x"
        self.write(ADD, lines)
        self.commit("odd line")
        self.git("tag", "odd")
        lines[19] = "++ y"
        lines[20] = "--- z"
        self.write(ADD, lines)
        self.commit("odder line")
        self.report_path = self.report(add_file(), dict(add_file(), file=SUB))
        status, output = self.run_gate("--diff-base", "odd")
        self.assertEqual(status, 0, output)
        self.assertIn("1 changed file(s)", output)

    def test_quoted_path(self):
        self.write('Source/BasicMathFunctions/arm_"q".c', ["x"])
        self.commit("quoted")
        self.report_path = self.report(add_file())
        status, output = self.run_gate(*self.diff())
        self.assertEqual(status, 1, output)
        self.assertIn("quoted path", output)

    def test_unknown_base(self):
        self.report_path = self.report(add_file())
        status, output = self.run_gate("--diff-base", "no-such-ref")
        self.assertEqual(status, 1)
        self.assertIn("git diff against no-such-ref failed", output)

    def test_not_a_checkout(self):
        self.report_path = self.report(add_file())
        with tempfile.TemporaryDirectory() as elsewhere:
            output, errors = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
                status = coverage_gate.main(["check", "--report", self.report_path, "--root", elsewhere,
                                             "--waivers", self.waivers, "--baseline", self.baseline,
                                             "--diff-base", "base"])
        self.assertEqual(status, 1)
        self.assertIn("git diff against base failed", errors.getvalue())


class Ratchet(Gate):
    def read_baseline(self):
        with open(self.baseline) as source:
            return json.load(source)

    def ratchet(self, *files):
        self.report_path = self.report(*files)
        with contextlib.redirect_stdout(io.StringIO()):
            status = coverage_gate.main(["ratchet", "--report", self.report_path, "--baseline", self.baseline])
        self.assertEqual(status, 0)
        with open(self.baseline) as source:
            return json.load(source)

    def test_first_baseline_is_the_current_coverage(self):
        self.assertEqual(self.ratchet(add_file(branches=(1, 1, 0))), {ADD: 66.6666})

    def test_one_branch_less_in_a_large_file(self):
        self.ratchet(add_file(branches=(1,) * 19999 + (0,)))
        self.report_path = self.report(add_file(branches=(1,) * 19998 + (0, 0)))
        self.waive({"file": ADD, "lines": [18, 18], "reason": "Neon only."})
        status, output = self.run_gate()
        self.assertEqual(status, 1, output)
        self.assertIn("99.9900% (19998/20000) is below its baseline 99.9950%", output)

    def test_raises_but_never_lowers(self):
        self.path("branch_baseline.json", json.dumps({ADD: 75, SUB: 10}))
        self.assertEqual(self.ratchet(add_file(branches=(1, 0)), sub_file(count=1)), {ADD: 75, SUB: 50})

    def test_keeps_files_not_in_the_report(self):
        self.path("branch_baseline.json", json.dumps({SUB: 10}))
        self.assertEqual(self.ratchet(add_file()), {ADD: 100, SUB: 10})

    def test_skips_files_without_branches(self):
        self.assertEqual(self.ratchet(add_file(branches=())), {})

    def test_bad_report_leaves_the_baseline(self):
        self.path("branch_baseline.json", json.dumps({ADD: 10}))
        self.report_path = self.report(data="{")
        with contextlib.redirect_stderr(io.StringIO()):
            status = coverage_gate.main(["ratchet", "--report", self.report_path, "--baseline", self.baseline])
        self.assertEqual(status, 1)
        self.assertEqual(self.read_baseline(), {ADD: 10})


class Shipped(unittest.TestCase):
    """The waivers and baseline in the repository must load."""

    def test_files_load(self):
        here = os.path.dirname(os.path.abspath(__file__))
        coverage_gate.read_waivers(os.path.join(here, "waivers.toml"))
        coverage_gate.read_baseline(os.path.join(here, "branch_baseline.json"))


if __name__ == "__main__":
    unittest.main()
