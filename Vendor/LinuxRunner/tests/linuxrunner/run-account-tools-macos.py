#!/usr/bin/env python3
"""Exercise actual distro account/password/login tools in a disposable fixture.
Random test password travels only over a PTY, never argv, logs or persistence.
"""
import argparse,os,pathlib,pty,re,secrets,select,signal,subprocess,time,json
p=argparse.ArgumentParser();p.add_argument('--runtime',required=True);p.add_argument('--rootfs',required=True);p.add_argument('--distribution',choices=['debian','alpine'],required=True);p.add_argument('--output',required=True);a=p.parse_args()
runtime=str(pathlib.Path(a.runtime).resolve());out=pathlib.Path(a.output);out.mkdir(parents=True,exist_ok=True)
name='lrtest_'+str(os.getpid()); password='T9!'+secrets.token_urlsafe(24)
env=dict(os.environ,ISH_ARM64_MEMORY_PC_FUSION='0');env.pop('ISH_SYSTRACE',None)
base=[runtime,'-f',a.rootfs]
report={'environment':'Mac ARM64 interpreter guest; device UI not tested','distribution':a.distribution,'username':name}
def run(command):
 return subprocess.run(base+command,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90,check=True).stdout
created=False
try:
 create=['/usr/sbin/useradd','--create-home','--shell','/bin/bash','--',name] if a.distribution=='debian' else ['/usr/sbin/adduser','-D','-h','/home/'+name,'-s','/bin/sh',name]
 creation=run(create);created=True
 pid,master=pty.fork()
 if pid==0:
  os.execve(runtime,base+['/usr/bin/passwd',name],env)
 transcript=b'';sent=0;deadline=time.monotonic()+90;status=None
 try:
  while time.monotonic()<deadline:
   ready,_,_=select.select([master],[],[],.25)
   if ready:
    try: chunk=os.read(master,8192)
    except OSError:chunk=b''
    transcript+=chunk
    text=transcript.decode(errors='replace').lower()
    if sent==0 and re.search(r'(new password|enter new password):',text):os.write(master,password.encode()+b'\n');sent=1
    elif sent==1 and re.search(r'(retype.*password|repeat.*password):',text):os.write(master,password.encode()+b'\n');sent=2
   child,status0=os.waitpid(pid,os.WNOHANG)
   if child:status=status0;break
  if status is None:
   os.kill(pid,signal.SIGTERM);os.waitpid(pid,0);raise RuntimeError('passwd exceeded bounded PTY timeout')
 finally:os.close(master)
 text=transcript.decode(errors='replace')
 assert password not in text,'PTY password echo is unsafe'
 assert sent==2 and os.WIFEXITED(status) and os.WEXITSTATUS(status)==0,'actual guest passwd did not complete'
 # Only hash scheme is returned; the password hash never enters a saved log.
 check='''test -d "/home/$1"; awk -F: -v n="$1" '$1==n { split($2,a,"$"); if(a[2]!="y" && a[2]!="6" && a[2]!="5" && a[2]!="2b" && a[2]!="2y") exit 1; print "PASSWORD_SCHEME=" a[2]; found=1 } END{if(!found)exit 1}' /etc/shadow'''
 identity=run(['/bin/sh','-ec',check,'linuxrunner-account-check',name])
 login=run(['/bin/su','-l','-c','test "$(id -u)" != 0 && test "$USER" = "'+name+'" && test "$LOGNAME" = "$USER" && test "$(pwd)" = "$HOME" && ! test -r /etc/shadow && printf "ACCOUNT_LOGIN_PASS uid=%s home=%s\\n" "$(id -u)" "$HOME"',name])
 assert 'ACCOUNT_LOGIN_PASS' in login
 report.update(created_home=True,password_pty_no_echo=True,password_hash_scheme=re.search(r'PASSWORD_SCHEME=([^\s]+)',identity)[1],login_nonroot=True,home_environment=True,shadow_read_denied=True)
 (out/'guest-sanitized.log').write_text(creation+text+identity+login)
finally:
 if created:
  cleanup=['/usr/sbin/userdel','--remove',name] if a.distribution=='debian' else ['/usr/sbin/deluser','--remove-home',name]
  try:run(cleanup);report['cleanup']=True
  except Exception:report['cleanup']=False;raise
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n')
print('ACCOUNT_TOOLS_PASS',a.distribution,json.dumps(report))
