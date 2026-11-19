"""Run one test-suite executable and decide pass or fail from its output.

usage: suite_status.py [--timeout SECONDS] -- <command> [args...]

The framework always exits 0 (testmain.cpp), so the verdict comes from its status lines (FPGA.cpp):
"S: <id> 0 0 <cycles> Y" for a passed test, "S: <id> <error> <line> 0 N" for a failed one, "S: g|s <id>"
opening a group or suite and "S: p" closing it.
"""
import argparse
import re
import subprocess
import sys

PASSED = re.compile(r"^S: (\d+) 0 0 \S+ Y$")
FAILED = re.compile(r"^S: (\d+) (\d+) (\d+) 0 N$")
OPEN = re.compile(r"^S: [gs] \d+$")
CLOSE = "S: p"
FATAL = ("Exception", "NOT ENOUGH MEMORY")


def verdict(output, returncode):
    """Return (passed, failed, problems) for one run; the run passes only if problems is empty."""
    passed = failed = depth = opened = 0
    problems = []
    for raw in output.splitlines():
        line = raw.strip()
        if PASSED.match(line):
            passed += 1
        elif m := FAILED.match(line):
            failed += 1
            problems.append(f"test {m.group(1)} failed: error {m.group(2)} at line {m.group(3)}")
        elif line.startswith("E:"):
            if detail := line[2:].strip():
                problems.append(f"error detail: {detail}")
        elif OPEN.match(line):
            depth += 1
            opened += 1
        elif line == CLOSE:
            depth -= 1
            if depth < 0:
                problems.append("group closed that was never opened")
                depth = 0
        elif line in FATAL:
            problems.append(f"framework reported: {line}")
    if returncode != 0:
        problems.append(f"exit status {returncode}")
    if opened == 0:
        problems.append("no group or suite was run")
    elif depth != 0:
        problems.append(f"output ends inside {depth} open group(s): the run stopped early")
    if passed + failed == 0:
        problems.append("no test reported a status")
    return passed, failed, problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    if not command:
        parser.error("no command given")
    try:
        run = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=args.timeout)
    except subprocess.TimeoutExpired as exc:
        partial = exc.stdout or ""
        sys.stdout.write(partial.decode(errors="replace") if isinstance(partial, bytes) else partial)
        print(f"FAIL: timed out after {args.timeout:g} s")
        return 1
    except OSError as exc:
        print(f"FAIL: cannot run {command[0]}: {exc}")
        return 1
    sys.stdout.write(run.stdout)
    sys.stderr.write(run.stderr)
    passed, failed, problems = verdict(run.stdout, run.returncode)
    for problem in problems:
        print(f"FAIL: {problem}")
    print(f"{'PASS' if not problems else 'FAIL'}: {passed} passed, {failed} failed")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
