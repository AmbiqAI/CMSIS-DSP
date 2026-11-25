"""Merge the per-variant coverage reports into one report: gcovr JSON, a JSON summary and HTML.

usage: merge.py --root DIR --output-dir DIR REPORT...

Each REPORT is the coverage-<variant>.json that a coverage build's ctest writes (host_coverage.py report),
for the host and the target variants alike. The merged report is written to the output directory as
coverage.json, summary.json and html/index.html; earlier ones there are removed first, so a failed merge
never leaves an old report behind.

- Lines merge by number: a line counts as executed when any variant ran it, and as executable when any
  variant compiled it.
- Functions merge in gcovr's "separate" mode. A function whose definition starts on different lines in two
  variants (an MVE body and a scalar body, for example) stays two functions, each of which must run in a
  variant that compiles it.
- Each variant may appear once, and every report must hold only the scope of the gate: Source/ without
  CommonTables, plus Include/dsp/.
- gcovr 8.4 cannot merge, in this mode, reports whose functions carry start and end positions (GCC 14
  and later) with reports whose functions do not (llvm-cov, and GCC 13 and earlier), so all reports must be
  of one kind.
"""
import argparse
import json
import os
import posixpath
import re
import shutil
import subprocess
import sys

REPORT_NAME = re.compile(r"^coverage-([A-Za-z0-9_]+)\.json$")
OUTPUTS = ("coverage.json", "summary.json", "html")


class ReportError(Exception):
    pass


def in_scope(path):
    # gcovr writes paths relative to --root in normal form; any other spelling could hide its real place.
    if not isinstance(path, str) or "\\" in path or posixpath.normpath(path) != path:
        return False
    return (path.startswith("Source/") and not path.startswith("Source/CommonTables/")) or \
        path.startswith("Include/dsp/")


def read_report(path):
    """Return (variant, producer) of one per-variant report, or raise ReportError."""
    match = REPORT_NAME.match(os.path.basename(path))
    if not match:
        raise ReportError(f"{path}: not a coverage-<variant>.json report")
    try:
        with open(path) as report:
            data = json.load(report)
    except OSError as exc:
        raise ReportError(f"{path}: cannot read: {exc.strerror}") from None
    except ValueError as exc:
        raise ReportError(f"{path}: not JSON: {exc}") from None
    if not isinstance(data, dict) or "gcovr/format_version" not in data or not isinstance(data.get("files"), list):
        raise ReportError(f"{path}: not a gcovr JSON report")
    if not data["files"]:
        raise ReportError(f"{path}: the report covers no file")
    outside = [entry.get("file") for entry in data["files"] if not in_scope(entry.get("file"))]
    if outside:
        raise ReportError(f"{path}: {len(outside)} file(s) outside the scope, first {outside[0]}")
    # gcov from GCC 14 and later gives each function its start and end ("pos"); llvm-cov and older GCC do not.
    kinds = {"pos" in function for entry in data["files"] for function in entry.get("functions", [])}
    if not kinds:
        raise ReportError(f"{path}: the report lists no function")
    if len(kinds) > 1:
        raise ReportError(f"{path}: mixes functions with and without positions")
    return match.group(1), POSITIONED if kinds == {True} else UNPOSITIONED


POSITIONED = "with positions (GCC 14 or later)"
UNPOSITIONED = "without positions (llvm-cov, or GCC 13 or earlier)"


def gcovr_command(root, reports, output_dir):
    command = [sys.executable, "-m", "gcovr", "--root", os.path.realpath(root),
               "--merge-mode-functions", "separate"]
    for report in reports:
        command += ["--add-tracefile", report]
    return command + ["--json", os.path.join(output_dir, "coverage.json"),
                      "--json-summary", os.path.join(output_dir, "summary.json"),
                      "--html-details", os.path.join(output_dir, "html", "index.html"),
                      "--print-summary"]


def remove_outputs(output_dir):
    for name in OUTPUTS:
        path = os.path.join(output_dir, name)
        if os.path.isdir(path):
            shutil.rmtree(path)
        elif os.path.exists(path):
            os.remove(path)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", required=True, help="the CMSIS-DSP checkout the reports were made from")
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("reports", nargs="+")
    args = parser.parse_args(argv)

    remove_outputs(args.output_dir)
    os.makedirs(os.path.join(args.output_dir, "html"))

    variants = {}
    producers = {}
    errors = []
    for report in args.reports:
        try:
            variant, producer = read_report(report)
        except ReportError as exc:
            errors.append(str(exc))
            continue
        if variant in variants:
            errors.append(f"{report}: variant {variant} is also in {variants[variant]}")
        variants.setdefault(variant, report)
        producers.setdefault(producer, report)
    if len(producers) > 1:
        errors.append("reports with and without function positions cannot be merged: " +
                      ", ".join(f"{producer} ({report})" for producer, report in sorted(producers.items())))
    if errors:
        for error in errors:
            print(f"merge.py: {error}", file=sys.stderr)
        return 1

    status = subprocess.run(gcovr_command(args.root, args.reports, args.output_dir)).returncode
    if status != 0:
        remove_outputs(args.output_dir)
        print(f"merge.py: gcovr exited with status {status}", file=sys.stderr)
        return 1
    missing = [name for name in ("coverage.json", "summary.json", os.path.join("html", "index.html"))
               if not os.path.isfile(os.path.join(args.output_dir, name))]
    if missing:
        remove_outputs(args.output_dir)
        print(f"merge.py: gcovr wrote no {', '.join(missing)}", file=sys.stderr)
        return 1
    print(f"merge.py: merged variants {', '.join(sorted(variants))} into {args.output_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
