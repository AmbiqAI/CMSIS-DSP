"""Choose the suites a pull request's CI runs.

usage: ci_plan.py --root DIR --diff-base REF [--suites-file FILE] [--output FILE]

The host variants run every suite in the suites file (Testing/cmake/ci_suites.txt: one suite class per
line, # starts a comment; by default the one under --root). The target variants run those of them that
inventory.py maps the paths changed since the merge base with REF to. A path that was deleted or renamed
away counts too: as it is no longer in the tree, it maps to every suite.

The result is written as host_suites=... and target_suites=... lines (each a ;-separated CMake list,
target_suites empty when no target needs to run), to --output (such as $GITHUB_OUTPUT) or to stdout.
"""
import argparse
import os
import subprocess
import sys

from inventory import Inventory, InventoryError


def read_suites(path, inventory):
    try:
        with open(path) as source:
            lines = source.read().splitlines()
    except OSError as exc:
        raise InventoryError(f"{path}: cannot read: {exc.strerror}") from None
    suites = []
    for number, line in enumerate(lines, 1):
        name = line.split("#", 1)[0].strip()
        if not name:
            continue
        if name not in inventory.suites:
            raise InventoryError(f"{path}:{number}: {name} is not a suite with a test source")
        if name in suites:
            raise InventoryError(f"{path}:{number}: {name} is listed twice")
        suites.append(name)
    if not suites:
        raise InventoryError(f"{path}: lists no suite")
    return suites


def changed_paths(root, base):
    command = ["git", "-C", root, "diff", "--name-only", "--no-renames", "-z", f"{base}...HEAD"]
    try:
        result = subprocess.run(command, capture_output=True)
    except OSError as exc:
        raise InventoryError(f"cannot run git: {exc.strerror}") from None
    if result.returncode != 0:
        raise InventoryError(f"git diff against {base} failed: {result.stderr.decode(errors='replace').strip()}")
    return sorted(path for path in result.stdout.decode().split("\0") if path)


def plan(inventory, enabled, paths):
    """(target suites, {path: suites it maps to}) for the changed paths."""
    mapped = {}
    for path in paths:
        mapped[path] = inventory.suites_for(path)[0]
    wanted = set().union(*mapped.values()) if mapped else set()
    return [name for name in enabled if name in wanted], mapped


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", required=True)
    parser.add_argument("--diff-base", required=True)
    parser.add_argument("--suites-file", help="default: <root>/Testing/cmake/ci_suites.txt")
    parser.add_argument("--output")
    args = parser.parse_args(argv)

    try:
        inventory = Inventory(args.root)
        enabled = read_suites(args.suites_file or os.path.join(args.root, "Testing", "cmake", "ci_suites.txt"),
                              inventory)
        paths = changed_paths(args.root, args.diff_base)
        targets, mapped = plan(inventory, enabled, paths)
    except InventoryError as exc:
        print(f"ci_plan.py: {exc}", file=sys.stderr)
        return 1

    print(f"{len(paths)} changed path(s) since the merge base with {args.diff_base}", file=sys.stderr)
    for path, suites in mapped.items():
        shown = [name for name in enabled if name in suites]
        print(f"  {path}: {', '.join(shown) if shown else 'no enabled suite'}", file=sys.stderr)
    print(f"host suites: {', '.join(enabled)}", file=sys.stderr)
    print(f"target suites: {', '.join(targets) if targets else 'none'}", file=sys.stderr)
    text = f"host_suites={';'.join(enabled)}\ntarget_suites={';'.join(targets)}\n"
    if args.output:
        try:
            with open(args.output, "a") as out:
                out.write(text)
        except OSError as exc:
            print(f"ci_plan.py: {args.output}: cannot write: {exc.strerror}", file=sys.stderr)
            return 1
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
