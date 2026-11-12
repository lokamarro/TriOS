#!/usr/bin/env python3
"""Bounded Mac guest tests for the HQ interposer; never device/cube proof."""
import argparse,hashlib,json,os,pathlib,shutil,subprocess,time
p=argparse.ArgumentParser();p.add_argument('--mode',choices=['contract','blender'],required=True);p.add_argument('--rebuild',action='store_true',help='Build the native runtime and Metal backend from this worktree');p.add_argument('--scene',choices=['baseline','hidden','moved'],default='baseline',help='Controlled Blender scene variant for framebuffer comparison');p.add_argument('--program-filter',default='',help='Optional GL program label substring: other draws stay on Mesa');p.add_argument('--max-draws',type=int,default=128);a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1];os.chdir(root)
fixture=root/'build/fixtures/debian-rootfs';exchange=root/'build/hq-exchange';tools=root/'build/hq-gpu-tools'
out=root/'build/hq-gpu-tests'/f'{a.mode}-{time.time_ns()}';out.mkdir(parents=True)
(root/'build/qt-tests').mkdir(parents=True,exist_ok=True)
# Source hashes alone do not prove which fs/gpu.o a historical driver linked.
# Rebuild the static runtime too, rather than validating a stale native archive.
backend=['app/LinuxRunner/GuestMetalBackend.h','graphics/shader_compiler.cpp','graphics/guest_gpu.h','graphics/shader_compiler.h','fs/gpu.c','tests/linuxrunner/gpu/host_driver.m']
if not a.rebuild:
 raise SystemExit('BACKEND_REBUILD_REQUIRED: use --rebuild; historical static-library provenance is unavailable')
runtime=out/'ish-metal'
with (out/'native-build.log').open('w') as log:
 def build(argv):subprocess.run(argv,stdout=log,stderr=subprocess.STDOUT,timeout=300,check=True)
 if not (root/'build/host/build.ninja').exists():build(['meson','setup','build/host','-Dguest_arch=arm64','-Dengine=asbestos','-Djit=false','-Djit_emit=false','-Dbuildtype=release'])
 build(['ninja','-C','build/host'])
 build(['bash','scripts/build-shader-compiler.sh','macosx'])
 build(['clang','-O2','-g','-I.','-Ibuild/host','-DGUEST_ARM64=1','-DENGINE_ASBESTOS=1','-D_XOPEN_SOURCE=700','-D_DARWIN_C_SOURCE','-DLOG_HANDLER_DPRINTF=1','-Dmain=lr_cli_main','-c','main.c','-o',str(out/'main.o')])
 build(['clang++','-std=c++17','-O2','-Ideps/glslang','-Ideps/spirv-cross','-c','graphics/shader_compiler.cpp','-o',str(out/'shader.o')])
 build(['clang','-O2','-g','-fobjc-arc','-I.','tests/linuxrunner/gpu/host_driver.m',str(out/'main.o'),str(out/'shader.o'),'build/shader-compiler/macosx/liblrshader.a','-lc++','build/host/libish.a','build/host/libish_emu.a','build/host/libfakefs.a','-lm','-ldl','-lsqlite3','-framework','Foundation','-framework','Metal','-o',str(runtime)])
native={'runtime_sha256':hashlib.sha256(runtime.read_bytes()).hexdigest(),'sources':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in backend},'archives':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in ['build/host/libish.a','build/host/libish_emu.a','build/host/libfakefs.a','build/shader-compiler/macosx/liblrshader.a']}}
(out/'backend-source-identity.json').write_text(json.dumps(native,indent=2)+'\n')
for f in ['graphics/mesa_metal_dispatch.c','graphics/guest_gpu.h','graphics/shader_compiler.h','tests/linuxrunner/gpu/mesa_metal_contract.c','tests/linuxrunner/gpu/interop_transfer_contract.c']:
 shutil.copy2(root/f,exchange/pathlib.Path(f).name)
if a.mode=='blender': shutil.copy2(root/'tests/linuxrunner/gpu/blender_startup_probe.py',exchange/'blender_startup_probe-task9.py')
identity={'environment':'macOS ARM64 interpreter + freshly rebuilt Metal backend','physical_device':False,'runtime_sha256':hashlib.sha256(runtime.read_bytes()).hexdigest(),'interposer_source_sha256':hashlib.sha256((root/'graphics/mesa_metal_dispatch.c').read_bytes()).hexdigest(),'source_revision':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'source_worktree_dirty':bool(subprocess.check_output(['git','status','--porcelain'],text=True).strip()),'mode':a.mode}
(out/'execution-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange))
with (out/'compile.log').open('w') as log:
 subprocess.run([str(root/'build/host/ish'),'-f',str(fixture),'/bin/sh','-ec','cd /home/mobile/Downloads; chmod 755 .linuxrunner/gui/focus-manager; if [ -f .linuxrunner/gui/window-observer ]; then chmod 755 .linuxrunner/gui/window-observer; fi; gcc -O2 -Wall -Werror=maybe-uninitialized -fPIC -shared mesa_metal_dispatch.c -lGL -lX11 -ldl -lm -lz -pthread -o liblinuxrunner-mesa-metal.so; gcc -O2 mesa_metal_contract.c -lGL -lX11 -ldl -o gpu-mesa-metal-contract; gcc -O2 interop_transfer_contract.c -o gpu-interop-transfer-contract'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
if a.mode=='contract':
 command=exchange/'hybrid-contract-command.sh';probe=tools/'mesa-metal-probe';geometry='640x480';timeout=90
else:
 command=out/'blender-command.sh';probe=tools/'blender-display-probe';geometry='960x540';timeout=300
 # Never reuse a previous modeling report or saved scene as current evidence.
 lines=(exchange/'blender-hybrid-command.sh').read_text().splitlines()
 lines=[line for line in lines if not line.startswith('export LR_BLENDER_PROBE_DIR=')]
 insert=next(i for i,line in enumerate(lines) if line.startswith('exec '))
 if a.scene=='baseline':
  lines.insert(insert,'export LR_BLENDER_PROBE_DIR=/home/mobile/Downloads/'+out.name+'-blender-probe')
  lines.insert(insert,'export LR_MESA_METAL_TRACE=1')
 else:
  lines.insert(insert,'export LR_BLENDER_PROBE_DIR=/home/mobile/Downloads/'+out.name+'-blender-probe')
  lines.insert(insert,'export LR_MESA_METAL_TRACE=1')
  lines.insert(insert,'export LR_BLENDER_SCENE_VARIANT='+a.scene)
 import shlex
 lines.insert(insert,'export LR_MESA_METAL_MAX_DRAWS='+str(a.max_draws))
 lines.insert(insert,'export LR_MESA_METAL_PROGRAM_FILTER='+shlex.quote(a.program_filter))
 command.write_text('\n'.join(lines)+'\n');command.chmod(0o600)
with (out/'run.log').open('w') as log:
 run=subprocess.run(['python3','tests/linuxrunner/gui/run_guest_session.py','--rootfs',str(fixture),'--exchange',str(exchange),'--runtime',str(runtime),'--probe',str(probe),'--mode','custom','--script',str(command),'--geometry',geometry,'--output',str(out/'session'),'--timeout',str(timeout)],env=dict(os.environ,LR_FIXTURE_GLX='software',LR_BLENDER_SECONDS='180',LR_MESA_METAL_TRACE='1'),stdout=log,stderr=subprocess.STDOUT,timeout=timeout+20)
print('HQ_GPU_RESULT',json.dumps({'mode':a.mode,'exit_code':run.returncode,'output':str(out),'identity':identity}))
raise SystemExit(run.returncode)
