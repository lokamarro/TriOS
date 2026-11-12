#!/usr/bin/env python3
"""Native RFB paste ordering and deterministic writer-backpressure contract."""
import argparse,json,pathlib,socket,struct,subprocess,tempfile,threading
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--probe',required=True);p.add_argument('--output',required=True)
p.add_argument('--count',type=int,default=16384);p.add_argument('--hold-writer',action='store_true')
p.add_argument('--expect-overflow',action='store_true');a=p.parse_args()
server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(6)
port=server.getsockname()[1];keys=[];pointers=[];errors=[]
def exact(s,n):
 b=bytearray()
 while len(b)<n:
  part=s.recv(n-len(b))
  if not part:raise EOFError('client disconnected')
  b.extend(part)
 return b
def serve():
 try:
  s,_=server.accept()
  with s:
   s.settimeout(6);s.sendall(b'RFB 003.008\n');exact(s,12)
   s.sendall(b'\x01\x02');assert exact(s,1)==b'\x02'
   s.sendall(bytes(16));exact(s,16);s.sendall(bytes(4));exact(s,1)
   s.sendall(struct.pack('!HH',32,32)+bytes(20));exact(s,20)
   e=exact(s,4);exact(s,struct.unpack('!H',e[2:])[0]*4);exact(s,10)
   s.sendall(bytes(3)+b'\x01'+struct.pack('!HHHHi',0,0,32,32,0)+bytes(4096))
   while True:
    kind=exact(s,1)[0]
    if kind==3:exact(s,9)
    elif kind==4:
     b=exact(s,7);keys.append((b[0],struct.unpack('!I',b[3:])[0]))
    elif kind==5:
     b=exact(s,5);point=(b[0],*struct.unpack('!HH',b[1:]));pointers.append(point)
     if point==(0,31,31):break
    else:raise AssertionError(f'bad message {kind}')
 except Exception as e:errors.append(str(e))
 finally:server.close()
out=pathlib.Path(a.output).resolve();out.parent.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
 password=pathlib.Path(tmp)/'password';password.write_text('Test2345');password.chmod(0o600)
 t=threading.Thread(target=serve,daemon=True);t.start()
 r=subprocess.run([a.probe,str(port),str(password),str(out),str(a.count),str(int(a.hold_writer))],capture_output=True,text=True,timeout=8)
 t.join(7)
assert not t.is_alive(),'server did not terminate'
report=json.loads(out.read_text());report.update(received_keys=len(keys),received_pointers=pointers,server_errors=errors,probe_exit=r.returncode)
out.write_text(json.dumps(report,indent=2)+'\n')
if a.expect_overflow:
 assert r.returncode==1 and report['failed'] and any('queue limit' in x.lower() for x in report['statuses']),report
 print('BASELINE_OVERFLOW_REPRODUCED',len(keys),'/',a.count*2+2)
else:
 assert r.returncode==0 and report['completed'] and not report['failed'] and not errors,report
 expected=[]
 for i in range(a.count):
  sym=0x010003bb if i%11==0 else ord('a')+i%26
  expected.extend(((1,sym),(0,sym)))
 expected.extend(((1,0xff0d),(0,0xff0d)))
 assert keys==expected,'key loss, reordering, or broken press/release pair'
 assert pointers==[(0,0,0),(0,31,31)],pointers
 print('RFB_PASTE_PASS',a.count,'characters, ordered unicode key pairs; enqueue_ms',round(report['enqueue_ms'],3))
