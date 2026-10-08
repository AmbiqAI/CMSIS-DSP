#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE Q31 FFT path-parity regression on Corstone-300 (M55).

arm_cfft_q31.c and arm_rfft_q31.c are compiled twice: as configured (the
GCC inline-assembly paths under Arm GNU), and with ARM_MATH_MVE_FFT_REFERENCE
plus renamed entry points (Arm's intrinsic paths). The test links both and
compares their output bit for bit.
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
parser.add_argument('--opt', choices=['O2', 'O3'], default='O3')
parser.add_argument('--build-dir', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
is_clang = 'clang' in Path(args.compiler).name
cflags = ['-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', f'-{args.opt}', '-ffast-math',
          '-flax-vector-conversions', '-ffunction-sections', '-fdata-sections', '-DARMCM55',
          '-DDISABLEFLOAT16']
if is_clang:
    cflags = ['--target=arm-none-eabihf'] + cflags
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cflags += ['-I' + str(include.resolve())]
paths_under_test = ['TransformFunctions/arm_cfft_q31.c', 'TransformFunctions/arm_rfft_q31.c']
shared = ['TransformFunctions/arm_cfft_init_q31.c', 'TransformFunctions/arm_rfft_init_q31.c',
          'TransformFunctions/arm_cfft_radix4_q31.c', 'TransformFunctions/arm_bitreversal2.c',
          'CommonTables/arm_common_tables.c', 'CommonTables/arm_const_structs.c',
          'CommonTables/arm_mve_tables.c', 'BasicMathFunctions/arm_shift_q31.c']
reference_defines = ['-DARM_MATH_MVE_FFT_REFERENCE', '-Darm_cfft_q31=arm_cfft_q31_reference',
                     '-Darm_rfft_q31=arm_rfft_q31_reference',
                     '-Darm_split_rfft_q31=arm_split_rfft_q31_reference',
                     '-Darm_split_rifft_q31=arm_split_rifft_q31_reference']
objects = []
def compile_one(source, obj, extra):
    subprocess.run([args.compiler, *cflags, *extra, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
for f in paths_under_test:
    compile_one(repo / 'Source' / f, build / (Path(f).stem + '.o'), [])
    compile_one(repo / 'Source' / f, build / (Path(f).stem + '_reference.o'), reference_defines)
for f in shared:
    compile_one(repo / 'Source' / f, build / (Path(f).stem + '.o'), [])
for f in ['mve_cfft_q31_paths.c', 'corstone300_console.c']:
    compile_one(here / f, build / (Path(f).stem + '.o'), [])
elf = build / 'mve_cfft_q31_paths.elf'
link = [args.linker, '-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', *objects,
        str(device / 'Source/startup_ARMCM55.c'), str(device / 'Source/system_ARMCM55.c'),
        '-DARMCM55', '-I' + str(args.cmsis_core.resolve() / 'Include'),
        '-I' + str(device / 'Include'), '-T' + str(args.platform_linker.resolve()),
        '--specs=nosys.specs', '-Wl,--gc-sections', '-lm', '-o', str(elf)]
subprocess.run(link, check=True)
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
result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=900)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
# The UART shutdown path returns zero even when the test reports failure.
if result.returncode or 'FAIL:' in log or 'PASS: 28 MVE Q31 FFT path-parity cases' not in log:
    raise SystemExit(1)
