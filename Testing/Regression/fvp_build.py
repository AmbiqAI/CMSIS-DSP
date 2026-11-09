# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Build a guarded regression for the Corstone-300 FVP with Arm GNU or ATfE.

The runners describe each build as one Arm GNU compile-and-link command.
With Arm GNU as the compiler, compile_and_link() runs it unchanged. With an
ATfE clang, it compiles each test and library source with clang
--target=arm-none-eabihf and the command's flags, compiles the CMSIS device
startup and system files with the Arm GNU driver given as linker, and links
with that driver, the method of the ledger's Toolchain section.
"""
from pathlib import Path
import subprocess

DEVICE_SOURCES = ('startup_ARMCM55.c', 'system_ARMCM55.c')
LINK_FLAGS = ('-T', '--specs', '-Wl', '-l')
TARGET_FLAGS = ('-mcpu', '-mthumb', '-mfloat-abi')


def compile_and_link(cmd, linker):
    compiler = cmd[0]
    if 'clang' not in Path(compiler).name:
        subprocess.run(cmd, check=True)
        return
    i = cmd.index('-o')
    out = Path(cmd[i + 1])
    rest = cmd[1:i] + cmd[i + 2:]
    sources = [a for a in rest if a.endswith('.c')]
    link = [a for a in rest if a.startswith(LINK_FLAGS)]
    cflags = [a for a in rest if a not in sources and a not in link]
    objdir = out.parent / (out.stem + '_objects')
    objdir.mkdir(parents=True, exist_ok=True)
    objects = []
    for n, source in enumerate(sources):
        obj = objdir / f'{n}_{Path(source).stem}.o'
        if Path(source).name in DEVICE_SOURCES:
            cc = [linker]
        else:
            cc = [compiler, '--target=arm-none-eabihf']
        subprocess.run(cc + cflags + ['-c', source, '-o', str(obj)], check=True)
        objects.append(str(obj))
    target = [a for a in cflags if a.startswith(TARGET_FLAGS)]
    subprocess.run([linker, *target, *objects, *link, '-o', str(out)], check=True)
