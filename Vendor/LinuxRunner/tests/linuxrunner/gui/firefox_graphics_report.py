#!/usr/bin/env python3
"""Read Firefox's own Graphics tables through opt-in Marionette in a test profile.
No account/profile information is collected. Not a performance benchmark.
"""
import argparse,json,socket,time,pathlib
p=argparse.ArgumentParser();p.add_argument('--port',type=int,default=2828);p.add_argument('--output',required=True);p.add_argument('--timeout',type=int,default=120);a=p.parse_args()
began=time.monotonic();deadline=began+a.timeout;s=None
phase='connect';phase_began=began;phases=[]
def finish_phase():
 phases.append({'stage':phase,'elapsed_ms':(time.monotonic()-phase_began)*1000})
def start_phase(name):
 global phase,phase_began
 phase=name;phase_began=time.monotonic()
def read():
 remaining=deadline-time.monotonic()
 if remaining<=0:raise TimeoutError('Overall diagnostic deadline exceeded')
 s.settimeout(remaining)
 size=b''
 while True:
  b=s.recv(1)
  if not b:raise RuntimeError('Marionette closed')
  if b==b':':break
  size+=b
  if len(size)>10:raise RuntimeError('Invalid packet length')
 n=int(size);assert n<=2*1024*1024
 raw=b''
 while len(raw)<n:
  part=s.recv(n-len(raw))
  if not part:raise RuntimeError('Truncated packet')
  raw+=part
 return json.loads(raw)
ident=0
def command(name,args):
 global ident
 start_phase(name)
 ident+=1;packet=json.dumps([0,ident,name,args]).encode();s.sendall(str(len(packet)).encode()+b':'+packet)
 while True:
  r=read()
  if r[0]==1 and r[1]==ident:
   if r[2]:raise RuntimeError(r[2])
   finish_phase();return r[3]
try:
 while True:
  try:s=socket.create_connection(('127.0.0.1',a.port),timeout=min(1,max(.001,deadline-time.monotonic())));break
  except OSError:
   if time.monotonic()>deadline:raise TimeoutError('MARIONETTE_UNAVAILABLE')
   time.sleep(min(.5,max(0,deadline-time.monotonic())))
 finish_phase();start_phase('greeting');greeting=read()
 if not isinstance(greeting,dict) or greeting.get('marionetteProtocol')!=3:raise RuntimeError('Unsupported Marionette protocol')
 finish_phase()
 command('WebDriver:NewSession',{'capabilities':{'alwaysMatch':{'acceptInsecureCerts':False}}})
 command('WebDriver:Navigate',{'url':'about:support'})
 report=command('WebDriver:ExecuteScript',{'script':"return Array.from(document.querySelectorAll('[id^=graphics]')).map(e => ({id:e.id,text:e.innerText})).filter(e=>e.text);",'args':[],'newSandbox':True})
 command('WebDriver:DeleteSession',{})
 pathlib.Path(a.output).write_text(json.dumps({'status':'PASS','environment':'macOS Linux ARM64 fixture','graphics':report,'phases':phases,'total_ms':(time.monotonic()-began)*1000},indent=2)+'\n')
except (OSError,RuntimeError,ValueError) as error:
 pathlib.Path(a.output).write_text(json.dumps({'status':'FAIL','environment':'macOS Linux ARM64 fixture','error':str(error),'failed_stage':phase,'failed_stage_ms':(time.monotonic()-phase_began)*1000,'phases':phases,'total_ms':(time.monotonic()-began)*1000},indent=2)+'\n')
 raise SystemExit('GRAPHICS_REPORT_FAILED: '+str(error))
finally:
 if s:s.close()
