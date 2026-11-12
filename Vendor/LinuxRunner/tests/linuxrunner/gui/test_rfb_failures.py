#!/usr/bin/env python3
"""Exercise the actual native viewer against deliberately invalid local servers."""
import argparse,json,os,pathlib,socket,struct,subprocess,threading
p=argparse.ArgumentParser();p.add_argument('--probe',required=True);p.add_argument('--output',default='build/gui-tests/protocol-negative');a=p.parse_args()
probe=pathlib.Path(a.probe).resolve()
if not probe.is_file() or not os.access(probe,os.X_OK):p.error('Native probe must exist and be executable')
out=pathlib.Path(a.output);out.mkdir(parents=True,exist_ok=True);password=out/'password';password.write_text('Test2345');password.chmod(0o600)
def exact(s,n):
 data=b''
 while len(data)<n:
  part=s.recv(n-len(data))
  if not part:raise EOFError
  data+=part
 return data
results=[]
for case,needle in [('no-auth','authentication required'),('oversize','exceed limits'),('bad-rectangle','invalid rectangle'),('hextile-overflow','Hextile'),('hextile-no-background','Hextile')]:
 server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(10);port=server.getsockname()[1]
 def serve():
  try:
   s,_=server.accept();s.settimeout(10)
   with s:
    s.sendall(b'RFB 003.008\n');exact(s,12)
    if case=='no-auth':s.sendall(b'\x01\x01');s.recv(16);return
    s.sendall(b'\x01\x02');assert exact(s,1)==b'\x02';s.sendall(bytes(range(16)));exact(s,16);s.sendall(bytes(4));exact(s,1)
    width=65535 if case=='oversize' else 32
    s.sendall(struct.pack('!HH',width,32)+bytes(16)+bytes(4))
    if case=='oversize':s.recv(16);return
    exact(s,20);header=exact(s,4);exact(s,struct.unpack('!H',header[2:])[0]*4);exact(s,10)
    if case=='bad-rectangle':s.sendall(b'\x00\x00\x00\x01'+struct.pack('!HHHHi',31,31,2,2,0))
    else:
     tile=b'\x0e'+bytes(8)+b'\x01\xff\xff' if case=='hextile-overflow' else b'\x00'
     s.sendall(b'\x00\x00\x00\x01'+struct.pack('!HHHHi',0,0,8,8,5)+tile)
    s.recv(16)
  except (EOFError,ConnectionError,TimeoutError):pass
  finally:server.close()
 thread=threading.Thread(target=serve,daemon=True);thread.start()
 result=subprocess.run([str(probe),str(port),str(password),str(out/case)],text=True,capture_output=True,timeout=15);thread.join(timeout=2)
 report=json.loads((out/case/'probe.json').read_text());passed=result.returncode==1 and needle.lower() in result.stderr.lower() and not report['rendered']
 results.append({'case':case,'passed':passed,'viewer_status':result.stderr.strip(),'frame_presented':report['rendered']});assert passed,results[-1]
password.unlink();(out/'results.json').write_text(json.dumps(results,indent=2)+'\n');print('Authentication refusal, oversized surface and malformed rectangle rejected before presentation')
