#!/usr/bin/env python3
"""Reuse host Meson flags to test the exact private PI graph implementation."""
import json,pathlib,shlex,subprocess,sys
root=pathlib.Path(__file__).resolve().parents[2]
build=root/'build/host'; out=root/'build/telegram-pi-841';out.mkdir(parents=True,exist_ok=True)
entry=next(e for e in json.loads((build/'compile_commands.json').read_text()) if e['file'].endswith('/kernel/futex.c'))
args=shlex.split(entry['command']);args=args[:args.index('-o')];args=[v for v in args if v!='-c']
binary=out/'host-pi-donation';args += [str(root/'tests/linuxrunner/host_pi_donation.c'),'-Wl,-dead_strip','-o',str(binary)]
subprocess.run(args,cwd=build,check=True)
result=subprocess.run([str(binary)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=15)
(out/'host-pi-donation.log').write_bytes(result.stdout);print(result.stdout.decode(),end='');sys.exit(result.returncode)
