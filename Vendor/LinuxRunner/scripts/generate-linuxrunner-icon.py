#!/usr/bin/env python3
"""Generate original opaque icon geometry matching Branding/LinuxRunnerIcon.svg."""
import json, pathlib
from PIL import Image, ImageDraw
root=pathlib.Path(__file__).resolve().parents[1]
out=root/'app/Assets.xcassets/LinuxRunnerAppIcon.appiconset';out.mkdir(exist_ok=True)
manifest=json.loads((root/'app/Assets.xcassets/AppIcon.appiconset/Contents.json').read_text())
scale=3;n=1024*scale
image=Image.new('RGB',(n,n));draw=ImageDraw.Draw(image)
for y in range(n):
 t=y/(n-1);color=tuple(round(a+(b-a)*t) for a,b in zip((11,20,35),(23,47,70)));draw.line((0,y,n,y),fill=color)
def line(points,color,width):
 p=[(x*scale,y*scale) for x,y in points];draw.line(p,fill=color,width=width*scale,joint='curve');r=width*scale/2
 for x,y in p:draw.ellipse((x-r,y-r,x+r,y+r),fill=color)
line([(385,290),(270,290),(270,734),(385,734)],'#eff6ff',64)
line([(515,305),(725,512),(515,719)],'#63b9f2',64)
line([(650,765),(765,765)],'#8adeef',42)
for item in manifest['images']:
 if 'filename' not in item:continue
 size=round(float(item['size'].split('x')[0])*float(item['scale'].rstrip('x')))
 image.resize((size,size),Image.Resampling.LANCZOS).save(out/item['filename'])
(out/'Contents.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('Generated opaque LinuxRunner icon set from original geometry')
