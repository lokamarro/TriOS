#!/usr/bin/env python3
"""Real callback delivery when a concurrent-equivalent getter clears its property."""
import argparse,json,pathlib,socket,struct,subprocess,tempfile,threading
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',required=True);p.add_argument('--output',required=True);p.add_argument('--expect',choices=['reread','snapshot'],required=True);a=p.parse_args()
def exact(s,n):
 b=bytearray()
 while len(b)<n:
  x=s.recv(n-len(b))
  if not x:raise EOFError
  b.extend(x)
 return bytes(b)
out=pathlib.Path(a.output).resolve();out.mkdir(parents=True,exist_ok=True);reports=[]
for case in ['status','patch','damage','frame']:
 server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(2);port=server.getsockname()[1]
 def serve():
  try:
   s,_=server.accept()
   with s:
    s.settimeout(2);s.sendall(b'RFB 003.008\n');exact(s,12);s.sendall(b'\1\2');assert exact(s,1)==b'\2';s.sendall(bytes(16));exact(s,16);s.sendall(bytes(4));exact(s,1)
    s.sendall(struct.pack('!HH',32,16)+bytes(20));exact(s,20);h=exact(s,4);exact(s,struct.unpack('!H',h[2:])[0]*4);exact(s,10)
    s.sendall(b'\0\0\0\1'+struct.pack('!HHHHi',0,0,32,16,0)+bytes([0x6d])*2048)
    while s.recv(64):pass
  except (EOFError,ConnectionError,TimeoutError):pass # baseline clears status before handshake
  finally:server.close()
 with tempfile.TemporaryDirectory() as tmp:
  password=pathlib.Path(tmp)/'password';password.write_text('Test2345');password.chmod(0o600)
  t=threading.Thread(target=serve,daemon=True);t.start()
  r=subprocess.run([str(pathlib.Path(a.probe).resolve()),str(port),str(password),str(out/(case+'.json')),case],capture_output=True,text=True,timeout=6);t.join(3)
  assert not t.is_alive() and r.returncode==0,(case,r.returncode,r.stderr)
 report=json.loads((out/(case+'.json')).read_text());assert report['getter_clear_injected'],report
 if a.expect=='reread':assert report['cleared_callback_reads']>=1 and report['sentinel_invoked'],report
 else:
  assert report['valid_pixels'] and report['frames']==1 and not report['sentinel_invoked'],report
  # status may be queried later for another status; only NULL invocation is forbidden.
  if case!='status':assert report['cleared_callback_reads']==0,report
 reports.append(report)
(out/'results.json').write_text(json.dumps(reports,indent=2)+'\n');print('RFB_CALLBACK_PASS',a.expect,'status/patch/damage/frame; no test process crash used')
