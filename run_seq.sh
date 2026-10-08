#!/bin/bash
# usage: run_seq.sh <outdir> <seq> <secs> [dump_every]
cd /c/Users/mail/Documents/mygame/build
mkdir -p ../$1; rm -f ../$1/*.bmp ../$1/*.png 2>/dev/null
RECOMP_MUTE=1 RECOMP_FPS=5 RECOMP_INPUT_SEQ="$2" RECOMP_HLE_D3D8_DUMP=../$1/h RECOMP_HLE_D3D8_DUMP_EVERY=${4:-200} RECOMP_HLE_D3D8_DUMP_MINDRAWS=1 timeout $3 ./futureperfect_recomp.exe > ../$1.log 2>&1
cd ..
py -3 xboxrecomp/scripts/bmp_to_png.py -o $1 $1/h*.bmp >/dev/null
py -3 - <<PY
from PIL import Image
import glob
fs=sorted(glob.glob('$1/h*.png')); w,h=320,240; cols=4
m=Image.new('RGB',(cols*w,((len(fs)+cols-1)//cols)*h))
for i,f in enumerate(fs): m.paste(Image.open(f).resize((w,h)),((i%cols)*w,(i//cols)*h))
m.save('$1/montage.png'); print(len(fs),'frames')
PY
grep -n "CRASH" $1.log | head -3
