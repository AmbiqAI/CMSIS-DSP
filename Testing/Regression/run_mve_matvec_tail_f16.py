#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the standalone f16 matrix-tail regression on Corstone-300 (M55).

Compiles with the given compiler (Arm GNU, or ATfE clang with
--target=arm-none-eabihf) and links with Arm GNU.
"""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--cortex-dfp', type=Path, required=True)
parser.add_argument('--platform-linker', type=Path, required=True)
parser.add_argument('--fvp-root', type=Path, required=True, help='Corstone-300 model package root')
parser.add_argument('--compiler', default='arm-none-eabi-gcc',
                    help='arm-none-eabi-gcc, or an ATfE clang (compiles with --target=arm-none-eabihf)')
parser.add_argument('--linker', default='arm-none-eabi-gcc', help='Arm GNU driver used to link')
parser.add_argument('--opt', choices=['O0', 'O1', 'O2', 'O3'], default='O3')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--matrix-source', type=Path,
                    help='Optional upstream arm_mat_vec_mult_f16.c, for the negative control')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
source = args.matrix_source or repo / 'Source/MatrixFunctions/arm_mat_vec_mult_f16.c'
cflags = ['-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', f'-{args.opt}', '-ffast-math',
          '-flax-vector-conversions', '-ffunction-sections', '-fdata-sections', '-DARMCM55']
if 'clang' in Path(args.compiler).name:
    cflags = ['--target=arm-none-eabihf'] + cflags
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cflags += ['-I' + str(include.resolve())]
objects = []
for src in [here / 'mve_matvec_tail_f16.c', here / 'corstone300_console.c', source.resolve()]:
    obj = build / (src.stem + '.o')
    subprocess.run([args.compiler, *cflags, '-c', str(src), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = build / 'mve_matvec_tail_f16.elf'
subprocess.run([args.linker, '-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', *objects,
                str(device / 'Source/startup_ARMCM55.c'), str(device / 'Source/system_ARMCM55.c'),
                '-DARMCM55', '-I' + str(args.cmsis_core.resolve() / 'Include'), '-I' + str(device / 'Include'),
                '-T' + str(args.platform_linker.resolve()), '--specs=nosys.specs', '-Wl,--gc-sections',
                '-lm', '-o', str(elf)], check=True)
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
result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
# The UART shutdown path returns zero even when the test reports failure.
if result.returncode or 'FAIL:' in log or 'PASS: 119 MVE f16 matrix-tail cases' not in log:
    raise SystemExit(1)
