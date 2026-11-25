"""Check a merged coverage report against the gate: lines and functions, waivers and the branch ratchet.

usage: coverage_gate.py check --report FILE --root DIR [--waivers FILE] [--baseline FILE] [--diff-base REF]
       coverage_gate.py ratchet --report FILE --baseline FILE

The report is the coverage.json that merge.py writes. check fails on:
- a line or a function that never ran in any variant and has no waiver;
- a file whose branch coverage is below its value in branch_baseline.json;
- a waiver that waives nothing that never ran;
- a baseline entry for a file that is not in the report (full mode only).

With --diff-base, check runs in diff mode: it judges only the lines that changed since the merge base with
REF, and only the waivers and baseline entries of files that changed, so a report from part of the matrix
can be checked. Without it, check judges everything (full mode).

ratchet raises each file's baseline to its current branch coverage, rounded down to 0.0001% (well under one
branch of any file), and never lowers one.

waivers.toml holds [[waiver]] tables. Each one names a file, and optionally a function or a line range
(lines = [first, last]), and gives a reason:

    [[waiver]]
    file = "Source/BasicMathFunctions/arm_add_f32.c"
    lines = [150, 152]
    reason = "Only reachable on a Neon build, which no variant compiles."

A waiver for a function covers its lines and every definition of that name in the file. A waiver with
neither covers the whole file.
"""
import argparse
import json
import math
import os
import re
import subprocess
import sys

from merge import in_scope

HERE = os.path.dirname(os.path.abspath(__file__))
WAIVER_KEYS = {"file", "function", "lines", "reason"}
HUNK = re.compile(r"^@@ -\d+(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")


class GateError(Exception):
    """The inputs cannot be judged: a bad report, waiver file, baseline or diff."""


def is_count(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def is_line_number(value):
    return isinstance(value, int) and not isinstance(value, bool) and value >= 1


class FileCoverage:
    def __init__(self, path):
        self.path = path
        self.lines = {}        # line number -> executed in any variant
        self.line_function = {}  # line number -> function names
        self.functions = {}    # (name, first line) -> (executed, last line or None)
        self.branches = [0, 0]  # covered, total

    def branch_percent(self):
        covered, total = self.branches
        return 100.0 if total == 0 else 100.0 * covered / total


def read_report(path):
    try:
        with open(path) as report:
            data = json.load(report)
    except OSError as exc:
        raise GateError(f"{path}: cannot read: {exc.strerror}") from None
    except ValueError as exc:
        raise GateError(f"{path}: not JSON: {exc}") from None
    if not isinstance(data, dict) or "gcovr/format_version" not in data or not isinstance(data.get("files"), list):
        raise GateError(f"{path}: not a gcovr JSON report")
    if not data["files"]:
        raise GateError(f"{path}: the report covers no file")
    files = {}
    for entry in data["files"]:
        name = entry.get("file") if isinstance(entry, dict) else None
        if not in_scope(name):
            raise GateError(f"{path}: file {name!r} is outside the scope")
        if name in files:
            raise GateError(f"{path}: {name} is listed twice")
        coverage = files[name] = FileCoverage(name)
        lines, functions = entry.get("lines"), entry.get("functions")
        if not isinstance(lines, list) or not isinstance(functions, list):
            raise GateError(f"{path}: {name} has no lines or functions list")
        for line in lines:
            number, count = line.get("line_number"), line.get("count")
            if not is_line_number(number) or not is_count(count) or not isinstance(line.get("branches"), list):
                raise GateError(f"{path}: {name} has a malformed line entry {line!r}")
            if line.get("gcovr/excluded"):
                raise GateError(f"{path}: {name}:{number} is excluded in the source; use a waiver instead")
            coverage.lines[number] = coverage.lines.get(number, False) or count > 0
            if line.get("function_name"):
                coverage.line_function.setdefault(number, set()).add(line["function_name"])
            for branch in line["branches"]:
                if not isinstance(branch, dict) or not is_count(branch.get("count")):
                    raise GateError(f"{path}: {name}:{number} has a malformed branch {branch!r}")
                if branch.get("gcovr/excluded"):
                    raise GateError(f"{path}: {name}:{number} has a branch excluded in the source; use a waiver "
                                    "instead")
                coverage.branches[0] += branch["count"] > 0
                coverage.branches[1] += 1
        for function in functions:
            fname, first, count = function.get("name"), function.get("lineno"), function.get("execution_count")
            if not isinstance(fname, str) or not fname or not is_line_number(first) or not is_count(count):
                raise GateError(f"{path}: {name} has a malformed function entry {function!r}")
            last = None
            pos = function.get("pos")
            if pos is not None:
                try:
                    last = int(pos[1].split(":")[0])
                except (TypeError, ValueError, IndexError, AttributeError):
                    raise GateError(f"{path}: {name} function {fname} has a malformed pos {pos!r}") from None
            key = (fname, first)
            executed = coverage.functions.get(key, (False, last))[0] or count > 0
            coverage.functions[key] = (executed, last)
    return files


class Waiver:
    def __init__(self, index, entry, source):
        where = f"{source}: waiver {index + 1}"
        if not isinstance(entry, dict):
            raise GateError(f"{where}: not a table")
        unknown = sorted(set(entry) - WAIVER_KEYS)
        if unknown:
            raise GateError(f"{where}: unknown key(s) {', '.join(unknown)}")
        self.file, self.function, self.lines = entry.get("file"), entry.get("function"), entry.get("lines")
        reason = entry.get("reason")
        if not in_scope(self.file):
            raise GateError(f"{where}: file {self.file!r} is not a path in the scope")
        if not isinstance(reason, str) or not reason.strip():
            raise GateError(f"{where}: {self.file} gives no reason")
        if self.function is not None and self.lines is not None:
            raise GateError(f"{where}: {self.file} gives both a function and a line range")
        if self.function is not None and (not isinstance(self.function, str) or not self.function):
            raise GateError(f"{where}: {self.file} has a bad function name {self.function!r}")
        if self.lines is not None:
            if not (isinstance(self.lines, list) and len(self.lines) == 2 and
                    all(is_line_number(n) for n in self.lines) and self.lines[0] <= self.lines[1]):
                raise GateError(f"{where}: {self.file} lines must be [first, last], got {self.lines!r}")
        self.used = False

    def key(self):
        return self.file, self.function, tuple(self.lines) if self.lines else None

    def describe(self):
        if self.function:
            return f"{self.file} function {self.function}"
        if self.lines:
            return f"{self.file} lines {self.lines[0]}-{self.lines[1]}"
        return f"{self.file} (whole file)"

    def covers_line(self, path, number, functions):
        if path != self.file:
            return False
        if self.function:
            return self.function in functions
        if self.lines:
            return self.lines[0] <= number <= self.lines[1]
        return True

    def covers_function(self, path, name, first):
        if path != self.file:
            return False
        if self.function:
            return self.function == name
        if self.lines:
            return self.lines[0] <= first <= self.lines[1]
        return True


def read_waivers(path):
    try:
        import tomllib
    except ImportError:
        raise GateError("reading waivers needs Python 3.11 or later (tomllib)") from None
    try:
        with open(path, "rb") as source:
            data = tomllib.load(source)
    except OSError as exc:
        raise GateError(f"{path}: cannot read: {exc.strerror}") from None
    except tomllib.TOMLDecodeError as exc:
        raise GateError(f"{path}: not TOML: {exc}") from None
    unknown = sorted(set(data) - {"waiver"})
    if unknown:
        raise GateError(f"{path}: unknown key(s) {', '.join(unknown)}")
    entries = data.get("waiver", [])
    if not isinstance(entries, list):
        raise GateError(f"{path}: waiver must be an array of tables ([[waiver]])")
    waivers, seen = [], set()
    for index, entry in enumerate(entries):
        waiver = Waiver(index, entry, path)
        if waiver.key() in seen:
            raise GateError(f"{path}: {waiver.describe()} is waived twice")
        seen.add(waiver.key())
        waivers.append(waiver)
    return waivers


def read_baseline(path):
    try:
        with open(path) as source:
            data = json.load(source)
    except OSError as exc:
        raise GateError(f"{path}: cannot read: {exc.strerror}") from None
    except ValueError as exc:
        raise GateError(f"{path}: not JSON: {exc}") from None
    if not isinstance(data, dict):
        raise GateError(f"{path}: not a JSON object of file: branch percentage")
    for name, value in data.items():
        if not in_scope(name):
            raise GateError(f"{path}: {name!r} is not a path in the scope")
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 <= value <= 100:
            raise GateError(f"{path}: {name} has a bad percentage {value!r}")
    return data


def diff_path(text, prefix):
    path = text[len(prefix):]
    if path.startswith('"'):
        raise GateError(f"git diff names a quoted path {path}; rename the file")
    return path


def changed_lines(root, base):
    """Since the merge base of base and HEAD: the lines each in-scope file gained or changed (a file renamed
    without a change has none), and the in-scope paths that were deleted or renamed away."""
    command = ["git", "-C", root, "-c", "core.quotePath=false", "diff", "--no-color", "--no-ext-diff",
               "--unified=0", "--diff-filter=ACDMR", "-M", f"{base}...HEAD", "--", "Source", "Include/dsp"]
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except OSError as exc:
        raise GateError(f"cannot run git: {exc.strerror}") from None
    if result.returncode != 0:
        raise GateError(f"git diff against {base} failed: {result.stderr.strip()}")
    changed, removed, current, old, body = {}, set(), None, None, 0
    for text in result.stdout.splitlines():
        if body:
            # A hunk's own lines are skipped by count, so a source line that reads like a header is not one.
            if not text.startswith(("+", "-", "\\")):
                raise GateError(f"unexpected line in a git diff hunk {text!r}")
            body -= not text.startswith("\\")
            continue
        if text.startswith("diff --git "):
            current = old = None
        elif text.startswith("rename from "):
            removed.add(diff_path(text, "rename from "))
        elif text.startswith("rename to "):
            changed.setdefault(diff_path(text, "rename to "), set())
        elif text.startswith("--- "):
            source = diff_path(text, "--- ")
            old = source[2:] if source.startswith("a/") else None
        elif text.startswith("+++ "):
            target = diff_path(text, "+++ ")
            if target == "/dev/null":
                if old is None:
                    raise GateError(f"unexpected git diff deletion {text!r}")
                removed.add(old)
                current = None
            elif target.startswith("b/"):
                current = target[2:]
                changed.setdefault(current, set())
            else:
                raise GateError(f"unexpected git diff line {text!r}")
        elif text.startswith("@@"):
            match = HUNK.match(text)
            if not match:
                raise GateError(f"unexpected git diff hunk {text!r}")
            first, count = int(match.group(2)), int(match.group(3) or 1)
            body = int(match.group(1) or 1) + count
            if current:
                changed[current].update(range(first, first + count))
    return {path: lines for path, lines in changed.items() if in_scope(path)}, {path for path in removed
                                                                                   if in_scope(path)}


def ranges(numbers):
    numbers = sorted(numbers)
    spans, start = [], None
    for index, number in enumerate(numbers):
        if start is None:
            start = number
        if index + 1 == len(numbers) or numbers[index + 1] != number + 1:
            spans.append(str(start) if start == number else f"{start}-{number}")
            start = None
    return ", ".join(spans)


def check(files, waivers, baseline, changed=None, removed=()):
    """Return the gate's failures. In diff mode, changed maps each changed file to its changed lines and removed
    holds the files deleted or renamed away; in full mode changed is None."""
    failures = []
    by_file = {}
    for waiver in waivers:
        by_file.setdefault(waiver.file, []).append(waiver)

    def waived_line(path, number):
        functions = files[path].line_function.get(number, ())
        hits = [w for w in by_file.get(path, ()) if w.covers_line(path, number, functions)]
        for waiver in hits:
            waiver.used = True
        return bool(hits)

    def waived_function(path, name, first):
        hits = [w for w in by_file.get(path, ()) if w.covers_function(path, name, first)]
        for waiver in hits:
            waiver.used = True
        return bool(hits)

    # Waivers are matched against the whole of each judged file; in diff mode only changed lines can fail.
    judged = sorted(files) if changed is None else sorted(set(changed) & set(files))
    for path in judged:
        coverage = files[path]
        lines = changed.get(path) if changed is not None else None
        missed = [n for n, ran in sorted(coverage.lines.items())
                  if not ran and not waived_line(path, n) and (lines is None or n in lines)]
        if missed:
            failures.append(f"{path}: line(s) {ranges(missed)} never ran")
        for (name, first), (ran, last) in sorted(coverage.functions.items(), key=lambda item: item[0][1]):
            if ran or waived_function(path, name, first):
                continue
            if lines is None or any(n in lines for n in range(first, (last or first) + 1)):
                failures.append(f"{path}:{first}: function {name} never ran")
        if path in baseline and coverage.branch_percent() < baseline[path] - 1e-9:
            covered, total = coverage.branches
            failures.append(f"{path}: branch coverage {coverage.branch_percent():.4f}% ({covered}/{total}) "
                            f"is below its baseline {baseline[path]:.4f}%")
    if changed is not None:
        # A changed source file that no variant compiled has lines no report can show as run. That holds for a
        # rename or a deletion-only edit too: either can drop the file from the build.
        for path in sorted(set(changed) - set(files)):
            if path.endswith(".c") and not any(w.file == path and not w.function and not w.lines for w in waivers):
                failures.append(f"{path}: changed, but no variant compiled it")
            if path in baseline:
                failures.append(f"{path}: changed and has a baseline, but is not in the report")
            for waiver in waivers:
                if waiver.file == path and not waiver.function and not waiver.lines:
                    waiver.used = True
        # A baseline or waiver left on a path the change removed would no longer gate anything.
        for path in sorted(set(removed) - set(changed)):
            if path in baseline:
                failures.append(f"{path}: removed or renamed, but still has a baseline")
            if path in by_file:
                failures.append(f"{path}: removed or renamed, but still has {len(by_file[path])} waiver(s)")
    else:
        for path in sorted(set(baseline) - set(files)):
            failures.append(f"{path}: has a baseline but is not in the report")
    for waiver in waivers:
        if not waiver.used and (changed is None or waiver.file in changed):
            failures.append(f"{waiver.describe()}: the waiver matches nothing that never ran")
    return failures


def ratchet(files, baseline):
    raised = dict(baseline)
    for path, coverage in files.items():
        if coverage.branches[1] == 0:
            continue
        value = math.floor(coverage.branch_percent() * 10000) / 10000
        if value > raised.get(path, 0):
            raised[path] = value
    return dict(sorted(raised.items()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    gate = commands.add_parser("check")
    gate.add_argument("--report", required=True)
    gate.add_argument("--root", required=True, help="the CMSIS-DSP checkout the report was made from")
    gate.add_argument("--waivers", default=os.path.join(HERE, "waivers.toml"))
    gate.add_argument("--baseline", default=os.path.join(HERE, "branch_baseline.json"))
    gate.add_argument("--diff-base", help="judge only what changed since the merge base with this ref")
    raise_ = commands.add_parser("ratchet")
    raise_.add_argument("--report", required=True)
    raise_.add_argument("--baseline", default=os.path.join(HERE, "branch_baseline.json"))
    args = parser.parse_args(argv)

    try:
        files = read_report(args.report)
        baseline = read_baseline(args.baseline)
        if args.command == "ratchet":
            raised = ratchet(files, baseline)
            try:
                with open(args.baseline + ".tmp", "w") as out:
                    json.dump(raised, out, indent=2)
                    out.write("\n")
                os.replace(args.baseline + ".tmp", args.baseline)
            except OSError as exc:
                raise GateError(f"{args.baseline}: cannot write: {exc.strerror}") from None
            changed = sorted(p for p in raised if raised[p] != baseline.get(p))
            print(f"coverage_gate.py: raised {len(changed)} baseline(s)")
            for path in changed:
                print(f"  {path}: {baseline.get(path, 0):.4f}% -> {raised[path]:.4f}%")
            return 0
        waivers = read_waivers(args.waivers)
        changed, removed = changed_lines(args.root, args.diff_base) if args.diff_base else (None, ())
    except GateError as exc:
        print(f"coverage_gate.py: {exc}", file=sys.stderr)
        return 1

    failures = check(files, waivers, baseline, changed, removed)
    mode = f"diff mode against {args.diff_base}, {len(changed)} changed file(s)" if changed is not None else \
        "full mode"
    for failure in failures:
        print(failure)
    if failures:
        print(f"coverage_gate.py: FAIL ({mode}): {len(failures)} problem(s)", file=sys.stderr)
        return 1
    print(f"coverage_gate.py: PASS ({mode})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
