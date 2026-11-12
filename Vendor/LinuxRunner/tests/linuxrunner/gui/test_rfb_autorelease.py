#!/usr/bin/env python3
"""32 real RFB updates: callback autoreleases drain while retained snapshots survive."""
import argparse,json,pathlib,socket,struct,subprocess,tempfile,threading,time
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--probe',required=True);p.add_argument('--output',required=True)
p.add_argument('--expect',choices=['session','frame'],required=True)
p.add_argument('--expect-raw-bytes',type=int)
a=p.parse_args();server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(6);errors=[]
def exact(s,n):
 d=bytearray()
 while len(d)<n:
  part=s.recv(n-len(d))
  if not part:raise EOFError('client disconnected')
  d.extend(part)
 return bytes(d)
def serve():
 try:
  s,_=server.accept()
  with s:
   s.settimeout(6);s.sendall(b'RFB 003.008\n');exact(s,12);s.sendall(b'\x01\x02');assert exact(s,1)==b'\x02'
   s.sendall(bytes(range(16)));exact(s,16);s.sendall(bytes(4));exact(s,1)
   s.sendall(struct.pack('!HH',32,16)+bytes(16)+bytes(4));exact(s,20)
   h=exact(s,4);exact(s,struct.unpack('!H',h[2:])[0]*4)
   for i in range(32):
    assert exact(s,10)[0]==3
    s.sendall(b'\x00\x00\x00\x01'+struct.pack('!HHHHi',0,0,32,16,0)+bytes([i+1])*2048)
    time.sleep(.002)
   # Client keeps its incremental request outstanding, then stops itself.
   while s.recv(64):pass
 except Exception as e:errors.append(repr(e))
 finally:server.close()
out=pathlib.Path(a.output);out.parent.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
 password=pathlib.Path(tmp)/'password';password.write_text('Test2345');password.chmod(0o600)
 t=threading.Thread(target=serve,daemon=True);t.start()
 r=subprocess.run([str(pathlib.Path(a.probe).resolve()),str(server.getsockname()[1]),str(password),str(out.resolve())],capture_output=True,text=True,timeout=8)
 t.join(7);assert r.returncode==0,(r.returncode,r.stderr);assert not t.is_alive() and not errors,errors
report=json.loads(out.read_text());assert report['frames']==32 and report['retained_pixels_correct'] and report['live_after_stop']==0,report
if a.expect=='frame':assert report['peak_autoreleased_objects']==1 and report['live_before_stop']==0,report
else:assert report['peak_autoreleased_objects']==32 and report['live_before_stop']==32,report
if a.expect_raw_bytes is not None:assert report['peak_intermediate_raw_bytes']==a.expect_raw_bytes,report
print('RFB_AUTORELEASE_PASS',a.expect,json.dumps(report,sort_keys=True))
