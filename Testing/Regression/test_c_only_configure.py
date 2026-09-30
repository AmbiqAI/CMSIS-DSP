#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Verify both CMSIS-DSP CMake entry points work without a C++ compiler."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-dsp', type=Path, default=Path(__file__).resolve().parents[2],
                    help='CMSIS-DSP checkout to configure (default: this repository)')
parser.add_argument('--expect-failure', action='store_true',
                    help='Negative control: pass only if every configuration fails')
args = parser.parse_args()
failures = 0
repo = args.cmsis_dsp.resolve()
with tempfile.TemporaryDirectory(prefix='cmsis-c-only-') as tmp:
    root = Path(tmp)
    for entry in [repo, repo / 'Source']:
        name = 'root' if entry == repo else 'Source'
        consumer = root / ('consumer-' + name)
        consumer.mkdir()
        # Forward-slash CMake paths work on Unix and Windows.
        quoted = entry.as_posix().replace('"', '\\"')
        (consumer / 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.14)\n'
            'project(consumer LANGUAGES C)\n'
            f'add_subdirectory("{quoted}" cmsisdsp)\n')
        for mode, source in [('standalone', entry), ('subdirectory', consumer)]:
            build = root / (name + '-' + mode)
            result = subprocess.run(
                ['cmake', '-S', str(source), '-B', str(build), '-DHOST=ON',
                 '-DDISABLEFLOAT16=ON', '-DCMSISDSP_INSTALL=OFF',
                 '-DCMAKE_CXX_COMPILER=' + str(root / 'no-cxx-compiler')],
                capture_output=True, text=True)
            if result.returncode:
                failures += 1
                if not args.expect_failure:
                    print(result.stdout + result.stderr)
                    raise SystemExit(result.returncode)
                print(f'EXPECTED FAILURE: {name} entry point, {mode}, unavailable C++ compiler')
            elif args.expect_failure:
                print(f'FAIL: {name} entry point, {mode} configured without a C++ compiler')
                raise SystemExit(1)
            else:
                print(f'PASS: {name} entry point, {mode}, unavailable C++ compiler')
if args.expect_failure:
    print(f'PASS: negative control, {failures} configurations failed as expected')
