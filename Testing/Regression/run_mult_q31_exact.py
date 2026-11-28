#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check the scalar arm_mult_q31 bit-exactly against VQDMULH, with and without ARM_MATH_LOOPUNROLL."""
import argparse
import os
import random
import subprocess
from pathlib import Path

INT_MIN, INT_MAX = -2**31, 2**31 - 1
SIZES = (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 255)


def vqdmulh(a, b):
    return max(INT_MIN, min(INT_MAX, (2 * a * b) >> 32))


def cases(count):
    edges = [INT_MIN, INT_MIN + 1, -3, -1, 0, 1, 3, INT_MAX - 1, INT_MAX]
    pairs = [(INT_MIN, INT_MIN), (INT_MIN, INT_MAX), (INT_MAX, INT_MAX), (INT_MIN, -1), (-1, -1)]
    pairs += [(a, b) for a in edges for b in edges]
    rng = random.Random(7)
    while len(pairs) < count:
        pairs.append((rng.randint(INT_MIN, INT_MAX), rng.randint(INT_MIN, INT_MAX)))
    pairs = pairs[:count]
    expected = [vqdmulh(a, b) for a, b in pairs]
    # The old scalar path cleared the LSB: these must be among the first elements of every path.
    for want in ((lambda e: e > 0 and e % 2), (lambda e: e < 0 and e % 2), (lambda e: e == INT_MAX)):
        if not any(want(e) for e in expected[:4]):
            first = next(i for i, e in enumerate(expected) if want(e))
            pairs.insert(0, pairs.pop(first))
            expected.insert(0, expected.pop(first))
    return pairs, expected


def header(pairs, expected, build):
    def array(name, values):
        body = ",\n".join(f"    (q31_t){v}" if v != INT_MIN else "    (q31_t)0x80000000" for v in values)
        return f"static const q31_t {name}[{len(values)}] = {{\n{body}\n}};\n"
    return (f"#define MULT_Q31_COUNT {len(pairs)}u\n#define MULT_Q31_BUILD \"{build}\"\n"
            f"static const uint32_t MULT_Q31_SIZES[] = {{{', '.join(map(str, SIZES))}}};\n"
            + array("MULT_Q31_A", [a for a, _ in pairs]) + array("MULT_Q31_B", [b for _, b in pairs])
            + array("MULT_Q31_EXPECTED", expected))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--compiler", default=os.environ.get("CC", "cc"))
    parser.add_argument("--mult-source", type=Path,
                        help="arm_mult_q31.c to test instead of the checkout's (the negative control)")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    here = Path(__file__).resolve().parent
    source = (args.mult_source or repo / "Source/BasicMathFunctions/arm_mult_q31.c").resolve()
    pairs, expected = cases(max(SIZES))
    failed = False
    for build, defines in (("loop unrolling", ["-DARM_MATH_LOOPUNROLL"]), ("no loop unrolling", [])):
        out = args.build_dir.resolve() / build.replace(" ", "_")
        out.mkdir(parents=True, exist_ok=True)
        (out / "mult_q31_cases.h").write_text(header(pairs, expected, build))
        exe = out / "mult_q31_exact"
        subprocess.run([args.compiler, "-O2", "-std=c99", "-Wall", "-Werror", "-D__GNUC_PYTHON__", *defines,
                        "-I" + str(out), "-I" + str(repo / "Include"), "-I" + str(repo / "PrivateInclude"),
                        str(here / "mult_q31_exact.c"), str(source), "-o", str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        print(result.stdout + result.stderr, end="")
        if result.returncode or f"PASS: {len(SIZES)} arm_mult_q31 sizes bit-exact ({build})" not in result.stdout:
            failed = True
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
