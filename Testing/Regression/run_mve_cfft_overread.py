#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE CFFT buffer-bound regression on Corstone-300 (M55).

The regression compiles with the given compiler (Arm GNU or ATfE clang) and
links with an Arm GNU toolchain. Float16 is enabled, so the f16 transform is
covered. For the negative control, pass unmodified upstream copies of one or
more of the four arm_cfft_<type>.c files through --source; each replaces the
checkout's file of the same name and must make the run fail at the guard.
"""
import argparse
import os
from pathlib import Path
import subprocess

KERNELS = ['TransformFunctions/arm_cfft_f32.c', 'TransformFunctions/arm_cfft_q31.c',
           'TransformFunctions/arm_cfft_q15.c', 'TransformFunctions/arm_cfft_f16.c']
SHARED = ['TransformFunctions/arm_cfft_init_f32.c', 'TransformFunctions/arm_cfft_init_q31.c',
          'TransformFunctions/arm_cfft_init_q15.c', 'TransformFunctions/arm_cfft_init_f16.c',
          'TransformFunctions/arm_cfft_radix4_f32.c', 'TransformFunctions/arm_cfft_radix4_q31.c',
          'TransformFunctions/arm_cfft_radix4_q15.c', 'TransformFunctions/arm_cfft_radix8_f32.c',
          'TransformFunctions/arm_cfft_radix8_f16.c', 'TransformFunctions/arm_bitreversal2.c',
          'CommonTables/arm_common_tables.c', 'CommonTables/arm_const_structs.c',
          'CommonTables/arm_mve_tables.c', 'CommonTables/arm_common_tables_f16.c',
          'CommonTables/arm_const_structs_f16.c', 'CommonTables/arm_mve_tables_f16.c',
          'BasicMathFunctions/arm_shift_q31.c', 'BasicMathFunctions/arm_shift_q15.c']
EXPECTED = 'PASS: 72 MVE CFFT last-stage cases'

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
parser.add_argument('--source', type=Path, action='append', default=[],
                    help='Upstream arm_cfft_<type>.c replacing the checkout file of the same name (negative control)')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
replacements = {s.name: s.resolve() for s in args.source}
unknown = set(replacements) - {Path(s).name for s in KERNELS}
if unknown:
    parser.error(f'--source names not among the CFFT kernels: {sorted(unknown)}')
sources = [replacements.get(Path(s).name, repo / 'Source' / s) for s in KERNELS]
sources += [repo / 'Source' / s for s in SHARED]
sources += [here / 'mve_cfft_overread.c', here / 'corstone300_console.c']
is_clang = 'clang' in Path(args.compiler).name
cflags = ['-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', f'-{args.opt}', '-ffast-math',
          '-flax-vector-conversions', '-ffunction-sections', '-fdata-sections', '-DARMCM55']
if is_clang:
    cflags = ['--target=arm-none-eabihf'] + cflags
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cflags += ['-I' + str(include.resolve())]
objects = []
for source in sources:
    obj = build / (source.stem + '.o')
    subprocess.run([args.compiler, *cflags, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = build / 'mve_cfft_overread.elf'
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
result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=1800)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
# The UART shutdown path returns zero even when the test reports failure.
if result.returncode or 'FAIL:' in log or EXPECTED not in log:
    raise SystemExit(1)
