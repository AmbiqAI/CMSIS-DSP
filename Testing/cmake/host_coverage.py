"""Coverage data of a host test build: clear the counters before a run, export them after it.

usage: host_coverage.py reset --build-dir DIR
       host_coverage.py report --build-dir DIR --root DIR --gcov COMMAND --output FILE --summary FILE

The report covers Source/ (except CommonTables, which only holds data) and the inline functions in
Include/dsp/, and is written as gcovr JSON so the reports of several variants can be merged later.
"""
import argparse
import os
import re
import subprocess
import sys


def counter_files(build_dir):
    for directory, _, names in os.walk(build_dir):
        for name in names:
            if name.endswith(".gcda"):
                yield os.path.join(directory, name)


def reset(build_dir):
    if not os.path.isdir(build_dir):
        print(f"host_coverage.py: no build directory {build_dir}", file=sys.stderr)
        return 1
    removed = 0
    for path in counter_files(build_dir):
        os.remove(path)
        removed += 1
    print(f"host_coverage.py: removed {removed} counter file(s)")
    return 0


def gcovr_command(build_dir, root, gcov, output, summary):
    root = os.path.realpath(root)

    def under(*parts):
        return re.escape(os.path.join(root, *parts) + os.sep)

    return [sys.executable, "-m", "gcovr",
            "--root", root,
            "--object-directory", os.path.realpath(build_dir),
            "--gcov-executable", gcov,
            "--filter", under("Source"),
            "--filter", under("Include", "dsp"),
            "--exclude", under("Source", "CommonTables"),
            "--json", output,
            "--json-summary", summary,
            "--print-summary"]


def report(build_dir, root, gcov, output, summary):
    if not any(True for _ in counter_files(build_dir)):
        print(f"host_coverage.py: no counter files under {build_dir}: run the tests of a coverage build first",
              file=sys.stderr)
        return 1
    for path in (output, summary):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        if os.path.exists(path):
            os.remove(path)
    status = subprocess.run(gcovr_command(build_dir, root, gcov, output, summary)).returncode
    if status != 0:
        print(f"host_coverage.py: gcovr exited with status {status}", file=sys.stderr)
        return status
    if not (os.path.isfile(output) and os.path.isfile(summary)):
        print("host_coverage.py: gcovr wrote no report", file=sys.stderr)
        return 1
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    reset_parser = commands.add_parser("reset")
    reset_parser.add_argument("--build-dir", required=True)
    report_parser = commands.add_parser("report")
    for option in ("--build-dir", "--root", "--gcov", "--output", "--summary"):
        report_parser.add_argument(option, required=True)
    args = parser.parse_args(argv)
    if args.command == "reset":
        return reset(args.build_dir)
    return report(args.build_dir, args.root, args.gcov, args.output, args.summary)


if __name__ == "__main__":
    sys.exit(main())
