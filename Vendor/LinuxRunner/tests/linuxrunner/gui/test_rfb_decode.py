#!/usr/bin/env python3
"""Actual receiver pixels: fragmented full/partial Raw, Hextile, resize and truncation."""
import argparse,hashlib,json,pathlib,socket,struct,subprocess,tempfile,threading
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--probe',required=True);p.add_argument('--output',required=True);p.add_argument('--delivery',choices=['patch','snapshot'],default='patch');p.add_argument('--expect-raw-bytes',required=True,type=int);a=p.parse_args()
server=socket.socket();server.bind(('127.0.0.1',0));server.listen(1);server.settimeout(8);port=server.getsockname()[1];errors=[];expected=[]
def exact(s,n):
 b=bytearray()
 while len(b)<n:
  part=s.recv(n-len(b))
  if not part:raise EOFError('client disconnected')
  b.extend(part)
 return bytes(b)
def rectangle(x,y,w,h,encoding):return struct.pack('!HHHHi',x,y,w,h,encoding)
def remember(pixels,w,h):expected.append({'width':w,'height':h,'frame':len(expected)+1,'sha256':hashlib.sha256(pixels).hexdigest()})
def update(s,body,count=1):
 assert exact(s,10)[0]==3
 payload=b'\0\0'+struct.pack('!H',count)+body
 for i in range(0,len(payload),37):s.sendall(payload[i:i+37])
def serve():
 try:
  s,_=server.accept()
  with s:
   s.settimeout(8);s.sendall(b'RFB 003.008\n');exact(s,12);s.sendall(b'\1\2');assert exact(s,1)==b'\2'
   s.sendall(bytes(range(16)));exact(s,16);s.sendall(bytes(4));exact(s,1)
   s.sendall(struct.pack('!HH',32,16)+bytes(20));exact(s,20);header=exact(s,4);exact(s,struct.unpack('!H',header[2:])[0]*4)
   image=bytearray(bytes(range(256))*8);remember(image,32,16);update(s,rectangle(0,0,32,16,0)+image)
   strip=bytes([0xab])*32*3*4;image[4*32*4:7*32*4]=strip;remember(image,32,16);update(s,rectangle(0,4,32,3,0)+strip)
   small=bytes([0xcd])*9*5*4
   for y in range(2,7):image[(y*32+7)*4:(y*32+16)*4]=small[(y-2)*9*4:(y-1)*9*4]
   remember(image,32,16);update(s,rectangle(7,2,9,5,0)+small)
   color=bytes([0x12,0x34,0x56,0xff])
   for y in range(8):image[y*32*4:(y*32+8)*4]=color*8
   remember(image,32,16);update(s,rectangle(0,0,8,8,5)+b'\2'+color)
   image=bytearray(bytes(range(128))*4);remember(image,16,8);update(s,rectangle(0,0,16,8,-223)+rectangle(0,0,16,8,0)+image,2)
   # An incomplete full-width frame must never be delivered to either callback.
   update(s,rectangle(0,0,16,8,0)+bytes([0x7f])*150)
 except Exception as e:errors.append(repr(e))
 finally:server.close()
out=pathlib.Path(a.output).resolve();out.parent.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory() as tmp:
 password=pathlib.Path(tmp)/'password';password.write_text('Test2345');password.chmod(0o600)
 thread=threading.Thread(target=serve,daemon=True);thread.start()
 r=subprocess.run([str(pathlib.Path(a.probe).resolve()),str(port),str(password),str(out),a.delivery],capture_output=True,text=True,timeout=9)
 thread.join(8);assert not thread.is_alive() and not errors,errors;assert r.returncode==0,(r.returncode,r.stderr)
report=json.loads(out.read_text());assert report['frames']==expected,report
assert report['retained_sha256']==[e['sha256'] for e in expected],report
assert report['peak_intermediate_raw_bytes']==a.expect_raw_bytes,report
print('RFB_DECODE_PASS',a.delivery,'5 pixel snapshots, resize and truncation; peak Raw scratch',report['peak_intermediate_raw_bytes'])
