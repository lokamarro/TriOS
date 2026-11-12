#!/usr/bin/python3
"""Run in a disposable Debian guest: interrupt index refresh, never dpkg writes."""
import os,pathlib,signal,subprocess,time
support=pathlib.Path('/home/mobile/Downloads/.linuxrunner/gui')
p=subprocess.Popen(['/bin/sh',str(support/'package-manager.sh'),'update'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,preexec_fn=os.setsid)
try:
 deadline=time.monotonic()+15
 while time.monotonic()<deadline:
  if p.poll() is not None:raise RuntimeError('refresh ended before interrupt; test inconclusive')
  if (support/'package-output.tmp').exists() and b'PACKAGE_OPERATION:' in (support/'package-output.tmp').read_bytes():break
  time.sleep(.05)
 else:raise RuntimeError('worker did not start')
 os.killpg(p.pid,signal.SIGINT)
 output,_=p.communicate(timeout=60);print(output.decode(errors='replace'))
 assert p.returncode==130,p.returncode
 assert (support/'package-result').read_text().strip()=='130'
 assert (support/'package-interrupted').exists()
 assert not (support/'.install-lock').exists()
 subprocess.run(['dpkg','--audit'],check=True)
 subprocess.run(['apt-get','check'],check=True)
 print('PACKAGE_INTERRUPT_PASS real_SIGINT status=130 marker_retained worker_reaped locks_released apt_check')
finally:
 if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.communicate(timeout=10)
