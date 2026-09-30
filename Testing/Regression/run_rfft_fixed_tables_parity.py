#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build the host library and check fixed-length vs generic Q15/Q31 RFFT parity."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--compiler', default=os.environ.get('CC', 'cc'))
parser.add_argument('--master-guards', action='store_true',
                    help='Also require the two zero guard elements after the 8192-entry master tables')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
lib = build / 'lib'
subprocess.run(['cmake', '-S', str(repo), '-B', str(lib), '-DHOST=ON', '-DDISABLEFLOAT16=ON',
                '-DCMSISDSP_INSTALL=OFF', '-DCMAKE_BUILD_TYPE=Release'], check=True)
subprocess.run(['cmake', '--build', str(lib), '--parallel', '4'], check=True)
exe = build / 'rfft_fixed_tables_parity'
cmd = [args.compiler, '-O2', '-D__GNUC_PYTHON__', '-I' + str(repo / 'Include'), '-I' + str(repo / 'PrivateInclude')]
if args.master_guards:
    cmd.append('-DHAVE_MASTER_GUARDS')
cmd += [str(here / 'rfft_fixed_tables_parity.c'), str(lib / 'Source/libCMSISDSP.a'), '-lm', '-o', str(exe)]
subprocess.run(cmd, check=True)
result = subprocess.run([str(exe)], capture_output=True, text=True)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
if result.returncode or 'PASS: 36 fixed-point RFFT parity cases' not in log:
    raise SystemExit(1)
