#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check that the GCC 15.0-15.2 __QSUB/__QADD workaround in arm_math_types.h is
active exactly for the compilers that crash on gcc15_qsub_ifcvt_canary.c, and
that the library sources that crashed without it compile."""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
LEVELS = ['-O1', '-O2', '-O3', '-Os']
# The sources on which GCC 15.2.1 crashed without the workaround, and the
# FASTBUILD aggregates that include them.
SOURCES = ['BasicMathFunctions/arm_abs_q31.c', 'FastMathFunctions/arm_atan2_q31.c',
           'StatisticsFunctions/arm_absmax_q31.c', 'StatisticsFunctions/arm_absmin_q31.c',
           'StatisticsFunctions/arm_absmax_no_idx_q31.c', 'StatisticsFunctions/arm_absmin_no_idx_q31.c',
           'BasicMathFunctions/BasicMathFunctions.c', 'FastMathFunctions/FastMathFunctions.c',
           'StatisticsFunctions/StatisticsFunctions.c']

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--compiler', default='arm-none-eabi-gcc',
                    help='Arm GNU gcc or clang (ATfE) executable')
parser.add_argument('--cmsis-core', type=Path, required=True,
                    help='CMSIS-Core 6 directory containing Include/')
parser.add_argument('--cpu', default='cortex-m4', help='-mcpu value (a core with the DSP extension)')
args = parser.parse_args()

flags = [f'-mcpu={args.cpu}', '-mthumb', '-mfloat-abi=soft']
if 'clang' in Path(args.compiler).name:
    flags.insert(0, '--target=arm-none-eabi')
version = subprocess.run([args.compiler, '--version'], capture_output=True, text=True,
                         check=True).stdout.splitlines()[0]
print(f'compiler: {version}')

with tempfile.TemporaryDirectory(prefix='gcc15-canary-') as tmp:
    probe = Path(tmp) / 'probe.c'
    probe.write_text('#include "arm_math_types.h"\n'
                     'int32_t probe(int32_t a, int32_t b) { return __QSUB(a, b) + __QADD(a, b); }\n')
    result = subprocess.run([args.compiler, *flags, '-E', '-I', str(REPO / 'Include'),
                             '-I', str(args.cmsis_core / 'Include'), str(probe)],
                            capture_output=True, text=True)
    if result.returncode:
        print(result.stderr)
        raise SystemExit('FAIL: arm_math_types.h does not preprocess')
    active = 'qsub %0' in result.stdout and 'qadd %0' in result.stdout
    print(f'workaround in arm_math_types.h: {"active" if active else "inactive"}')

    mismatches = 0
    for level in LEVELS:
        result = subprocess.run([args.compiler, *flags, level, '-c', str(HERE / 'gcc15_qsub_ifcvt_canary.c'),
                                 '-o', str(Path(tmp) / 'canary.o')], capture_output=True, text=True)
        crashed = result.returncode != 0 and 'internal compiler error' in result.stderr
        if result.returncode and not crashed:
            print(result.stderr)
            raise SystemExit(f'FAIL: canary does not compile at {level} for a reason other than the crash')
        state = 'internal compiler error' if crashed else 'compiles'
        if crashed == active:
            print(f'PASS: canary {level} {state}')
        else:
            mismatches += 1
            fix = 'narrow the gate' if active else 'widen the gate'
            print(f'FAIL: canary {level} {state}, workaround {"active" if active else "inactive"}: {fix}')

    includes = ['-I', str(REPO / 'Include'), '-I', str(REPO / 'PrivateInclude'),
                '-I', str(args.cmsis_core / 'Include')]
    broken = 0
    for level in LEVELS:
        for source in SOURCES:
            result = subprocess.run([args.compiler, *flags, level, '-DARM_MATH_LOOPUNROLL', *includes,
                                     '-c', str(REPO / 'Source' / source), '-o', str(Path(tmp) / 'source.o')],
                                    capture_output=True, text=True)
            if result.returncode:
                broken += 1
                print(result.stderr[-2000:])
                print(f'FAIL: {source} does not compile at {level}')
if mismatches or broken:
    raise SystemExit(f'FAIL: canary disagrees with the workaround gate at {mismatches} of {len(LEVELS)} levels; '
                     f'{broken} of {len(LEVELS) * len(SOURCES)} source compilations fail')
print(f'PASS: canary agrees with the workaround gate at {len(LEVELS)} levels; '
      f'{len(LEVELS) * len(SOURCES)} source compilations succeed')
