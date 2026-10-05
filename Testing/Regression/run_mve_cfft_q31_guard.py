#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE Q31 CFFT guard regression on Corstone-300 (M55)."""
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
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--opt', default='O3', help='Optimization level without the dash, e.g. O2 (default O3)')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
elf = build / 'mve_cfft_q31_guard.elf'
cmd = [args.compiler, '-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard',
       '-' + args.opt, '-ffast-math', '-flax-vector-conversions', '-ffunction-sections',
       '-fdata-sections', '-DARMCM55', '-DDISABLEFLOAT16']
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cmd += ['-I' + str(include.resolve())]
cmd += [str(here / 'mve_cfft_q31_guard.c'), str(here / 'corstone300_console.c')]
cmd += [str(repo / 'Source' / f) for f in (
    'TransformFunctions/arm_cfft_q31.c', 'TransformFunctions/arm_cfft_init_q31.c',
    'TransformFunctions/arm_cfft_radix4_q31.c', 'TransformFunctions/arm_bitreversal2.c',
    'CommonTables/arm_common_tables.c', 'CommonTables/arm_const_structs.c',
    'CommonTables/arm_mve_tables.c')]
cmd += [str(device / 'Source/startup_ARMCM55.c'),
        str(device / 'Source/system_ARMCM55.c'), '-T' + str(args.platform_linker.resolve()),
        '--specs=nosys.specs', '-Wl,--gc-sections', '-lm', '-o', str(elf)]
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
result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
# The UART shutdown path returns zero even when the test reports failure.
if result.returncode or 'FAIL:' in log or 'PASS: 14 MVE CFFT Q31 guard cases' not in log:
    raise SystemExit(1)
