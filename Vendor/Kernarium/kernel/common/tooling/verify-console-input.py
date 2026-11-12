#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify actual UML host-console input preservation after a Darwin Kbuild."""
from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import platform
import shlex
import shutil
import subprocess
import tempfile


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=pathlib.Path, required=True)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        raise SystemExit('console input probe requires an arm64 Darwin host')
    source = args.source.resolve()
    build = args.build.resolve()
    command_file = build / 'arch/um/drivers/.chan_user.o.cmd'
    record = command_file.read_text().splitlines()[0]
    prefix = 'savedcmd_arch/um/drivers/chan_user.o :='
    if not record.startswith(prefix):
        raise SystemExit('unrecognized Kbuild console compile command')
    command = shlex.split(record[len(prefix):])
    console_source = source / 'arch/um/drivers/chan_user.c'
    if pathlib.Path(command[-1]).resolve() != console_source:
        raise SystemExit('Kbuild console command belongs to another source tree')
    compiler = shutil.which(command[0])
    if compiler is None:
        raise SystemExit(f'Kbuild compiler is unavailable: {command[0]}')
    command[0] = compiler
    # Kernel host-service objects use Darwin libc declarations in an ELF object.
    # Compile that same source/flags as Mach-O so the PTY assertion can call it
    # directly. Keep the original Kbuild object and dependency record untouched.
    command = [argument for argument in command
               if not argument.startswith('-Wp,-MD,')
               and argument != '-fno-stack-clash-protection']
    targets = [index for index, value in enumerate(command)
               if value.startswith('--target=')]
    if len(targets) != 1:
        raise SystemExit('Kbuild console command must have one explicit target')
    command[targets[0]] = '--target=arm64-apple-macos13'
    fixture = pathlib.Path(__file__).with_name('console_input_probe.cpp')
    with tempfile.TemporaryDirectory(prefix='uml-console-input-') as temporary:
        output = pathlib.Path(temporary)
        native_object = output / 'chan_user.o'
        executable = output / 'console-input-probe'
        command[command.index('-o') + 1] = str(native_object)
        subprocess.run(command, cwd=build, check=True, timeout=45)
        subprocess.run([
            compiler, '-std=c++20', '-Wall', '-Wextra', '-Werror',
            str(fixture), str(native_object), '-Wl,-dead_strip', '-lc++',
            '-o', str(executable),
        ], check=True, timeout=45)
        subprocess.run([str(executable)], check=True, timeout=10)
    print('verified kernel console preserves queued host-terminal input '
          'and other threads\' signal masks')


if __name__ == '__main__':
    main()
