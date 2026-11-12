import sys, numpy as np
from PIL import Image, ImageDraw, ImageFont
from measure import backdrops, SUITE
fnt=ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",14)
b=backdrops()["visuals"]
before,after,out=sys.argv[1],sys.argv[2],sys.argv[3]; names=sys.argv[4:]
rows=[]
for n in names:
    g=np.load(f"{SUITE}/{n}.npz"); src=Image.open(f"{SUITE}/{n}.png").convert("RGBA")
    bg=Image.new("RGBA",src.size,(128,128,128,255)); bg.alpha_composite(src)
    ideal=g["gt_c"]+b*(1-g["gt_a"][...,None])
    def over(run):
        o=np.asarray(Image.open(f"{run}/render/{n}_0.png").convert("RGBA")).astype(float)/255
        return o[...,:3]+b*(1-o[...,3:])
    tiles=[(bg.convert("RGB"),"source"),(Image.fromarray((np.clip(over(before),0,1)*255).astype(np.uint8)),"before"),
           (Image.fromarray((np.clip(over(after),0,1)*255).astype(np.uint8)),"now"),(Image.fromarray((np.clip(ideal,0,1)*255).astype(np.uint8)),"ideal")]
    row=Image.new("RGB",(4*330,205),(25,25,25)); d=ImageDraw.Draw(row)
    for i,(t,l) in enumerate(tiles):
        row.paste(t.resize((320,180)),(5+i*330,22)); d.text((5+i*330,4),f"{n}: {l}",font=fnt,fill=(230,230,230))
    rows.append(row)
sheet=Image.new("RGB",(4*330,205*len(rows)),(25,25,25))
for i,r in enumerate(rows): sheet.paste(r,(0,i*205))
sheet.save(out); print(out, sheet.size)
