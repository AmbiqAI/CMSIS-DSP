#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE biquad df1 bounded-access regression on Corstone-300 (M55)."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--cortex-dfp', type=Path, required=True)
parser.add_argument('--platform-linker', type=Path, required=True)
parser.add_argument('--fvp-root', type=Path, required=True, help='Corstone-300 model package root')
parser.add_argument('--compiler', default='arm-none-eabi-gcc')
parser.add_argument('--opt', choices=['O2', 'O3'], default='O3')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--biquad-source-dir', type=Path,
                    help='Optional directory with other arm_biquad_cascade_df1_q15.c, arm_biquad_cascade_df1_q31.c and arm_biquad_cascade_df1_32x64_q31.c for the negative control')
CONTROLS = {
    'sparse-read-32': '-DREGRESSION_SPARSE_READ_BYTES=32',
    'sparse-read-far': '-DREGRESSION_SPARSE_READ_FAR',
}
parser.add_argument('--control', choices=sorted(CONTROLS),
                    help='Build a negative control instead; the run then succeeds only if the test fails')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
sources = args.biquad_source_dir or repo / 'Source/FilteringFunctions'
elf = build / 'mve_biquad_df1_io_bounds.elf'
cmd = [args.compiler, '-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard',
       '-' + args.opt, '-ffast-math', '-flax-vector-conversions', '-ffunction-sections',
       '-fdata-sections', '-DARMCM55', '-DDISABLEFLOAT16']
if args.control:
    cmd += [CONTROLS[args.control]]
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cmd += ['-I' + str(include.resolve())]
cmd += [str(here / 'mve_biquad_df1_io_bounds.c'), str(here / 'corstone300_console.c')]
cmd += [str(sources.resolve() / f'arm_biquad_cascade_df1_{q}.c') for q in ('q15', 'q31', '32x64_q31')]
cmd += [str(repo / 'Source/FilteringFunctions' / f'arm_biquad_cascade_df1_{q}.c') for q in ('init_q15', 'init_q31', '32x64_init_q31')]
cmd += [str(device / 'Source/startup_ARMCM55.c'),
        str(device / 'Source/system_ARMCM55.c'), '-T' + str(args.platform_linker.resolve()),
        '--specs=nosys.specs', '-Wl,--gc-sections', '-o', str(elf)]
subprocess.run(cmd, check=True)
fvp = args.fvp_root.resolve()
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = ':'.join([str(fvp / x) for x in
    ['python/lib', 'fmtplib', 'models/Linux64_GCC-9.3']] + [env.get('LD_LIBRARY_PATH', '')])
cmd = [str(fvp / 'models/Linux64_GCC-9.3/FVP_Corstone_SSE-300_Ethos-U55'), '-a', str(elf)]
for setting in ['mps3_board.visualisation.disable-visualisation=1',
                'mps3_board.uart0.out_file=-', 'mps3_board.uart0.unbuffered_output=1',
                'mps3_board.uart0.shutdown_on_eot=1', 'mps3_board.uart0.shutdown_tag=EXITTHESIM']:
    cmd += ['-C', setting]
for port in [0, 1, 2, 5]:
    cmd += ['-C', f'mps3_board.telnetterminal{port}.start_telnet=0']
result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=300)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
# The UART shutdown path returns zero even when the test reports failure.
if args.control:
    if 'FAIL:' not in log:
        print(f'control {args.control} did not fail as expected')
        raise SystemExit(1)
    print(f'control {args.control} failed as expected')
elif result.returncode or 'FAIL:' in log or 'PASS: ' not in log:
    raise SystemExit(1)
