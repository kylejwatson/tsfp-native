#!/bin/bash
# usage: run_seq.sh <outdir> <seq> <secs> [dump_every]
# Output goes to runs/<outdir>/ (frames, montage.png) and runs/<outdir>.log.
# <seq> may be @file (e.g. @../seqs/brain_drain.seq). RECOMP_MOUSE=0 keeps the
# window from capturing the desktop cursor; scripted mouse_* steps still work.
cd /c/Users/mail/Documents/mygame/build
mkdir -p ../runs/$1; rm -f ../runs/$1/*.bmp ../runs/$1/*.png 2>/dev/null
env RECOMP_MOUSE=0 $EXTRA RECOMP_MUTE=1 RECOMP_FPS=5 RECOMP_INPUT_SEQ="$2" RECOMP_HLE_D3D8_DUMP=../runs/$1/h RECOMP_HLE_D3D8_DUMP_EVERY=${4:-200} RECOMP_HLE_D3D8_DUMP_MINDRAWS=${MINDRAWS:-1} timeout $3 ./futureperfect_recomp.exe > ../runs/$1.log 2>&1
cd ../runs
py -3 ../xboxrecomp/scripts/bmp_to_png.py -o $1 $1/h*.bmp >/dev/null
py -3 - <<PY
from PIL import Image
import glob
fs=sorted(glob.glob('$1/h*.png')); w,h=320,240; cols=4
m=Image.new('RGB',(cols*w,((len(fs)+cols-1)//cols)*h))
for i,f in enumerate(fs): m.paste(Image.open(f).resize((w,h)),((i%cols)*w,(i//cols)*h))
m.save('$1/montage.png'); print(len(fs),'frames')
PY
grep -n "CRASH" $1.log | head -3
