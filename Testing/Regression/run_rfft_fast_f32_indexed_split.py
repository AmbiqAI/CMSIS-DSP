#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build the host library with RFFTFASTINDEXEDSPLIT and compare arm_rfft_fast_f32 with the default path."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--compiler', default=os.environ.get('CC', 'cc'))
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
lib = build / 'lib'
subprocess.run(['cmake', '-S', str(repo), '-B', str(lib), '-DHOST=ON', '-DDISABLEFLOAT16=ON',
                '-DRFFTFASTINDEXEDSPLIT=ON', '-DCMSISDSP_INSTALL=OFF', '-DCMAKE_BUILD_TYPE=Release'], check=True)
subprocess.run(['cmake', '--build', str(lib), '--parallel', '4'], check=True)
common = [args.compiler, '-O2', '-D__GNUC_PYTHON__', '-I' + str(repo / 'Include'), '-I' + str(repo / 'PrivateInclude')]
# The same source without the macro is the reference path, under another name.
reference = build / 'arm_rfft_fast_f32_reference.o'
subprocess.run(common + ['-DARM_MATH_LOOPUNROLL', '-Darm_rfft_fast_f32=arm_rfft_fast_f32_reference', '-c',
                         str(repo / 'Source/TransformFunctions/arm_rfft_fast_f32.c'), '-o', str(reference)],
               check=True)
exe = build / 'rfft_fast_f32_indexed_split'
subprocess.run(common + [str(here / 'rfft_fast_f32_indexed_split.c'), str(reference),
                         str(lib / 'Source/libCMSISDSP.a'), '-lm', '-o', str(exe)], check=True)
result = subprocess.run([str(exe)], capture_output=True, text=True)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
if result.returncode or 'PASS: 8 indexed split RFFT lengths' not in log:
    raise SystemExit(1)
