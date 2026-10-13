#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the MVE tail-load regression for one kernel group on Corstone-300 (M55).

The regression compiles with the given compiler (Arm GNU or ATfE clang) and
links with an Arm GNU toolchain, so that one runner serves both compiler
families. A group is available when its test file mve_tail_loads_<group>.c
is present next to this runner; the groups arrive in separate pull requests. For the negative control, pass unmodified upstream copies of one
or more kernels through --source; each replaces the checkout's file of the
same name.
"""
import argparse
import os
from pathlib import Path
import subprocess

GROUPS = {
    'f32': {
        'test': 'mve_tail_loads_f32.c',
        'expected': 'PASS: 168 MVE f32/u32 tail-load cases',
        'defines': ['-DDISABLEFLOAT16'],
        'sources': [
            'BasicMathFunctions/arm_abs_f32.c', 'BasicMathFunctions/arm_add_f32.c',
            'BasicMathFunctions/arm_and_u32.c', 'BasicMathFunctions/arm_dot_prod_f32.c',
            'BasicMathFunctions/arm_mult_f32.c', 'BasicMathFunctions/arm_negate_f32.c',
            'BasicMathFunctions/arm_not_u32.c', 'BasicMathFunctions/arm_offset_f32.c',
            'BasicMathFunctions/arm_or_u32.c', 'BasicMathFunctions/arm_scale_f32.c',
            'BasicMathFunctions/arm_sub_f32.c', 'BasicMathFunctions/arm_xor_u32.c',
            'MatrixFunctions/arm_mat_add_f32.c', 'MatrixFunctions/arm_mat_scale_f32.c',
            'MatrixFunctions/arm_mat_sub_f32.c', 'StatisticsFunctions/arm_absmin_f32.c',
            'StatisticsFunctions/arm_accumulate_f32.c', 'StatisticsFunctions/arm_mse_f32.c',
        ],
    },
    'fixed': {
        'test': 'mve_tail_loads_fixed.c',
        'expected': 'PASS: 923 MVE fixed-point tail-load cases',
        'defines': ['-DDISABLEFLOAT16'],
        'sources': [
            'BasicMathFunctions/arm_abs_q15.c', 'BasicMathFunctions/arm_abs_q31.c',
            'BasicMathFunctions/arm_abs_q7.c', 'BasicMathFunctions/arm_add_q15.c',
            'BasicMathFunctions/arm_add_q31.c', 'BasicMathFunctions/arm_add_q7.c',
            'BasicMathFunctions/arm_and_u16.c', 'BasicMathFunctions/arm_and_u8.c',
            'BasicMathFunctions/arm_dot_prod_q15.c', 'BasicMathFunctions/arm_dot_prod_q31.c',
            'BasicMathFunctions/arm_dot_prod_q7.c', 'BasicMathFunctions/arm_mult_q15.c',
            'BasicMathFunctions/arm_mult_q31.c', 'BasicMathFunctions/arm_mult_q7.c',
            'BasicMathFunctions/arm_negate_q15.c', 'BasicMathFunctions/arm_negate_q31.c',
            'BasicMathFunctions/arm_negate_q7.c', 'BasicMathFunctions/arm_not_u16.c',
            'BasicMathFunctions/arm_not_u8.c', 'BasicMathFunctions/arm_offset_q15.c',
            'BasicMathFunctions/arm_offset_q31.c', 'BasicMathFunctions/arm_offset_q7.c',
            'BasicMathFunctions/arm_or_u16.c', 'BasicMathFunctions/arm_or_u8.c',
            'BasicMathFunctions/arm_scale_q15.c', 'BasicMathFunctions/arm_scale_q31.c',
            'BasicMathFunctions/arm_scale_q7.c', 'BasicMathFunctions/arm_shift_q15.c',
            'BasicMathFunctions/arm_shift_q31.c', 'BasicMathFunctions/arm_shift_q7.c',
            'BasicMathFunctions/arm_sub_q15.c', 'BasicMathFunctions/arm_sub_q31.c',
            'BasicMathFunctions/arm_sub_q7.c', 'BasicMathFunctions/arm_xor_u16.c',
            'BasicMathFunctions/arm_xor_u8.c', 'MatrixFunctions/arm_mat_add_q15.c',
            'MatrixFunctions/arm_mat_add_q31.c', 'MatrixFunctions/arm_mat_scale_q15.c',
            'MatrixFunctions/arm_mat_scale_q31.c', 'MatrixFunctions/arm_mat_sub_q15.c',
            'MatrixFunctions/arm_mat_sub_q31.c', 'MatrixFunctions/arm_mat_trans_q7.c',
            'StatisticsFunctions/arm_mse_q15.c', 'StatisticsFunctions/arm_mse_q31.c',
            'StatisticsFunctions/arm_mse_q7.c',
        ],
    },
    'f16': {
        'test': 'mve_tail_loads_f16.c',
        'expected': 'PASS: 288 MVE f16 tail-load cases',
        'defines': [],
        'sources': [
            'BasicMathFunctions/arm_abs_f16.c', 'BasicMathFunctions/arm_add_f16.c',
            'BasicMathFunctions/arm_dot_prod_f16.c', 'BasicMathFunctions/arm_mult_f16.c',
            'BasicMathFunctions/arm_negate_f16.c', 'BasicMathFunctions/arm_offset_f16.c',
            'BasicMathFunctions/arm_scale_f16.c', 'BasicMathFunctions/arm_sub_f16.c',
            'ComplexMathFunctions/arm_cmplx_mult_real_f16.c', 'MatrixFunctions/arm_mat_add_f16.c',
            'MatrixFunctions/arm_mat_scale_f16.c', 'MatrixFunctions/arm_mat_sub_f16.c',
            'StatisticsFunctions/arm_absmin_f16.c', 'StatisticsFunctions/arm_mse_f16.c',
            'SupportFunctions/arm_q15_to_f16.c',
        ],
    },
    'remainder': {
        'test': 'mve_tail_loads_remainder.c',
        'expected': 'PASS: 112 MVE remainder tail-load cases',
        'defines': [],
        'sources': [
            'ComplexMathFunctions/arm_cmplx_mag_f16.c',
            'ComplexMathFunctions/arm_cmplx_mag_squared_f16.c',
            'MatrixFunctions/arm_mat_add_f32.c', 'MatrixFunctions/arm_mat_sub_f32.c',
            'SupportFunctions/arm_float_to_f16.c',
        ],
    },
}

here = Path(__file__).resolve().parent
available = sorted(g for g in GROUPS if (here / GROUPS[g]['test']).exists())
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group', choices=sorted(GROUPS), required=True,
                    help=f"kernel group; present in this checkout: {', '.join(available) or 'none'}")
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--cortex-dfp', type=Path, required=True)
parser.add_argument('--platform-linker', type=Path, required=True)
parser.add_argument('--fvp-root', type=Path, required=True, help='Corstone-300 model package root')
parser.add_argument('--compiler', default='arm-none-eabi-gcc',
                    help='arm-none-eabi-gcc, or an ATfE clang (compiles with --target=arm-none-eabihf)')
parser.add_argument('--linker', default='arm-none-eabi-gcc', help='Arm GNU driver used to link')
parser.add_argument('--opt', choices=['O0', 'O1', 'O2', 'O3'], default='O3',
                    help='O0 and O1 are for negative controls of kernels whose over-read a tail-predicated loop hides at O2/O3')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--source', type=Path, action='append', default=[],
                    help='Upstream kernel replacing the checkout file of the same name (negative control)')
args = parser.parse_args()
group = GROUPS[args.group]
if args.group not in available:
    parser.error(f"group {args.group}: {group['test']} is not in this checkout "
                 f"(present: {', '.join(available) or 'none'})")
repo = Path(__file__).resolve().parents[2]
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
replacements = {s.name: s.resolve() for s in args.source}
unknown = set(replacements) - {Path(s).name for s in group['sources']}
if unknown:
    parser.error(f'--source names not in group {args.group}: {sorted(unknown)}')
sources = [replacements.get(Path(s).name, repo / 'Source' / s) for s in group['sources']]
sources += [here / group['test'], here / 'corstone300_console.c']
is_clang = 'clang' in Path(args.compiler).name
cflags = ['-mcpu=cortex-m55', '-mthumb', '-mfloat-abi=hard', f'-{args.opt}', '-ffast-math',
          '-flax-vector-conversions', '-ffunction-sections', '-fdata-sections', '-DARMCM55']
cflags += group['defines']
if is_clang:
    cflags = ['--target=arm-none-eabihf'] + cflags
for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
    cflags += ['-I' + str(include.resolve())]
objects = []
for source in sources:
    obj = build / (source.stem + '.o')
    subprocess.run([args.compiler, *cflags, '-c', str(source), '-o', str(obj)], check=True)
    objects.append(str(obj))
elf = build / f'mve_tail_loads_{args.group}.elf'
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
result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120)
log = result.stdout + result.stderr
(build / 'run.log').write_text(log)
print(log)
# The UART shutdown path returns zero even when the test reports failure.
if result.returncode or 'FAIL:' in log or group['expected'] not in log:
    raise SystemExit(1)
