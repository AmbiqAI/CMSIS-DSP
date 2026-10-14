#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check the float16 guard of arm_math_types_f16.h across compilers and targets.

Arm GNU before 15.3 compiles scalar half-precision loads and stores for
Armv8.1-M without MVE to Advanced SIMD lane loads and stores (vld1.16,
vst1.16), which M-profile cores do not implement. For each compiler and
target, a small float16_t copy and arm_mat_qr_f16.c are compiled to
assembly:
  - Arm GNU before 15.3, Armv8.1-M without MVE, float16 enabled: the
    compile must stop with the header's #error;
  - the same with DISABLEFLOAT16: the compile must succeed;
  - every other compiler and target: the compile must succeed and the
    assembly must contain no vld1/vst1 instruction.
--expect-failure runs the negative control on a checkout with the previous
header, where the first group compiles and its assembly holds vld1.16.
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

TARGETS = {
    # name: (flags, Armv8.1-M without MVE)
    'cortex-m55+nomve': (['-mcpu=cortex-m55+nomve'], True),
    'cortex-m85+nomve': (['-mcpu=cortex-m85+nomve'], True),
    'cortex-m52+nomve': (['-mcpu=cortex-m52+nomve'], True),
    'armv8.1-m.main+fp.dp': (['-march=armv8.1-m.main+fp.dp'], True),
    'cortex-m55+nomve.fp': (['-mcpu=cortex-m55+nomve.fp'], False),
    'cortex-m55': (['-mcpu=cortex-m55'], False),
    'cortex-m4': (['-mcpu=cortex-m4', '-mfpu=fpv4-sp-d16'], False),
}
COPY = '''#include "arm_math_types_f16.h"
#if defined(ARM_FLOAT16_SUPPORTED)
void copy_f16(float16_t *restrict d, const float16_t *restrict s, int n)
{
    for (int i = 0; i < n; i++)
        d[i * 3] = s[i * 5];
}
#endif
'''
SIMD = re.compile(r'^\s+v(ld|st)[1-4]\.\d+\s+\{d', re.M)
ERROR = 'float16 on Armv8.1-M without MVE needs GCC 15.3 or later'

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-dsp', type=Path, default=Path(__file__).resolve().parents[2],
                    help='CMSIS-DSP checkout (default: this repository)')
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--gcc', action='append', default=[], help='arm-none-eabi-gcc to test (repeatable)')
parser.add_argument('--clang', action='append', default=[], help='ATfE clang to test (repeatable)')
parser.add_argument('--expect-failure', action='store_true',
                    help='Negative control: pass only if an affected configuration compiles')
args = parser.parse_args()
repo = args.cmsis_dsp.resolve()
includes = ['-I' + str(repo / 'Include'), '-I' + str(repo / 'PrivateInclude'),
            '-I' + str((args.cmsis_core / 'Include').resolve())]


def gcc_version(cc):
    out = subprocess.run([cc, '-dumpfullversion'], capture_output=True, text=True, check=True).stdout
    return tuple(int(x) for x in out.strip().split('.')[:2])


def compile_asm(cc, flags, src, extra):
    with tempfile.NamedTemporaryFile(suffix='.s') as asm:
        r = subprocess.run([cc, *flags, '-mthumb', '-mfloat-abi=hard', '-O2', '-DARMCM55', *extra,
                            *includes, '-S', '-o', asm.name, str(src)], capture_output=True, text=True)
        return r.returncode, r.stderr, Path(asm.name).read_text() if r.returncode == 0 else ''


failures = []
controls = 0
with tempfile.TemporaryDirectory(prefix='cmsis-f16-guard-') as tmp:
    copy_src = Path(tmp) / 'copy_f16.c'
    copy_src.write_text(COPY)
    sources = [copy_src, repo / 'Source/MatrixFunctions/arm_mat_qr_f16.c']
    compilers = [(cc, 'gcc', gcc_version(cc)) for cc in args.gcc] + [(cc, 'clang', None) for cc in args.clang]
    for cc, kind, version in compilers:
        label = f'{Path(cc).name} {".".join(map(str, version)) if version else ""}'.strip()
        for target, (flags, no_mve) in TARGETS.items():
            if kind == 'clang':
                flags = ['--target=arm-none-eabihf', *flags]
            affected = kind == 'gcc' and no_mve and version < (15, 3)
            for src in sources:
                name = f'{label} {target} {src.name}'
                rc, err, asm = compile_asm(cc, flags, src, [])
                if affected:
                    if args.expect_failure:
                        if rc == 0 and SIMD.search(asm):
                            controls += 1
                            print(f'EXPECTED FAILURE: {name}: compiles with Advanced SIMD loads/stores')
                        continue
                    if rc == 0 or ERROR not in err:
                        failures.append(f'{name}: compiled without the float16 #error')
                    rc2, err2, _ = compile_asm(cc, flags, src, ['-DDISABLEFLOAT16'])
                    if rc2:
                        failures.append(f'{name} with DISABLEFLOAT16: {err2.strip().splitlines()[-1:]}')
                    else:
                        print(f'PASS: {name}: #error; compiles with DISABLEFLOAT16')
                    continue
                if args.expect_failure:
                    continue
                if rc:
                    failures.append(f'{name}: {err.strip().splitlines()[-1:]}')
                elif SIMD.search(asm):
                    failures.append(f'{name}: Advanced SIMD load/store in the assembly')
                else:
                    print(f'PASS: {name}: compiles, no Advanced SIMD loads/stores')
if args.expect_failure:
    if controls == 0:
        raise SystemExit('FAIL: negative control: no affected configuration compiled with Advanced SIMD loads/stores')
    print(f'PASS: negative control, {controls} affected configurations compiled with Advanced SIMD loads/stores')
elif failures:
    for f in failures:
        print('FAIL: ' + f)
    raise SystemExit(1)
