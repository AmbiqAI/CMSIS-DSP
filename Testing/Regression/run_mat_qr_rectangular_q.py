#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the QR-with-Q regression on Corstone-300, once with the Helium
kernels (Cortex-M55) and once with the scalar code (Cortex-M55+nomve)."""
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
parser.add_argument('--qr-source-dir', type=Path,
                    help='Optional directory with other arm_mat_qr_f32.c, arm_mat_qr_f16.c and arm_mat_qr_f64.c for the negative control')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
sources = args.qr_source_dir or repo / 'Source/MatrixFunctions'

fvp = args.fvp_root.resolve()
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = ':'.join([str(fvp / x) for x in
    ['python/lib', 'fmtplib', 'models/Linux64_GCC-9.3']] + [env.get('LD_LIBRARY_PATH', '')])
failed = False
for cpu in ('cortex-m55', 'cortex-m55+nomve'):
    elf = build / f'mat_qr_rectangular_q_{cpu.replace("+", "_")}.elf'
    cmd = [args.compiler, f'-mcpu={cpu}', '-mthumb', '-mfloat-abi=hard', '-' + args.opt,
           '-ffast-math', '-flax-vector-conversions', '-ffunction-sections', '-fdata-sections',
           '-DARMCM55']
    if cpu.endswith('+nomve') and 'gcc' in Path(args.compiler).name:
        # Arm GNU 14.3.1 emits Advanced SIMD lane loads for scalar f16 code
        # on Cortex-M55+nomve; see mat_qr_rectangular_q.c.
        cmd += ['-DSKIP_F16']
    for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
        cmd += ['-I' + str(include.resolve())]
    cmd += [str(here / 'mat_qr_rectangular_q.c'), str(here / 'corstone300_console.c')]
    cmd += [str(sources.resolve() / f'arm_mat_qr_{t}.c') for t in ('f32', 'f16', 'f64')]
    cmd += [str(repo / 'Source' / d / f'{k}_{t}.c') for t in ('f32', 'f16', 'f64')
            for d, k in (('MatrixFunctions', 'arm_householder'), ('BasicMathFunctions', 'arm_dot_prod'),
                         ('BasicMathFunctions', 'arm_scale'))]
    cmd += [str(device / 'Source/startup_ARMCM55.c'), str(device / 'Source/system_ARMCM55.c'),
            '-T' + str(args.platform_linker.resolve()), '--specs=nosys.specs', '-Wl,--gc-sections',
            '-lm', '-o', str(elf)]
    subprocess.run(cmd, check=True)
    run = [str(fvp / 'models/Linux64_GCC-9.3/FVP_Corstone_SSE-300_Ethos-U55'), '-a', str(elf)]
    for setting in ['mps3_board.visualisation.disable-visualisation=1',
                    'mps3_board.uart0.out_file=-', 'mps3_board.uart0.unbuffered_output=1',
                    'mps3_board.uart0.shutdown_on_eot=1', 'mps3_board.uart0.shutdown_tag=EXITTHESIM']:
        run += ['-C', setting]
    for port in [0, 1, 2, 5]:
        run += ['-C', f'mps3_board.telnetterminal{port}.start_telnet=0']
    result = subprocess.run(run, env=env, capture_output=True, text=True, timeout=300)
    log = result.stdout + result.stderr
    (build / f'run_{cpu.replace("+", "_")}.log').write_text(log)
    print(f'== {cpu}')
    print(log)
    # The UART shutdown path returns zero even when the test reports failure.
    if result.returncode or 'FAIL:' in log or 'PASS: ' not in log:
        failed = True
if failed:
    raise SystemExit(1)
