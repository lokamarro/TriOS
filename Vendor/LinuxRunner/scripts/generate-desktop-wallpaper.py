#!/usr/bin/env python3
"""Original code-generated LinuxRunner wallpaper; no third-party visual assets.
Requires Pillow. Output is a packaged PNG displayed by real guest PCManFM.
"""
from pathlib import Path
from PIL import Image
import math
w,h=1280,960
image=Image.new('RGB',(w,h));pixels=image.load()
for y in range(h):
 for x in range(w):
  a=math.exp(-((x-w*.8)**2/(w*.55)**2+(y-h*.2)**2/(h*.55)**2)*3)
  b=math.exp(-((x-w*.25)**2/(w*.8)**2+(y-h*.85)**2/(h*.4)**2)*4)
  ribbon=math.exp(-((y/h-.55-.14*math.sin(x/w*5))/.035)**2)*.19
  pixels[x,y]=(int(9+5*a+5*b+4*ribbon),int(17+35*a+10*b+28*ribbon),int(31+49*a+31*b+38*ribbon))
image.save(Path(__file__).resolve().parents[1]/'app/LinuxRunner/GUISupport.bundle/desktop-wallpaper.png',optimize=True)
