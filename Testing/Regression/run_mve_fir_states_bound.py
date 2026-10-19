#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE FIR q7/q15 and FIR interpolator state-bound regression on Corstone-300 (M55)."""
import argparse
import os
from pathlib import Path
import subprocess

CONTROLS = {
    'sparse-read-32': '-DREGRESSION_SPARSE_READ_BYTES=32',
    'sparse-read-far': '-DREGRESSION_SPARSE_READ_FAR',
}
KERNELS = ['arm_fir_q7.c', 'arm_fir_q15.c', 'arm_fir_interpolate_f32.c',
           'arm_fir_interpolate_q15.c', 'arm_fir_interpolate_q31.c']
INITS = ['arm_fir_init_q7.c', 'arm_fir_init_q15.c', 'arm_fir_interpolate_init_f32.c',
         'arm_fir_interpolate_init_q15.c', 'arm_fir_interpolate_init_q31.c']

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--cortex-dfp', type=Path, required=True)
parser.add_argument('--platform-linker', type=Path, required=True)
parser.add_argument('--fvp-root', type=Path, required=True, help='Corstone-300 model package root')
parser.add_argument('--compiler', default='arm-none-eabi-gcc')
parser.add_argument('--opt', default='O3', help='Optimization level (default O3)')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--fir-source', type=Path, action='append', default=[],
                    help='Kernel file replacing the checkout file of the same name (negative control)')
parser.add_argument('--control', choices=sorted(CONTROLS),
                    help='Build a negative control instead; the run then succeeds only if the test fails')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
replacements = {s.name: s.resolve() for s in args.fir_source}
unknown = set(replacements) - set(KERNELS)
if unknown:
    parser.error(f'--fir-source names not among the kernels: {sorted(unknown)}')
filt = repo / 'Source/FilteringFunctions'
elf = build / 'mve_fir_states_bound.elf'
cmd = [args.compiler, '-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard',
       '-' + args.opt, '-ffast-math', '-flax-vector-conversions', '-ffunction-sections',
       '-fdata-sections', '-DARMCM55', '-DDISABLEFLOAT16']
if args.control:
    cmd += [CONTROLS[args.control]]
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cmd += ['-I' + str(include.resolve())]
cmd += [str(here / 'mve_fir_states_bound.c'), str(here / 'corstone300_console.c')]
cmd += [str(replacements.get(k, filt / k)) for k in KERNELS] + [str(filt / k) for k in INITS]
cmd += [str(device / 'Source/startup_ARMCM55.c'), str(device / 'Source/system_ARMCM55.c'),
        '-T' + str(args.platform_linker.resolve()), '--specs=nosys.specs', '-Wl,--gc-sections',
        '-lm', '-o', str(elf)]
subprocess.run(cmd, check=True)
fvp = args.fvp_root.resolve()
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = ':'.join([str(fvp / x) for x in
    ['python/lib', 'fmtplib', 'models/Linux64_GCC-9.3']] + [env.get('LD_LIBRARY_PATH', '')])
run = [str(fvp / 'models/Linux64_GCC-9.3/FVP_Corstone_SSE-300_Ethos-U55'), '-a', str(elf)]
for setting in ['mps3_board.visualisation.disable-visualisation=1',
                'mps3_board.uart0.out_file=-', 'mps3_board.uart0.unbuffered_output=1',
                'mps3_board.uart0.shutdown_on_eot=1', 'mps3_board.uart0.shutdown_tag=EXITTHESIM']:
    run += ['-C', setting]
for port in [0, 1, 2, 5]:
    run += ['-C', f'mps3_board.telnetterminal{port}.start_telnet=0']
result = subprocess.run(run, env=env, capture_output=True, text=True, timeout=1800)
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
