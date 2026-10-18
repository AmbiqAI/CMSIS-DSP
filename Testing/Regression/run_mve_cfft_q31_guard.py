#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE Q31 CFFT guard regression on Corstone-300 (M55).

The regression compiles with the given compiler (Arm GNU or ATfE clang) and
links with an Arm GNU toolchain, so that one runner serves both compiler
families. For the negative control, pass an arm_cfft_q31.c that uses the
writeback gather through --source and compile with Arm GNU 15.2.1.
"""
import argparse
import os
from pathlib import Path
import subprocess

# Negative controls (regression_checks.h): compile-time define for each; a
# control run succeeds only if the test reports a failure.
CONTROLS = {
    'sparse-read-32': '-DREGRESSION_SPARSE_READ_BYTES=32',
    'sparse-read-far': '-DREGRESSION_SPARSE_READ_FAR',
}

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--cortex-dfp', type=Path, required=True)
parser.add_argument('--platform-linker', type=Path, required=True)
parser.add_argument('--fvp-root', type=Path, required=True, help='Corstone-300 model package root')
parser.add_argument('--compiler', default='arm-none-eabi-gcc',
                    help='arm-none-eabi-gcc, or an ATfE clang (compiles with --target=arm-none-eabihf)')
parser.add_argument('--linker',
                    help='Arm GNU driver used to link (default: --compiler, or arm-none-eabi-gcc for clang)')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--opt', default='O3', help='Optimization level without the dash, e.g. O2 (default O3)')
parser.add_argument('--source', type=Path,
                    help='arm_cfft_q31.c replacing the checkout file (negative control)')
parser.add_argument('--control', choices=sorted(CONTROLS),
                    help='Build a negative control instead; the run then succeeds only if the test fails')
args = parser.parse_args()
if args.source and args.source.name != 'arm_cfft_q31.c':
    parser.error('--source must name an arm_cfft_q31.c')
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
is_clang = 'clang' in Path(args.compiler).name
linker = args.linker or ('arm-none-eabi-gcc' if is_clang else args.compiler)
cflags = ['-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard',
          '-' + args.opt, '-ffast-math', '-flax-vector-conversions', '-ffunction-sections',
          '-fdata-sections', '-DARMCM55', '-DDISABLEFLOAT16']
if is_clang:
    cflags = ['--target=arm-none-eabihf'] + cflags
if args.control:
    cflags += [CONTROLS[args.control]]
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cflags += ['-I' + str(include.resolve())]
sources = [here / 'mve_cfft_q31_guard.c', here / 'corstone300_console.c']
sources += [args.source.resolve() if args.source else repo / 'Source/TransformFunctions/arm_cfft_q31.c']
sources += [repo / 'Source' / f for f in (
    'TransformFunctions/arm_cfft_init_q31.c',
    'TransformFunctions/arm_cfft_radix4_q31.c', 'TransformFunctions/arm_bitreversal2.c',
    'CommonTables/arm_common_tables.c', 'CommonTables/arm_const_structs.c',
    'CommonTables/arm_mve_tables.c')]
objects = []
for source in sources:
    obj = build / (source.stem + '.o')
    subprocess.run([args.compiler, *cflags, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = build / 'mve_cfft_q31_guard.elf'
link = [linker, '-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', *objects,
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
result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=600)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
# The UART shutdown path returns zero even when the test reports failure.
if args.control:
    if 'FAIL:' not in log or 'PASS: 14 MVE CFFT Q31 guard cases' in log:
        print(f'control {args.control} did not fail as expected')
        raise SystemExit(1)
    print(f'control {args.control} failed as expected')
elif result.returncode or 'FAIL:' in log or 'PASS: 14 MVE CFFT Q31 guard cases' not in log:
    raise SystemExit(1)
