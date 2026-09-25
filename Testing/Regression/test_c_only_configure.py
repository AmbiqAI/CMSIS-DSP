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
                    help='Optional unpatched checkout for negative control')
args = parser.parse_args()
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
                print(result.stdout + result.stderr)
                raise SystemExit(result.returncode)
            print(f'PASS: {name} entry point, {mode}, unavailable C++ compiler')
