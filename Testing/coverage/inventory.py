"""Map CMSIS-DSP kernel files to the test suites that reach them, and list what no suite reaches.

usage: inventory.py report --root DIR [--json FILE]
       inventory.py suites --root DIR PATH...

A suite is a class of Testing/desc.txt or desc_f16.txt that has a Testing/Source/Tests/<class>.cpp, as in
the CMake build. A suite reaches a kernel function when one of its enabled tests (a Functions line not
inside disabled{...}), or its setUp or tearDown, calls the function, directly or through other kernel
functions. The analysis reads the sources as text: it does not preprocess them, so a function called only
through a macro that pastes its name is not seen, and every #if branch counts.

report prints the kernel files whose names appear in no test source, then the kernel functions no suite
reaches, by category. --json writes the whole inventory.

suites prints, one per line, the suites to run for the given changed paths (relative to --root):
- a kernel file: the suites that reach it (none when no suite does);
- a suite's test source, its header in Testing/Include/Tests, or a file under its pattern or parameter
  folder: that suite;
- any other file under Source/, Include/ or Testing/ (headers, tables, desc files, the framework): every
  suite, since the analysis cannot bound its effect;
- anything else: no suite.
"""
import argparse
import json
import posixpath
import re
import sys
from pathlib import Path

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
DEFINITION = re.compile(r"^[A-Za-z_][\w \t\*]*?\b(arm_\w+)\s*\(([^;{]*?)\)\s*\{", re.M | re.S)
CALL = re.compile(r"\b(arm_\w+)\s*\(")
DESC_FILES = ("desc.txt", "desc_f16.txt")
BLOCK = re.compile(r"^(suite|group)\b")
SETTING = re.compile(r"^(class|folder)\s*=\s*(\w+)\s*$")
TEST_LINE = re.compile(r"^(.+?):\s*(\w+)\s*$")
DISABLED = re.compile(r"^disabled\s*\{(.*)\}\s*$")
FIXTURES = ("setUp", "tearDown")


class InventoryError(Exception):
    pass


def source_text(path):
    try:
        return COMMENT.sub(" ", path.read_text(errors="replace"))
    except OSError as exc:
        raise InventoryError(f"{path}: cannot read: {exc.strerror}") from None


def body_end(text, start):
    """The index just past the brace block opening at text[start], or len(text) if it never closes."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index + 1
    return len(text)


class Suite:
    def __init__(self, name, folder, source):
        self.name, self.folder, self.source = name, folder, source
        self.enabled, self.disabled = [], []
        self.calls = set()
        self.whole_file = False  # the calls come from the whole test source: a test method was not found


def parse_desc(path):
    """Return [(class, folder path, enabled methods, disabled methods)] for each suite block."""
    try:
        lines = path.read_text().splitlines()
    except OSError as exc:
        raise InventoryError(f"{path}: cannot read: {exc.strerror}") from None
    stack = []  # entries: [kind, class, folder]; kind is suite, group, functions or other
    suites = []
    for number, raw in enumerate(lines, 1):
        line = raw.split("//", 1)[0].strip()
        if not line:
            continue
        where = f"{path.name}:{number}"
        in_functions = bool(stack) and stack[-1][0] == "functions"
        disabled = DISABLED.match(line)
        if disabled:
            if not in_functions:
                raise InventoryError(f"{where}: disabled{{...}} outside a Functions block")
            test = TEST_LINE.match(disabled.group(1).strip())
            if not test:
                raise InventoryError(f"{where}: cannot read the disabled test {line!r}")
            stack[-2][3][1].append(test.group(2))
            continue
        setting = SETTING.match(line)
        if setting and stack and stack[-1][0] in ("suite", "group"):
            stack[-1][1 if setting.group(1) == "class" else 2] = setting.group(2)
            continue
        if in_functions and "{" not in line and "}" not in line:
            test = TEST_LINE.match(line)
            if not test:
                raise InventoryError(f"{where}: cannot read the test line {line!r}")
            stack[-2][3][0].append(test.group(2))
            continue
        opened = False
        for char in line:
            if char == "{":
                # Only a line's first brace can open a suite, group or Functions block.
                block = None if opened else BLOCK.match(line)
                if block:
                    kind = block.group(1)
                elif not opened and line.startswith("Functions") and stack and stack[-1][0] == "suite":
                    kind = "functions"
                else:
                    kind = "other"
                opened = True
                stack.append([kind, None, None, ([], [])])
            elif char == "}":
                if not stack:
                    raise InventoryError(f"{where}: unbalanced }}")
                kind, name, folder, tests = stack.pop()
                if kind == "suite":
                    if not name:
                        raise InventoryError(f"{where}: a suite without a class")
                    folders = [entry[2] for entry in stack if entry[0] in ("suite", "group") and entry[2]]
                    suites.append((name, "/".join(folders + ([folder] if folder else [])), *tests))
    if stack:
        raise InventoryError(f"{path.name}: {len(stack)} block(s) never closed")
    return suites


def method_calls(text, suite, method):
    """The arm_* calls in Suite::method, or None when its definition is not found."""
    match = re.search(rf"\b{re.escape(suite)}\s*::\s*{re.escape(method)}\s*\([^;{{]*\)\s*\{{", text)
    if not match:
        return None
    return set(CALL.findall(text[match.end() - 1:body_end(text, match.end() - 1)]))


class Inventory:
    def __init__(self, root):
        self.root = Path(root)
        if not (self.root / "Source").is_dir() or not (self.root / "Testing").is_dir():
            raise InventoryError(f"{root}: not a CMSIS-DSP checkout (no Source/ and Testing/)")
        self.kernels = sorted(p.relative_to(self.root).as_posix() for p in (self.root / "Source").rglob("arm_*.c")
                              if "CommonTables" not in p.relative_to(self.root).parts)
        self.defined = {}    # function -> files defining it
        self.functions = {}  # file -> functions it defines
        self.calls = {}      # function -> kernel functions its bodies call
        for path in self.kernels:
            text = source_text(self.root / path)
            names = set()
            for match in DEFINITION.finditer(text):
                name = match.group(1)
                names.add(name)
                self.defined.setdefault(name, set()).add(path)
                body = text[match.end() - 1:body_end(text, match.end() - 1)]
                self.calls.setdefault(name, set()).update(CALL.findall(body))
            self.functions[path] = names
        for name in self.calls:
            self.calls[name] = {called for called in self.calls[name] if called in self.defined and called != name}

        self.suites = {}
        tests_dir = self.root / "Testing" / "Source" / "Tests"
        for desc in DESC_FILES:
            for name, folder, enabled, disabled in parse_desc(self.root / "Testing" / desc):
                source = tests_dir / f"{name}.cpp"
                if not source.is_file():
                    continue  # a benchmark or an example class, which the test build does not run
                if name in self.suites:
                    raise InventoryError(f"suite {name} is described twice")
                suite = self.suites[name] = Suite(name, folder, source.relative_to(self.root).as_posix())
                suite.enabled, suite.disabled = enabled, disabled
                text = source_text(source)
                for method in [*dict.fromkeys(enabled), *FIXTURES]:
                    found = method_calls(text, name, method)
                    if found is None and method not in FIXTURES:
                        suite.whole_file = True
                    suite.calls |= found or set()
                if suite.whole_file:
                    suite.calls = set(CALL.findall(text))
        if not self.suites:
            raise InventoryError("no suite in desc.txt or desc_f16.txt has a test source")

        self.reached_by = {name: set() for name in self.defined}  # function -> suites
        for suite in self.suites.values():
            frontier = [name for name in suite.calls if name in self.defined]
            seen = set(frontier)
            while frontier:
                name = frontier.pop()
                self.reached_by[name].add(suite.name)
                for called in self.calls.get(name, ()):
                    if called not in seen:
                        seen.add(called)
                        frontier.append(called)
        self.test_words = set()
        for path in sorted(tests_dir.glob("*.cpp")):
            self.test_words.update(re.findall(r"\w+", source_text(path)))

    def file_suites(self, path):
        return sorted(set().union(*(self.reached_by[name] for name in self.functions.get(path, ()))))

    def named_in_no_test(self):
        return [path for path in self.kernels if posixpath.basename(path)[:-2] not in self.test_words]

    def unreached(self):
        """{category: sorted functions} of the kernel functions no suite reaches."""
        result = {}
        for path in self.kernels:
            for name in sorted(self.functions[path]):
                if not self.reached_by[name]:
                    result.setdefault(path.split("/")[1], set()).add(name)
        return {category: sorted(names) for category, names in sorted(result.items())}

    def suites_for(self, path):
        """(suites, reason) for one changed path."""
        if posixpath.isabs(path) or "\\" in path or posixpath.normpath(path) != path or path.startswith("../"):
            raise InventoryError(f"{path!r} is not a normal path relative to the checkout")
        everything = sorted(self.suites)
        if path in self.functions:
            return self.file_suites(path), "kernel"
        parts = path.split("/")
        if parts[:3] == ["Testing", "Source", "Tests"] or parts[:3] == ["Testing", "Include", "Tests"]:
            stem = posixpath.splitext(parts[-1])[0]
            if len(parts) == 4 and stem in self.suites:
                return [stem], "test source"
        if parts[0] == "Testing" and len(parts) > 2 and parts[1] in ("Patterns", "Parameters"):
            folder = "/".join(parts[2:-1])
            owners = sorted(s.name for s in self.suites.values()
                            if s.folder and (folder == s.folder or folder.startswith(s.folder + "/")))
            if owners:
                return owners, "patterns"
        if parts[0] in ("Source", "Include", "Testing"):
            return everything, "unbounded"
        return [], "outside"

    def as_json(self):
        files_of = {name: set() for name in self.suites}
        for function, suites in self.reached_by.items():
            for suite in suites:
                files_of[suite] |= self.defined[function]
        unnamed = set(self.named_in_no_test())
        return {
            "suites": {name: {"source": s.source, "folder": s.folder, "enabled_tests": len(s.enabled),
                              "disabled_tests": len(s.disabled), "whole_file": s.whole_file,
                              "files": sorted(files_of[name])}
                       for name, s in sorted(self.suites.items())},
            "files": [{"file": path, "category": path.split("/")[1],
                       "named_in_tests": path not in unnamed,
                       "suites": self.file_suites(path),
                       "functions": {name: sorted(self.reached_by[name]) for name in sorted(self.functions[path])}}
                      for path in self.kernels],
        }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    report = commands.add_parser("report")
    report.add_argument("--root", required=True)
    report.add_argument("--json")
    suites = commands.add_parser("suites")
    suites.add_argument("--root", required=True)
    suites.add_argument("paths", nargs="+")
    args = parser.parse_args(argv)

    try:
        inventory = Inventory(args.root)
        if args.command == "suites":
            chosen = set()
            for path in args.paths:
                found, reason = inventory.suites_for(path)
                if reason == "unbounded":
                    print(f"inventory.py: {path} can affect every suite", file=sys.stderr)
                elif reason == "kernel" and not found:
                    print(f"inventory.py: no suite reaches {path}", file=sys.stderr)
                chosen.update(found)
            for name in sorted(chosen):
                print(name)
            return 0
        unnamed = inventory.named_in_no_test()
        unreached = inventory.unreached()
        if args.json:
            try:
                Path(args.json).write_text(json.dumps(inventory.as_json(), indent=1) + "\n")
            except OSError as exc:
                raise InventoryError(f"{args.json}: cannot write: {exc.strerror}") from None
    except InventoryError as exc:
        print(f"inventory.py: {exc}", file=sys.stderr)
        return 1

    reached = sum(1 for path in inventory.kernels if inventory.file_suites(path))
    fallbacks = sorted(s.name for s in inventory.suites.values() if s.whole_file)
    print(f"{len(inventory.kernels)} kernel files, {len(inventory.defined)} functions, {len(inventory.suites)} "
          f"suites; {reached} files reached by a suite")
    if fallbacks:
        print(f"suites read as a whole file (a test method was not found): {', '.join(fallbacks)}")
    print(f"\nkernel files whose names appear in no test source ({len(unnamed)}):")
    for path in unnamed:
        print(f"  {path}")
    total = sum(len(names) for names in unreached.values())
    print(f"\nkernel functions no suite reaches ({total}):")
    for category, names in unreached.items():
        print(f"  {category} ({len(names)}): {', '.join(names)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
