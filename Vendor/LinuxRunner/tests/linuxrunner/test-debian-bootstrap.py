#!/usr/bin/env python3
"""Focused parser/codec tests against the Xcode-built archive library, not macOS system libarchive."""
import argparse,io,lzma,os,pathlib,subprocess,tarfile,tempfile
p=argparse.ArgumentParser();p.add_argument('--driver',required=True);p.add_argument('--apt',required=True);a=p.parse_args();driver=str(pathlib.Path(a.driver).resolve());apt=str(pathlib.Path(a.apt).resolve())
def tar(files):
 stream=io.BytesIO()
 with tarfile.open(fileobj=stream,mode='w',format=tarfile.GNU_FORMAT) as archive:
  for name,data in files:
   e=tarfile.TarInfo(name);e.size=len(data);e.mode=0o644;archive.addfile(e,io.BytesIO(data))
 return lzma.compress(stream.getvalue())
def deb(path,control,data):
 content=bytearray(b'!<arch>\n')
 for name,payload in [('debian-binary',b'2.0\n'),('control.tar.xz',control),('data.tar.xz',data)]:
  header=f'{name+"/":<16}{0:<12}{0:<6}{0:<6}{100644:<8}{len(payload):<10}`\n'.encode();assert len(header)==60
  content+=header+payload+(b'\n' if len(payload)%2 else b'')
 path.write_bytes(content)
with tempfile.TemporaryDirectory(prefix='lr-bootstrap-test-') as folder:
 root=pathlib.Path(folder);env=dict(os.environ,PATH='/no-external-codecs')
 def run(name,package,expected=None,extra=None,output=None):
  dest=output or root/(name+'.tar.gz');result=subprocess.run([driver,str(dest),str(package)],capture_output=True,text=True,env=dict(env,**(extra or {})))
  if expected:
   assert result.returncode!=0 and expected in result.stderr,(name,result.stderr)
   assert not dest.exists(),name+' left a partial archive'
  else:assert result.returncode==0,(name,result.stderr)
  print('PASS:',name)
  return result
 run('first-official-apt-no-external-xz',apt)
 bad=root/'bad.deb';bad.write_bytes(b'not a Debian archive');run('invalid-ar',bad,'category=archive_open')
 alias=root/'alias.deb';alias.symlink_to(apt);run('symlink-input-rejected',alias,'category=filesystem')
 control=tar([('./control',b'Package: probe\nVersion: 1\nArchitecture: arm64\nDescription: probe\n')])
 unsafe=root/'unsafe.deb';deb(unsafe,control,tar([('../escaped',b'no')]));run('traversal-rejected',unsafe,'category=validation');assert not (root.parent/'escaped').exists()
 broken=root/'broken.deb';deb(broken,b'\xfd7zXZ\x00'+b'broken',tar([('./safe',b'ok')]));run('corrupt-xz',broken,'category=decompression')
 run('cancellation-is-distinct',apt,'category=cancellation',{'LR_BOOTSTRAP_CANCEL_AFTER':'5'})
 run('output-open-is-filesystem-error',apt,'category=filesystem',output=root/'absent/output.tar.gz')
 unicode_package=root/'unicode.deb';unicode_name='./certs/Főtanúsítvány.crt';deb(unicode_package,control,tar([(unicode_name,b'certificate')]))
 run('linux-filename-bytes-in-C-locale',unicode_package,extra={'LC_ALL':'C','LANG':'C'})
 with tarfile.open(root/'linux-filename-bytes-in-C-locale.tar.gz') as archive:
  assert archive.extractfile(unicode_name).read()==b'certificate'
 print('PASS: 8 targeted bootstrap/codec/error-classification regressions')
