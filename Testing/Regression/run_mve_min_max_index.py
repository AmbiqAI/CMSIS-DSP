#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE min/max value-and-index regression on Corstone-300 (M55).

The regression compiles with the given compiler (Arm GNU or ATfE clang) and
links with an Arm GNU toolchain. Float16 is enabled, so the f16 kernels are
covered. For the negative control, pass unmodified upstream copies of one or
more of the twenty kernels through --source; each replaces the checkout's
file of the same name, and the unmodified arm_min_f32.c must make the run
fail. --control builds one of the shared negative controls instead; the run
then succeeds only if the test reports a failure.
"""
import argparse
import os
from pathlib import Path
import subprocess

KERNELS = [f'StatisticsFunctions/arm_{op}_{t}.c'
           for t in ('f32', 'q31', 'q15', 'q7', 'f16')
           for op in ('min', 'max', 'absmin', 'absmax')]
EXPECTED = 'PASS: 102400 MVE min/max index cases'
# Negative controls (regression_checks.h): compile-time define for each.
CONTROLS = {
    'sparse-read-32': '-DREGRESSION_SPARSE_READ_BYTES=32',
    'sparse-read-far': '-DREGRESSION_SPARSE_READ_FAR',
    'nan': '-DREGRESSION_NAN_OUTPUT',
}

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
parser.add_argument('--source', type=Path, action='append', default=[],
                    help='Upstream kernel replacing the checkout file of the same name (negative control)')
parser.add_argument('--control', choices=sorted(CONTROLS),
                    help='Build a negative control instead; the run then succeeds only if the test fails')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
replacements = {s.name: s.resolve() for s in args.source}
unknown = set(replacements) - {Path(s).name for s in KERNELS}
if unknown:
    parser.error(f'--source names not among the min/max kernels: {sorted(unknown)}')
sources = [replacements.get(Path(s).name, repo / 'Source' / s) for s in KERNELS]
sources += [here / 'mve_min_max_index.c', here / 'corstone300_console.c']
is_clang = 'clang' in Path(args.compiler).name
cflags = ['-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', f'-{args.opt}', '-ffast-math',
          '-flax-vector-conversions', '-ffunction-sections', '-fdata-sections', '-DARMCM55']
if is_clang:
    cflags = ['--target=arm-none-eabihf'] + cflags
if args.control:
    cflags += [CONTROLS[args.control]]
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cflags += ['-I' + str(include.resolve())]
objects = []
for source in sources:
    obj = build / (source.stem + '.o')
    subprocess.run([args.compiler, *cflags, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = build / 'mve_min_max_index.elf'
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
if args.control:
    if 'FAIL:' not in log or EXPECTED in log:
        print(f'control {args.control} did not fail as expected')
        raise SystemExit(1)
    print(f'control {args.control} failed as expected')
elif result.returncode or 'FAIL:' in log or EXPECTED not in log:
    raise SystemExit(1)
