#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build/run the LDL^t reconstruction regression on Corstone-300, once with the
Helium kernels (Cortex-M55) and once with the scalar code (Cortex-M55+nomve)."""
import argparse
import os
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cmsis-core', type=Path, required=True, help='CMSIS/Core directory')
parser.add_argument('--cortex-dfp', type=Path, required=True)
parser.add_argument('--platform-linker', type=Path, required=True)
parser.add_argument('--fvp-root', type=Path, required=True, help='Corstone-300 model package root')
parser.add_argument('--compiler', default='arm-none-eabi-gcc')
parser.add_argument('--opt', choices=['O2', 'O3'], default='O3')
parser.add_argument('--build-dir', type=Path, required=True)
parser.add_argument('--ldlt-source-dir', type=Path,
                    help='Optional directory with other arm_mat_ldlt_f32.c and arm_mat_ldlt_f64.c for the negative control')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
here = Path(__file__).resolve().parent
build = args.build_dir.resolve()
build.mkdir(parents=True, exist_ok=True)
device = args.cortex_dfp.resolve() / 'Device/ARMCM55'
sources = args.ldlt_source_dir or repo / 'Source/MatrixFunctions'
patterns = repo / 'Testing/Patterns/DSP/Matrix/Unary'


def values(path):
    """Values of a Testing/Patterns file: a type line, a count, then hex words."""
    lines = [l.strip() for l in path.read_text().splitlines() if l.strip() and not l.startswith('//')]
    return lines[0], [l for l in lines[2:]]


def c_array(ctype, name, words, kind):
    """Exact decimal literals: 9 significant digits round-trip a float, 17 a double, in exponent form."""
    if kind == 'W':
        body = ', '.join('%.8ef' % struct.unpack('<f', struct.pack('<I', int(w, 16)))[0] for w in words)
    elif kind == 'D':
        body = ', '.join('%.16e' % struct.unpack('<d', struct.pack('<Q', int(w, 16)))[0] for w in words)
    else:
        body = ', '.join(str(int(w, 16)) for w in words)
    return f'static const {ctype} {name}[] = {{{body}}};\n'


header = ['#include <stdint.h>\n']
for tag in ('SDPO', 'DPO'):
    kind, dims = values(patterns / 'UnaryF32' / f'DimsCholesky{tag}1_s16.txt')
    header.append(c_array('unsigned', f'{tag.lower()}_dims', dims, kind))
    header.append(f'#define {tag}_N {len(dims)}U\n')
    kind, f32 = values(patterns / 'UnaryF32' / f'InputCholesky{tag}1_f32.txt')
    header.append(c_array('float', f'{tag.lower()}_f32', f32, kind))
    kind, f64 = values(patterns / 'UnaryF64' / f'InputCholesky{tag}1_f64.txt')
    header.append(c_array('double', f'{tag.lower()}_f64', f64, kind))
(build / 'ldlt_patterns.h').write_text(''.join(header))

fvp = args.fvp_root.resolve()
env = os.environ.copy()
env['LD_LIBRARY_PATH'] = ':'.join([str(fvp / x) for x in
    ['python/lib', 'fmtplib', 'models/Linux64_GCC-9.3']] + [env.get('LD_LIBRARY_PATH', '')])
failed = False
for cpu in ('cortex-m55', 'cortex-m55+nomve'):
    elf = build / f'mat_ldlt_rank_deficient_{cpu.replace("+", "_")}.elf'
    cmd = [args.compiler, f'-mcpu={cpu}', '-mthumb', '-mfloat-abi=hard', '-' + args.opt,
           '-ffast-math', '-flax-vector-conversions', '-ffunction-sections', '-fdata-sections',
           '-DARMCM55', '-DDISABLEFLOAT16', '-I' + str(build)]
    for include in [repo / 'Include', repo / 'PrivateInclude', args.cmsis_core / 'Include', device / 'Include']:
        cmd += ['-I' + str(include.resolve())]
    cmd += [str(here / 'mat_ldlt_rank_deficient.c'), str(here / 'corstone300_console.c'),
            str(sources.resolve() / 'arm_mat_ldlt_f32.c'), str(sources.resolve() / 'arm_mat_ldlt_f64.c'),
            str(device / 'Source/startup_ARMCM55.c'), str(device / 'Source/system_ARMCM55.c'),
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
