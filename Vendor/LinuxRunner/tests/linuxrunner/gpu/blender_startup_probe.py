"""Blender's normal Python interface; isolated --factory-startup test only.

Records actual backend capability strings. No version spoofing, shader/source
patches, user preferences saved, or production profile access.
"""
import bpy, gpu, json, pathlib, time, os
start=time.monotonic()
out=pathlib.Path(os.environ.get('LR_BLENDER_PROBE_DIR','/home/mobile/Downloads/blender-probe-857'))
out.mkdir(exist_ok=True)
caps={name:getattr(gpu.platform,name)() for name in ('vendor_get','renderer_get','version_get','backend_type_get')}
libraries=sorted({line.split()[-1] for line in pathlib.Path('/proc/self/maps').read_text().splitlines()
                  if '/' in line and any(name in line for name in ('libGL','libEGL','libLLVM','_dri.so','libglapi'))})
caps['loaded_graphics_libraries']=libraries
print('BLENDER_GRAPHICS_ACTUAL '+json.dumps(caps),flush=True)
(out/'graphics.json').write_text(json.dumps(caps,indent=2))
bpy.context.preferences.view.show_splash=False
attempts=0
def inspect():
    global attempts
    attempts+=1
    windows=[{'width':w.width,'height':w.height,'areas':[a.type for a in w.screen.areas]} for w in bpy.context.window_manager.windows]
    cube=bpy.data.objects.get('Cube')
    variant=os.environ.get('LR_BLENDER_SCENE_VARIANT','baseline')
    if cube and variant=='hidden': cube.hide_viewport=True;cube.hide_render=True
    if cube and variant=='moved': cube.location.x=1.0
    model={'location':list(cube.location),'scale':list(cube.scale),'vertices':len(cube.data.vertices),'mode':cube.mode,'selected':cube.select_get()} if cube else {}
    report={'version':bpy.app.version_string,'windows':windows,'objects':list(bpy.data.objects.keys()),'model':model,'seconds_since_python':time.monotonic()-start,'backend':caps}
    (out/'startup.json').write_text(json.dumps(report,indent=2))
    print('BLENDER_STARTUP_STATE '+json.dumps(report),flush=True)
    if cube and cube.mode=='OBJECT' and len(cube.data.vertices)>8:
        bpy.ops.wm.save_as_mainfile(filepath=str(out/'input-scene.blend'))
        print('BLENDER_UI_MODEL_SAVED '+json.dumps(model),flush=True)
        return None
    return 5.0 if attempts<50 else None
bpy.app.timers.register(inspect,first_interval=2.0)
