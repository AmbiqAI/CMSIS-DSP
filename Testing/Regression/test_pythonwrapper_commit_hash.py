#!/usr/bin/env python3
# Copyright (c) 2026 Ambiq Micro, Inc.
# SPDX-License-Identifier: Apache-2.0
"""Check the commit the CMSIS-DSP Python wrapper reports (cmsisdsp.commit_hash).

setup.py writes cmsisdsp/_build_info.py from this repository's own Git
checkout; a source distribution carries the file it was made with. Each case
starts from a fresh clone of one commit of this repository (--commit, default
HEAD) in a temporary directory:

  clean       `setup.py --name` in the clone writes the commit
  dirty       a modified tracked file writes the commit with "-dirty"
  untracked   untracked and ignored files leave it clean
  no-git      an export without .git and without the file writes none
  nested      the clone's sdist, unpacked inside an unrelated Git repository,
              keeps the carried commit through `setup.py --name`

With --wheel, a wheel is also built from the nested sdist: it must carry the
commit, and installed into a directory on PYTHONPATH (the interpreter's
numpy is used) it must report it through
cmsisdsp.commit_hash, add two vectors with arm_add_f32, and, with its
_build_info.py removed, report the fallback literal in cmsisdsp/__init__.py.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--commit', default='HEAD', help='Commit of this repository to test (default HEAD)')
parser.add_argument('--wheel', action='store_true', help='Also build, inspect and install a wheel (compiles the wrapper)')
parser.add_argument('--python', default=sys.executable,
                    help='Interpreter with numpy, setuptools and build installed (default: this one)')
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
failures = []


def run(*cmd, cwd=None, env=None):
    result = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)
    if result.returncode:
        print(result.stdout + result.stderr)
        raise SystemExit(f'command failed: {" ".join(map(str, cmd))}')
    return result.stdout.strip()


def check(name, got, want):
    ok = got == want
    print(f'{"PASS" if ok else "FAIL"}: {name}: {got!r}' + ('' if ok else f', expected {want!r}'))
    if not ok:
        failures.append(name)


def carried(tree):
    info = Path(tree) / 'cmsisdsp' / '_build_info.py'
    if not info.exists():
        return None
    return re.search(r'commit_hash = "([^"]*)"', info.read_text()).group(1)


def clone(dst):
    run('git', 'clone', '--quiet', '--no-checkout', str(repo), str(dst))
    run('git', '-C', str(dst), 'checkout', '--quiet', '--detach', commit)
    return dst


def setup_name(tree):
    run(args.python, 'setup.py', '--name', cwd=tree)


commit = run('git', '-C', str(repo), 'rev-parse', args.commit)
literal = re.search(r'commit_hash="([0-9a-f]+)"', (repo / 'cmsisdsp' / '__init__.py').read_text()).group(1)
print(f'commit {commit}')
with tempfile.TemporaryDirectory(prefix='cmsisdsp-commit-hash-') as tmp:
    tmp = Path(tmp)

    tree = clone(tmp / 'clean')
    setup_name(tree)
    check('clean checkout', carried(tree), commit)

    tree = clone(tmp / 'dirty')
    with open(tree / 'README.md', 'a') as f:
        f.write('\n')
    setup_name(tree)
    check('dirty checkout', carried(tree), commit + '-dirty')

    tree = clone(tmp / 'untracked')
    (tree / 'untracked.txt').write_text('untracked\n')
    (tree / 'disasm.s').write_text('ignored\n')
    setup_name(tree)
    check('untracked and ignored files', carried(tree), commit)

    export = tmp / 'no-git'
    export.mkdir()
    archive = tmp / 'export.tar'
    run('git', '-C', str(repo), 'archive', '--format=tar', '-o', str(archive), commit)
    with tarfile.open(archive) as t:
        t.extractall(export, filter='data')
    setup_name(export)
    check('no Git, no carried file', carried(export), None)

    # The sdist of the clean clone, unpacked inside an unrelated repository.
    run(args.python, '-m', 'build', '--sdist', '--no-isolation', '--outdir', str(tmp / 'dist'),
        cwd=tmp / 'clean')
    sdist = next((tmp / 'dist').glob('*.tar.gz'))
    parent = tmp / 'parent'
    parent.mkdir()
    run('git', 'init', '--quiet', str(parent))
    (parent / 'unrelated.txt').write_text('unrelated\n')
    run('git', '-C', str(parent), 'add', 'unrelated.txt')
    run('git', '-C', str(parent), '-c', 'user.name=test', '-c', 'user.email=test@example.invalid',
        'commit', '--quiet', '-m', 'unrelated')
    with tarfile.open(sdist) as t:
        t.extractall(parent, filter='data')
    nested = next(p for p in parent.iterdir() if p.is_dir() and p.name.startswith('cmsisdsp'))
    check('sdist carries the commit', carried(nested), commit)
    setup_name(nested)
    check('nested sdist after setup.py --name', carried(nested), commit)

    if args.wheel:
        run(args.python, '-m', 'build', '--wheel', '--no-isolation', '--outdir', str(tmp / 'wheel'),
            cwd=nested)
        wheel = next((tmp / 'wheel').glob('*.whl'))
        with zipfile.ZipFile(wheel) as z:
            text = z.read('cmsisdsp/_build_info.py').decode()
        check('nested sdist wheel carries the commit', re.search(r'commit_hash = "([^"]*)"', text).group(1), commit)
        site = tmp / 'site'
        run(args.python, '-m', 'pip', 'install', '--quiet', '--no-deps', '--no-index',
            '--target', str(site), str(wheel))
        env = dict(os.environ, PYTHONPATH=str(site))
        probe = ('import cmsisdsp, numpy as np; a = np.arange(7, dtype=np.float32); '
                 'b = np.full(7, 0.5, dtype=np.float32); '
                 'print(cmsisdsp.commit_hash, bool(np.array_equal(cmsisdsp.arm_add_f32(a, b), a + b)))')
        out = run(args.python, '-c', probe, cwd=tmp, env=env).split()
        check('installed wheel commit_hash', out[0], commit)
        check('installed wheel arm_add_f32', out[1], 'True')
        package = Path(run(args.python, '-c', 'import cmsisdsp, os; print(os.path.dirname(cmsisdsp.__file__))',
                           cwd=tmp, env=env))
        check('wheel imported from the install directory', package.parent, site)
        (package / '_build_info.py').unlink()
        out = run(args.python, '-c', 'import cmsisdsp; print(cmsisdsp.commit_hash)', cwd=tmp, env=env)
        check('installed wheel without _build_info.py', out, literal)

if failures:
    raise SystemExit(f'FAIL: {len(failures)} case(s): {", ".join(failures)}')
print('PASS: Python wrapper commit-hash cases')
