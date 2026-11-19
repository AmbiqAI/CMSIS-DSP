"""Run a source generator and write a depfile naming every file it read.

usage: generate_suite.py --depfile FILE --output FILE -- <script.py> [args...]

processTests.py embeds the suite's pattern and parameter files in Patterns.h, and which files those are
depends on the suite, so the build learns them by recording what the generator opens for reading.
"""
import argparse
import builtins
import os
import runpy
import sys


def escape(path):
    return path.replace("\\", "\\\\").replace(" ", "\\ ").replace("#", "\\#").replace("$", "$$")


def run_recording_reads(script, args):
    """Run script as __main__ with args; return (exit status, sorted absolute paths it opened for reading)."""
    read = set()
    real_open = builtins.open

    def recording_open(file, mode="r", *rest, **kwargs):
        handle = real_open(file, mode, *rest, **kwargs)
        if isinstance(file, (str, bytes, os.PathLike)) and not set(mode) & set("wax+"):
            read.add(os.path.abspath(os.fsdecode(file)))
        return handle

    saved_argv, saved_path = sys.argv, list(sys.path)
    sys.argv = [script, *args]
    sys.path.insert(0, os.path.dirname(os.path.abspath(script)))
    builtins.open = recording_open
    try:
        runpy.run_path(script, run_name="__main__")
        status = 0
    except SystemExit as exc:
        status = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
    finally:
        builtins.open = real_open
        sys.argv, sys.path[:] = saved_argv, saved_path
    return status, sorted(read)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--depfile", required=True)
    parser.add_argument("--output", required=True, help="the first output of the build rule")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no generator script given")
    status, read = run_recording_reads(command[0], command[1:])
    if status != 0:
        print(f"generate_suite.py: {command[0]} exited with status {status}", file=sys.stderr)
        return status
    temporary = args.depfile + ".tmp"
    with open(temporary, "w") as depfile:
        depfile.write(escape(os.path.abspath(args.output)) + ":")
        for path in read:
            depfile.write(" \\\n  " + escape(path))
        depfile.write("\n")
    os.replace(temporary, args.depfile)
    return 0


if __name__ == "__main__":
    sys.exit(main())
