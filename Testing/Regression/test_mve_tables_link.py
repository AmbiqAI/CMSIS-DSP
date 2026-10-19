#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Link complex FFT programs against the CMake-built library for a Helium core
without the HELIUM and MVEF options, and check that the MVE table objects of a
Cortex-M4 build define no symbols.

The Helium kernels are selected by the compiler (__ARM_FEATURE_MVE), so a
Cortex-M55 build needs the rearranged_twiddle_* tables whatever the CMake
options say. Cases: Cortex-M55 at HELIUM=OFF with FASTBUILD OFF and ON, and at
HELIUM=ON, each linking a program that calls arm_cfft_f32, arm_cfft_q31 and
arm_cfft_q15; Cortex-M4 at HELIUM=OFF, FASTBUILD OFF and ON, whose
arm_mve_tables.c and arm_mve_tables_f16.c objects must define no symbols.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-dsp', type=Path, default=Path(__file__).resolve().parents[2],
                    help='CMSIS-DSP checkout to build (default: this repository)')
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--compiler', default='arm-none-eabi-gcc', help='Arm GNU C compiler')
parser.add_argument('--expect-failure', action='store_true',
                    help='Negative control: pass only if a Cortex-M55 case at HELIUM=OFF fails to link')
args = parser.parse_args()
repo = args.cmsis_dsp.resolve()
compiler = shutil.which(args.compiler) or args.compiler
nm = str(Path(compiler).with_name(Path(compiler).name.replace('gcc', 'nm')))

PROGRAM = '''#include "arm_math.h"
static float32_t f32[512];
static q31_t q31[512];
static q15_t q15[512];
int main(void)
{
    arm_cfft_instance_f32 Sf;
    arm_cfft_instance_q31 S31;
    arm_cfft_instance_q15 S15;
    arm_cfft_init_f32(&Sf, 256);
    arm_cfft_f32(&Sf, f32, 0, 1);
    arm_cfft_init_q31(&S31, 256);
    arm_cfft_q31(&S31, q31, 0, 1);
    arm_cfft_init_q15(&S15, 256);
    arm_cfft_q15(&S15, q15, 0, 1);
    return (int)f32[0] + (int)q31[0] + (int)q15[0];
}
'''
CPUS = {
    'cortex-m55': '-mcpu=cortex-m55 -mthumb -mfloat-abi=hard',
    'cortex-m4': '-mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16',
}
CASES = [('cortex-m55', 'OFF', 'OFF'), ('cortex-m55', 'ON', 'OFF'), ('cortex-m55', 'OFF', 'ON'),
         ('cortex-m4', 'OFF', 'OFF'), ('cortex-m4', 'ON', 'OFF')]

failures = 0
with tempfile.TemporaryDirectory(prefix='cmsis-mve-tables-') as tmp:
    root = Path(tmp)
    consumer = root / 'consumer'
    consumer.mkdir()
    quoted = (repo / 'Source').as_posix().replace('"', '\\"')
    (consumer / 'main.c').write_text(PROGRAM)
    (consumer / 'CMakeLists.txt').write_text(
        'cmake_minimum_required(VERSION 3.14)\n'
        'project(consumer LANGUAGES C)\n'
        f'add_subdirectory("{quoted}" cmsisdsp)\n'
        'add_executable(cfft main.c)\n'
        'target_link_libraries(cfft PRIVATE CMSISDSP m)\n'
        'target_link_options(cfft PRIVATE --specs=nosys.specs)\n')
    # The negative control runs the Cortex-M55 cases only.
    for cpu, fastbuild, helium in [c for c in CASES if not args.expect_failure or c[0] == 'cortex-m55']:
        name = f'{cpu} FASTBUILD={fastbuild} HELIUM={helium}'
        build = root / f'{cpu}-fb{fastbuild}-he{helium}'
        configure = subprocess.run(
            ['cmake', '-S', str(consumer), '-B', str(build), '-DCMAKE_SYSTEM_NAME=Generic',
             '-DCMAKE_SYSTEM_PROCESSOR=arm', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY',
             '-DCMAKE_C_COMPILER=' + compiler, '-DCMAKE_C_FLAGS=' + CPUS[cpu] + ' -O2',
             '-DCMSISCORE=' + str(args.cmsis_core.resolve()), '-DDISABLEFLOAT16=ON',
             f'-DFASTBUILD={fastbuild}', f'-DHELIUM={helium}'],
            capture_output=True, text=True)
        if configure.returncode:
            print(configure.stdout + configure.stderr)
            raise SystemExit(f'FAIL: {name} configure')
        link = subprocess.run(['cmake', '--build', str(build), '--parallel', '8', '--target', 'cfft'],
                              capture_output=True, text=True)
        if link.returncode:
            undefined = sorted({line.split('`')[1].rstrip("'") for line in (link.stdout + link.stderr).splitlines()
                                if 'undefined reference to `' in line})
            failures += 1
            print(f'{"EXPECTED FAILURE" if args.expect_failure else "FAIL"}: {name} link, '
                  f'{len(undefined)} undefined symbols, first {undefined[:1]}')
            if not args.expect_failure:
                raise SystemExit(1)
            continue
        if cpu == 'cortex-m4':
            for table in ['arm_mve_tables.c', 'arm_mve_tables_f16.c']:
                objects = [o for o in build.rglob(table + '.o*') if o.name in (table + '.o', table + '.obj')]
                if len(objects) != 1:
                    raise SystemExit(f'FAIL: {name} {table}: {len(objects)} objects')
                symbols = subprocess.run([nm, str(objects[0])], capture_output=True, text=True,
                                         check=True).stdout.split()
                if symbols:
                    raise SystemExit(f'FAIL: {name} {table} object defines symbols')
            print(f'PASS: {name} links; MVE table objects define no symbols')
        else:
            print(f'PASS: {name} links arm_cfft_f32, arm_cfft_q31 and arm_cfft_q15')
if args.expect_failure:
    if failures == 0:
        raise SystemExit('FAIL: negative control linked every case')
    print(f'PASS: negative control, {failures} cases failed to link as expected')
