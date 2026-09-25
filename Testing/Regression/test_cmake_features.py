#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check configDsp feature agreement for in-tree and installed consumers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--config-dsp-file', type=Path,
                    help='Optional original configDsp.cmake for negative control')
args = parser.parse_args()
source = Path(__file__).resolve().parent / 'cmake_features'

def run(*cmd):
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(result.returncode)

with tempfile.TemporaryDirectory(prefix='cmsis-consumer-features-') as tmp:
    root = Path(tmp)
    for mask in range(8):
        build = root / f'build-{mask}'
        prefix = root / f'prefix-{mask}'
        options = [f'-D{name}={"ON" if mask & (1 << bit) else "OFF"}'
                   for bit, name in enumerate(['AUTOVECTORIZE', 'MVEFLOAT16', 'DISABLEFLOAT16'])]
        if args.config_dsp_file:
            options.append('-DCONFIG_DSP_FILE=' + str(args.config_dsp_file.resolve()))
        run('cmake', '-S', str(source), '-B', str(build),
            '-DCMAKE_INSTALL_PREFIX=' + str(prefix), f'-DEXPECTED_FEATURES={mask}', *options)
        run('cmake', '--build', str(build))
        run('ctest', '--test-dir', str(build), '--output-on-failure')
        run('cmake', '--install', str(build))
        installed = root / f'consumer-{mask}'
        # No feature options are supplied: they must come from the exported target.
        run('cmake', '-S', str(source), '-B', str(installed),
            '-DPROBE_PACKAGE=' + str(prefix), f'-DEXPECTED_FEATURES={mask}')
        run('cmake', '--build', str(installed))
        run('ctest', '--test-dir', str(installed), '--output-on-failure')
        print(f'PASS: feature mask {mask}, in-tree and installed consumers', flush=True)
