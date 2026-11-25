"""Export CMSIS-DSP's public function signatures as hct kernel contracts (hct/kernel-contracts/1).

usage: export_contracts.py export [--doxygen PATH] [--output FILE]
       export_contracts.py check  [--doxygen PATH] [--output FILE]

`export` writes Testing/hct/kernel_contracts.json; `check` regenerates it and fails when the
committed file differs, so a header change that is not re-exported fails CI.

Where each part of a record comes from:

- The functions, their header, line and `#if` guards come from a scan of the public headers
  (Include/dsp/*_functions.h and *_functions_f16.h). Doxygen does not keep those: with
  preprocessing on it records the definition in Source/, and it drops the conditions.
- Return and parameter types and each parameter's direction come from Doxygen's XML over
  Include/dsp and Source/, which merges the @param[in|out] blocks of the header and the source
  file (transforms document theirs only in Source/).

The export is the Cortex-M build: declarations that exist only for NEON, or only for the
non-default MFCC and mixed-radix FFT configurations, are left out (CONFIGURATION). Every other
condition a declaration sits under is kept as a guard, so Helium-only functions carry
`defined(ARM_MATH_MVEF) && !defined(ARM_MATH_AUTOVECTORIZE)`.

Fail closed: Doxygen of any version other than DOXYGEN_VERSION, a header function Doxygen did
not see, parameters that differ between the header and Doxygen, conflicting directions, or a
pointer parameter without a direction (and no entry in contract_overrides.json) stop the export.
"""
import argparse
import bisect
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

DOXYGEN_VERSION = "1.17.0"
SCHEMA = "hct/kernel-contracts/1"
LIBRARY = "cmsis-dsp"
HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.dirname(os.path.dirname(HERE))
DEFAULT_OUTPUT = os.path.join(HERE, "kernel_contracts.json")
OVERRIDES = os.path.join(HERE, "contract_overrides.json")
HEADER_PATTERNS = ("Include/dsp/*_functions.h", "Include/dsp/*_functions_f16.h")
# Macros whose value the export fixes: the Cortex-M build with the default MFCC and FFT choices.
CONFIGURATION = {
    "ARM_MATH_NEON": False,
    "ARM_MATH_NEON_FLOAT16": False,
    "ARM_MATH_NEON_EXPERIMENTAL": False,
    "ARM_MFCC_USE_CFFT": False,
    "ARM_MFCC_CFFT_BASED": False,
    "ARM_MIXED_RADIX_FFT": False,
    "DOXYGEN": False,
    "__cplusplus": False,  # the C API; the extern "C" wrapper is not part of it
}
# What Doxygen's preprocessor is told, so every Cortex-M declaration and definition parses.
PREDEFINED = ("ARM_DSP_ATTRIBUTE= ARM_DSP_TABLE_ATTRIBUTE= HAS_VECTOR HAS_PREDICATED_LOOP ARM_FLOAT16_SUPPORTED=1 "
              "__STATIC_FORCEINLINE= __STATIC_INLINE= \"__ALIGNED(x)=\" \"__attribute__(x)=\" ARM_MATH_MVEF=1 "
              "ARM_MATH_MVEI=1 ARM_MATH_HELIUM=1 ARM_MATH_DSP=1 ARM_MATH_MVE_FLOAT16=1")
DIRECTIONS = {"in": "in", "out": "out", "inout": "in,out"}

_COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
_DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)$")
_DEFINE = re.compile(r"^\s*#")
_DEFINED = re.compile(r"\bdefined\s*\(\s*(\w+)\s*\)|\bdefined\s+(\w+)")
_IDENT = re.compile(r"\b[A-Za-z_]\w*\b")
_DECL = re.compile(r"\b(arm_\w+)\s*\(")


class ExportError(Exception):
    pass


def _strip_comments(text):
    """The text with comments blanked, keeping every newline so line numbers hold."""
    return _COMMENT.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), text)


def fixed_value(condition):
    """True or False when the condition names only CONFIGURATION macros, else None (a guard)."""
    names = {a or b for a, b in _DEFINED.findall(condition)}
    rest = _IDENT.findall(_DEFINED.sub(" ", condition))
    if not names or rest or not names <= set(CONFIGURATION):
        return None
    expression = _DEFINED.sub(lambda m: " %s " % CONFIGURATION[m.group(1) or m.group(2)], condition)
    expression = expression.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    if not re.fullmatch(r"[\s()]*(?:(?:True|False|and|or|not)[\s()]*)+", expression):
        raise ExportError("cannot evaluate #if %r" % condition)
    return bool(eval(expression, {"__builtins__": {}}))  # noqa: S307 - only True/False/and/or/not


class _Level:
    """One #if level. `active`: the current branch is compiled in the exported build. `guard`:
    the condition the branch adds to a declaration's guards (None when it adds none)."""

    def __init__(self, condition, header_guard):
        self.header_guard = header_guard
        self.decided = False  # an earlier branch was fixed-true
        self.previous = []  # guards of earlier unfixed branches
        self._enter(condition)

    def _enter(self, condition):
        condition = " ".join(condition.split())
        if self.header_guard:
            self.active, self.guard = True, None
            return
        value = fixed_value(condition)
        if self.decided:
            self.active, self.guard = False, None
        elif value is None:
            self.active = True
            self.guard = " && ".join(["!(%s)" % g for g in self.previous] + [condition])
            self.previous.append(condition)
        else:
            self.active = value
            self.guard = " && ".join("!(%s)" % g for g in self.previous) or None
            self.decided = value

    def elif_(self, condition):
        self._enter(condition)

    def else_(self):
        if self.header_guard:
            return
        if self.decided:
            self.active, self.guard = False, None
        else:
            self.active = True
            self.guard = " && ".join("!(%s)" % g for g in self.previous) or None


def _conditions(lines, relpath):
    """Per line: (compiled in the exported build, guards it sits under)."""
    stack, state, seen_directive = [], [], False
    for number, line in enumerate(lines, 1):
        match = _DIRECTIVE.match(line)
        if match:
            kind, rest = match.group(1), match.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                if kind != "if":
                    if not rest:
                        raise ExportError("%s:%d: #%s without a name" % (relpath, number, kind))
                    rest = ("defined(%s)" if kind == "ifdef" else "!defined(%s)") % rest.split()[0]
                stack.append(_Level(rest, header_guard=not seen_directive and kind == "ifndef"))
            elif not stack:
                raise ExportError("%s:%d: #%s without #if" % (relpath, number, kind))
            elif kind == "elif":
                stack[-1].elif_(rest)
            elif kind == "else":
                stack[-1].else_()
            else:
                stack.pop()
            seen_directive = True
            state.append((False, ()))
            continue
        active = all(level.active for level in stack)
        state.append((active, tuple(level.guard for level in stack if level.guard)))
    if stack:
        raise ExportError("%s: %d #if level(s) not closed" % (relpath, len(stack)))
    return state


def _split_params(text):
    depth, current, params = 0, [], []
    for char in text:
        if char == "," and depth == 0:
            params.append("".join(current))
            current = []
            continue
        depth += char in "(["
        depth -= char in ")]"
        current.append(char)
    params.append("".join(current))
    params = [" ".join(p.split()) for p in params]
    return [] if params in ([""], ["void"]) else params


def scan_header(path, relpath):
    """{name: (line, guards, [parameter text])} for each top-level declaration of an arm_ function
    the exported build compiles."""
    try:
        with open(path, encoding="utf-8") as handle:
            text = handle.read()
    except (OSError, UnicodeDecodeError) as exc:
        raise ExportError("%s: cannot read (%s)" % (relpath, exc)) from None
    lines = _strip_comments(text).split("\n")
    state = _conditions(lines, relpath)
    live = "\n".join(line if active and not _DEFINE.match(line) else "" for line, (active, _) in zip(lines, state))
    starts = [m.start() for m in re.finditer(r"^", live, re.M)]
    found = {}
    depth = 0
    position = 0
    for match in _DECL.finditer(live):
        depth += live.count("{", position, match.start()) - live.count("}", position, match.start())
        position = match.start()
        if depth != 0:
            continue
        before = live[:match.start()].rstrip()
        if not before or before[-1] in "=,(":
            continue
        close, level = match.end(), 1
        while level:
            if close >= len(live):
                raise ExportError("%s: unbalanced parentheses after %s" % (relpath, match.group(1)))
            level += {"(": 1, ")": -1}.get(live[close], 0)
            close += 1
        tail = live[close:].lstrip()[:1]
        if tail not in (";", "{"):
            continue
        name = match.group(1)
        line = bisect.bisect_right(starts, match.start())
        if name in found:
            raise ExportError("%s:%d: %s is declared twice in the exported build (first at line %d)"
                              % (relpath, line, name, found[name][0]))
        found[name] = (line, list(state[line - 1][1]), _split_params(live[match.end():close - 1]))
    return found


def doxygen_version(doxygen):
    try:
        out = subprocess.run([doxygen, "--version"], check=True, capture_output=True, text=True).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise ExportError("cannot run %s --version (%s)" % (doxygen, error))
    return out.split()[0] if out.split() else ""


def run_doxygen(doxygen, root, output_dir):
    version = doxygen_version(doxygen)
    if version != DOXYGEN_VERSION:
        raise ExportError("%s is Doxygen %s; the export is pinned to %s" % (doxygen, version, DOXYGEN_VERSION))
    doxyfile = os.path.join(output_dir, "Doxyfile")
    settings = {
        "OUTPUT_DIRECTORY": output_dir, "GENERATE_HTML": "NO", "GENERATE_LATEX": "NO", "GENERATE_XML": "YES",
        "XML_PROGRAMLISTING": "NO", "INPUT": "Include/dsp Source", "FILE_PATTERNS": "*_functions.h *_functions_f16.h *.c",
        "EXCLUDE": "Source/DistanceFunctions/arm_boolean_distance.c Source/MatrixFunctions/_arm_mat_mult_neon_buffers.c",
        "RECURSIVE": "YES", "EXTRACT_ALL": "YES", "EXTRACT_STATIC": "YES", "ENABLE_PREPROCESSING": "YES",
        "MACRO_EXPANSION": "YES", "EXPAND_ONLY_PREDEF": "NO", "SEARCH_INCLUDES": "NO", "PREDEFINED": PREDEFINED,
        "QUIET": "YES", "WARNINGS": "NO", "WARN_IF_UNDOCUMENTED": "NO", "WARN_IF_DOC_ERROR": "NO",
        "STRIP_FROM_PATH": root, "FULL_PATH_NAMES": "YES",
    }
    with open(doxyfile, "w", encoding="utf-8") as handle:
        handle.writelines("%s = %s\n" % item for item in settings.items())
    result = subprocess.run([doxygen, doxyfile], cwd=root, capture_output=True, text=True)
    if result.returncode != 0:
        raise ExportError("doxygen failed (%d): %s" % (result.returncode, result.stderr.strip()[-2000:]))
    return os.path.join(output_dir, "xml")


def _text(node):
    return " ".join("".join(node.itertext()).split()) if node is not None else ""


def read_doxygen(xml_dir):
    """{name: [record]} for each function memberdef; a record is (type, [(name, type, extent)],
    {param: {direction}})."""
    functions = {}
    for path in sorted(glob.glob(os.path.join(xml_dir, "*.xml"))):
        if os.path.basename(path) == "index.xml":
            continue
        for member in ET.parse(path).getroot().iter("memberdef"):
            if member.get("kind") != "function":
                continue
            params = []
            for param in member.findall("param"):
                params.append((param.findtext("declname") or "", _text(param.find("type")),
                               _text(param.find("array"))))
            directions = {}
            for item in member.iter("parameteritem"):
                for name in item.iter("parametername"):
                    if name.get("direction"):
                        # Some blocks document `*pBuffer`; the parameter is `pBuffer`.
                        directions.setdefault(_text(name).lstrip("*").strip(), set()).add(name.get("direction"))
            functions.setdefault(member.findtext("name"), []).append(
                (_text(member.find("type")), params, directions, member.get("id")))
    return functions


def load_overrides(path=OVERRIDES):
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as handle:
        document = json.load(handle)
    overrides = {}
    for key, entry in document.get("directions", {}).items():
        if not isinstance(entry, dict) or entry.get("direction") not in DIRECTIONS.values() \
                or not str(entry.get("reason", "")).strip():
            raise ExportError("%s: directions[%r] needs a direction in %s and a reason"
                              % (path, key, sorted(DIRECTIONS.values())))
        overrides[key] = entry["direction"]
    return overrides


def _param_name(text):
    match = re.search(r"(\w+)\s*(\[[^\]]*\])*\s*$", text)
    return match.group(1) if match else ""


def build(root, xml_dir, overrides):
    """The export document."""
    doxygen = read_doxygen(xml_dir)
    records, problems, used = [], [], set()
    for pattern in HEADER_PATTERNS:
        for path in sorted(glob.glob(os.path.join(root, pattern))):
            relpath = os.path.relpath(path, root).replace(os.sep, "/")
            for name, (line, guards, header_params) in scan_header(path, relpath).items():
                candidates = doxygen.get(name)
                if not candidates:
                    problems.append("%s:%d: %s is not in Doxygen's output" % (relpath, line, name))
                    continue
                shapes = {(c[0], tuple((t, a) for _, t, a in c[1])) for c in candidates}
                if len(shapes) != 1:
                    problems.append("%s: Doxygen has %d different signatures for it" % (name, len(shapes)))
                    continue
                returns, params, _, _ = candidates[0]
                names = [_param_name(text) for text in header_params]
                if len(names) != len(params):
                    problems.append("%s:%d: %s has %d parameters in the header and %d in Doxygen"
                                    % (relpath, line, name, len(names), len(params)))
                    continue
                directions = {}
                for candidate in candidates:
                    for param, found in candidate[2].items():
                        directions.setdefault(param, set()).update(found)
                exported = []
                for header_name, (doxygen_name, c_type, extent) in zip(names, params):
                    key = "%s.%s" % (name, header_name)
                    found = directions.get(header_name, set()) | directions.get(doxygen_name, set())
                    if key in overrides:
                        direction = overrides[key]
                        used.add(key)
                    elif len(found) == 1:
                        direction = DIRECTIONS[found.pop()]
                    elif len(found) > 1:
                        problems.append("%s: directions %s conflict" % (key, sorted(found)))
                        continue
                    elif "*" in c_type or extent:
                        problems.append("%s: pointer parameter without a direction" % key)
                        continue
                    else:
                        direction = "in"
                    record = {"direction": direction, "name": header_name, "type": c_type}
                    if extent:
                        record["extent"] = extent
                    exported.append(record)
                records.append({"name": name, "header": relpath, "line": line, "guards": guards,
                                "returns": returns, "params": exported})
    stale = sorted(set(overrides) - used)
    if stale:
        problems.append("contract_overrides.json names parameters the export does not have: %s" % ", ".join(stale))
    if problems:
        raise ExportError("%d problem(s):\n  " % len(problems) + "\n  ".join(problems))
    seen = set()
    for record in records:
        if record["name"] in seen:
            raise ExportError("%s is declared in two public headers" % record["name"])
        seen.add(record["name"])
    return {"schema": SCHEMA, "library": LIBRARY, "functions": records}


def render(document):
    return json.dumps(document, indent=2) + "\n"


def generate(root, doxygen):
    with tempfile.TemporaryDirectory() as work:
        return render(build(root, run_doxygen(doxygen, root, work), load_overrides()))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("command", choices=("export", "check"))
    parser.add_argument("--root", default=DEFAULT_ROOT, help="CMSIS-DSP checkout (default: this one)")
    parser.add_argument("--doxygen", default=shutil.which("doxygen") or "doxygen")
    parser.add_argument("--output", default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        text = generate(os.path.abspath(args.root), args.doxygen)
    except ExportError as error:
        print("export_contracts: %s" % error, file=sys.stderr)
        return 2
    count = text.count('"header":')
    if args.command == "export":
        with open(args.output, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(text)
        print("wrote %s: %d functions" % (args.output, count))
        return 0
    try:
        with open(args.output, encoding="utf-8") as handle:
            committed = handle.read()
    except OSError as error:
        print("export_contracts: cannot read %s (%s)" % (args.output, error), file=sys.stderr)
        return 1
    if committed != text:
        print("export_contracts: %s is stale; run python Testing/hct/export_contracts.py export with Doxygen %s"
              % (args.output, DOXYGEN_VERSION), file=sys.stderr)
        return 1
    print("%s is current: %d functions" % (args.output, count))
    return 0


if __name__ == "__main__":
    sys.exit(main())
